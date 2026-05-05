#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_BDF="0000:17:00.0"
DEFAULT_DURATION_S=15
DEFAULT_QUEUES=(1 2 4 8)
DEFAULT_BATCH_SIZES=(1 2 4 8 16 32 64 128 256 512 1024)
DEFAULT_REPEAT_COUNT=1
PERF_EVENTS=(
  "instructions"
  "cycles"
  "stalled-cycles-frontend"
  "stalled-cycles-backend"
  "L1-dcache-load-misses"
  "L1-icache-load-misses"
  "LLC-load-misses"
  "LLC-stores"
  "LLC-store-misses"
  "L1-dcache-prefetches"
  "dTLB-load-misses"
  "iTLB-load-misses"
  "branches"
  "branch-misses"
  "node-loads"
  "node-load-misses"
  "faults"
  "cs"
  "cpu-migrations"
  "bus-cycles"
)
PLOT_SCRIPT="${REPO_ROOT}/scripts/plot_multi_rx_results.py"

QUEUES=("${DEFAULT_QUEUES[@]}")
RX_BATCH_SIZES=("${DEFAULT_BATCH_SIZES[@]}")
TX_BATCH_SIZES=("${DEFAULT_BATCH_SIZES[@]}")

BDF="${DEFAULT_BDF}"
DURATION_S="${DEFAULT_DURATION_S}"
REPEAT_COUNT="${DEFAULT_REPEAT_COUNT}"
RESULTS_ROOT="${REPO_ROOT}/results/multi-rx"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="${RESULTS_ROOT}/${TIMESTAMP}"
CSV_PATH="${RESULT_DIR}/aggregate.csv"
PERF_CSV_PATH="${RESULT_DIR}/perf.csv"
PERF_EVENTS_PATH="${RESULT_DIR}/perf_events.csv"
MATRIX_MODE=0
PERF_MODE=0
PERF_ACTIVE_EVENTS=()
EXTRA_ARGS=()

usage() {
  cat <<'EOF'
Usage: sudo ./scripts/multi_rx_bench.sh [options] [-- extra my_ice args]

Options:
  --bdf <pci-bdf>     PCI BDF to test (default: 0000:17:00.0)
  --seconds <n>       Duration per run in seconds (default: 15)
  --queues <list...>  Queue counts to test (default: 1 2 4 8)
  --rx-batch-size     RX batch sizes to test (default: 1 2 4 ... 1024)
  --tx-batch-size     TX batch sizes to test (default: 1 2 4 ... 1024)
  --matrix            Sweep all queue counts x rx batch sizes x tx batch sizes
  --repeat <n>        Repeat each parameter point n times (default: 1)
  --perf              Wrap each run in perf stat and collect perf.csv sidecars
  -h, --help          Show this help

Behavior:
  - Builds my_ice via make before benchmarking
  - Runs ./my_ice -vvvv <bdf> --rx-reflect <seconds> --rx-queues {1,2,4,8}
    with --rx-batch-size and --tx-batch-size matched at {1,2,4,...,1024}
  - With --matrix, runs the full queue x rx-batch-size x tx-batch-size matrix
  - With --repeat, appends -rep<N> to log/perf sidecar names for repeated runs
  - Saves logs to results/multi-rx/<timestamp>/q<N>-rb<R>-tb<T>[-rep<N>].log
  - With --perf, saves raw perf stat sidecars beside each log and parsed rows in perf.csv
  - Saves parsed aggregate stats to results/multi-rx/<timestamp>/aggregate.csv
  - Best-effort generates PNG plots in results/multi-rx/<timestamp>/plots/
  - Extra args after '--' are appended to every my_ice invocation

Example:
  sudo ./scripts/multi_rx_bench.sh
  sudo ./scripts/multi_rx_bench.sh --repeat 5 --perf
  sudo ./scripts/multi_rx_bench.sh --matrix --queues 4 8 --rx-batch-size 64 1024 --tx-batch-size 64 1024
  sudo ./scripts/multi_rx_bench.sh --bdf 0000:17:00.0 --seconds 20 -- --hugepages --hugepage-dir /mnt/huge
EOF
}

