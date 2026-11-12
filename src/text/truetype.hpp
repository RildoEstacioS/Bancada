// Reads TrueType fonts and turns glyphs into grayscale bitmaps. Self-contained, so that the game needs no
// font library and the portable .exe stays a single file. Everything read from the file is bounds-checked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bancada::text {

struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int left = 0;  // pixels from the pen to the bitmap's left edge
    int top = 0;   // pixels from the baseline up to the bitmap's top row
    std::vector<std::uint8_t> coverage;  // top row first, 0 to 255
};

class Font {
public:
    // Returns nothing if this isn't a font it can read. The bytes must outlive the font.
    static std::optional<Font> parse(std::span<const std::uint8_t> file);

    std::uint16_t glyphFor(char32_t character) const;  // 0, the "missing" glyph, if it has none

    // All in pixels at this size, with ascent up and descent down from the baseline.
    float advance(std::uint16_t glyph, float pixels) const;
    float ascent(float pixels) const;
    float descent(float pixels) const;
    float lineHeight(float pixels) const;

    GlyphBitmap rasterize(std::uint16_t glyph, float pixels) const;

private:
    struct Point {
        float x;
        float y;
        bool onCurve;
    };
    struct Outline {
        std::vector<Point> points;
        std::vector<std::size_t> contourEnds;  // one past each contour's last point
    };
    struct Range {
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    std::uint8_t u8(std::size_t at) const { return at < file_.size() ? file_[at] : std::uint8_t{0}; }
    std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>((u8(at) << 8) | u8(at + 1)); }
    std::int16_t i16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
    std::uint32_t u32(std::size_t at) const {
        return (static_cast<std::uint32_t>(u16(at)) << 16) | u16(at + 2);
    }

    std::optional<Range> table(const char tag[4]) const;
    Range glyphData(std::uint16_t glyph) const;
    void appendOutline(std::uint16_t glyph, Outline& out, int depth) const;
    void appendSimple(const Range& data, int contours, Outline& out) const;
    void appendComposite(const Range& data, Outline& out, int depth) const;

    std::span<const std::uint8_t> file_;
    Range hmtx_, loca_, glyf_;
    std::size_t cmap_ = 0;       // the character map's own subtable
    std::uint16_t cmapFormat_ = 0;
    int unitsPerEm_ = 1000;
    int ascender_ = 0;
    int descender_ = 0;
    int lineGap_ = 0;
    int metricsCount_ = 0;
    int glyphCount_ = 0;
    bool longOffsets_ = false;
};

// One character of UTF-8 at text[at], moving at past it. Anything malformed comes back as U+FFFD.
char32_t nextCharacter(std::span<const char> text, std::size_t& at);

}  // namespace bancada::text
