// Text on screen. Glyphs are drawn once at the exact pixel size asked for, kept in one texture, and placed
// on whole pixels, which is what keeps small text sharp at any zoom.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "text/truetype.hpp"

namespace bancada::app {

enum class Face : std::uint8_t { Regular, Medium, Mono };

class TextRenderer {
public:
    explicit TextRenderer(SDL_Renderer* renderer);
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    float measure(Face face, float size, std::string_view text) const;
    float lineHeight(Face face, float size) const;

    // y is the baseline. Returns where the pen ended up.
    float draw(Face face, float size, float x, float y, std::string_view text, SDL_FColor color);

private:
    struct Glyph {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int left = 0;
        int top = 0;
    };

    const text::Font& font(Face face) const { return *fonts_[static_cast<std::size_t>(face)]; }
    const Glyph& glyph(Face face, int size, std::uint16_t index);
    void clearAtlas();

    SDL_Renderer* renderer_;
    SDL_Texture* atlas_ = nullptr;
    std::vector<std::optional<text::Font>> fonts_;
    std::unordered_map<std::uint64_t, Glyph> glyphs_;
    int penX_ = 1;
    int penY_ = 1;
    int shelfHeight_ = 0;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

}  // namespace bancada::app
