#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/api-rust/src/bin/studio.rs"
BIN="$ROOT/api-rust/target/release/studio"
MARKER='Kawari expression UI protocol 3'
BROADCAST_MARKER='broadcast settings protocol 1'
PLUGIN_MARKER='plugin manager protocol 1'
API_SOURCE="$ROOT/api-rust/src/main.rs"
API_BIN="$ROOT/api-rust/target/release/studio-vts-api"
# An overlay archive can preserve old timestamps. Do not trust an existing binary.
if ! grep -Fq -- "$MARKER" "$SOURCE" || ! grep -Fq -- "$BROADCAST_MARKER" "$SOURCE" || ! grep -Fq -- "$MARKER" "$API_SOURCE" || ! grep -Fq -- "$PLUGIN_MARKER" "$SOURCE" || ! grep -Fq -- "$PLUGIN_MARKER" "$API_SOURCE"; then
  echo 'Application Core source is outdated: extract Kawari P0.5g into this project.' >&2
  echo "Expected updated sources: $SOURCE and $API_SOURCE" >&2
  exit 1
fi
"$ROOT/scripts/build-api.sh" --force-release
[[ -x "$BIN" ]] || { echo "Native supervisor binary missing: $BIN" >&2; exit 1; }
HELP="$("$BIN" --help)"
if [[ "$HELP" != *"$MARKER"* || "$HELP" != *"$BROADCAST_MARKER"* || "$HELP" != *"$PLUGIN_MARKER"* ]]; then
  echo "Built binary does not support Kawari expression UI protocol 3 / broadcast settings protocol 1 / plugin manager protocol 1: $BIN" >&2
  printf '%s\n' "$HELP" >&2
  exit 1
fi
[[ -x "$API_BIN" ]] || { echo "Native API binary missing: $API_BIN" >&2; exit 1; }
API_HELP="$("$API_BIN" --help)"
if [[ "$API_HELP" != *"$MARKER"* || "$API_HELP" != *"$PLUGIN_MARKER"* ]]; then
  echo "Built API binary does not support Kawari expression UI protocol 3 / plugin manager protocol 1: $API_BIN" >&2
  exit 1
fi
ln -sfn studio "$ROOT/api-rust/target/release/kawari"
ln -sfn studio-vts-api "$ROOT/api-rust/target/release/kawari-api"
printf 'Kawari ready (expression UI protocol 3): %s/scripts/run-kawari.sh\n' "$ROOT"