die() {
  echo "$*" >&2
  exit 1
}

parse_int_list() {
  local flag_name="$1"
  local min_value="$2"
  local max_value="$3"
  shift 3

  local -n out_ref="$1"
  shift

  out_ref=()
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == --* ]]; then
      break
    fi
    [[ "$1" =~ ^[0-9]+$ ]] || die "${flag_name} requires integer values"
    (( $1 >= min_value && $1 <= max_value )) ||
      die "${flag_name} values must be in ${min_value}..${max_value}"
    out_ref+=("$1")
    shift
  done

  ((${#out_ref[@]} > 0)) || die "${flag_name} requires at least one value"
}

validate_queue_list() {
  local q
  for q in "$@"; do
    case "$q" in
      1|2|4|8) ;;
      *) die "--queues supports only: 1 2 4 8" ;;
    esac
  done
}

join_by_comma() {
  local IFS=,
  printf '%s' "$*"
}

parse_metric() {
  local line="$1"
  local key="$2"
  sed -n "s/.*${key}=\([^ ]*\).*/\1/p" <<<"${line}" | tail -n1
}

parse_wire_metric() {
  local line="$1"
  local key="$2"
  sed -n "s/.* ${key}=\([^ ]*\) wire-Gbps.*/\1/p" <<<"${line}" | tail -n1
}

extract_final_line() {
  local log_file="$1"
  grep -E '^\[my_ice\] rx-reflect done:' "${log_file}" | tail -n1 || true
}

extract_queue_line() {
  local log_file="$1"
  grep -E '^\[my_ice\] RSS final queue stats' "${log_file}" | tail -n1 || true
}

parse_queue_metric() {
  local line="$1"
  local qidx="$2"
  local section="$3"
  local field="$4"

  python3 - "$line" "$qidx" "$section" "$field" <<'PY'
import re
import sys

line, qidx, section, field = sys.argv[1:5]
qidx = int(qidx)

pattern = re.compile(
    rf'{section}q{qidx}\(pkts=(\d+),([0-9.]+)% bytes=(\d+)\)'
)
m = pattern.search(line)
if not m:
    print("")
    sys.exit(0)

mapping = {
    'pkts': m.group(1),
    'pkts_pct': m.group(2),
    'bytes': m.group(3),
    'bytes_pct': m.group(2),
}
print(mapping.get(field, ""))
PY
}

write_csv_header() {
  {
    printf 'timestamp,bdf,duration_s,rx_queues,rx_batch_size,tx_batch_size,repeat_index,status,log_file,'
    printf 'seconds,tx_wire_gbps,rx_wire_gbps,tx_mpps,rx_mpps,tx_l2_gbps,rx_l2_gbps,'
    printf 'rx_pkts,rx_bytes,tx_pkts,tx_bytes,zero_copy_pkts,zero_copy_bytes,'
    printf 'tx_ring_full,rx_short,rx_errors,pool_empty,doorbells,vsi,gorc_delta,gotc_delta'
    local q
    for q in 0 1 2 3 4 5 6 7; do
      printf ',rxq%d_pkts,rxq%d_pkts_pct,rxq%d_bytes,rxq%d_bytes_pct' "$q" "$q" "$q" "$q"
      printf ',txq%d_pkts,txq%d_pkts_pct,txq%d_bytes,txq%d_bytes_pct' "$q" "$q" "$q" "$q"
    done
    printf '\n'
  } >"${CSV_PATH}"
}

write_perf_csv_header() {
  printf 'timestamp,bdf,duration_s,rx_queues,rx_batch_size,tx_batch_size,repeat_index,status,log_file,perf_file,event,value,unit,counter_runtime,running_pct,metric_value,metric_unit\n' >"${PERF_CSV_PATH}"
}

