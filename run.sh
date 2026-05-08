#!/usr/bin/env bash
set -euo pipefail

ROOT="/users/manvik12/my_ice"
RUST_ROOT="/users/manvik12/my_ice_rust"

BDF="0000:17:00.0"
DURATION_S="10"
BATCH_SIZE="64"

EVENTS="instructions,cycles,stalled-cycles-frontend,stalled-cycles-backend,L1-dcache-load-misses,L1-icache-load-misses,LLC-load-misses,LLC-stores,LLC-store-misses,L1-dcache-prefetches,dTLB-load-misses,iTLB-load-misses,branches,branch-misses,node-loads,node-load-misses,faults,cs,cpu-migrations,bus-cycles"

C_BIN="${ROOT}/my_ice"
if [[ -x "${RUST_ROOT}/target/release/my_ice_rust" ]]; then
  RUST_BIN="${RUST_ROOT}/target/release/my_ice_rust"
else
  RUST_BIN="${RUST_ROOT}/target/debug/my_ice_rust"
fi

C_PERF_CSV="${ROOT}/c_batch064_perf.csv"
C_METRICS_LOG="${ROOT}/c_batch064_metrics.log"
C_STDOUT_LOG="${ROOT}/c_batch064.stdout.log"
C_STDERR_LOG="${ROOT}/c_batch064.stderr.log"

RUST_PERF_CSV="${ROOT}/rust_batch064_perf.csv"
RUST_STDOUT_LOG="${ROOT}/rust_batch064.stdout.log"
RUST_STDERR_LOG="${ROOT}/rust_batch064.stderr.log"

sudo perf stat --no-big-num -x ';' \
  -e "${EVENTS}" \
  -o "${C_PERF_CSV}" -- \
  "${C_BIN}" "${BDF}" \
  --rx-reflect "${DURATION_S}" \
  --reflect-batch "${BATCH_SIZE}" \
  --metrics-log "${C_METRICS_LOG}" \
  >"${C_STDOUT_LOG}" 2>"${C_STDERR_LOG}"

sudo perf stat --no-big-num -x ';' \
  -e "${EVENTS}" \
  -o "${RUST_PERF_CSV}" -- \
  env RUST_LOG=info "${RUST_BIN}" "${BDF}" \
  --rx-reflect "${DURATION_S}" \
  --reflect-batch "${BATCH_SIZE}" \
  >"${RUST_STDOUT_LOG}" 2>"${RUST_STDERR_LOG}"
