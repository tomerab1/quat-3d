#include "app/window_icon.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>
#include <stb_image.h>

#include "engine/core/utf8_path.hpp"

namespace quat::app {

namespace {

constexpr std::uintmax_t k_max_icon_bytes = 16ULL << 20;
constexpr int            k_max_icon_extent = 1024;

[[nodiscard]] std::unexpected<engine::core::Error> fail(std::string what) {
    return std::unexpected(engine::core::Error{std::move(what)});
}

} // namespace

std::expected<void, engine::core::Error> set_window_icon(SDL_Window* window,
                                                         const std::filesystem::path& png) {
    const std::string name = engine::core::path_to_utf8(png);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(png, ec);
    if (ec || size == 0 || size > k_max_icon_bytes) return fail("icon '" + name + "': cannot read");
    // Read through std::filesystem (wide paths on Windows), decode from memory.
    std::ifstream in(png, std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in),
                                           std::istreambuf_iterator<char>()};
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h,
                                            &channels, 4);
    if (pixels == nullptr) return fail("icon '" + name + "': not a decodable PNG");
    if (w <= 0 || h <= 0 || w > k_max_icon_extent || h > k_max_icon_extent) {
        stbi_image_free(pixels);
        return fail("icon '" + name + "': expected at most 1024x1024 pixels");
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surface == nullptr) {
        stbi_image_free(pixels);
        return fail(std::string("icon: ") + SDL_GetError());
    }
    const bool ok = SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    if (!ok) return fail(std::string("icon: ") + SDL_GetError());
    return {};
}

} // namespace quat::app
