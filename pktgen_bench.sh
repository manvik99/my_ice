#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-enp23s0f0}"
DST_MAC="${2:-40:a6:b7:c3:43:d8}"
DURATION="${3:-60}"      # seconds
PKT_SIZE="${4:-60}"    # 14-byte Ethernet header + payload
THREAD_SPEC="${5:-"0,1,2,3,4,5,6,7,8,9,10,11,12"}"    # kpktgend threads: "0" or "0,1,2,3" or "all"
QUEUE_BASE="${6:-0}"     # first TX queue index to use
BURST="${7:-32}"         # packets per scheduling round; helps for small packets
START_PID=""
THREADS=()
DEVICES=()
QUEUE_COUNT=0

pgset() {
  local file="$1"
  local cmd="$2"
  echo "$cmd" > "$file"
}

cleanup() {
  # Best-effort cleanup so Ctrl-C does not leave pktgen running forever.
  if [[ -n "${START_PID}" ]]; then
    kill "${START_PID}" >/dev/null 2>&1 || true
    wait "${START_PID}" 2>/dev/null || true
  fi
  if [[ -w /proc/net/pktgen/pgctrl ]]; then
    echo "stop" > /proc/net/pktgen/pgctrl || true
  fi
  local kpgen
  for thread in "${THREADS[@]:-}"; do
    kpgen="/proc/net/pktgen/kpktgend_${thread}"
    if [[ -w "$kpgen" ]]; then
      echo "rem_device_all" > "$kpgen" || true
    fi
  done
}

usage() {
  echo "Run as root: sudo $0 <iface> <dst-mac> <seconds> <pkt-size> [thread|thread-list|all] [queue-base] [burst]"
}

expand_threads() {
  local spec="$1"
  local -a out=()
  local txq_count="$2"
  local max_threads="$3"
  local part

  if [[ "$spec" == "all" ]]; then
    local i
    for ((i = 0; i < txq_count && i < max_threads; i++)); do
      out+=("$i")
    done
  else
    IFS=',' read -r -a out <<< "$spec"
  fi

  if [[ ${#out[@]} -eq 0 ]]; then
    echo "No pktgen threads selected"
    exit 1
  fi

  THREADS=()
  for part in "${out[@]}"; do
    if [[ ! "$part" =~ ^[0-9]+$ ]]; then
      echo "Invalid thread index: $part"
      exit 1
    fi
    THREADS+=("$part")
  done
}

trap cleanup EXIT INT TERM

if [[ $EUID -ne 0 ]]; then
  usage
  exit 1
fi

modprobe pktgen

if [[ ! -d /proc/net/pktgen ]]; then
  echo "pktgen not available"
  exit 1
fi

ip link set dev "$IFACE" up

carrier=$(cat "/sys/class/net/$IFACE/carrier")
if [[ "$carrier" != "1" ]]; then
  echo "No carrier on $IFACE (cable/link down)"
  exit 1
fi

stat() { cat "/sys/class/net/$IFACE/statistics/$1"; }

mapfile -t TX_QUEUE_PATHS < <(find "/sys/class/net/$IFACE/queues" -maxdepth 1 -type d -name 'tx-*' | sort)
QUEUE_COUNT="${#TX_QUEUE_PATHS[@]}"
if (( QUEUE_COUNT == 0 )); then
  echo "No TX queues found for $IFACE"
  exit 1
fi

mapfile -t KPGEN_PATHS < <(find /proc/net/pktgen -maxdepth 1 -type f -name 'kpktgend_*' | sort)
KPGEN_COUNT="${#KPGEN_PATHS[@]}"
if (( KPGEN_COUNT == 0 )); then
  echo "No kpktgend threads found under /proc/net/pktgen"
  exit 1
fi

expand_threads "$THREAD_SPEC" "$QUEUE_COUNT" "$KPGEN_COUNT"

if (( QUEUE_BASE < 0 )); then
  echo "queue-base must be >= 0"
  exit 1
fi

if (( BURST <= 0 )); then
  echo "burst must be > 0"
  exit 1
fi

if (( QUEUE_BASE + ${#THREADS[@]} > QUEUE_COUNT )); then
  echo "Requested ${#THREADS[@]} queues starting at $QUEUE_BASE, but $IFACE only has $QUEUE_COUNT TX queues"
  exit 1
fi

for thread in "${THREADS[@]}"; do
  kpgen="/proc/net/pktgen/kpktgend_${thread}"
  if [[ ! -w "$kpgen" ]]; then
    echo "pktgen thread file not writable: $kpgen"
    exit 1
  fi
done

B0=$(stat tx_bytes)
P0=$(stat tx_packets)

echo "[pktgen] configuring iface=$IFACE dst=$DST_MAC size=$PKT_SIZE duration=${DURATION}s threads=${THREADS[*]} queue_base=$QUEUE_BASE queue_count=${#THREADS[@]} burst=$BURST"
for idx in "${!THREADS[@]}"; do
  thread="${THREADS[$idx]}"
  queue=$((QUEUE_BASE + idx))
  kpgen="/proc/net/pktgen/kpktgend_${thread}"
  dev_name="${IFACE}@${thread}"
  dev="/proc/net/pktgen/${dev_name}"

  DEVICES+=("$dev_name")

  pgset "$kpgen" "rem_device_all"
  pgset "$kpgen" "add_device $dev_name"

  if [[ ! -w "$dev" ]]; then
    echo "pktgen device file not writable: $dev"
    exit 1
  fi

  pgset "$dev" "count 0"              # run until explicit stop
  pgset "$dev" "clone_skb 0"
  pgset "$dev" "pkt_size $PKT_SIZE"
  pgset "$dev" "dst_mac $DST_MAC"
  pgset "$dev" "delay 0"
  pgset "$dev" "burst $BURST"
  pgset "$dev" "queue_map_min $queue"
  pgset "$dev" "queue_map_max $queue"
done

echo "[pktgen] start"
# Writing "start" can block until generation ends; do it in background.
( echo "start" > /proc/net/pktgen/pgctrl ) &
START_PID=$!
sleep "$DURATION"
echo "[pktgen] stop"
echo "stop" > /proc/net/pktgen/pgctrl
wait "$START_PID" 2>/dev/null || true
START_PID=""

B1=$(stat tx_bytes)
P1=$(stat tx_packets)

DB=$((B1 - B0))
DP=$((P1 - P0))

awk -v b="$DB" -v p="$DP" -v t="$DURATION" 'BEGIN {
  printf("pktgen: Gbps=%.3f Mbps=%.3f Mpps=%.3f PPS=%.0f bytes=%d pkts=%d\n",
         (b*8)/(t*1e9), (b*8)/(t*1e6), (p/t)/1e6, p/t, b, p)
}'
