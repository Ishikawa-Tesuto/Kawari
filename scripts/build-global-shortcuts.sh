#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/api-rust/target/release/kawari-global-shortcuts"
mkdir -p "$(dirname "$OUT")"
if ! pkg-config --exists x11 gio-2.0; then
  echo 'Global shortcut build needs X11 and GIO development files (Arch: sudo pacman -S --needed libx11 glib2 pkgconf gcc).' >&2
  exit 1
fi
TMP="$(mktemp "${OUT}.XXXXXX")"; trap 'rm -f "$TMP"' EXIT
# pkg-config flags are compiler arguments, intentionally word-split.
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -DKAWARI_PORTAL \
  $(pkg-config --cflags x11 gio-2.0) "$ROOT/native/global_shortcuts.cpp" \
  -o "$TMP" $(pkg-config --libs x11 gio-2.0)
chmod 755 "$TMP"; mv -f "$TMP" "$OUT"
printf 'Global shortcut helper: %s\n' "$OUT"
