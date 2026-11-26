#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT/build"
if ! pkg-config --exists 'libcurl >= 8.14' json-c; then
  echo 'Native Twitch needs libcurl >= 8.14 with WebSocket support and json-c. Arch: sudo pacman -S --needed curl json-c pkgconf gcc' >&2
  exit 1
fi
out="$ROOT/build/kawari-twitch"
tmp="$(mktemp "$ROOT/build/.kawari-twitch.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
# shellcheck disable=SC2046
${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -Werror "$ROOT/native/twitch_bridge.cpp" $(pkg-config --cflags --libs libcurl json-c) -o "$tmp"
"$tmp" --check
mv -f "$tmp" "$out"
echo "Built $out"
