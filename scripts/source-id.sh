#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
sha256sum native/studio_controls.cpp native/studio_controls.hpp native/studio_broadcast.cpp native/studio_broadcast.hpp native/studio_render_size.hpp native/studio_background.hpp native/studio_parameters.hpp native/studio_parameter_socket.hpp native/studio_ui_layout.hpp native/studio_ui.hpp native/studio_ui.cpp native/studio_ui_theme.hpp native/studio_ui_preferences.hpp native/studio_physics.hpp native/studio_physics_settings.hpp native/studio_animations.hpp native/studio_items.hpp native/studio_item_attachment.hpp native/studio_items_image.cpp native/studio_items_vk.hpp native/studio_items_vk.cpp native/items.vert native/items.frag \
  native/studio_app_icon.{cpp,hpp} assets/icons/{16,32,48,64}.png \
  scripts/embed-item-shaders.py scripts/embed-app-icon.py scripts/patch_sdk.py scripts/build.sh scripts/source-id.sh | sha256sum | cut -d ' ' -f 1
