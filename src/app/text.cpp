#include "app/text.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include "app/fonts_data.hpp"

namespace bancada::app {

namespace {

constexpr int kAtlasSize = 2048;
constexpr int kMinSize = 7;
constexpr int kMaxSize = 160;

int wholeSize(float size) { return std::clamp(static_cast<int>(std::lround(size)), kMinSize, kMaxSize); }

std::uint64_t keyOf(Face face, int size, std::uint16_t glyph) {
    return (static_cast<std::uint64_t>(face) << 32) | (static_cast<std::uint64_t>(size) << 16) | glyph;
}

// Thin light strokes on a dark background look thinner than they are. A small lift evens them out.
std::uint8_t lifted(std::uint8_t coverage) {
    static const std::array<std::uint8_t, 256> table = [] {
        std::array<std::uint8_t, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<std::uint8_t>(std::lround(std::pow(static_cast<double>(i) / 255.0, 0.82) * 255.0));
        }
        return values;
    }();
    return table[coverage];
}

}  // namespace

TextRenderer::TextRenderer(SDL_Renderer* renderer) : renderer_(renderer) {
    fonts_.push_back(text::Font::parse(std::span(fonts::kRegular, fonts::kRegularSize)));
    fonts_.push_back(text::Font::parse(std::span(fonts::kMedium, fonts::kMediumSize)));
    fonts_.push_back(text::Font::parse(std::span(fonts::kMono, fonts::kMonoSize)));
    atlas_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kAtlasSize, kAtlasSize);
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas_, SDL_SCALEMODE_NEAREST);
    const std::vector<std::uint8_t> clear(static_cast<std::size_t>(kAtlasSize * kAtlasSize * 4), 0);
    SDL_UpdateTexture(atlas_, nullptr, clear.data(), kAtlasSize * 4);
}

TextRenderer::~TextRenderer() {
    if (atlas_ != nullptr) SDL_DestroyTexture(atlas_);
}

float TextRenderer::lineHeight(Face face, float size) const { return font(face).lineHeight(static_cast<float>(wholeSize(size))); }

float TextRenderer::measure(Face face, float size, std::string_view text) const {
    const text::Font& f = font(face);
    const auto pixels = static_cast<float>(wholeSize(size));
    float total = 0.0f;
    std::size_t at = 0;
    while (at < text.size()) total += f.advance(f.glyphFor(text::nextCharacter(text, at)), pixels);
    return total;
}

void TextRenderer::clearAtlas() {
    glyphs_.clear();
    penX_ = 1;
    penY_ = 1;
    shelfHeight_ = 0;
}

const TextRenderer::Glyph& TextRenderer::glyph(Face face, int size, std::uint16_t index) {
    const std::uint64_t key = keyOf(face, size, index);
    const auto found = glyphs_.find(key);
    if (found != glyphs_.end()) return found->second;

    const text::GlyphBitmap bitmap = font(face).rasterize(index, static_cast<float>(size));
    if (penX_ + bitmap.width + 1 >= kAtlasSize) {
        penX_ = 1;
        penY_ += shelfHeight_ + 1;
        shelfHeight_ = 0;
    }
    // Full: start again. Anything already sent to the renderer this frame stays as it was drawn only if the
    // frame ends first, so this must be rare, and a page of 2048 pixels makes it so.
    if (penY_ + bitmap.height + 1 >= kAtlasSize) clearAtlas();

    Glyph out;
    out.x = penX_;
    out.y = penY_;
    out.width = bitmap.width;
    out.height = bitmap.height;
    out.left = bitmap.left;
    out.top = bitmap.top;
    if (bitmap.width > 0 && bitmap.height > 0) {
        std::vector<std::uint8_t> pixels(bitmap.coverage.size() * 4);
        for (std::size_t i = 0; i < bitmap.coverage.size(); ++i) {
            pixels[i * 4] = 255;
            pixels[i * 4 + 1] = 255;
            pixels[i * 4 + 2] = 255;
            pixels[i * 4 + 3] = lifted(bitmap.coverage[i]);
        }
        const SDL_Rect area{penX_, penY_, bitmap.width, bitmap.height};
        SDL_UpdateTexture(atlas_, &area, pixels.data(), bitmap.width * 4);
        penX_ += bitmap.width + 1;
        shelfHeight_ = std::max(shelfHeight_, bitmap.height);
    }
    return glyphs_.emplace(key, out).first->second;
}

float TextRenderer::draw(Face face, float size, float x, float y, std::string_view text, SDL_FColor color) {
    const text::Font& f = font(face);
    const int whole = wholeSize(size);
    const auto pixels = static_cast<float>(whole);
    const float baseline = std::round(y);
    vertices_.clear();
    indices_.clear();
    const auto atlasSize = static_cast<float>(kAtlasSize);

    float pen = x;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::uint16_t index = f.glyphFor(text::nextCharacter(text, at));
        const Glyph& g = glyph(face, whole, index);
        if (g.width > 0 && g.height > 0) {
            const float left = std::round(pen) + static_cast<float>(g.left);
            const float top = baseline - static_cast<float>(g.top);
            const auto w = static_cast<float>(g.width);
            const auto h = static_cast<float>(g.height);
            const float u0 = static_cast<float>(g.x) / atlasSize;
            const float v0 = static_cast<float>(g.y) / atlasSize;
            const float u1 = (static_cast<float>(g.x) + w) / atlasSize;
            const float v1 = (static_cast<float>(g.y) + h) / atlasSize;
            const int start = static_cast<int>(vertices_.size());
            vertices_.push_back({{left, top}, color, {u0, v0}});
            vertices_.push_back({{left + w, top}, color, {u1, v0}});
            vertices_.push_back({{left + w, top + h}, color, {u1, v1}});
            vertices_.push_back({{left, top + h}, color, {u0, v1}});
            for (int offset : {0, 1, 2, 0, 2, 3}) indices_.push_back(start + offset);
        }
        pen += f.advance(index, pixels);
    }
    if (!indices_.empty()) {
        SDL_RenderGeometry(renderer_, atlas_, vertices_.data(), static_cast<int>(vertices_.size()), indices_.data(), static_cast<int>(indices_.size()));
    }
    return pen;
}

}  // namespace bancada::app
