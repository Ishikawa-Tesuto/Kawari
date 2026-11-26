#pragma once

struct GLFWmonitor;
struct GLFWwindow;
struct ImFontAtlas;
struct ImVec2;

namespace studio { namespace app_icon {

GLFWwindow* create_window(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share);
bool add_to_atlas(ImFontAtlas& atlas, ImVec2& uv_min, ImVec2& uv_max);

}}
