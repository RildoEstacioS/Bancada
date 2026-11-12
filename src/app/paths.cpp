#include "app/paths.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <system_error>

namespace bancada::app {

namespace {

// SDL gives paths in UTF-8, and a user's folder can have accents in it.
std::filesystem::path fromUtf8(const char* text) {
    const std::string bytes(text);
    return std::filesystem::path(std::u8string(bytes.begin(), bytes.end()));
}

// The folder the player sees the program in. An AppImage runs from a read-only mount, so its own
// location comes from the APPIMAGE variable instead.
std::filesystem::path programFolder() {
    if (const char* image = SDL_getenv("APPIMAGE")) return fromUtf8(image).parent_path();
    const char* base = SDL_GetBasePath();
    return base != nullptr ? fromUtf8(base) : std::filesystem::path{};
}

}  // namespace

DataLocation findDataLocation() {
    const std::filesystem::path portable = programFolder() / "dados";
    std::error_code error;
    if (std::filesystem::is_directory(portable, error)) return {portable, true};

    DataLocation location;
    if (char* folder = SDL_GetPrefPath("bancada", "bancada")) {
        location.directory = fromUtf8(folder);
        SDL_free(folder);
    }
    return location;
}

}  // namespace bancada::app
