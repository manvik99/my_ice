#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_ROOT="${SCRIPT_DIR}"
RUST_ROOT="$(cd "${C_ROOT}/../my_ice_rust" && pwd)"
RESULTS_ROOT="${C_ROOT}/results/reflect_perf_compare"

DEFAULT_BDF="0000:17:00.0"
DEFAULT_DURATION_S=10
DEFAULT_BATCHES=(16 32 64)
DEFAULT_REPEATS=1
DEFAULT_HUGEPAGE_DIR="/mnt/huge"
DEFAULT_IMPLS=(c rust)
DEFAULT_RUST_PROFILE="release"
DEFAULT_PERF_RECORD_BATCHES=(64)
DEFAULT_PERF_RECORD_DURATION_S=30
DEFAULT_PERF_RECORD_FREQ=999

ICE_AQ_NUM_DESC=64
ICE_AQ_MAX_BUF_LEN=4096
ICE_AQ_DESC_SIZE=64
ICE_TX_DESC_COUNT=2048
ICE_RX_DESC_COUNT=2048
ICE_TX_DESC_SIZE=128
ICE_RX_DESC_SIZE=128
ICE_TX_PKT_BUF_SIZE=2048
ICE_RX_BUF_SIZE=2048
DMA_SLACK_BYTES=8192

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

BDF="${DEFAULT_BDF}"
DURATION_S="${DEFAULT_DURATION_S}"
BATCHES=("${DEFAULT_BATCHES[@]}")
REPEATS="${DEFAULT_REPEATS}"
HUGEPAGE_DIR="${DEFAULT_HUGEPAGE_DIR}"
USE_HUGEPAGES=1
PIN_CPUS=0
OUTDIR=""
IMPLS=("${DEFAULT_IMPLS[@]}")
BUILD_C=1
BUILD_RUST=1
RUST_PROFILE="${DEFAULT_RUST_PROFILE}"
PERF_RECORD_ENABLED=1
PERF_RECORD_BATCHES=("${DEFAULT_PERF_RECORD_BATCHES[@]}")
PERF_RECORD_DURATION_S="${DEFAULT_PERF_RECORD_DURATION_S}"
PERF_RECORD_FREQ="${DEFAULT_PERF_RECORD_FREQ}"

C_BIN="${C_ROOT}/my_ice"
RUST_BIN="${RUST_ROOT}/target/${RUST_PROFILE}/my_ice_rust"

RESULT_DIR=""
RUNS_DIR=""
SUMMARY_CSV=""
RAW_PERF_CSV=""
PERF_EVENTS_CSV=""
PERF_RECORD_INDEX_CSV=""
METADATA_TXT=""
PERF_ACTIVE_EVENTS=()

declare -A PERF_VALUES=()
declare -A PERF_RUNNING_PCTS=()

SUMMARY_BASE_COLUMNS=(
  timestamp_utc implementation run_id batch_size repeat_index status
  bdf requested_duration_s pin_cpus hugepages hugepage_dir
  stdout_log stderr_log metrics_log perf_file
  seconds_total seconds_active seconds_join seconds_drain worker_threads
  interval_samples
  steady_tx_wire_gbps steady_rx_wire_gbps steady_tx_mpps steady_rx_mpps
  steady_tx_l2_gbps steady_rx_l2_gbps steady_ns_per_pkt
  final_tx_wire_gbps final_rx_wire_gbps final_tx_mpps final_rx_mpps
  final_tx_l2_gbps final_rx_l2_gbps
  rx_pkts rx_bytes tx_pkts tx_bytes zero_copy_pkts zero_copy_bytes
  tx_ring_full rx_short rx_errors pool_empty
  doorbells avg_pkts_per_doorbell
  port_rx_gbps port_tx_gbps gorc_delta gotc_delta
  total_mmio_writes mmio_per_sec tail_advances pkts_per_mmio_write
  total_iterations empty_polls empty_poll_pct reclaim_calls avg_pending_rearm_depth
  port_rx_unicast_pkts port_tx_unicast_pkts port_tx_drop_linkdown
  crc_errors illegal_bytes
)

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

note() {
  printf '[reflect-compare] %s\n' "$*"
}

usage() {
  cat <<EOF
Usage:
  sudo ./common.sh [options]

Options:
  --bdf <BDF>                  PCI BDF to benchmark (default: ${DEFAULT_BDF})
  --duration <seconds>         Reflect duration per run (default: ${DEFAULT_DURATION_S})
  --batches <list...>          Batch sizes to sweep (default: ${DEFAULT_BATCHES[*]})
  --repeats <n>                Repeats per implementation and batch (default: ${DEFAULT_REPEATS})
  --impls <csv>                Implementations to run: c, rust, or c,rust (default: c,rust)
  --out <dir>                  Output directory
  --pin-cpus                   Pass --pin-cpus to both binaries
  --no-hugepages               Disable hugepages
  --hugepage-dir <dir>         Hugepage directory (default: ${DEFAULT_HUGEPAGE_DIR})
  --c-bin <path>               Override C binary path
  --rust-bin <path>            Override Rust binary path
  --rust-profile <name>        Rust cargo profile: debug or release (default: ${DEFAULT_RUST_PROFILE})
  --no-build                   Skip building both binaries
  --no-build-c                 Skip building the C binary
  --no-build-rust              Skip building the Rust binary
  --no-perf-record             Skip hotspot capture with perf record
  --perf-record-batches <...>  Batch sizes to sample with perf record (default: ${DEFAULT_PERF_RECORD_BATCHES[*]})
  --perf-record-duration <s>   Duration for perf record runs (default: ${DEFAULT_PERF_RECORD_DURATION_S})
  --perf-record-freq <hz>      perf record sampling frequency (default: ${DEFAULT_PERF_RECORD_FREQ})
  -h, --help                   Show this help

What it collects:
  - One normalized summary row per implementation/batch/repeat
  - perf stat counters for the same event set across C and Rust
  - Optional perf record captures plus non-interactive perf report text
  - Per-run stdout/stderr/perf/metrics sidecars under runs/

Examples:
  sudo ./common.sh --bdf 0000:17:00.0 --batches 16 32 64 --repeats 3
  sudo ./common.sh --bdf 0000:17:00.0 --impls rust --batches 64 --no-build
EOF
}

require_root() {
  [[ "${EUID}" -eq 0 ]] || die "run this script as root (VFIO + perf require it)"
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
}

join_by_comma() {
  local IFS=,
  printf '%s' "$*"
}

sanitize_event_name() {
  printf '%s' "$1" | tr '[:upper:]-' '[:lower:]_' | sed 's/[^a-z0-9_]/_/g'
}

command_to_string() {
  local out=""
  local part
  for part in "$@"; do
    printf -v out '%s%q ' "${out}" "${part}"
  done
  printf '%s' "${out% }"
}

write_csv_row() {
  local file="$1"
  shift
  local first=1
  local field

  for field in "$@"; do
    if (( first )); then
      printf '%s' "${field}" >>"${file}"
      first=0
    else
      printf ',%s' "${field}" >>"${file}"
    fi
  done
  printf '\n' >>"${file}"
}

is_batch_in_list() {
  local needle="$1"
  local item
  for item in "${PERF_RECORD_BATCHES[@]}"; do
    [[ "${item}" == "${needle}" ]] && return 0
  done
  return 1
}

implementation_enabled() {
  local needle="$1"
  local item
  for item in "${IMPLS[@]}"; do
    [[ "${item}" == "${needle}" ]] && return 0
  done
  return 1
}

parse_metric() {
  local line="$1"
  local key="$2"
  sed -n "s/.*${key}=\([^ ]*\).*/\1/p" <<<"${line}" | tail -n1
}

parse_kv_file_metric() {
  local file="$1"
  local key="$2"
  [[ -f "${file}" ]] || return 0
  sed -n "s/^${key}=//p" "${file}" | tail -n1
}

