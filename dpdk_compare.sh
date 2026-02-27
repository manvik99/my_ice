#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MYICE_BIN="${SCRIPT_DIR}/my_ice"
PKTGEN_SCRIPT="${SCRIPT_DIR}/pktgen_bench.sh"
DPDK_DEVBIND=""

MYICE_BDF="0000:17:00.0"
KERNEL_IFACE="enp23s0f0"
DST_MAC="40:a6:b7:c3:43:e8"
DURATION_S=10
PAYLOAD_LEN=1000
PKTGEN_THREAD=0

RUN_MYICE=1
RUN_PKTGEN=1

MYICE_MBPS="-"
MYICE_MPPS="-"
MYICE_PPS="-"
MYICE_BYTES="-"
MYICE_PKTS="-"

PKTGEN_MBPS="-"
PKTGEN_MPPS="-"
PKTGEN_PPS="-"
PKTGEN_BYTES="-"
PKTGEN_PKTS="-"

usage() {
  cat <<'EOF'
Usage: sudo ./dpdk_compare.sh [options]

Options:
  --bdf <pci-bdf>         my_ice PCI BDF (default: 0000:17:00.0)
  --iface <ifname>        fallback kernel iface if BDF netdev is not detected (default: enp23s0f0)
  --dst-mac <mac>         destination MAC (default: 40:a6:b7:c3:43:e8)
  --seconds <n>           duration for each benchmark (default: 10)
  --payload-len <n>       payload length in bytes (default: 1000)
  --pktgen-thread <n>     kpktgend thread index (default: 0)
  --skip-myice            skip my_ice benchmark
  --skip-pktgen           skip pktgen benchmark
  -h, --help              show help

Notes:
  - This script auto-binds --bdf:
      1) bind to vfio-pci and run my_ice
      2) bind to ice and run pktgen
  - Requires dpdk-devbind.py from DPDK tools.
  - pktgen requires carrier=1 on the selected interface.
EOF
}

parse_metric() {
  local line="$1"
  local key="$2"
  sed -n "s/.*${key}=\([0-9.]*\).*/\1/p" <<<"${line}" | tail -n1
}

find_dpdk_devbind() {
  local candidates=(
    "/usr/share/dpdk/usertools/dpdk-devbind.py"
    "/usr/local/share/dpdk/usertools/dpdk-devbind.py"
  )
  local p
  for p in "${candidates[@]}"; do
    if [[ -x "$p" ]]; then
      DPDK_DEVBIND="$p"
      return 0
    fi
  done

  if command -v dpdk-devbind.py >/dev/null 2>&1; then
    DPDK_DEVBIND="$(command -v dpdk-devbind.py)"
    return 0
  fi

  return 1
}

bind_bdf_to_driver() {
  local bdf="$1"
  local driver="$2"

  if [[ -z "${DPDK_DEVBIND}" ]]; then
    echo "[compare] internal error: DPDK_DEVBIND is empty" >&2
    return 1
  fi

  echo "[compare] binding ${bdf} -> ${driver}"
  modprobe "${driver}"
  "${DPDK_DEVBIND}" -b "${driver}" "${bdf}"
}

detect_iface_for_bdf() {
  local bdf="$1"
  local i
  local path="/sys/bus/pci/devices/${bdf}/net"
  local iface

  for i in $(seq 1 30); do
    if [[ -d "${path}" ]]; then
      iface="$(ls "${path}" 2>/dev/null | head -n1 || true)"
      if [[ -n "${iface}" ]]; then
        echo "${iface}"
        return 0
      fi
    fi
    sleep 0.2
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bdf)
      MYICE_BDF="${2:?missing value for --bdf}"
      shift 2
      ;;
    --iface)
      KERNEL_IFACE="${2:?missing value for --iface}"
      shift 2
      ;;
    --dst-mac)
      DST_MAC="${2:?missing value for --dst-mac}"
      shift 2
      ;;
    --seconds)
      DURATION_S="${2:?missing value for --seconds}"
      shift 2
      ;;
    --payload-len)
      PAYLOAD_LEN="${2:?missing value for --payload-len}"
      shift 2
      ;;
    --pktgen-thread)
      PKTGEN_THREAD="${2:?missing value for --pktgen-thread}"
      shift 2
      ;;
    --skip-myice)
      RUN_MYICE=0
      shift
      ;;
    --skip-pktgen)
      RUN_PKTGEN=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run as root (sudo)." >&2
  exit 1
fi

if ! find_dpdk_devbind; then
  echo "dpdk-devbind.py not found. Install DPDK tools or update this script path." >&2
  exit 1
