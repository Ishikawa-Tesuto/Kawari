#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "$ROOT/api-rust/target/release/studio" ]] || { echo 'Run ./scripts/build-core.sh first.' >&2; exit 1; }
export STUDIO_UI="${STUDIO_UI:-1}"
exec "$ROOT/api-rust/target/release/studio" "$@"
