#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <png.h>

#include "imgui.h"
#include "studio_app_icon.hpp"
#include "studio_app_icon_data.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace studio { namespace app_icon {
namespace {
constexpr char desktop_id[] = "ru.tesutoidclub.Kawari";

template<std::size_t Size>
bool decode(const unsigned char (&data)[Size], int side, std::vector<unsigned char>& pixels) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    bool ok = false;
    if (png_image_begin_read_from_memory(&image, data, Size) &&
        image.width == unsigned(side) && image.height == unsigned(side)) {
        image.format = PNG_FORMAT_RGBA;
        pixels.resize(PNG_IMAGE_SIZE(image));
        ok = png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0;
    }
    png_image_free(&image);
    return ok;
}

void set_x11_icon(GLFWwindow* window) {
    std::vector<unsigned char> small, medium, large;
    if (!decode(png_16, 16, small) || !decode(png_32, 32, medium) || !decode(png_48, 48, large)) return;
    const GLFWimage images[] = {
        {16, 16, small.data()}, {32, 32, medium.data()}, {48, 48, large.data()},
    };
    glfwSetWindowIcon(window, 3, images);
}
}

GLFWwindow* create_window(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share) {
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, desktop_id);
    glfwWindowHintString(GLFW_X11_CLASS_NAME, desktop_id);
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, desktop_id);
    auto* window = glfwCreateWindow(width, height, title, monitor, share);
    // On Wayland the compositor resolves the icon from the desktop entry.
    if (window && glfwGetPlatform() == GLFW_PLATFORM_X11) set_x11_icon(window);
    return window;
}

bool add_to_atlas(ImFontAtlas& atlas, ImVec2& uv_min, ImVec2& uv_max) {
    constexpr int side = 64;
    std::vector<unsigned char> pixels;
    if (!decode(png_64, side, pixels)) return false;

    // The Vulkan backend uploads the font atlas as RGBA before the first frame.
    const int index = atlas.AddCustomRectRegular(side, side);
    if (!atlas.Build()) return false;
    const auto* rect = atlas.GetCustomRectByIndex(index);
    if (!rect->IsPacked()) return false;

    unsigned char* texture = nullptr;
    int width = 0;
    atlas.GetTexDataAsRGBA32(&texture, &width, nullptr);
    if (!texture) return false;
    const std::size_t row_bytes = side * 4;
    for (int y = 0; y < side; ++y) {
        const std::size_t offset = (std::size_t(rect->Y + y) * width + rect->X) * 4;
        std::memcpy(texture + offset, pixels.data() + y * row_bytes, row_bytes);
    }
    atlas.TexPixelsUseColors = true;
    atlas.CalcCustomRectUV(rect, &uv_min, &uv_max);
    return true;
}

}}