strip_parens() {
  local value="${1:-}"
  value="${value#(}"
  value="${value%)}"
  printf '%s' "${value}"
}

strip_suffix_s() {
  local value="${1:-}"
  printf '%s' "${value%s}"
}

calc_gbps_from_bytes_and_seconds() {
  local bytes="$1"
  local seconds="$2"
  awk -v bytes="${bytes}" -v seconds="${seconds}" 'BEGIN {
    if (bytes == "" || seconds == "" || seconds == 0) {
      print ""
    } else {
      printf "%.6f", (bytes * 8.0) / (seconds * 1000000000.0)
    }
  }'
}

calc_avg_pkts_per_doorbell() {
  local packets="$1"
  local doorbells="$2"
  awk -v packets="${packets}" -v doorbells="${doorbells}" 'BEGIN {
    if (packets == "" || doorbells == "" || doorbells == 0) {
      print ""
    } else {
      printf "%.6f", packets / doorbells
    }
  }'
}

calc_ns_per_pkt_from_mpps() {
  local mpps="$1"
  awk -v mpps="${mpps}" 'BEGIN {
    if (mpps == "" || mpps == 0) {
      print ""
    } else {
      printf "%.6f", 1000.0 / mpps
    }
  }'
}

calc_scaled_rate() {
  local rate="$1"
  local active_s="$2"
  local total_s="$3"
  awk -v rate="${rate}" -v active_s="${active_s}" -v total_s="${total_s}" 'BEGIN {
    if (rate == "" || active_s == "" || total_s == "" || total_s == 0) {
      print ""
    } else {
      printf "%.6f", rate * active_s / total_s
    }
  }'
}

calc_l2_gbps_from_mpps_and_bytes_per_pkt() {
  local mpps="$1"
  local bytes_per_pkt="$2"
  awk -v mpps="${mpps}" -v bytes_per_pkt="${bytes_per_pkt}" 'BEGIN {
    if (mpps == "" || bytes_per_pkt == "") {
      print ""
    } else {
      printf "%.6f", mpps * bytes_per_pkt * 0.008
    }
  }'
}

calc_bytes_per_pkt() {
  local bytes="$1"
  local packets="$2"
  awk -v bytes="${bytes}" -v packets="${packets}" 'BEGIN {
    if (bytes == "" || packets == "" || packets == 0) {
      print ""
    } else {
      printf "%.6f", bytes / packets
    }
  }'
}

clear_perf_maps() {
  PERF_VALUES=()
  PERF_RUNNING_PCTS=()
}

clear_run_metrics() {
  RUN_SECONDS_TOTAL=""
  RUN_SECONDS_ACTIVE=""
  RUN_SECONDS_JOIN=""
  RUN_SECONDS_DRAIN=""
  RUN_WORKER_THREADS=""
  RUN_INTERVAL_SAMPLES=""

  RUN_STEADY_TX_WIRE_GBPS=""
  RUN_STEADY_RX_WIRE_GBPS=""
  RUN_STEADY_TX_MPPS=""
  RUN_STEADY_RX_MPPS=""
  RUN_STEADY_TX_L2_GBPS=""
  RUN_STEADY_RX_L2_GBPS=""
  RUN_STEADY_NS_PER_PKT=""

  RUN_FINAL_TX_WIRE_GBPS=""
  RUN_FINAL_RX_WIRE_GBPS=""
  RUN_FINAL_TX_MPPS=""
  RUN_FINAL_RX_MPPS=""
  RUN_FINAL_TX_L2_GBPS=""
  RUN_FINAL_RX_L2_GBPS=""

  RUN_RX_PKTS=""
  RUN_RX_BYTES=""
  RUN_TX_PKTS=""
  RUN_TX_BYTES=""
  RUN_ZERO_COPY_PKTS=""
  RUN_ZERO_COPY_BYTES=""
  RUN_TX_RING_FULL=""
  RUN_RX_SHORT=""
  RUN_RX_ERRORS=""
  RUN_POOL_EMPTY=""

  RUN_DOORBELLS=""
  RUN_AVG_PKTS_PER_DOORBELL=""
  RUN_PORT_RX_GBPS=""
  RUN_PORT_TX_GBPS=""
  RUN_GORC_DELTA=""
  RUN_GOTC_DELTA=""

  RUN_TOTAL_MMIO_WRITES=""
  RUN_MMIO_PER_SEC=""
  RUN_TAIL_ADVANCES=""
  RUN_PKTS_PER_MMIO_WRITE=""
  RUN_TOTAL_ITERATIONS=""
  RUN_EMPTY_POLLS=""
  RUN_EMPTY_POLL_PCT=""
  RUN_RECLAIM_CALLS=""
  RUN_AVG_PENDING_REARM_DEPTH=""

  RUN_PORT_RX_UNICAST_PKTS=""
  RUN_PORT_TX_UNICAST_PKTS=""
  RUN_PORT_TX_DROP_LINKDOWN=""
  RUN_CRC_ERRORS=""
  RUN_ILLEGAL_BYTES=""
}

