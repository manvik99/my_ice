#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUST_ROOT="$(cd "${C_ROOT}/../my_ice_rust" && pwd)"

C_BIN="${C_ROOT}/my_ice"
RUST_BIN="${RUST_ROOT}/target/release/my_ice_rust"

BDF=""
DURATION=30
RUNS=1
TX_BATCH_SIZE=64
RX_BATCH_SIZE=64
USE_HUGEPAGES=0
HUGEPAGE_DIR="/mnt/huge"
PIN_CPUS=0
OUTDIR="${C_ROOT}/results/perf_compare_1q/$(date -u +%Y%m%d_%H%M%S)"

EVENTS=(
    instructions
    cycles
    ref-cycles
    task-clock
    cpu-clock
    context-switches
    cpu-migrations
    page-faults
    minor-faults
    major-faults
    branches
    branch-misses
    cache-references
    cache-misses
    stalled-cycles-frontend
    stalled-cycles-backend
    L1-dcache-loads
    L1-dcache-load-misses
    L1-dcache-stores
    L1-icache-loads
    L1-icache-load-misses
    dTLB-loads
    dTLB-load-misses
    dTLB-stores
    dTLB-store-misses
    iTLB-loads
    iTLB-load-misses
    LLC-loads
    LLC-load-misses
    LLC-stores
    LLC-store-misses
)

