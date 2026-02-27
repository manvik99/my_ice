#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-enp23s0f0}"
DST_MAC="${2:-40:a6:b7:c3:43:e8}"
DURATION="${3:-10}"      # seconds
PKT_SIZE="${4:-1014}"    # 14-byte Ethernet header + payload
THREAD="${5:-0}"         # kpktgend_<THREAD>
KPGEN="/proc/net/pktgen/kpktgend_${THREAD}"
DEV="/proc/net/pktgen/$IFACE"
START_PID=""

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
  if [[ -w "$KPGEN" ]]; then
    echo "rem_device_all" > "$KPGEN" || true
  fi
}

trap cleanup EXIT INT TERM

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0 <iface> <dst-mac> <seconds> <pkt-size> [thread]"
  exit 1
fi

modprobe pktgen

if [[ ! -d /proc/net/pktgen ]]; then
  echo "pktgen not available"
  exit 1
fi

if [[ ! -w "$KPGEN" ]]; then
  echo "pktgen thread file not writable: $KPGEN"
  exit 1
fi

ip link set dev "$IFACE" up

carrier=$(cat "/sys/class/net/$IFACE/carrier")
if [[ "$carrier" != "1" ]]; then
  echo "No carrier on $IFACE (cable/link down)"
  exit 1
fi

stat() { cat "/sys/class/net/$IFACE/statistics/$1"; }

B0=$(stat tx_bytes)
P0=$(stat tx_packets)

echo "[pktgen] configuring thread=$THREAD iface=$IFACE dst=$DST_MAC size=$PKT_SIZE duration=${DURATION}s"
pgset "$KPGEN" "rem_device_all"
pgset "$KPGEN" "add_device $IFACE"

if [[ ! -w "$DEV" ]]; then
  echo "pktgen device file not writable: $DEV"
  exit 1
fi

pgset "$DEV" "count 0"              # run until explicit stop
pgset "$DEV" "clone_skb 0"
pgset "$DEV" "pkt_size $PKT_SIZE"
pgset "$DEV" "dst_mac $DST_MAC"
pgset "$DEV" "delay 0"

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
  printf("pktgen: Mbps=%.3f Mpps=%.3f PPS=%.0f bytes=%d pkts=%d\n",
         (b*8)/(t*1e6), (p/t)/1e6, p/t, b, p)
}'
