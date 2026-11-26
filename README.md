<p align="center">
  <img src="assets/icons/256.png" width="112" height="112" alt="Kawari's purple shark mascot">
</p>
<h1 align="center">Kawari</h1>

Kawari is a native VTuber studio for Linux. Load a Live2D model, configure tracking and expressions, add props to your scene, and send video with a transparent background to OBS over PipeWire.

**P0.5h3 · Prototype · Linux x86-64**

[Build](#build-on-arch-linux) · [Launch](#first-launch) · [OBS](#stream-to-obs) · [Integrations](#integrations)

## Features

| Area | What you can do |
| --- | --- |
| **Live2D models** | Open `.model3.json` models, switch between them from the model library, configure tracking and physics, and control animation playback. |
| **Expressions and hotkeys** | Create expressions that change model parameters and item visibility. Set fade-in and fade-out durations, assign hotkeys, and make expressions mutually exclusive. |
| **Scene items** | Import PNGs and GIFs, layer them in front of or behind the model, and adjust their size, rotation, and opacity. Attach items to the model root so they follow its position and scale. |
| **Workspace** | Dock panels on the right or open them as floating windows. Docked panels overlay the preview while the model keeps its position and scale. |
| **Video output** | Send video with an alpha channel over PipeWire using DMA-BUF. Set the stream resolution independently of the window size. |
| **Integrations** | Connect VBridger and other clients through Kawari's partial implementation of the VTube Studio Public API. Bind Twitch Channel Points rewards to expressions. |

## Build on Arch Linux

### 1. Install dependencies

Install the packages below to build with PipeWire support. You also need a Vulkan driver for your GPU. Use `vulkaninfo --summary` to check which GPU Vulkan detects.

```bash
sudo pacman -S --needed \
  base-devel rust cmake meson ninja git python pkgconf shaderc \
  vulkan-headers vulkan-icd-loader vulkan-tools \
  pipewire libdrm mesa wayland wayland-protocols libxkbcommon \
  libx11 libxrandr libxinerama libxcursor libxi libxext \
  libpng giflib ttf-dejavu
```

### 2. Set up Cubism SDK and libfunnel

Download **Cubism 5 SDK for Native R5** (`5-r.5`). The extracted SDK root should contain `Core`, `Framework`, and `Samples`. Place it in `vendor/cubism-sdk` or set `CUBISM_SDK_ROOT` to its location. [Download the SDK from Live2D](https://www.live2d.com/en/sdk/download/native/)

From the project root, clone libfunnel if `vendor/libfunnel` is missing. Kawari's build requires the revision shown below.

```bash
git clone https://github.com/hoshinolina/libfunnel.git vendor/libfunnel
git -C vendor/libfunnel checkout --detach 4f049b39264afae316f5e04738957b462403bb49
git -C vendor/libfunnel submodule update --init --recursive
```

### 3. Build the renderer and core

Run these commands from the project root.

```bash
./scripts/build.sh --with-broadcast
./scripts/build-core.sh
```

The renderer build patches a copy of the Cubism SDK in `build/sdk`. The first build may need GitHub access to fetch Dear ImGui and GLFW. If you update the SDK in place, refresh the build copy with `./scripts/build.sh --clean-native --with-broadcast`.

## First launch

Pass the path to your model's original `.model3.json` file. `STUDIO_HOT_SWITCH=1` enables in-session model switching while preview and PipeWire output are active.

```bash
STUDIO_HOT_SWITCH=1 ./scripts/run-kawari.sh "$HOME/Models/MyModel/MyModel.model3.json"
```

On later launches, Kawari can reopen the last model when you omit the path. Use `--library-dir` to add a model directory to the library, or run `./scripts/run-kawari.sh --help` for all launch options.

To launch Kawari from your desktop's application menu, install the launcher and its icon. Run the command again if you move the project folder.

```bash
./scripts/install-portal-identity.sh
```

## Stream to OBS

1. Install the PipeWire video source plugin for OBS. [obs-pwvideo](https://github.com/hoshinolina/obs-pwvideo)
2. Launch a Kawari build with PipeWire support.
3. Add the plugin's PipeWire video source in OBS and select **Live2D**.

Set the stream resolution and target frame rate under **Settings → Broadcast**. Changes to either setting take effect the next time you start Kawari. Run `./scripts/broadcast-status.sh` to check whether Kawari is publishing the stream.

Use `--no-preview` to stream with the preview window hidden. If you saved that mode as your default, run `./scripts/run-kawari.sh --preview` to show the window for one launch.

## Integrations

- **VBridger and plugins.** Connect to `ws://127.0.0.1:8001`, or use the port you passed to Kawari with `--port`. Approve the client's request under **Integrations → Plugins**.
- **Twitch.** Install `curl`, `json-c`, and `xdg-utils`, then run `./scripts/build-twitch.sh`. Open **Integrations → Twitch**, enter your Twitch application's public Client ID, and click **Connect** to authorize your account. You can then bind a Channel Points reward to an expression. In **Timed** mode, the countdown shows how long remains before the expression starts fading out. [Twitch Developer Console](https://dev.twitch.tv/console/apps)
- **Global hotkeys.** Build the shortcut helper with `./scripts/build-global-shortcuts.sh`. On X11, enable **Global** for an assigned hotkey. On Wayland, global registration is experimental and disabled by default.

## Data and settings

Kawari stores its data in `~/.local/share/kawari/` by default, or in `$XDG_DATA_HOME/kawari/` when that variable is set. Each model has a separate profile under `profiles/` for its parameters, expressions, and hotkeys. Interface settings, broadcast settings, and scene items are shared across models. Kawari treats your original model files as read-only.

## Development

The application core and WebSocket server are written in Rust. The C++ renderer uses Cubism SDK and Vulkan, and the interface uses Dear ImGui. Run build and SDK preparation scripts in `scripts/` from the project root.

```bash
python3 tests/test_workspace.py -v
python3 tests/test_render_size.py -v
python3 tests/test_expression_management.py -v
```
