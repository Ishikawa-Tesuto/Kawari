#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
APPLICATIONS="$DATA/applications"
ICONS="$DATA/icons/hicolor"
APP_ID=ru.tesutoidclub.Kawari

sizes=(16 32 48 64 128 256)
for size in "${sizes[@]}"; do
  [[ -r "$ROOT/assets/icons/$size.png" ]] || { echo "Missing Kawari icon: $size.png" >&2; exit 1; }
done
for size in "${sizes[@]}"; do
  install -Dm644 -- "$ROOT/assets/icons/$size.png" "$ICONS/${size}x${size}/apps/$APP_ID.png"
done

mkdir -p "$APPLICATIONS"
# The desktop entry serves both the application menu and portal registration.
# Escape the quoted executable, then escape backslashes for the desktop file.
EXEC="$ROOT/scripts/run-kawari.sh"
EXEC="${EXEC//\\/\\\\}"; EXEC="${EXEC//\"/\\\"}"; EXEC="${EXEC//\$/\\\$}"; EXEC="${EXEC//\`/\\\`}"; EXEC="${EXEC//%/%%}"
EXEC="${EXEC//\\/\\\\}"
TMP="$(mktemp "$APPLICATIONS/.kawari.XXXXXX")"; trap 'rm -f "$TMP"' EXIT
printf '[Desktop Entry]\nType=Application\nName=Kawari\nExec="%s" --allow-plugin VBridger\nIcon=%s\nStartupWMClass=%s\nTerminal=false\nCategories=AudioVideo;\n' "$EXEC" "$APP_ID" "$APP_ID" > "$TMP"
chmod 644 "$TMP"; mv -f -- "$TMP" "$APPLICATIONS/$APP_ID.desktop"
echo "Kawari desktop entry: $APPLICATIONS/$APP_ID.desktop"
