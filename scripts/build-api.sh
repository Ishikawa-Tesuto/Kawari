#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/api-rust"
export CARGO_TARGET_DIR="$ROOT/api-rust/target"
FORCE=0
if [[ $# -gt 0 ]]; then
  if [[ $# -eq 1 && "$1" == --force-release ]]; then FORCE=1; else echo 'Usage: scripts/build-api.sh [--force-release]' >&2; exit 1; fi
fi
command -v cargo >/dev/null || { echo 'Install cargo (Arch: sudo pacman -S --needed rust)' >&2; exit 1; }
if [[ ! -f Cargo.lock ]]; then cargo generate-lockfile; fi
if [[ "$FORCE" == 1 ]]; then
  # Only clean our package in release mode; retain cached third-party dependencies.
  cargo clean --release -p studio-vts-api
fi
cargo test --locked
cargo build --locked --release --bins
printf 'Native API ready: %s/api-rust/target/release/studio-vts-api\n' "$ROOT"