append_raw_perf_rows() {
  local perf_file="$1"
  local ts_utc="$2"
  local implementation="$3"
  local run_id="$4"
  local batch_size="$5"
  local repeat_index="$6"
  local run_status="$7"

  [[ -f "${perf_file}" ]] || return 0

  while IFS=';' read -r value unit event counter_runtime running_pct metric_value metric_unit; do
    local event_status="ok"

    [[ -n "${value}" ]] || continue
    [[ "${value}" == \#* ]] && continue
    [[ -n "${event}" ]] || continue

    if [[ "${value}" == \<* ]]; then
      event_status="${value}"
      value=""
    fi

    PERF_VALUES["${event}"]="${value}"
    PERF_RUNNING_PCTS["${event}"]="${running_pct}"

    write_csv_row "${RAW_PERF_CSV}" \
      "${ts_utc}" "${implementation}" "${run_id}" "${batch_size}" "${repeat_index}" "${run_status}" \
      "${event}" "${event_status}" "${value}" "${unit}" "${counter_runtime}" "${running_pct}" \
      "${metric_value}" "${metric_unit}" "${perf_file##${RESULT_DIR}/}"
  done <"${perf_file}"
}

parse_c_interval_metrics() {
  local stdout_log="$1"
  local stderr_log="$2"
  local parsed

  parsed="$(awk '
    /^\[my_ice\] rx-reflect t=[0-9.]+s interval:/ {
      tx_line = $0
      sub(/^.*TX=/, "", tx_line)
      split(tx_line, tx_parts, " ")
      tx += tx_parts[1]

      rx_line = $0
      sub(/^.*RX=/, "", rx_line)
      split(rx_line, rx_parts, " ")
      rx += rx_parts[1]

      tx_mpps_line = $0
      sub(/^.*tx_mpps=/, "", tx_mpps_line)
      split(tx_mpps_line, tx_mpps_parts, " ")
      tx_mpps += tx_mpps_parts[1]

      rx_mpps_line = $0
      sub(/^.*rx_mpps=/, "", rx_mpps_line)
      split(rx_mpps_line, rx_mpps_parts, " ")
      rx_mpps += rx_mpps_parts[1]

      count++
    }
    END {
      if (count > 0)
        printf "%d,%.6f,%.6f,%.6f,%.6f", count, tx / count, rx / count, tx_mpps / count, rx_mpps / count
    }
  ' "${stdout_log}" "${stderr_log}")"

  [[ -n "${parsed}" ]] || return 0

  IFS=',' read -r RUN_INTERVAL_SAMPLES RUN_STEADY_TX_WIRE_GBPS RUN_STEADY_RX_WIRE_GBPS \
    RUN_STEADY_TX_MPPS RUN_STEADY_RX_MPPS <<<"${parsed}"
}

parse_c_logs() {
  local stdout_log="$1"
  local stderr_log="$2"
  local metrics_log="${3:-}"
  local final_line
  local bytes_per_tx_pkt
  local bytes_per_rx_pkt

  clear_run_metrics

  if [[ -n "${metrics_log}" && -s "${metrics_log}" ]]; then
    RUN_SECONDS_TOTAL="$(parse_kv_file_metric "${metrics_log}" 'seconds_total')"
    RUN_SECONDS_ACTIVE="${RUN_SECONDS_TOTAL}"
    RUN_SECONDS_JOIN="0.000000"
    RUN_SECONDS_DRAIN="0.000000"
    RUN_WORKER_THREADS="1"

    RUN_FINAL_TX_WIRE_GBPS="$(parse_kv_file_metric "${metrics_log}" 'tx_wire_gbps')"
    RUN_FINAL_RX_WIRE_GBPS="$(parse_kv_file_metric "${metrics_log}" 'rx_wire_gbps')"
    RUN_FINAL_TX_MPPS="$(parse_kv_file_metric "${metrics_log}" 'tx_mpps')"
    RUN_FINAL_RX_MPPS="$(parse_kv_file_metric "${metrics_log}" 'rx_mpps')"
    RUN_FINAL_TX_L2_GBPS="$(parse_kv_file_metric "${metrics_log}" 'tx_l2_gbps')"
    RUN_FINAL_RX_L2_GBPS="$(parse_kv_file_metric "${metrics_log}" 'rx_l2_gbps')"

    RUN_RX_PKTS="$(parse_kv_file_metric "${metrics_log}" 'rx_pkts')"
    RUN_RX_BYTES="$(parse_kv_file_metric "${metrics_log}" 'rx_bytes')"
    RUN_TX_PKTS="$(parse_kv_file_metric "${metrics_log}" 'tx_pkts')"
    RUN_TX_BYTES="$(parse_kv_file_metric "${metrics_log}" 'tx_bytes')"
    RUN_ZERO_COPY_PKTS="$(parse_kv_file_metric "${metrics_log}" 'zero_copy_pkts')"
    RUN_ZERO_COPY_BYTES="$(parse_kv_file_metric "${metrics_log}" 'zero_copy_bytes')"
    RUN_TX_RING_FULL="$(parse_kv_file_metric "${metrics_log}" 'tx_ring_full')"
    RUN_RX_SHORT="$(parse_kv_file_metric "${metrics_log}" 'rx_short')"
    RUN_RX_ERRORS="$(parse_kv_file_metric "${metrics_log}" 'rx_errors')"
    RUN_POOL_EMPTY="$(parse_kv_file_metric "${metrics_log}" 'pool_empty')"
    RUN_DOORBELLS="$(parse_kv_file_metric "${metrics_log}" 'doorbells')"
    RUN_GORC_DELTA="$(parse_kv_file_metric "${metrics_log}" 'gorc_delta')"
    RUN_GOTC_DELTA="$(parse_kv_file_metric "${metrics_log}" 'gotc_delta')"
  fi

  if [[ -z "${RUN_SECONDS_TOTAL}" ]]; then
    final_line="$(grep -h -E '^\[my_ice\] rx-reflect done:' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
    [[ -n "${final_line}" ]] || return 1

    RUN_SECONDS_TOTAL="$(parse_metric "${final_line}" 'seconds')"
    RUN_SECONDS_ACTIVE="${RUN_SECONDS_TOTAL}"
    RUN_SECONDS_JOIN="0.000000"
    RUN_SECONDS_DRAIN="0.000000"
    RUN_WORKER_THREADS="1"

    RUN_FINAL_TX_WIRE_GBPS="$(parse_metric "${final_line}" 'TX')"
    RUN_FINAL_RX_WIRE_GBPS="$(parse_metric "${final_line}" 'RX')"
    RUN_FINAL_TX_MPPS="$(parse_metric "${final_line}" 'tx_mpps')"
    RUN_FINAL_RX_MPPS="$(parse_metric "${final_line}" 'rx_mpps')"
    RUN_FINAL_TX_L2_GBPS="$(parse_metric "${final_line}" 'tx_l2_gbps')"
    RUN_FINAL_RX_L2_GBPS="$(parse_metric "${final_line}" 'rx_l2_gbps')"

    RUN_RX_PKTS="$(parse_metric "${final_line}" 'rx_pkts')"
    RUN_RX_BYTES="$(parse_metric "${final_line}" 'rx_bytes')"
    RUN_TX_PKTS="$(parse_metric "${final_line}" 'tx_pkts')"
    RUN_TX_BYTES="$(parse_metric "${final_line}" 'tx_bytes')"
    RUN_ZERO_COPY_PKTS="$(parse_metric "${final_line}" 'zero_copy_pkts')"
    RUN_ZERO_COPY_BYTES="$(parse_metric "${final_line}" 'zero_copy_bytes')"
    RUN_TX_RING_FULL="$(parse_metric "${final_line}" 'tx_ring_full')"
    RUN_RX_SHORT="$(parse_metric "${final_line}" 'rx_short')"
    RUN_RX_ERRORS="$(parse_metric "${final_line}" 'rx_errors')"
    RUN_POOL_EMPTY="$(parse_metric "${final_line}" 'pool_empty')"
    RUN_DOORBELLS="$(parse_metric "${final_line}" 'doorbells')"
    RUN_GORC_DELTA="$(parse_metric "${final_line}" 'GORC_delta')"
    RUN_GOTC_DELTA="$(parse_metric "${final_line}" 'GOTC_delta')"
  fi

  RUN_AVG_PKTS_PER_DOORBELL="$(calc_avg_pkts_per_doorbell "${RUN_TX_PKTS}" "${RUN_DOORBELLS}")"
  RUN_PORT_RX_GBPS="$(calc_gbps_from_bytes_and_seconds "${RUN_GORC_DELTA}" "${RUN_SECONDS_TOTAL}")"
  RUN_PORT_TX_GBPS="$(calc_gbps_from_bytes_and_seconds "${RUN_GOTC_DELTA}" "${RUN_SECONDS_TOTAL}")"

  parse_c_interval_metrics "${stdout_log}" "${stderr_log}"

  if [[ -z "${RUN_STEADY_TX_WIRE_GBPS}" ]]; then
    RUN_STEADY_TX_WIRE_GBPS="${RUN_FINAL_TX_WIRE_GBPS}"
    RUN_STEADY_RX_WIRE_GBPS="${RUN_FINAL_RX_WIRE_GBPS}"
    RUN_STEADY_TX_MPPS="${RUN_FINAL_TX_MPPS}"
    RUN_STEADY_RX_MPPS="${RUN_FINAL_RX_MPPS}"
  fi
  if [[ -z "${RUN_INTERVAL_SAMPLES}" ]]; then
    RUN_INTERVAL_SAMPLES="0"
  fi

  bytes_per_tx_pkt="$(calc_bytes_per_pkt "${RUN_TX_BYTES}" "${RUN_TX_PKTS}")"
  bytes_per_rx_pkt="$(calc_bytes_per_pkt "${RUN_RX_BYTES}" "${RUN_RX_PKTS}")"
  RUN_STEADY_TX_L2_GBPS="$(calc_l2_gbps_from_mpps_and_bytes_per_pkt "${RUN_STEADY_TX_MPPS}" "${bytes_per_tx_pkt}")"
  RUN_STEADY_RX_L2_GBPS="$(calc_l2_gbps_from_mpps_and_bytes_per_pkt "${RUN_STEADY_RX_MPPS}" "${bytes_per_rx_pkt}")"
  RUN_STEADY_NS_PER_PKT="$(calc_ns_per_pkt_from_mpps "${RUN_STEADY_TX_MPPS}")"

  return 0
}

parse_rust_logs() {
  local stdout_log="$1"
  local stderr_log="$2"
  local timing_line
  local sw_line
  local packets_line
  local packets_status_line
  local batching_line
  local batching_avg_line
  local polling_line
  local reclaim_line
  local port_bytes_line
  local port_packets_line
  local port_errors_line
  local threads_line
  local final_line
  local bytes_per_tx_pkt
  local bytes_per_rx_pkt

  clear_run_metrics

  timing_line="$(grep -h -E '^  active=.*join=.*drain=.*total=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  sw_line="$(grep -h -E '^  sw:' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  packets_line="$(grep -h -E '^  total: rx ' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  packets_status_line="$(grep -h -E 'tx_ring_full=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  batching_line="$(grep -h -E '^  doorbells=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  batching_avg_line="$(grep -h -E '^  avg_pkts_per_doorbell=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  polling_line="$(grep -h -E '^  total_iterations=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  reclaim_line="$(grep -h -E '^  reclaim_calls=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  port_bytes_line="$(grep -h -E '^  port: GORC=' "${stdout_log}" "${stderr_log}" | head -n1 || true)"
  port_packets_line="$(grep -h -E '^  port: rx_unicast_pkts=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  port_errors_line="$(grep -h -E '^  port: crc_errors=' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  threads_line="$(grep -h -E 'worker threads spawned' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"
  final_line="$(grep -h -E 'rx-reflect done:' "${stdout_log}" "${stderr_log}" | tail -n1 || true)"

  [[ -n "${timing_line}" && -n "${sw_line}" && -n "${packets_line}" ]] || return 1

  RUN_SECONDS_ACTIVE="$(strip_suffix_s "$(parse_metric "${timing_line}" 'active')")"
  RUN_SECONDS_JOIN="$(strip_suffix_s "$(parse_metric "${timing_line}" 'join')")"
  RUN_SECONDS_DRAIN="$(strip_suffix_s "$(parse_metric "${timing_line}" 'drain')")"
  RUN_SECONDS_TOTAL="$(strip_suffix_s "$(parse_metric "${timing_line}" 'total')")"

  RUN_WORKER_THREADS="$(sed -n 's/.*rx-reflect: \([0-9][0-9]*\) worker threads spawned.*/\1/p' <<<"${threads_line}" | tail -n1)"
  [[ -n "${RUN_WORKER_THREADS}" ]] || RUN_WORKER_THREADS="1"

  RUN_STEADY_TX_WIRE_GBPS="$(awk '/^  sw:/{print $6; exit}' <<<"${sw_line}")"
  RUN_STEADY_RX_WIRE_GBPS="$(awk '/^  sw:/{print $3; exit}' <<<"${sw_line}")"
  RUN_STEADY_TX_MPPS="$(awk '/^  sw:/{print $8; exit}' <<<"${sw_line}")"
  RUN_STEADY_RX_MPPS="${RUN_STEADY_TX_MPPS}"
  RUN_STEADY_NS_PER_PKT="$(awk '/^  sw:/{print $10; exit}' <<<"${sw_line}")"

  RUN_RX_PKTS="$(awk '/^  total:/{print $3; exit}' <<<"${packets_line}")"
  RUN_RX_BYTES="$(strip_parens "$(awk '/^  total:/{print $5; exit}' <<<"${packets_line}")")"
  RUN_TX_PKTS="$(awk '/^  total:/{print $8; exit}' <<<"${packets_line}")"
  RUN_TX_BYTES="$(strip_parens "$(awk '/^  total:/{print $10; exit}' <<<"${packets_line}")")"
  RUN_ZERO_COPY_PKTS="${RUN_TX_PKTS}"
  RUN_ZERO_COPY_BYTES="${RUN_TX_BYTES}"

  RUN_TX_RING_FULL="$(parse_metric "${packets_status_line}" 'tx_ring_full')"
  RUN_RX_SHORT="$(parse_metric "${packets_status_line}" 'rx_short')"
  RUN_RX_ERRORS="$(parse_metric "${final_line}" 'rx_errors')"

  RUN_DOORBELLS="$(parse_metric "${batching_line}" 'doorbells')"
  RUN_TAIL_ADVANCES="$(parse_metric "${batching_line}" 'tail_advances')"
  RUN_TOTAL_MMIO_WRITES="$(parse_metric "${batching_line}" 'total_mmio_writes')"
  RUN_MMIO_PER_SEC="$(parse_metric "${batching_line}" 'mmio/sec')"
  RUN_AVG_PKTS_PER_DOORBELL="$(parse_metric "${batching_avg_line}" 'avg_pkts_per_doorbell')"
  RUN_PKTS_PER_MMIO_WRITE="$(parse_metric "${batching_avg_line}" 'pkts_per_mmio_write')"

  RUN_TOTAL_ITERATIONS="$(parse_metric "${polling_line}" 'total_iterations')"
  RUN_EMPTY_POLLS="$(parse_metric "${polling_line}" 'empty_polls')"
  RUN_EMPTY_POLL_PCT="$(sed -n 's/.*empty_polls=[0-9][0-9]* (\([0-9.][0-9.]*\)%).*/\1/p' <<<"${polling_line}" | tail -n1)"
  RUN_RECLAIM_CALLS="$(parse_metric "${reclaim_line}" 'reclaim_calls')"
  RUN_AVG_PENDING_REARM_DEPTH="$(parse_metric "${reclaim_line}" 'avg_pending_rearm_depth')"

  RUN_GORC_DELTA="$(parse_metric "${port_bytes_line}" 'GORC')"
  RUN_GOTC_DELTA="$(parse_metric "${port_bytes_line}" 'GOTC')"
  RUN_PORT_RX_GBPS="$(strip_parens "$(awk '/^  port: GORC=/{print $3; exit}' <<<"${port_bytes_line}")")"
  RUN_PORT_TX_GBPS="$(strip_parens "$(awk '/^  port: GORC=/{print $6; exit}' <<<"${port_bytes_line}")")"

  RUN_PORT_RX_UNICAST_PKTS="$(parse_metric "${port_packets_line}" 'rx_unicast_pkts')"
  RUN_PORT_TX_UNICAST_PKTS="$(parse_metric "${port_packets_line}" 'tx_unicast_pkts')"
  RUN_PORT_TX_DROP_LINKDOWN="$(parse_metric "${port_packets_line}" 'tx_drop_linkdown')"
  RUN_CRC_ERRORS="$(parse_metric "${port_errors_line}" 'crc_errors')"
  RUN_ILLEGAL_BYTES="$(parse_metric "${port_errors_line}" 'illegal_bytes')"

  RUN_INTERVAL_SAMPLES="$(grep -h -c '^STATUS t=' "${stdout_log}" "${stderr_log}" | awk '{s+=$1} END {print s}')"
  [[ -n "${RUN_INTERVAL_SAMPLES}" ]] || RUN_INTERVAL_SAMPLES="0"

  bytes_per_tx_pkt="$(calc_bytes_per_pkt "${RUN_TX_BYTES}" "${RUN_TX_PKTS}")"
  bytes_per_rx_pkt="$(calc_bytes_per_pkt "${RUN_RX_BYTES}" "${RUN_RX_PKTS}")"
  RUN_STEADY_TX_L2_GBPS="$(calc_l2_gbps_from_mpps_and_bytes_per_pkt "${RUN_STEADY_TX_MPPS}" "${bytes_per_tx_pkt}")"
  RUN_STEADY_RX_L2_GBPS="$(calc_l2_gbps_from_mpps_and_bytes_per_pkt "${RUN_STEADY_RX_MPPS}" "${bytes_per_rx_pkt}")"
  RUN_FINAL_TX_L2_GBPS="$(calc_gbps_from_bytes_and_seconds "${RUN_TX_BYTES}" "${RUN_SECONDS_TOTAL}")"
  RUN_FINAL_RX_L2_GBPS="$(calc_gbps_from_bytes_and_seconds "${RUN_RX_BYTES}" "${RUN_SECONDS_TOTAL}")"
  RUN_FINAL_TX_WIRE_GBPS="$(calc_scaled_rate "${RUN_STEADY_TX_WIRE_GBPS}" "${RUN_SECONDS_ACTIVE}" "${RUN_SECONDS_TOTAL}")"
  RUN_FINAL_RX_WIRE_GBPS="$(calc_scaled_rate "${RUN_STEADY_RX_WIRE_GBPS}" "${RUN_SECONDS_ACTIVE}" "${RUN_SECONDS_TOTAL}")"
  RUN_FINAL_TX_MPPS="$(calc_scaled_rate "${RUN_STEADY_TX_MPPS}" "${RUN_SECONDS_ACTIVE}" "${RUN_SECONDS_TOTAL}")"
  RUN_FINAL_RX_MPPS="${RUN_FINAL_TX_MPPS}"

  if [[ -n "${final_line}" ]]; then
    local maybe_seconds
    maybe_seconds="$(parse_metric "${final_line}" 'seconds')"
    if [[ -n "${maybe_seconds}" ]]; then
      RUN_SECONDS_ACTIVE="${maybe_seconds}"
    fi
    local maybe_tx_gbps
    maybe_tx_gbps="$(parse_metric "${final_line}" 'Tx_Gbps')"
    [[ -n "${maybe_tx_gbps}" ]] && RUN_FINAL_TX_WIRE_GBPS="${maybe_tx_gbps}"
    local maybe_rx_gbps
    maybe_rx_gbps="$(parse_metric "${final_line}" 'Rx_Gbps')"
    [[ -n "${maybe_rx_gbps}" ]] && RUN_FINAL_RX_WIRE_GBPS="${maybe_rx_gbps}"
    local maybe_tx_mpps
    maybe_tx_mpps="$(parse_metric "${final_line}" 'Tx_Mpps')"
    [[ -n "${maybe_tx_mpps}" ]] && RUN_FINAL_TX_MPPS="${maybe_tx_mpps}"
    local maybe_rx_mpps
    maybe_rx_mpps="$(parse_metric "${final_line}" 'Rx_Mpps')"
    [[ -n "${maybe_rx_mpps}" ]] && RUN_FINAL_RX_MPPS="${maybe_rx_mpps}"
  fi

  return 0
}

parse_run_logs() {
  local implementation="$1"
  local stdout_log="$2"
  local stderr_log="$3"
  local metrics_log="${4:-}"

  case "${implementation}" in
    c) parse_c_logs "${stdout_log}" "${stderr_log}" "${metrics_log}" ;;
    rust) parse_rust_logs "${stdout_log}" "${stderr_log}" ;;
    *) return 1 ;;
  esac
}

write_perf_events_header() {
  write_csv_row "${PERF_EVENTS_CSV}" requested_event status
}

append_perf_event_status() {
  write_csv_row "${PERF_EVENTS_CSV}" "$1" "$2"
}

write_raw_perf_header() {
  write_csv_row "${RAW_PERF_CSV}" \
    timestamp_utc implementation run_id batch_size repeat_index run_status \
    event event_status value unit counter_runtime running_pct metric_value metric_unit perf_file
}

write_summary_header() {
  local cols=("${SUMMARY_BASE_COLUMNS[@]}")
  local event
  local sanitized

  for event in "${PERF_EVENTS[@]}"; do
    sanitized="perf_$(sanitize_event_name "${event}")"
    cols+=("${sanitized}" "${sanitized}_running_pct")
  done
  write_csv_row "${SUMMARY_CSV}" "${cols[@]}"
}

write_perf_record_index_header() {
  write_csv_row "${PERF_RECORD_INDEX_CSV}" \
    timestamp_utc implementation run_id batch_size status bdf duration_s frequency_hz \
    pin_cpus hugepages hugepage_dir stdout_log stderr_log metrics_log perf_data perf_report
}

init_output() {
  local timestamp

  if [[ -n "${OUTDIR}" ]]; then
    RESULT_DIR="${OUTDIR}"
  else
    timestamp="$(date -u +%Y%m%d_%H%M%S)"
    RESULT_DIR="${RESULTS_ROOT}/${timestamp}"
  fi

  RUNS_DIR="${RESULT_DIR}/runs"
  SUMMARY_CSV="${RESULT_DIR}/summary.csv"
  RAW_PERF_CSV="${RESULT_DIR}/raw_perf.csv"
  PERF_EVENTS_CSV="${RESULT_DIR}/perf_events.csv"
  PERF_RECORD_INDEX_CSV="${RESULT_DIR}/perf_record_index.csv"
  METADATA_TXT="${RESULT_DIR}/metadata.txt"

  mkdir -p "${RUNS_DIR}"
  write_perf_events_header
  write_raw_perf_header
  write_summary_header
  write_perf_record_index_header
}

write_metadata() {
  local c_commit=""
  local c_branch=""
  local rust_commit=""
  local rust_branch=""

  c_commit="$(git -C "${C_ROOT}" rev-parse HEAD 2>/dev/null || true)"
  c_branch="$(git -C "${C_ROOT}" branch --show-current 2>/dev/null || true)"
  rust_commit="$(git -C "${RUST_ROOT}" rev-parse HEAD 2>/dev/null || true)"
  rust_branch="$(git -C "${RUST_ROOT}" branch --show-current 2>/dev/null || true)"

  {
    printf 'timestamp_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'hostname=%s\n' "$(hostname)"
    printf 'kernel=%s\n' "$(uname -r)"
    printf 'result_dir=%s\n' "${RESULT_DIR}"
    printf 'bdf=%s\n' "${BDF}"
    printf 'duration_seconds=%s\n' "${DURATION_S}"
    printf 'batch_sizes=%s\n' "$(join_by_comma "${BATCHES[@]}")"
    printf 'repeats=%s\n' "${REPEATS}"
    printf 'implementations=%s\n' "$(join_by_comma "${IMPLS[@]}")"
    printf 'pin_cpus=%s\n' "${PIN_CPUS}"
    printf 'hugepages=%s\n' "${USE_HUGEPAGES}"
    printf 'hugepage_dir=%s\n' "${HUGEPAGE_DIR}"
    printf 'c_repo_root=%s\n' "${C_ROOT}"
    printf 'c_binary=%s\n' "${C_BIN}"
    printf 'c_git_branch=%s\n' "${c_branch}"
    printf 'c_git_commit=%s\n' "${c_commit}"
    printf 'rust_repo_root=%s\n' "${RUST_ROOT}"
    printf 'rust_binary=%s\n' "${RUST_BIN}"
    printf 'rust_profile=%s\n' "${RUST_PROFILE}"
    printf 'rust_git_branch=%s\n' "${rust_branch}"
    printf 'rust_git_commit=%s\n' "${rust_commit}"
    printf 'perf_record_enabled=%s\n' "${PERF_RECORD_ENABLED}"
    printf 'perf_record_batches=%s\n' "$(join_by_comma "${PERF_RECORD_BATCHES[@]}")"
    printf 'perf_record_duration_seconds=%s\n' "${PERF_RECORD_DURATION_S}"
    printf 'perf_record_frequency_hz=%s\n' "${PERF_RECORD_FREQ}"
    printf 'requested_perf_events=%s\n' "$(join_by_comma "${PERF_EVENTS[@]}")"
    printf 'supported_perf_events=%s\n' "$(join_by_comma "${PERF_ACTIVE_EVENTS[@]}")"
  } >"${METADATA_TXT}"
}

probe_perf_events() {
  local event
  local probe_out="${RESULT_DIR}/.perf-probe.out"
  local probe_err="${RESULT_DIR}/.perf-probe.err"

  PERF_ACTIVE_EVENTS=()
  for event in "${PERF_EVENTS[@]}"; do
    rm -f "${probe_out}" "${probe_err}"
    set +e
    perf stat --no-big-num -x ';' -e "${event}" -o "${probe_out}" -- true >/dev/null 2>"${probe_err}"
    local rc=$?
    set -e

    if [[ ${rc} -eq 0 ]]; then
      PERF_ACTIVE_EVENTS+=("${event}")
      append_perf_event_status "${event}" "enabled"
    else
      append_perf_event_status "${event}" "unsupported"
    fi
  done

  rm -f "${probe_out}" "${probe_err}"
  ((${#PERF_ACTIVE_EVENTS[@]} > 0)) || die "none of the requested perf events are available"
  note "enabled perf events: $(join_by_comma "${PERF_ACTIVE_EVENTS[@]}")"
}

build_c_binary() {
  implementation_enabled c || return 0
  (( BUILD_C )) || return 0
  note "building C binary"
  make -C "${C_ROOT}"
}

build_rust_binary() {
  implementation_enabled rust || return 0
  (( BUILD_RUST )) || return 0
  note "building Rust binary (${RUST_PROFILE})"
  if [[ "${RUST_PROFILE}" == "release" ]]; then
    cargo build --release --manifest-path "${RUST_ROOT}/Cargo.toml"
  else
    cargo build --manifest-path "${RUST_ROOT}/Cargo.toml"
  fi
}

ensure_bins_exist() {
  if implementation_enabled c; then
    [[ -x "${C_BIN}" ]] || die "C binary not found or not executable: ${C_BIN}"
  fi
  if implementation_enabled rust; then
    [[ -x "${RUST_BIN}" ]] || die "Rust binary not found or not executable: ${RUST_BIN}"
  fi
}

hugepage_size_bytes() {
  awk '/^Hugepagesize:/ {print $2 * 1024}' /proc/meminfo
}

hugepages_free() {
  awk '/^HugePages_Free:/ {print $2}' /proc/meminfo
}

calc_dma_bytes() {
  local queue_count=1
  local aq_desc aq_buf tx_desc tx_buf rx_desc rx_buf

  aq_desc=$((ICE_AQ_NUM_DESC * ICE_AQ_DESC_SIZE))
  aq_buf=$((ICE_AQ_NUM_DESC * ICE_AQ_MAX_BUF_LEN))
  tx_desc=$((queue_count * ICE_TX_DESC_COUNT * ICE_TX_DESC_SIZE))
  tx_buf=$((queue_count * ICE_TX_DESC_COUNT * ICE_TX_PKT_BUF_SIZE))
  rx_desc=$((queue_count * ICE_RX_DESC_COUNT * ICE_RX_DESC_SIZE))
  rx_buf=$((queue_count * ICE_RX_DESC_COUNT * ICE_RX_BUF_SIZE))

  echo $((aq_desc + aq_desc + aq_buf + aq_buf + tx_desc + tx_buf + rx_desc + rx_buf + DMA_SLACK_BYTES))
}

hugepage_mount_options() {
  local dir="$1"
  awk -v dir="${dir}" '$2 == dir && $3 == "hugetlbfs" { print $4; exit }' /proc/mounts
}

validate_hugepage_dir() {
  local fs_type
  local mount_opts

  (( USE_HUGEPAGES )) || return 0
  [[ -d "${HUGEPAGE_DIR}" ]] || die "hugepage dir missing: ${HUGEPAGE_DIR}"

  fs_type="$(stat -f -c %T "${HUGEPAGE_DIR}" 2>/dev/null || true)"
  [[ "${fs_type}" == "hugetlbfs" ]] || die "hugepage dir is not hugetlbfs: ${HUGEPAGE_DIR} (found ${fs_type:-unknown})"

  mount_opts="$(hugepage_mount_options "${HUGEPAGE_DIR}")"
  if [[ -n "${mount_opts}" && ",${mount_opts}," == *,ro,* ]]; then
    die "hugetlbfs mount is read-only: ${HUGEPAGE_DIR} (${mount_opts})"
  fi
}

cleanup_stale_hugepage_files() {
  if [[ -d "${HUGEPAGE_DIR}" ]]; then
    local removed
    removed="$(find "${HUGEPAGE_DIR}" -maxdepth 1 -type f -name 'my_ice_hp_*' -print -delete 2>/dev/null | wc -l)"
    if (( removed > 0 )); then
      note "removed ${removed} stale hugepage backing files from ${HUGEPAGE_DIR}"
    fi
  fi
}

list_reflector_pids() {
  local exe1 exe2 pid_dir pid cmd exe

  exe1="$(readlink -f "${C_BIN}" 2>/dev/null || printf '%s' "${C_BIN}")"
  exe2="$(readlink -f "${RUST_BIN}" 2>/dev/null || printf '%s' "${RUST_BIN}")"

  for pid_dir in /proc/[0-9]*; do
    pid="${pid_dir#/proc/}"
    [[ "${pid}" == "$$" ]] && continue
    [[ -r "${pid_dir}/cmdline" ]] || continue

    cmd="$(tr '\0' ' ' < "${pid_dir}/cmdline" 2>/dev/null || true)"
    [[ -n "${cmd}" ]] || continue
    [[ "${cmd}" == *"--rx-reflect"* ]] || continue
    [[ "${cmd}" == *"${BDF}"* ]] || continue

    exe="$(readlink -f "${pid_dir}/exe" 2>/dev/null || true)"
    [[ -n "${exe}" ]] || continue
    [[ "${exe}" == "${exe1}" || "${exe}" == "${exe2}" ]] || continue

    printf '%s\n' "${pid}"
  done
}

stop_leftover_reflectors() {
  local pids still

  pids="$(list_reflector_pids | xargs || true)"
  [[ -n "${pids}" ]] || return 0

  note "stopping leftover reflector PIDs: ${pids}"
  kill ${pids} 2>/dev/null || true
  sleep 1

  still="$(list_reflector_pids | xargs || true)"
  if [[ -n "${still}" ]]; then
    note "force-killing leftover reflector PIDs: ${still}"
    kill -9 ${still} 2>/dev/null || true
  fi

  still="$(list_reflector_pids | xargs || true)"
  [[ -z "${still}" ]] || die "failed to terminate leftover reflector PIDs: ${still}"
}

ensure_hugepages_ready() {
  local dma_bytes hp_size need_pages free_pages deficit current_nr target_nr

  (( USE_HUGEPAGES )) || return 0
  validate_hugepage_dir

  dma_bytes="$(calc_dma_bytes)"
  hp_size="$(hugepage_size_bytes)"
  [[ -n "${hp_size}" && "${hp_size}" -gt 0 ]] || die "failed to read Hugepagesize from /proc/meminfo"

  need_pages=$(((dma_bytes + hp_size - 1) / hp_size))
  free_pages="$(hugepages_free)"
  [[ -n "${free_pages}" ]] || die "failed to read HugePages_Free from /proc/meminfo"

  if (( free_pages >= need_pages )); then
    note "hugepages ok (need=${need_pages}, free=${free_pages}, size=${hp_size}B, dma=${dma_bytes}B)"
    return 0
  fi

  deficit=$((need_pages - free_pages))
  note "hugepages low (need=${need_pages}, free=${free_pages}); requesting +${deficit} pages"

  if [[ -w /proc/sys/vm/nr_hugepages ]]; then
    current_nr="$(cat /proc/sys/vm/nr_hugepages)"
    target_nr=$((current_nr + deficit))
    printf '%s' "${target_nr}" > /proc/sys/vm/nr_hugepages || true
  fi

  free_pages="$(hugepages_free)"
  (( free_pages >= need_pages )) || die "insufficient hugepages after resize attempt (need=${need_pages}, free=${free_pages})"
  note "hugepages ready after resize (need=${need_pages}, free=${free_pages})"
}

preflight_iteration() {
  stop_leftover_reflectors
  cleanup_stale_hugepage_files
  ensure_hugepages_ready
}

build_command() {
  local implementation="$1"
  local duration_s="$2"
  local batch_size="$3"
  local -n out_ref="$4"
  local metrics_log_path="${5:-}"

  out_ref=()
  case "${implementation}" in
    c) out_ref+=("${C_BIN}") ;;
    rust) out_ref+=("${RUST_BIN}") ;;
    *) die "unknown implementation: ${implementation}" ;;
  esac

  out_ref+=("${BDF}" "--rx-reflect" "${duration_s}" "--reflect-batch" "${batch_size}")

  if (( PIN_CPUS )); then
    out_ref+=("--pin-cpus")
  fi
  if (( USE_HUGEPAGES )); then
    out_ref+=("--hugepages" "--hugepage-dir" "${HUGEPAGE_DIR}")
  fi
  if [[ "${implementation}" == "c" && -n "${metrics_log_path}" ]]; then
    out_ref+=("--metrics-log" "${metrics_log_path}")
  fi
}

append_summary_row() {
  local ts_utc="$1"
  local implementation="$2"
  local run_id="$3"
  local batch_size="$4"
  local repeat_index="$5"
  local status="$6"
  local stdout_rel="$7"
  local stderr_rel="$8"
  local metrics_rel="$9"
  local perf_rel="${10}"
  shift 10

  local row=(
    "${ts_utc}" "${implementation}" "${run_id}" "${batch_size}" "${repeat_index}" "${status}"
    "${BDF}" "${DURATION_S}" "${PIN_CPUS}" "${USE_HUGEPAGES}" "${HUGEPAGE_DIR}"
    "${stdout_rel}" "${stderr_rel}" "${metrics_rel}" "${perf_rel}"
    "${RUN_SECONDS_TOTAL}" "${RUN_SECONDS_ACTIVE}" "${RUN_SECONDS_JOIN}" "${RUN_SECONDS_DRAIN}" "${RUN_WORKER_THREADS}"
    "${RUN_INTERVAL_SAMPLES}"
    "${RUN_STEADY_TX_WIRE_GBPS}" "${RUN_STEADY_RX_WIRE_GBPS}" "${RUN_STEADY_TX_MPPS}" "${RUN_STEADY_RX_MPPS}"
    "${RUN_STEADY_TX_L2_GBPS}" "${RUN_STEADY_RX_L2_GBPS}" "${RUN_STEADY_NS_PER_PKT}"
    "${RUN_FINAL_TX_WIRE_GBPS}" "${RUN_FINAL_RX_WIRE_GBPS}" "${RUN_FINAL_TX_MPPS}" "${RUN_FINAL_RX_MPPS}"
    "${RUN_FINAL_TX_L2_GBPS}" "${RUN_FINAL_RX_L2_GBPS}"
    "${RUN_RX_PKTS}" "${RUN_RX_BYTES}" "${RUN_TX_PKTS}" "${RUN_TX_BYTES}" "${RUN_ZERO_COPY_PKTS}" "${RUN_ZERO_COPY_BYTES}"
    "${RUN_TX_RING_FULL}" "${RUN_RX_SHORT}" "${RUN_RX_ERRORS}" "${RUN_POOL_EMPTY}"
    "${RUN_DOORBELLS}" "${RUN_AVG_PKTS_PER_DOORBELL}"
    "${RUN_PORT_RX_GBPS}" "${RUN_PORT_TX_GBPS}" "${RUN_GORC_DELTA}" "${RUN_GOTC_DELTA}"
    "${RUN_TOTAL_MMIO_WRITES}" "${RUN_MMIO_PER_SEC}" "${RUN_TAIL_ADVANCES}" "${RUN_PKTS_PER_MMIO_WRITE}"
    "${RUN_TOTAL_ITERATIONS}" "${RUN_EMPTY_POLLS}" "${RUN_EMPTY_POLL_PCT}" "${RUN_RECLAIM_CALLS}" "${RUN_AVG_PENDING_REARM_DEPTH}"
    "${RUN_PORT_RX_UNICAST_PKTS}" "${RUN_PORT_TX_UNICAST_PKTS}" "${RUN_PORT_TX_DROP_LINKDOWN}"
    "${RUN_CRC_ERRORS}" "${RUN_ILLEGAL_BYTES}"
  )

  local event
  for event in "${PERF_EVENTS[@]}"; do
    row+=("${PERF_VALUES[${event}]:-}" "${PERF_RUNNING_PCTS[${event}]:-}")
  done

  write_csv_row "${SUMMARY_CSV}" "${row[@]}"
}

run_one() {
  local implementation="$1"
  local batch_size="$2"
  local repeat_index="$3"
  local run_id="${implementation}_batch$(printf '%03d' "${batch_size}")_run$(printf '%02d' "${repeat_index}")"
  local run_dir="${RUNS_DIR}/${run_id}"
  local stdout_log="${run_dir}/stdout.log"
  local stderr_log="${run_dir}/stderr.log"
  local perf_file="${run_dir}/perf.csv"
  local metrics_log=""
  local cmd_file="${run_dir}/command.txt"
  local stdout_rel stderr_rel metrics_rel perf_rel ts_utc timeout_s perf_event_list status
  local cmd=()
  local exec_cmd=()
  local rc

  mkdir -p "${run_dir}"
  if [[ "${implementation}" == "c" ]]; then
    metrics_log="${run_dir}/metrics.log"
  fi
  build_command "${implementation}" "${DURATION_S}" "${batch_size}" cmd "${metrics_log}"

  if [[ "${implementation}" == "rust" ]]; then
    exec_cmd=(env RUST_LOG=info "${cmd[@]}")
  else
    exec_cmd=("${cmd[@]}")
  fi

  printf '%s\n' "$(command_to_string "${cmd[@]}")" >"${cmd_file}"
  stdout_rel="${stdout_log##${RESULT_DIR}/}"
  stderr_rel="${stderr_log##${RESULT_DIR}/}"
  metrics_rel="${metrics_log:+${metrics_log##${RESULT_DIR}/}}"
  perf_rel="${perf_file##${RESULT_DIR}/}"
  ts_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  timeout_s=$((DURATION_S + 45))
  perf_event_list="$(join_by_comma "${PERF_ACTIVE_EVENTS[@]}")"

  note "running ${run_id}"
  clear_perf_maps
  clear_run_metrics
  preflight_iteration

  set +e
  timeout --signal=INT "${timeout_s}" \
    perf stat --no-big-num -x ';' -e "${perf_event_list}" -o "${perf_file}" -- \
    "${exec_cmd[@]}" >"${stdout_log}" 2>"${stderr_log}"
  rc=$?
  set -e

  append_raw_perf_rows "${perf_file}" "${ts_utc}" "${implementation}" "${run_id}" "${batch_size}" "${repeat_index}" "$([[ ${rc} -eq 0 ]] && printf 'ok' || printf 'failed')"

  if [[ ${rc} -eq 0 ]] && parse_run_logs "${implementation}" "${stdout_log}" "${stderr_log}" "${metrics_log}"; then
    status="ok"
  else
    status="failed"
    note "${run_id}: parsing failed or command exited with rc=${rc}"
  fi

  append_summary_row "${ts_utc}" "${implementation}" "${run_id}" "${batch_size}" "${repeat_index}" "${status}" \
    "${stdout_rel}" "${stderr_rel}" "${metrics_rel}" "${perf_rel}"

  if [[ "${status}" == "ok" ]]; then
    note "${run_id}: cycles=$(printf '%s' "${PERF_VALUES[cycles]:-n/a}") instructions=$(printf '%s' "${PERF_VALUES[instructions]:-n/a}") steady_tx_wire=${RUN_STEADY_TX_WIRE_GBPS:-n/a}"
  else
    note "${run_id}: see ${run_dir}"
  fi
}

run_perf_record_capture() {
  local implementation="$1"
  local batch_size="$2"
  local run_id="perfrecord_${implementation}_batch$(printf '%03d' "${batch_size}")"
  local run_dir="${RUNS_DIR}/${run_id}"
  local stdout_log="${run_dir}/stdout.log"
  local stderr_log="${run_dir}/stderr.log"
  local metrics_log=""
  local perf_data="${run_dir}/perf.data"
  local perf_report="${run_dir}/perf.report.txt"
  local cmd_file="${run_dir}/command.txt"
  local stdout_rel stderr_rel metrics_rel perf_data_rel perf_report_rel ts_utc timeout_s status
  local cmd=()
  local exec_cmd=()
  local rc

  (( PERF_RECORD_ENABLED )) || return 0

  mkdir -p "${run_dir}"
  if [[ "${implementation}" == "c" ]]; then
    metrics_log="${run_dir}/metrics.log"
  fi
  build_command "${implementation}" "${PERF_RECORD_DURATION_S}" "${batch_size}" cmd "${metrics_log}"
  if [[ "${implementation}" == "rust" ]]; then
    exec_cmd=(env RUST_LOG=info "${cmd[@]}")
  else
    exec_cmd=("${cmd[@]}")
  fi

  printf '%s\n' "$(command_to_string "${cmd[@]}")" >"${cmd_file}"
  stdout_rel="${stdout_log##${RESULT_DIR}/}"
  stderr_rel="${stderr_log##${RESULT_DIR}/}"
  metrics_rel="${metrics_log:+${metrics_log##${RESULT_DIR}/}}"
  perf_data_rel="${perf_data##${RESULT_DIR}/}"
  perf_report_rel="${perf_report##${RESULT_DIR}/}"
  ts_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  timeout_s=$((PERF_RECORD_DURATION_S + 60))

  note "perf record ${run_id}"
  preflight_iteration

  set +e
  timeout --signal=INT "${timeout_s}" \
    perf record -F "${PERF_RECORD_FREQ}" -g -o "${perf_data}" -- \
    "${exec_cmd[@]}" >"${stdout_log}" 2>"${stderr_log}"
  rc=$?
  set -e

  if [[ ${rc} -eq 0 ]]; then
    perf report --stdio --percent-limit 0.5 -i "${perf_data}" >"${perf_report}" 2>>"${stderr_log}" || true
    status="ok"
  else
    status="failed"
  fi

  write_csv_row "${PERF_RECORD_INDEX_CSV}" \
    "${ts_utc}" "${implementation}" "${run_id}" "${batch_size}" "${status}" "${BDF}" \
    "${PERF_RECORD_DURATION_S}" "${PERF_RECORD_FREQ}" "${PIN_CPUS}" "${USE_HUGEPAGES}" "${HUGEPAGE_DIR}" \
    "${stdout_rel}" "${stderr_rel}" "${metrics_rel}" "${perf_data_rel}" "${perf_report_rel}"
}

parse_batches_arg() {
  local -n target_ref="$1"
  shift
  target_ref=()
  while [[ $# -gt 0 && "$1" != --* ]]; do
    target_ref+=("$1")
    shift
  done
  [[ ${#target_ref[@]} -gt 0 ]] || die "expected at least one value"
  PARSE_SHIFT_COUNT="$(( $# ))"
}

PARSE_SHIFT_COUNT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bdf)
      [[ $# -ge 2 ]] || die "--bdf requires <BDF>"
      BDF="$2"
      shift 2
      ;;
    --duration)
      [[ $# -ge 2 ]] || die "--duration requires <seconds>"
      DURATION_S="$2"
      shift 2
      ;;
    --repeats)
      [[ $# -ge 2 ]] || die "--repeats requires <n>"
      REPEATS="$2"
      shift 2
      ;;
    --impls)
      [[ $# -ge 2 ]] || die "--impls requires <csv>"
      IFS=',' read -r -a IMPLS <<<"$2"
      shift 2
      ;;
    --batches)
      shift
      parse_batches_arg BATCHES "$@"
      shift $(( $# - PARSE_SHIFT_COUNT ))
      ;;
    --perf-record-batches)
      shift
      parse_batches_arg PERF_RECORD_BATCHES "$@"
      shift $(( $# - PARSE_SHIFT_COUNT ))
      ;;
    --out)
      [[ $# -ge 2 ]] || die "--out requires <dir>"
      OUTDIR="$2"
      shift 2
      ;;
    --pin-cpus)
      PIN_CPUS=1
      shift
      ;;
    --no-hugepages)
      USE_HUGEPAGES=0
      shift
      ;;
    --hugepage-dir)
      [[ $# -ge 2 ]] || die "--hugepage-dir requires <dir>"
      HUGEPAGE_DIR="$2"
      shift 2
      ;;
    --c-bin)
      [[ $# -ge 2 ]] || die "--c-bin requires <path>"
      C_BIN="$2"
      shift 2
      ;;
    --rust-bin)
      [[ $# -ge 2 ]] || die "--rust-bin requires <path>"
      RUST_BIN="$2"
      shift 2
      ;;
    --rust-profile)
      [[ $# -ge 2 ]] || die "--rust-profile requires <name>"
      RUST_PROFILE="$2"
      RUST_BIN="${RUST_ROOT}/target/${RUST_PROFILE}/my_ice_rust"
      shift 2
      ;;
    --no-build)
      BUILD_C=0
      BUILD_RUST=0
      shift
      ;;
    --no-build-c)
      BUILD_C=0
      shift
      ;;
    --no-build-rust)
      BUILD_RUST=0
      shift
      ;;
    --no-perf-record)
      PERF_RECORD_ENABLED=0
      shift
      ;;
    --perf-record-duration)
      [[ $# -ge 2 ]] || die "--perf-record-duration requires <seconds>"
      PERF_RECORD_DURATION_S="$2"
      shift 2
      ;;
    --perf-record-freq)
      [[ $# -ge 2 ]] || die "--perf-record-freq requires <hz>"
      PERF_RECORD_FREQ="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

require_root
require_tool perf
require_tool timeout
require_tool awk
require_tool grep
require_tool sed
require_tool git
implementation_enabled c && (( BUILD_C )) && require_tool make
implementation_enabled rust && (( BUILD_RUST )) && require_tool cargo

case "${RUST_PROFILE}" in
  debug|release) ;;
  *) die "--rust-profile must be debug or release" ;;
esac

for impl in "${IMPLS[@]}"; do
  case "${impl}" in
    c|rust) ;;
    *) die "unknown implementation in --impls: ${impl}" ;;
  esac
done

init_output
probe_perf_events
build_c_binary
build_rust_binary
ensure_bins_exist
write_metadata

note "output directory: ${RESULT_DIR}"

for repeat_index in $(seq 1 "${REPEATS}"); do
  for batch_size in "${BATCHES[@]}"; do
    for impl in "${IMPLS[@]}"; do
      run_one "${impl}" "${batch_size}" "${repeat_index}"
    done
  done
done

if (( PERF_RECORD_ENABLED )); then
  for batch_size in "${PERF_RECORD_BATCHES[@]}"; do
    for impl in "${IMPLS[@]}"; do
      run_perf_record_capture "${impl}" "${batch_size}"
    done
  done
fi

note "summary CSV: ${SUMMARY_CSV}"
note "raw perf CSV: ${RAW_PERF_CSV}"
note "perf event status: ${PERF_EVENTS_CSV}"
note "perf record index: ${PERF_RECORD_INDEX_CSV}"
