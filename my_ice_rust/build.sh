#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ -f "$HOME/.cargo/env" ]]; then
  # shellcheck disable=SC1090
  source "$HOME/.cargo/env"
fi

if ! command -v rustc >/dev/null 2>&1; then
  echo "rustc not found. Install Rust toolchain first." >&2
  exit 1
fi

cc -O2 -Wall -Wextra -std=c11 -Dmain=c_driver_main -I"${ROOT_DIR}" -c "${ROOT_DIR}/main.c" -o "${SCRIPT_DIR}/my_ice_core.o"
ar rcs "${SCRIPT_DIR}/libmy_ice_core.a" "${SCRIPT_DIR}/my_ice_core.o"
rustc --edition=2021 -C opt-level=3 "${SCRIPT_DIR}/src/main.rs" -L "${SCRIPT_DIR}" -l static=my_ice_core -o "${SCRIPT_DIR}/my_ice_rust"

echo "built ${SCRIPT_DIR}/my_ice_rust"
