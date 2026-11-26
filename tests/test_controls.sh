#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -pthread -Itests/stubs -Inative native/studio_controls.cpp tests/test_controls.cpp -o "$tmp/controls"
env -u STUDIO_DEMO -u STUDIO_STATS -u STUDIO_OFFSCREEN "$tmp/controls"
