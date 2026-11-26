#!/usr/bin/env bash
# Independent upstream Vulkan/PipeWire transport test; no Cubism dependency.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${FUNNEL_SOURCE:-$ROOT/vendor/libfunnel}"
BUILD="$ROOT/build/funnel-probe-a1"
STAGE="$ROOT/build/funnel-source-a1"
case "${1:-}" in
  --build)
    for t in git meson ninja pkg-config python3; do command -v "$t" >/dev/null || { echo "Missing $t" >&2; exit 1; }; done
    if [[ ! -f "$SRC/meson.build" ]]; then
      if [[ -e "$SRC" ]]; then echo "Not a libfunnel source directory: $SRC" >&2; exit 1; fi
      mkdir -p "$(dirname "$SRC")"
      git clone --recurse-submodules https://github.com/hoshinolina/libfunnel.git "$SRC"
    fi
    SRC="$(realpath "$SRC")"
    # Keep the upstream checkout untouched; reuse only an identical staged source.
    REV="$(git -C "$SRC" rev-parse HEAD)"
    PATCH_ID="$(sha256sum "$ROOT/scripts/patch_funnel_probe.py" | cut -d ' ' -f 1)"
    STAGE_ID="$SRC:$REV:$PATCH_ID"
    if [[ ! -f "$STAGE/.studio-id" ]] || [[ "$(cat "$STAGE/.studio-id")" != "$STAGE_ID" ]]; then
      mkdir -p "$ROOT/build"
      rm -rf -- "$STAGE" "$BUILD"
      mkdir -p "$STAGE"
      cp -a --reflink=auto "$SRC/." "$STAGE/"
      python3 "$ROOT/scripts/patch_funnel_probe.py" "$STAGE/demo/test-vk.c"
      printf '%s\n' "$STAGE_ID" > "$STAGE/.studio-id"
    fi
    if [[ -f "$BUILD/meson-private/coredata.dat" ]]; then
      meson configure "$BUILD" -Degl=false -Dvulkan=true -Dtest_apps=true
    else
      meson setup "$BUILD" "$STAGE" -Degl=false -Dvulkan=true -Dtest_apps=true
    fi
    meson compile -C "$BUILD" funnel-test-vk
    printf '%s\n' "$REV" | tee "$BUILD/source-revision"
    ;;
  --run)
    [[ -n "${WAYLAND_DISPLAY:-}" ]] || { echo 'The upstream Vulkan demo requires Wayland.' >&2; exit 1; }
    [[ -x "$BUILD/funnel-test-vk" ]] || { echo 'Run --build first.' >&2; exit 1; }
    exec "$BUILD/funnel-test-vk"
    ;;
  --status)
    command -v pw-dump >/dev/null || { echo 'pw-dump missing' >&2; exit 2; }
    pw-dump | python3 "$ROOT/scripts/check_pipewire_video.py"
    ;;
  --diagnose)
    echo '=== Session ==='; printf 'XDG_SESSION_TYPE=%s\nWAYLAND_DISPLAY=%s\n' "${XDG_SESSION_TYPE:-}" "${WAYLAND_DISPLAY:-}"
    echo '=== PipeWire ==='; if command -v pw-cli >/dev/null; then pw-cli info 0 || true; else echo 'pw-cli missing'; fi
    echo '=== Vulkan devices ==='; if command -v vulkaninfo >/dev/null; then vulkaninfo --summary; else echo 'vulkaninfo missing'; fi
    echo '=== Vulkan external image extensions (all reported devices) ==='
    if command -v vulkaninfo >/dev/null; then vulkaninfo 2>/dev/null | grep -E 'deviceName[[:space:]]*=|VK_(KHR_external_(memory|semaphore)_fd|EXT_external_memory_dma_buf|EXT_image_drm_format_modifier)[[:space:]]*:' || true; fi
    ;;
  *) echo 'Usage: scripts/probe-broadcast.sh --build | --run | --status | --diagnose' >&2; exit 2;;
esac
