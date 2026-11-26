#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/api-rust/target/release/kawari"
[[ -x "$BIN" ]] || { echo 'Run ./scripts/build.sh first.' >&2; exit 1; }
export STUDIO_UI="${STUDIO_UI:-1}"
export STUDIO_HOT_SWITCH="${STUDIO_HOT_SWITCH:-1}"
exec "$BIN" "$@"
