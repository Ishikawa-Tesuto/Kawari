#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -Inative tests/test_animations.cpp -o "$tmp/animations"
"$tmp/animations"