fi

PKT_SIZE=$((PAYLOAD_LEN + 14))
if (( PKT_SIZE < 60 )); then
  PKT_SIZE=60
fi

echo "[compare] config: bdf=${MYICE_BDF} iface=${KERNEL_IFACE} dst=${DST_MAC} duration=${DURATION_S}s payload=${PAYLOAD_LEN} pkt_size=${PKT_SIZE}"
echo "[compare] using dpdk-devbind at ${DPDK_DEVBIND}"

run_myice() {
  local log line
  if [[ ! -x "${MYICE_BIN}" ]]; then
    echo "[compare] my_ice binary not executable: ${MYICE_BIN}" >&2
    return 1
  fi

  log="$(mktemp)"
  echo "[compare] running my_ice..."
  set +e
  "${MYICE_BIN}" "${MYICE_BDF}" --tx-bench "${DURATION_S}" "${DST_MAC}" "${PAYLOAD_LEN}" 2>&1 | tee "${log}"
  local rc=${PIPESTATUS[0]}
  set -e

  line="$(grep -E '\[my_ice\] tx-bench done:' "${log}" | tail -n1 || true)"
  rm -f "${log}"
  if [[ ${rc} -ne 0 || -z "${line}" ]]; then
    echo "[compare] my_ice result unavailable (check binding/driver)." >&2
    return 1
  fi

  MYICE_MBPS="$(parse_metric "${line}" "avg_Mbps")"
  MYICE_MPPS="$(parse_metric "${line}" "avg_Mpps")"
  MYICE_PPS="$(parse_metric "${line}" "avg_PPS")"
  MYICE_BYTES="$(parse_metric "${line}" "total_bytes")"
  MYICE_PKTS="$(parse_metric "${line}" "total_pkts")"
}

run_pktgen() {
  local log line
  if [[ ! -x "${PKTGEN_SCRIPT}" ]]; then
    echo "[compare] pktgen script not executable: ${PKTGEN_SCRIPT}" >&2
    return 1
  fi

  log="$(mktemp)"
  echo "[compare] running pktgen..."
  set +e
  "${PKTGEN_SCRIPT}" "${KERNEL_IFACE}" "${DST_MAC}" "${DURATION_S}" "${PKT_SIZE}" "${PKTGEN_THREAD}" 2>&1 | tee "${log}"
  local rc=${PIPESTATUS[0]}
  set -e

  line="$(grep -E '^pktgen:' "${log}" | tail -n1 || true)"
  rm -f "${log}"
  if [[ ${rc} -ne 0 || -z "${line}" ]]; then
    echo "[compare] pktgen result unavailable." >&2
    return 1
  fi

  PKTGEN_MBPS="$(parse_metric "${line}" "Mbps")"
  PKTGEN_MPPS="$(parse_metric "${line}" "Mpps")"
  PKTGEN_PPS="$(parse_metric "${line}" "PPS")"
  PKTGEN_BYTES="$(parse_metric "${line}" "bytes")"
  PKTGEN_PKTS="$(parse_metric "${line}" "pkts")"
}

if (( RUN_MYICE )); then
  bind_bdf_to_driver "${MYICE_BDF}" "vfio-pci" || true
  run_myice || true
fi

if (( RUN_PKTGEN )); then
  bind_bdf_to_driver "${MYICE_BDF}" "ice" || true
  if detected_iface="$(detect_iface_for_bdf "${MYICE_BDF}")"; then
    KERNEL_IFACE="${detected_iface}"
    echo "[compare] detected interface for ${MYICE_BDF}: ${KERNEL_IFACE}"
  else
    echo "[compare] could not auto-detect netdev for ${MYICE_BDF}; using --iface ${KERNEL_IFACE}" >&2
  fi
fi

(( RUN_PKTGEN )) && run_pktgen || true

echo
printf "%-8s %12s %10s %12s %16s %16s\n" "Method" "Mbps" "Mpps" "PPS" "Bytes" "Pkts"
printf "%-8s %12s %10s %12s %16s %16s\n" "my_ice" "${MYICE_MBPS}" "${MYICE_MPPS}" "${MYICE_PPS}" "${MYICE_BYTES}" "${MYICE_PKTS}"
printf "%-8s %12s %10s %12s %16s %16s\n" "pktgen" "${PKTGEN_MBPS}" "${PKTGEN_MPPS}" "${PKTGEN_PPS}" "${PKTGEN_BYTES}" "${PKTGEN_PKTS}"