usage() {
    cat <<'EOF'
Usage:
  sudo ./scripts/perf_compare_1q.sh --bdf <PCI-BDF> [options]

Options:
  --bdf <BDF>              PCI BDF to benchmark (required)
  --duration <seconds>     Reflect duration per run (default: 30)
  --runs <n>               Number of C/Rust runs each (default: 1)
  --tx-batch-size <n>      C tx batch size (default: 64)
  --rx-batch-size <n>      C rx reflect batch size and Rust reflect batch (default: 64)
  --out <dir>              Output directory
  --c-bin <path>           Override C binary path
  --rust-bin <path>        Override Rust binary path
  --hugepages              Pass hugepage options to both binaries
  --hugepage-dir <dir>     Hugepage directory (default: /mnt/huge)
  --pin-cpus               Pass --pin-cpus to both binaries
  -h, --help               Show this help

What it writes:
  raw_perf.csv             Long-form event table, one row per event/run
  summary.csv              Wide summary table, one row per implementation/run
  metadata.txt             Benchmark configuration
  runs/*                   Per-run stdout/stderr/perf/time logs
EOF
}

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "error: run this script as root (VFIO + perf bench)." >&2
        exit 1
    fi
}

require_tool() {
    local tool="$1"
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required tool not found: $tool" >&2
        exit 1
    fi
}

join_by_comma() {
    local IFS=,
    echo "$*"
}

is_numeric() {
    [[ "${1:-}" =~ ^-?[0-9]+([.][0-9]+)?$ ]]
}

safe_div() {
    local num="${1:-}"
    local den="${2:-}"
    if ! is_numeric "$num" || ! is_numeric "$den"; then
        return 0
    fi
    awk -v n="$num" -v d="$den" 'BEGIN { if (d == 0) exit 0; printf "%.6f", n / d }'
}

safe_pct() {
    local num="${1:-}"
    local den="${2:-}"
    if ! is_numeric "$num" || ! is_numeric "$den"; then
        return 0
    fi
    awk -v n="$num" -v d="$den" 'BEGIN { if (d == 0) exit 0; printf "%.6f", (100.0 * n) / d }'
}

write_metadata() {
    local meta_file="$1"
    {
        echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "hostname=$(hostname)"
        echo "kernel=$(uname -r)"
        echo "bdf=${BDF}"
        echo "duration_seconds=${DURATION}"
        echo "runs=${RUNS}"
        echo "c_binary=${C_BIN}"
        echo "rust_binary=${RUST_BIN}"
        echo "c_tx_batch_size=${TX_BATCH_SIZE}"
        echo "reflect_batch_size=${RX_BATCH_SIZE}"
        echo "use_hugepages=${USE_HUGEPAGES}"
        echo "hugepage_dir=${HUGEPAGE_DIR}"
        echo "pin_cpus=${PIN_CPUS}"
        echo "perf_events=$(join_by_comma "${EVENTS[@]}")"
    } > "${meta_file}"
}

append_time_rows() {
    local raw_csv="$1"
    local run_id="$2"
    local impl="$3"
    local time_file="$4"
    local ts_utc="$5"

    while IFS=, read -r key value; do
        [[ -z "${key}" ]] && continue
        local unit=""
        case "$key" in
            wall_seconds|user_seconds|sys_seconds) unit="seconds" ;;
            max_rss_kb) unit="kB" ;;
        esac
        printf '%s,%s,%s,time,%s,%s,%s,,,,ok\n' \
            "${ts_utc}" "${run_id}" "${impl}" "${key}" "${value}" "${unit}" >> "${raw_csv}"
    done < "${time_file}"
}

append_perf_rows() {
    local raw_csv="$1"
    local run_id="$2"
    local impl="$3"
    local perf_file="$4"
    local ts_utc="$5"

    while IFS=, read -r value unit event raw_runtime pct_running metric_value metric_unit _rest; do
        [[ -z "${value}" ]] && continue
        [[ "${value}" == \#* ]] && continue
        [[ -z "${event}" ]] && continue

        local status="ok"
        if [[ "${value}" == \<* ]]; then
            status="${value}"
            value=""
        fi

        printf '%s,%s,%s,perf,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${ts_utc}" "${run_id}" "${impl}" "${event}" "${value}" "${unit}" \
            "${raw_runtime}" "${pct_running}" "${metric_value}" "${metric_unit}" "${status}" \
            >> "${raw_csv}"
    done < "${perf_file}"
}

extract_raw_field() {
    local raw_csv="$1"
    local run_id="$2"
    local impl="$3"
    local event="$4"
    local column="$5"
    awk -F, -v run_id="$run_id" -v impl="$impl" -v event="$event" -v col="$column" '
        $2 == run_id && $3 == impl && $5 == event {
            print $col
            exit
        }
    ' "${raw_csv}"
}

append_summary_row() {
    local raw_csv="$1"
    local summary_csv="$2"
    local run_id="$3"
    local impl="$4"
    local exit_code="$5"

    local wall user sys rss
    local instructions cycles ref_cycles
    local task_clock cpu_clock context_switches cpu_migrations
    local page_faults minor_faults major_faults
    local branches branch_misses cache_refs cache_misses
    local stalled_front stalled_back
    local l1d_loads l1d_misses l1d_stores l1i_loads l1i_misses
    local dtlb_loads dtlb_misses dtlb_stores dtlb_store_misses
    local itlb_loads itlb_misses llc_loads llc_misses llc_stores llc_store_misses
    local ipc branch_miss_pct cache_miss_pct l1d_miss_pct l1i_miss_pct
    local dtlb_load_miss_pct dtlb_store_miss_pct itlb_load_miss_pct llc_load_miss_pct
    local cpus_utilized

    wall="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "wall_seconds" 6)"
    user="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "user_seconds" 6)"
    sys="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "sys_seconds" 6)"
    rss="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "max_rss_kb" 6)"

    instructions="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "instructions" 6)"
    cycles="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "cycles" 6)"
    ref_cycles="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "ref-cycles" 6)"
    task_clock="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "task-clock" 6)"
    cpu_clock="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "cpu-clock" 6)"
    context_switches="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "context-switches" 6)"
    cpu_migrations="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "cpu-migrations" 6)"
    page_faults="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "page-faults" 6)"
    minor_faults="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "minor-faults" 6)"
    major_faults="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "major-faults" 6)"
    branches="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "branches" 6)"
    branch_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "branch-misses" 6)"
    cache_refs="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "cache-references" 6)"
    cache_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "cache-misses" 6)"
    stalled_front="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "stalled-cycles-frontend" 6)"
    stalled_back="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "stalled-cycles-backend" 6)"
    l1d_loads="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "L1-dcache-loads" 6)"
    l1d_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "L1-dcache-load-misses" 6)"
    l1d_stores="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "L1-dcache-stores" 6)"
    l1i_loads="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "L1-icache-loads" 6)"
    l1i_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "L1-icache-load-misses" 6)"
    dtlb_loads="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "dTLB-loads" 6)"
    dtlb_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "dTLB-load-misses" 6)"
    dtlb_stores="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "dTLB-stores" 6)"
    dtlb_store_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "dTLB-store-misses" 6)"
    itlb_loads="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "iTLB-loads" 6)"
    itlb_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "iTLB-load-misses" 6)"
    llc_loads="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "LLC-loads" 6)"
    llc_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "LLC-load-misses" 6)"
    llc_stores="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "LLC-stores" 6)"
    llc_store_misses="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "LLC-store-misses" 6)"
    cpus_utilized="$(extract_raw_field "${raw_csv}" "${run_id}" "${impl}" "task-clock" 10)"

    ipc="$(safe_div "${instructions}" "${cycles}")"
    branch_miss_pct="$(safe_pct "${branch_misses}" "${branches}")"
    cache_miss_pct="$(safe_pct "${cache_misses}" "${cache_refs}")"
    l1d_miss_pct="$(safe_pct "${l1d_misses}" "${l1d_loads}")"
    l1i_miss_pct="$(safe_pct "${l1i_misses}" "${l1i_loads}")"
    dtlb_load_miss_pct="$(safe_pct "${dtlb_misses}" "${dtlb_loads}")"
    dtlb_store_miss_pct="$(safe_pct "${dtlb_store_misses}" "${dtlb_stores}")"
    itlb_load_miss_pct="$(safe_pct "${itlb_misses}" "${itlb_loads}")"
    llc_load_miss_pct="$(safe_pct "${llc_misses}" "${llc_loads}")"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${run_id}" "${impl}" "${exit_code}" "${wall}" "${user}" "${sys}" "${rss}" \
        "${task_clock}" "${cpu_clock}" "${cpus_utilized}" \
        "${instructions}" "${cycles}" "${ref_cycles}" "${ipc}" \
        "${branches}" "${branch_misses}" "${branch_miss_pct}" \
        "${cache_refs}" "${cache_misses}" "${cache_miss_pct}" \
        "${l1d_loads}" "${l1d_misses}" "${l1d_miss_pct}" "${l1d_stores}" \
        "${l1i_loads}" "${l1i_misses}" "${l1i_miss_pct}" \
        "${dtlb_loads}" "${dtlb_misses}" "${dtlb_load_miss_pct}" \
        "${dtlb_stores}" "${dtlb_store_misses}" "${dtlb_store_miss_pct}" \
        "${itlb_loads}" "${itlb_misses}" "${itlb_load_miss_pct}" \
        "${llc_loads}" "${llc_misses}" "${llc_load_miss_pct}" \
        "${llc_stores}" "${llc_store_misses}" \
        "${context_switches}" "${cpu_migrations}" "${page_faults}" "${minor_faults}" "${major_faults}" \
        "${stalled_front}" "${stalled_back}" \
        >> "${summary_csv}"
}

run_one() {
    local impl="$1"
    local run_idx="$2"
    shift 2
    local -a cmd=( "$@" )

    local run_id="${impl}_run$(printf '%02d' "${run_idx}")"
    local run_dir="${OUTDIR}/runs/${run_id}"
    local perf_file="${run_dir}/perf.csv"
    local time_file="${run_dir}/time.csv"
    local stdout_file="${run_dir}/stdout.log"
    local stderr_file="${run_dir}/stderr.log"
    local cmd_file="${run_dir}/command.txt"
    local ts_utc
    local timeout_seconds
    local exit_code

    mkdir -p "${run_dir}"
    ts_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    timeout_seconds=$((DURATION + 20))

    printf '%q ' "${cmd[@]}" > "${cmd_file}"
    printf '\n' >> "${cmd_file}"

    echo "[perf] ${run_id}: starting"
    set +e
    timeout --signal=INT "${timeout_seconds}" \
        /usr/bin/time -f 'wall_seconds,%e\nuser_seconds,%U\nsys_seconds,%S\nmax_rss_kb,%M' -o "${time_file}" \
        perf stat --no-big-num -x, -o "${perf_file}" -e "$(join_by_comma "${EVENTS[@]}")" -- \
        "${cmd[@]}" > "${stdout_file}" 2> "${stderr_file}"
    exit_code=$?
    set -e

    if [[ "${exit_code}" -ne 0 ]]; then
        echo "[perf] ${run_id}: failed with exit code ${exit_code}" >&2
        echo "[perf] logs: ${run_dir}" >&2
        exit "${exit_code}"
    fi

    append_time_rows "${RAW_CSV}" "${run_id}" "${impl}" "${time_file}" "${ts_utc}"
    append_perf_rows "${RAW_CSV}" "${run_id}" "${impl}" "${perf_file}" "${ts_utc}"
    append_summary_row "${RAW_CSV}" "${SUMMARY_CSV}" "${run_id}" "${impl}" "${exit_code}"

    echo "[perf] ${run_id}: done"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bdf)
            BDF="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --runs)
            RUNS="$2"
            shift 2
            ;;
        --tx-batch-size)
            TX_BATCH_SIZE="$2"
            shift 2
            ;;
        --rx-batch-size)
            RX_BATCH_SIZE="$2"
            shift 2
            ;;
        --out)
            OUTDIR="$2"
            shift 2
            ;;
        --c-bin)
            C_BIN="$2"
            shift 2
            ;;
        --rust-bin)
            RUST_BIN="$2"
            shift 2
            ;;
        --hugepages)
            USE_HUGEPAGES=1
            shift
            ;;
        --hugepage-dir)
            HUGEPAGE_DIR="$2"
            shift 2
            ;;
        --pin-cpus)
            PIN_CPUS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

require_root
require_tool perf
require_tool timeout
require_tool /usr/bin/time

if [[ -z "${BDF}" ]]; then
    echo "error: --bdf is required" >&2
    usage
    exit 1
fi

if [[ ! -x "${C_BIN}" ]]; then
    echo "error: C binary not found or not executable: ${C_BIN}" >&2
    exit 1
fi

if [[ ! -x "${RUST_BIN}" ]]; then
    echo "error: Rust binary not found or not executable: ${RUST_BIN}" >&2
    echo "build it with: cargo build --release" >&2
    exit 1
fi

mkdir -p "${OUTDIR}/runs"
RAW_CSV="${OUTDIR}/raw_perf.csv"
SUMMARY_CSV="${OUTDIR}/summary.csv"
METADATA_FILE="${OUTDIR}/metadata.txt"

cat > "${RAW_CSV}" <<'EOF'
timestamp_utc,run_id,impl,source,event,value,unit,raw_runtime,pct_running,metric_value,metric_unit,status
EOF

cat > "${SUMMARY_CSV}" <<'EOF'
run_id,impl,exit_code,wall_seconds,user_seconds,sys_seconds,max_rss_kb,task_clock_msec,cpu_clock_msec,cpus_utilized,instructions,cycles,ref_cycles,ipc,branches,branch_misses,branch_miss_pct,cache_references,cache_misses,cache_miss_pct,L1_dcache_loads,L1_dcache_load_misses,L1_dcache_load_miss_pct,L1_dcache_stores,L1_icache_loads,L1_icache_load_misses,L1_icache_load_miss_pct,dTLB_loads,dTLB_load_misses,dTLB_load_miss_pct,dTLB_stores,dTLB_store_misses,dTLB_store_miss_pct,iTLB_loads,iTLB_load_misses,iTLB_load_miss_pct,LLC_loads,LLC_load_misses,LLC_load_miss_pct,LLC_stores,LLC_store_misses,context_switches,cpu_migrations,page_faults,minor_faults,major_faults,stalled_cycles_frontend,stalled_cycles_backend
EOF

write_metadata "${METADATA_FILE}"

COMMON_ARGS=( "${BDF}" --rx-reflect "${DURATION}" --rx-queues 1 )
if [[ "${USE_HUGEPAGES}" -eq 1 ]]; then
    COMMON_ARGS+=( --hugepages --hugepage-dir "${HUGEPAGE_DIR}" )
fi
if [[ "${PIN_CPUS}" -eq 1 ]]; then
    COMMON_ARGS+=( --pin-cpus )
fi

C_ARGS=( "${COMMON_ARGS[@]}" --tx-batch-size "${TX_BATCH_SIZE}" --rx-batch-size "${RX_BATCH_SIZE}" )
RUST_ARGS=( "${COMMON_ARGS[@]}" --tx-queues 1 --reflect-batch "${RX_BATCH_SIZE}" )

echo "[perf] output directory: ${OUTDIR}"
echo "[perf] pktgen can stay running while this script benchmarks both implementations."

for ((run = 1; run <= RUNS; run++)); do
    run_one "c" "${run}" "${C_BIN}" "${C_ARGS[@]}"
    run_one "rust" "${run}" "${RUST_BIN}" "${RUST_ARGS[@]}"
done

echo "[perf] finished"
echo "[perf] summary CSV: ${SUMMARY_CSV}"
echo "[perf] raw CSV: ${RAW_CSV}"
