#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${CUBISM_SDK_ROOT:-$ROOT/vendor/cubism-sdk}"
SDK="$(realpath -e -- "$SDK")" || { echo 'Set CUBISM_SDK_ROOT to the unpacked Cubism 5 SDK for Native R5.' >&2; exit 1; }
for tool in cargo cmake ninja python3 glslc git; do command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }; done
for f in Core/lib/linux/x86_64/libLive2DCubismCore.a Core/include/Live2DCubismCore.h Samples/Vulkan/Demo/proj.linux.cmake/CMakeLists.txt Framework/CMakeLists.txt; do
  [[ -f "$SDK/$f" ]] || { echo "Incomplete SDK: $f" >&2; exit 1; }
done
mkdir -p "$ROOT/build"
rm -f -- "$ROOT/build/studio-build-id"
SOURCE_ID="$(bash "$ROOT/scripts/source-id.sh")"
STAGE="$ROOT/build/sdk"
# Keep source SDK pristine. Recreate stage on SDK-path change or explicit reset.
WITH_BROADCAST=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in --clean-native) CLEAN=1;; --with-broadcast) WITH_BROADCAST=1;; *) echo 'Usage: scripts/build.sh [--clean-native] [--with-broadcast]' >&2; exit 1;; esac
done
if [[ "$CLEAN" == 1 ]]; then rm -rf -- "$STAGE" "$ROOT/build/native"; fi
FUNNEL_BUILD=""
FUNNEL_INCLUDE=""
if [[ "$WITH_BROADCAST" == 1 ]]; then
  FUNNEL_SRC="${FUNNEL_SOURCE:-$ROOT/vendor/libfunnel}"
  [[ -f "$FUNNEL_SRC/meson.build" ]] || { echo 'Run scripts/probe-broadcast.sh --build first.' >&2; exit 1; }
  REV="$(git -C "$FUNNEL_SRC" rev-parse HEAD)"
  [[ "$REV" == 4f049b39264afae316f5e04738957b462403bb49 ]] || { echo "Unsupported libfunnel revision: $REV (expected tested 4f049b39264afae316f5e04738957b462403bb49)" >&2; exit 1; }
  "$ROOT/scripts/probe-broadcast.sh" --build
  FUNNEL_BUILD="$ROOT/build/funnel-probe-a1"
  FUNNEL_INCLUDE="$ROOT/build/funnel-source-a1/include"
  [[ -f "$FUNNEL_BUILD/libfunnel.so" && -f "$FUNNEL_BUILD/libfunnel-vk.so" && -f "$FUNNEL_INCLUDE/funnel-vk.h" ]] || { echo 'libfunnel build files not found' >&2; exit 1; }
fi
if [[ -f "$ROOT/build/sdk-origin" ]] && [[ "$(cat "$ROOT/build/sdk-origin")" != "$SDK" ]]; then
  echo 'SDK source changed; use --clean-native.' >&2; exit 1
fi
# Keep GUI dependency outside staged SDK so updates do not re-download it.
IMGUI_ROOT="$ROOT/vendor/imgui"
IMGUI_TAG=v1.91.9b
if [[ ! -d "$IMGUI_ROOT/.git" ]]; then
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git "$IMGUI_ROOT"
fi
[[ "$(git -C "$IMGUI_ROOT" describe --tags --exact-match HEAD)" == "$IMGUI_TAG" ]] || { echo "Expected Dear ImGui $IMGUI_TAG in vendor/imgui" >&2; exit 1; }
[[ -z "$(git -C "$IMGUI_ROOT" status --porcelain)" ]] || { echo 'Dear ImGui checkout has local modifications; use a clean checkout.' >&2; exit 1; }
PATCH_ID="$(sha256sum "$ROOT/scripts/patch_sdk.py" | cut -d ' ' -f 1)"
if [[ ! -f "$STAGE/.studio-ready" ]] || [[ "$(cat "$STAGE/.studio-ready")" != "$PATCH_ID" ]]; then
  rm -rf -- "$ROOT/build/native"
  rm -rf -- "$STAGE"
  mkdir -p "$STAGE"
  cp -a --reflink=auto "$SDK/." "$STAGE/"
  THIRD="$STAGE/Samples/Vulkan/thirdParty"
  if [[ ! -f "$THIRD/glfw/CMakeLists.txt" ]]; then
    mkdir -p "$THIRD"
    git clone --depth 1 --branch 3.4 https://github.com/glfw/glfw.git "$THIRD/glfw"
  fi
  [[ -f "$THIRD/stb/stb_image.h" ]] || { echo 'SDK is missing Samples/Vulkan/thirdParty/stb/stb_image.h' >&2; exit 1; }
  python3 "$ROOT/scripts/patch_sdk.py" "$STAGE" "$ROOT/native"
  printf '%s\n' "$SDK" > "$ROOT/build/sdk-origin"
  printf '%s\n' "$PATCH_ID" > "$STAGE/.studio-ready"
