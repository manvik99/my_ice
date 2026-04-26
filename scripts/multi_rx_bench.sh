#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_BDF="0000:17:00.0"
DEFAULT_DURATION_S=15
DEFAULT_QUEUES=(1 2 4 8)

BDF="${DEFAULT_BDF}"
DURATION_S="${DEFAULT_DURATION_S}"
RESULTS_ROOT="${REPO_ROOT}/results/multi-rx"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
RESULT_DIR="${RESULTS_ROOT}/${TIMESTAMP}"
CSV_PATH="${RESULT_DIR}/aggregate.csv"
EXTRA_ARGS=()

usage() {
  cat <<'EOF'
Usage: sudo ./scripts/multi_rx_bench.sh [options] [-- extra my_ice args]

Options:
  --bdf <pci-bdf>     PCI BDF to test (default: 0000:17:00.0)
  --seconds <n>       Duration per run in seconds (default: 15)
  -h, --help          Show this help

Behavior:
  - Builds my_ice via make before benchmarking
  - Runs ./my_ice <bdf> --rx-reflect <seconds> --rx-queues {1,2,4,8}
  - Saves logs to results/multi-rx/<timestamp>/q<N>.log
  - Saves parsed aggregate stats to results/multi-rx/<timestamp>/aggregate.csv
  - Extra args after '--' are appended to every my_ice invocation

Example:
  sudo ./scripts/multi_rx_bench.sh
  sudo ./scripts/multi_rx_bench.sh --bdf 0000:17:00.0 --seconds 20 -- --hugepages --hugepage-dir /mnt/huge
EOF
}

die() {
  echo "$*" >&2
  exit 1
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
    printf 'timestamp,bdf,duration_s,rx_queues,status,log_file,'
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

append_failed_row() {
  local rxq="$1"
  local log_name="$2"

  {
    printf '%s,%s,%s,%s,%s,%s' \
      "${TIMESTAMP}" "${BDF}" "${DURATION_S}" "${rxq}" "failed" "${log_name}"
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
  local log_name="$2"
  local final_line="$3"
  local queue_line="$4"
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
    printf '%s,%s,%s,%s,%s,%s,' \
      "${TIMESTAMP}" "${BDF}" "${DURATION_S}" "${rxq}" "ok" "${log_name}"
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

run_one() {
  local rxq="$1"
  local log_file="${RESULT_DIR}/q${rxq}.log"
  local log_name="q${rxq}.log"
  local final_line queue_line
  local -a cmd=("${REPO_ROOT}/my_ice" "${BDF}" "--rx-reflect" "${DURATION_S}" "--rx-queues" "${rxq}")

  if ((${#EXTRA_ARGS[@]} > 0)); then
    cmd+=("${EXTRA_ARGS[@]}")
  fi

  echo "[multi-rx] running rx-queues=${rxq}" | tee "${log_file}"
  echo "[multi-rx] command: ${cmd[*]}" | tee -a "${log_file}"

  set +e
  "${cmd[@]}" 2>&1 | tee -a "${log_file}"
  local rc=${PIPESTATUS[0]}
  set -e

  final_line="$(extract_final_line "${log_file}")"
  queue_line="$(extract_queue_line "${log_file}")"

  if [[ ${rc} -ne 0 || -z "${final_line}" ]]; then
    echo "[multi-rx] rx-queues=${rxq} failed; see ${log_file}" | tee -a "${log_file}"
    append_failed_row "${rxq}" "${log_name}"
    return 0
  fi

  append_success_row "${rxq}" "${log_name}" "${final_line}" "${queue_line}"
  echo "[multi-rx] rx-queues=${rxq} parsed successfully" | tee -a "${log_file}"
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

mkdir -p "${RESULTS_ROOT}"
mkdir -p "${RESULT_DIR}"

echo "[multi-rx] repo=${REPO_ROOT}"
echo "[multi-rx] results=${RESULT_DIR}"
echo "[multi-rx] building my_ice"
make -C "${REPO_ROOT}"

write_csv_header

for rxq in "${DEFAULT_QUEUES[@]}"; do
  run_one "${rxq}"
done

echo "[multi-rx] aggregate csv: ${CSV_PATH}"