write_perf_events_header() {
  printf 'requested_event,status\n' >"${PERF_EVENTS_PATH}"
}

append_perf_event_status() {
  printf '%s,%s\n' "$1" "$2" >>"${PERF_EVENTS_PATH}"
}

init_perf_events() {
  local event
  local probe_out="${RESULT_DIR}/.perf-probe.out"
  local probe_err="${RESULT_DIR}/.perf-probe.err"

  write_perf_csv_header
  write_perf_events_header

  for event in "${PERF_EVENTS[@]}"; do
    rm -f "${probe_out}" "${probe_err}"
    set +e
    perf stat --no-big-num -x ';' -e "${event}" -o "${probe_out}" -- true >/dev/null 2>"${probe_err}"
    local rc=$?
    set -e

    if [[ ${rc} -eq 0 ]]; then
      PERF_ACTIVE_EVENTS+=("${event}")
      append_perf_event_status "${event}" "enabled"
      continue
    fi

    append_perf_event_status "${event}" "unsupported"
    echo "[multi-rx] warning: perf event unavailable, skipping: ${event}" >&2
    if [[ -s "${probe_err}" ]]; then
      while IFS= read -r line; do
        [[ -n "${line}" ]] || continue
        echo "[multi-rx] perf: ${line}" >&2
      done <"${probe_err}"
    fi
  done

  rm -f "${probe_out}" "${probe_err}"

  if ((${#PERF_ACTIVE_EVENTS[@]} == 0)); then
    echo "[multi-rx] warning: no requested perf events were available; continuing without perf collection"
  else
    echo "[multi-rx] perf events enabled: $(join_by_comma "${PERF_ACTIVE_EVENTS[@]}")"
  fi
}

append_failed_row() {
  local rxq="$1"
  local rx_batch_size="$2"
  local tx_batch_size="$3"
  local repeat_index="$4"
  local log_name="$5"

  {
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s' \
      "${TIMESTAMP}" "${BDF}" "${DURATION_S}" "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}" "failed" "${log_name}"
    printf ',,,,,,,,,,,,,,,,,,,,,,'
    local q
    for q in 0 1 2 3 4 5 6 7; do
        printf ',,,,,,,,'
    done
    printf '\n'
  } >>"${CSV_PATH}"
}

append_success_row() {
  local rxq="$1"
  local rx_batch_size="$2"
  local tx_batch_size="$3"
  local repeat_index="$4"
  local log_name="$5"
  local final_line="$6"
  local queue_line="$7"
  local seconds tx_wire_gbps rx_wire_gbps tx_mpps rx_mpps tx_l2_gbps rx_l2_gbps
  local rx_pkts rx_bytes tx_pkts tx_bytes zero_copy_pkts zero_copy_bytes
  local tx_ring_full rx_short rx_errors pool_empty doorbells vsi gorc_delta gotc_delta

  seconds="$(parse_metric "${final_line}" 'seconds')"
  tx_wire_gbps="$(parse_wire_metric "${final_line}" 'TX')"
  rx_wire_gbps="$(parse_wire_metric "${final_line}" 'RX')"
  tx_mpps="$(parse_metric "${final_line}" 'tx_mpps')"
  rx_mpps="$(parse_metric "${final_line}" 'rx_mpps')"
  tx_l2_gbps="$(parse_metric "${final_line}" 'tx_l2_gbps')"
  rx_l2_gbps="$(parse_metric "${final_line}" 'rx_l2_gbps')"
  rx_pkts="$(parse_metric "${final_line}" 'rx_pkts')"
  rx_bytes="$(parse_metric "${final_line}" 'rx_bytes')"
  tx_pkts="$(parse_metric "${final_line}" 'tx_pkts')"
  tx_bytes="$(parse_metric "${final_line}" 'tx_bytes')"
  zero_copy_pkts="$(parse_metric "${final_line}" 'zero_copy_pkts')"
  zero_copy_bytes="$(parse_metric "${final_line}" 'zero_copy_bytes')"
  tx_ring_full="$(parse_metric "${final_line}" 'tx_ring_full')"
  rx_short="$(parse_metric "${final_line}" 'rx_short')"
  rx_errors="$(parse_metric "${final_line}" 'rx_errors')"
  pool_empty="$(parse_metric "${final_line}" 'pool_empty')"
  doorbells="$(parse_metric "${final_line}" 'doorbells')"
  vsi="$(sed -n 's/.*VSI\([0-9][0-9]*\) GORC_delta=.*/\1/p' <<<"${final_line}" | tail -n1)"
  gorc_delta="$(parse_metric "${final_line}" 'GORC_delta')"
  gotc_delta="$(parse_metric "${final_line}" 'GOTC_delta')"

  {
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,' \
      "${TIMESTAMP}" "${BDF}" "${DURATION_S}" "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}" "ok" "${log_name}"
    printf '%s,%s,%s,%s,%s,%s,%s,' \
      "${seconds}" "${tx_wire_gbps}" "${rx_wire_gbps}" "${tx_mpps}" "${rx_mpps}" \
      "${tx_l2_gbps}" "${rx_l2_gbps}"
    printf '%s,%s,%s,%s,%s,%s,' \
      "${rx_pkts}" "${rx_bytes}" "${tx_pkts}" "${tx_bytes}" \
      "${zero_copy_pkts}" "${zero_copy_bytes}"
    printf '%s,%s,%s,%s,%s,%s,%s,%s' \
      "${tx_ring_full}" "${rx_short}" "${rx_errors}" "${pool_empty}" \
      "${doorbells}" "${vsi}" "${gorc_delta}" "${gotc_delta}"

    local q
    for q in 0 1 2 3 4 5 6 7; do
      printf ',%s,%s,%s,%s' \
        "$(parse_queue_metric "${queue_line}" "${q}" 'rx' 'pkts')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'rx' 'pkts_pct')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'rx' 'bytes')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'rx' 'bytes_pct')"
      printf ',%s,%s,%s,%s' \
        "$(parse_queue_metric "${queue_line}" "${q}" 'tx' 'pkts')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'tx' 'pkts_pct')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'tx' 'bytes')" \
        "$(parse_queue_metric "${queue_line}" "${q}" 'tx' 'bytes_pct')"
    done
    printf '\n'
  } >>"${CSV_PATH}"
}

append_perf_rows() {
  local rxq="$1"
  local rx_batch_size="$2"
  local tx_batch_size="$3"
  local repeat_index="$4"
  local status="$5"
  local log_name="$6"
  local perf_name="$7"
  local perf_file="$8"

  [[ -f "${perf_file}" ]] || return 0

  python3 - \
    "${PERF_CSV_PATH}" \
    "${TIMESTAMP}" \
    "${BDF}" \
    "${DURATION_S}" \
    "${rxq}" \
    "${rx_batch_size}" \
    "${tx_batch_size}" \
    "${repeat_index}" \
    "${status}" \
    "${log_name}" \
    "${perf_name}" \
    "${perf_file}" <<'PY'
import csv
import sys

(
    csv_path,
    timestamp,
    bdf,
    duration_s,
    rx_queues,
    rx_batch_size,
    tx_batch_size,
    repeat_index,
    status,
    log_name,
    perf_name,
    perf_file,
) = sys.argv[1:13]

with open(csv_path, "a", newline="") as out_handle:
    writer = csv.writer(out_handle)
    with open(perf_file, newline="") as perf_handle:
        for raw_line in perf_handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split(";")
            value = parts[0] if len(parts) > 0 else ""
            unit = parts[1] if len(parts) > 1 else ""
            event = parts[2] if len(parts) > 2 else ""
            counter_runtime = parts[3] if len(parts) > 3 else ""
            running_pct = parts[4] if len(parts) > 4 else ""
            metric_value = parts[5] if len(parts) > 5 else ""
            metric_unit = ";".join(parts[6:]) if len(parts) > 6 else ""

            if not event:
                continue

            writer.writerow(
                [
                    timestamp,
                    bdf,
                    duration_s,
                    rx_queues,
                    rx_batch_size,
                    tx_batch_size,
                    repeat_index,
                    status,
                    log_name,
                    perf_name,
                    event,
                    value,
                    unit,
                    counter_runtime,
                    running_pct,
                    metric_value,
                    metric_unit,
                ]
            )
PY
}

run_one() {
  local rxq="$1"
  local rx_batch_size="$2"
  local tx_batch_size="$3"
  local repeat_index="$4"
  local run_name="q${rxq}-rb${rx_batch_size}-tb${tx_batch_size}"
  local perf_file=""
  local perf_name=""
  local perf_event_list=""
  local -a exec_cmd=()
  local final_line queue_line
  local -a cmd=("${REPO_ROOT}/my_ice" "-vvvv" "${BDF}" "--rx-reflect" "${DURATION_S}" "--rx-queues" "${rxq}")

  if (( REPEAT_COUNT > 1 )); then
    run_name+="-rep${repeat_index}"
  fi

  local log_file="${RESULT_DIR}/${run_name}.log"
  local log_name="${run_name}.log"

  if ((${#EXTRA_ARGS[@]} > 0)); then
    cmd+=("${EXTRA_ARGS[@]}")
  fi
  cmd+=("--rx-batch-size" "${rx_batch_size}" "--tx-batch-size" "${tx_batch_size}")

  if (( PERF_MODE && ${#PERF_ACTIVE_EVENTS[@]} > 0 )); then
    perf_event_list="$(join_by_comma "${PERF_ACTIVE_EVENTS[@]}")"
    perf_file="${RESULT_DIR}/${run_name}.perf.txt"
    perf_name="${run_name}.perf.txt"
    exec_cmd=(
      perf stat --no-big-num -x ';' -e "${perf_event_list}" -o "${perf_file}" --
      "${cmd[@]}"
    )
  else
    exec_cmd=("${cmd[@]}")
  fi

  echo "[multi-rx] running rx-queues=${rxq} rx-batch-size=${rx_batch_size} tx-batch-size=${tx_batch_size} repeat=${repeat_index}" | tee "${log_file}"
  echo "[multi-rx] command: ${cmd[*]}" | tee -a "${log_file}"
  if [[ -n "${perf_file}" ]]; then
    echo "[multi-rx] perf events: ${perf_event_list}" | tee -a "${log_file}"
    echo "[multi-rx] perf file: ${perf_name}" | tee -a "${log_file}"
  fi

  set +e
  "${exec_cmd[@]}" 2>&1 | tee -a "${log_file}"
  local rc=${PIPESTATUS[0]}
  set -e

  final_line="$(extract_final_line "${log_file}")"
  queue_line="$(extract_queue_line "${log_file}")"

  if [[ -n "${perf_file}" ]]; then
    append_perf_rows "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}" \
      "$([[ ${rc} -eq 0 && -n "${final_line}" ]] && printf 'ok' || printf 'failed')" \
      "${log_name}" "${perf_name}" "${perf_file}"
  fi

  if [[ ${rc} -ne 0 || -z "${final_line}" ]]; then
    echo "[multi-rx] rx-queues=${rxq} rx-batch-size=${rx_batch_size} tx-batch-size=${tx_batch_size} repeat=${repeat_index} failed; see ${log_file}" | tee -a "${log_file}"
    append_failed_row "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}" "${log_name}"
    return 0
  fi

  append_success_row "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}" "${log_name}" "${final_line}" "${queue_line}"
  echo "[multi-rx] rx-queues=${rxq} rx-batch-size=${rx_batch_size} tx-batch-size=${tx_batch_size} repeat=${repeat_index} parsed successfully" | tee -a "${log_file}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bdf)
      [[ $# -ge 2 ]] || die "missing value for --bdf"
      BDF="$2"
      shift 2
      ;;
    --seconds)
      [[ $# -ge 2 ]] || die "missing value for --seconds"
      DURATION_S="$2"
      shift 2
      ;;
    --queues)
      shift
      parse_int_list "--queues" 1 8 QUEUES "$@"
      validate_queue_list "${QUEUES[@]}"
      shift ${#QUEUES[@]}
      ;;
    --rx-batch-size)
      shift
      parse_int_list "--rx-batch-size" 1 1024 RX_BATCH_SIZES "$@"
      shift ${#RX_BATCH_SIZES[@]}
      ;;
    --tx-batch-size)
      shift
      parse_int_list "--tx-batch-size" 1 1024 TX_BATCH_SIZES "$@"
      shift ${#TX_BATCH_SIZES[@]}
      ;;
    --matrix)
      MATRIX_MODE=1
      shift
      ;;
    --repeat)
      [[ $# -ge 2 ]] || die "missing value for --repeat"
      REPEAT_COUNT="$2"
      shift 2
      ;;
    --perf)
      PERF_MODE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      EXTRA_ARGS=("$@")
      break
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ ${EUID} -eq 0 ]] || die "run as root (sudo)"
[[ "${DURATION_S}" =~ ^[0-9]+$ ]] || die "--seconds must be a positive integer"
(( DURATION_S > 0 )) || die "--seconds must be > 0"
[[ "${REPEAT_COUNT}" =~ ^[0-9]+$ ]] || die "--repeat must be a positive integer"
(( REPEAT_COUNT > 0 )) || die "--repeat must be > 0"

mkdir -p "${RESULTS_ROOT}"
mkdir -p "${RESULT_DIR}"

echo "[multi-rx] repo=${REPO_ROOT}"
echo "[multi-rx] results=${RESULT_DIR}"
echo "[multi-rx] building my_ice"
make -C "${REPO_ROOT}"

write_csv_header
if (( PERF_MODE )); then
  init_perf_events
fi

for rxq in "${QUEUES[@]}"; do
  for rx_batch_size in "${RX_BATCH_SIZES[@]}"; do
    if (( MATRIX_MODE )); then
      for tx_batch_size in "${TX_BATCH_SIZES[@]}"; do
        for ((repeat_index = 1; repeat_index <= REPEAT_COUNT; repeat_index++)); do
          run_one "${rxq}" "${rx_batch_size}" "${tx_batch_size}" "${repeat_index}"
        done
      done
    else
      for ((repeat_index = 1; repeat_index <= REPEAT_COUNT; repeat_index++)); do
        run_one "${rxq}" "${rx_batch_size}" "${rx_batch_size}" "${repeat_index}"
      done
    fi
  done
done

echo "[multi-rx] aggregate csv: ${CSV_PATH}"
if (( PERF_MODE )); then
  echo "[multi-rx] perf csv: ${PERF_CSV_PATH}"
  echo "[multi-rx] perf event status: ${PERF_EVENTS_PATH}"
fi

if [[ -x "${REPO_ROOT}/.venv/bin/python" && -f "${PLOT_SCRIPT}" ]]; then
  echo "[multi-rx] generating plots"
  set +e
  "${REPO_ROOT}/.venv/bin/python" "${PLOT_SCRIPT}" "${RESULT_DIR}"
  plot_rc=$?
  set -e
  if [[ ${plot_rc} -ne 0 ]]; then
    echo "[multi-rx] plot generation failed (rc=${plot_rc}); benchmark results remain in ${RESULT_DIR}"
  fi
else
  echo "[multi-rx] skipping plot generation; missing ${REPO_ROOT}/.venv/bin/python or ${PLOT_SCRIPT}"
fi
