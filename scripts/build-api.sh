#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/api-rust"
export CARGO_TARGET_DIR="$ROOT/api-rust/target"
case "$#:${1:-}" in
  0:|1:--force-release) ;;
  *) echo 'Usage: scripts/build-api.sh [--force-release]' >&2; exit 1 ;;
esac
command -v cargo >/dev/null || { echo 'Install cargo (Arch: sudo pacman -S --needed rust)' >&2; exit 1; }
if [[ ! -f Cargo.lock ]]; then cargo generate-lockfile; fi
if [[ "${1:-}" == --force-release ]]; then cargo clean --release -p studio-vts-api; fi
cargo build --locked --release --bins
