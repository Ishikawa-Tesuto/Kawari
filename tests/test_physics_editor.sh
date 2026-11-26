#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
D="$(mktemp -d)"
trap 'rm -rf -- "$D"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -I "$ROOT/native" "$ROOT/tests/test_physics_editor.cpp" -o "$D/test"
"$D/test"
echo 'physics editor settings: OK'
c++ -std=c++11 -Wall -Wextra -Werror -I "$ROOT/tests/stubs" -I "$ROOT/native" "$ROOT/tests/test_physics_runtime.cpp" -o "$D/runtime"
"$D/runtime"
echo 'physics editor runtime (SDK stub): OK'
