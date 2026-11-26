#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f "$ROOT/build/studio-build-id" ]] || [[ "$(cat "$ROOT/build/studio-build-id")" != "$(bash "$ROOT/scripts/source-id.sh")" ]]; then
  echo '[studio] Sources and build do not match. Run ./scripts/build.sh first.' >&2
  exit 1
fi
(cd "$ROOT" && cargo build --locked --release --target-dir "$ROOT/target")
exec "$ROOT/target/release/kawari-viewer" "$@"