fi
# Updating our controls does not require rebuilding the SDK staging directory.
cp -- "$ROOT/native/"studio_controls.{cpp,hpp} "$ROOT/native/"studio_broadcast.{cpp,hpp} "$ROOT/native/studio_render_size.hpp" "$ROOT/native/studio_background.hpp" "$ROOT/native/studio_parameters.hpp" "$ROOT/native/studio_parameter_socket.hpp" "$ROOT/native/studio_ui_layout.hpp" "$ROOT/native/studio_ui.hpp" "$ROOT/native/studio_ui.cpp" "$ROOT/native/studio_ui_theme.hpp" "$ROOT/native/studio_ui_preferences.hpp" "$ROOT/native/studio_physics.hpp" "$ROOT/native/studio_physics_settings.hpp" "$ROOT/native/studio_animations.hpp" "$ROOT/native/studio_items.hpp" "$ROOT/native/studio_items_image.cpp" "$ROOT/native/studio_items_vk.hpp" "$ROOT/native/studio_items_vk.cpp" "$STAGE/Samples/Vulkan/Demo/proj.linux.cmake/src/"
NATIVE_SRC="$STAGE/Samples/Vulkan/Demo/proj.linux.cmake/src"
cp -- "$ROOT/native/"studio_app_icon.{cpp,hpp} "$NATIVE_SRC/"
python3 "$ROOT/scripts/embed-app-icon.py" "$ROOT/assets/icons" "$NATIVE_SRC/studio_app_icon_data.hpp"
# Build the tiny item compositor shaders with the same SDK toolchain.
glslc "$ROOT/native/items.vert" -o "$ROOT/build/items.vert.spv"
glslc "$ROOT/native/items.frag" -o "$ROOT/build/items.frag.spv"
python3 "$ROOT/scripts/embed-item-shaders.py" "$ROOT/build/items.vert.spv" "$ROOT/build/items.frag.spv" "$NATIVE_SRC/studio_items_shaders.hpp"
cmake -S "$STAGE/Samples/Vulkan/Demo/proj.linux.cmake" -B "$ROOT/build/native" -G Ninja \
  -DSTUDIO_IMGUI_ROOT="$IMGUI_ROOT" -DSTUDIO_FUNNEL_BUILD="$FUNNEL_BUILD" -DSTUDIO_FUNNEL_INCLUDE="$FUNNEL_INCLUDE" \
  -DCMAKE_BUILD_TYPE=Release -DCSM_MINIMUM_DEMO=OFF -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON
cmake --build "$ROOT/build/native" --parallel "${JOBS:-4}"
(cd "$ROOT" && cargo build --locked --release)
[[ -x "$ROOT/build/native/bin/Demo/Demo" ]] || { echo 'Expected native viewer not found' >&2; exit 1; }
[[ "$(bash "$ROOT/scripts/source-id.sh")" == "$SOURCE_ID" ]] || { echo 'Sources changed during build. Rebuild.' >&2; exit 1; }
printf '%s\n' "$WITH_BROADCAST" > "$ROOT/build/studio-broadcast-enabled"
printf '%s\n' "$SOURCE_ID" > "$ROOT/build/studio-build-id"
printf '\nBuilt. Run: %s/scripts/run.sh --sample\n' "$ROOT"
