// The font reader, on the fonts the game really ships, and on what happens when a file is damaged.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "text/truetype.hpp"

using namespace bancada::text;

namespace {

// The folder is UTF-8 and may have accents, as in "Estácio". A narrow string would be read in the Windows code
// page and the file would not be found, so the path is built from UTF-8 explicitly.
std::vector<std::uint8_t> readFont(const char* name) {
    const std::string_view folder = BANCADA_FONTS;
    const std::filesystem::path path = std::filesystem::path(std::u8string(folder.begin(), folder.end())) / name;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (bytes.empty()) std::printf("  could not read the font %s\n", name);
    return bytes;
}

const std::vector<std::uint8_t>& regularBytes() {
    static const std::vector<std::uint8_t> bytes = readFont("Ubuntu-R.ttf");
    return bytes;
}

Font regular() { return Font::parse(regularBytes()).value(); }

int inked(const GlyphBitmap& bitmap) {
    return std::accumulate(bitmap.coverage.begin(), bitmap.coverage.end(), 0, [](int sum, std::uint8_t c) { return sum + (c > 127 ? 1 : 0); });
}

}  // namespace

TEST(the_fonts_the_game_ships_can_be_read) {
    for (const char* name : {"Ubuntu-R.ttf", "Ubuntu-M.ttf", "UbuntuMono-R.ttf"}) {
        const std::vector<std::uint8_t> bytes = readFont(name);
        CHECK(!bytes.empty());
        CHECK(Font::parse(bytes).has_value());
    }
}

TEST(every_letter_portuguese_uses_is_in_the_font) {
    const Font font = regular();
    const std::string_view letters = "ABCdefÁÂÃÀÇÉÊÍÓÔÕÚáâãàçéêíóôõú.,;:!?()";
    std::size_t at = 0;
    while (at < letters.size()) {
        const char32_t character = nextCharacter(letters, at);
        const std::uint16_t glyph = font.glyphFor(character);
        CHECK(glyph != 0);
        CHECK(inked(font.rasterize(glyph, 32.0f)) > 0);
    }
}

TEST(a_character_the_font_lacks_is_glyph_zero) { CHECK_EQ(regular().glyphFor(0x10FFFF), 0); }

TEST(a_space_has_width_and_nothing_to_draw) {
    const Font font = regular();
    const std::uint16_t space = font.glyphFor(U' ');
    CHECK(font.advance(space, 16.0f) > 0.0f);
    CHECK(font.rasterize(space, 16.0f).coverage.empty());
}

TEST(an_accent_sits_above_its_letter) {
    const Font font = regular();
    const GlyphBitmap plain = font.rasterize(font.glyphFor(U'a'), 64.0f);
    const GlyphBitmap accented = font.rasterize(font.glyphFor(U'á'), 64.0f);
    CHECK(accented.top > plain.top + 5);
    CHECK(inked(accented) > inked(plain));
}

TEST(a_cedilla_hangs_below_the_baseline) {
    const Font font = regular();
    const GlyphBitmap plain = font.rasterize(font.glyphFor(U'c'), 64.0f);
    const GlyphBitmap cedilla = font.rasterize(font.glyphFor(U'ç'), 64.0f);
    CHECK(cedilla.top - cedilla.height < plain.top - plain.height - 5);
}

TEST(a_capital_is_about_as_tall_as_the_font_says) {
    const Font font = regular();
    const GlyphBitmap h = font.rasterize(font.glyphFor(U'H'), 100.0f);
    CHECK(h.top > 60 && h.top < 80);
    CHECK(h.top - h.height >= -1 && h.top - h.height <= 1);
}

TEST(the_hole_in_an_o_is_empty_and_its_ring_is_solid) {
    const Font font = regular();
    const GlyphBitmap o = font.rasterize(font.glyphFor(U'o'), 64.0f);
    const auto at = [&](int x, int y) { return o.coverage[static_cast<std::size_t>(y * o.width + x)]; };
    CHECK_EQ(at(o.width / 2, o.height / 2), 0);
    CHECK_EQ(at(o.width / 2, 2), 255);
    CHECK_EQ(at(2, o.height / 2), 255);
    CHECK_EQ(at(0, 0), 0);
}

TEST(bigger_means_proportionally_more_ink) {
    const Font font = regular();
    const std::uint16_t a = font.glyphFor(U'B');
    const double small = inked(font.rasterize(a, 50.0f));
    const double large = inked(font.rasterize(a, 100.0f));
    CHECK(large / small > 3.7 && large / small < 4.3);
}

TEST(every_letter_of_the_monospaced_font_is_equally_wide) {
    const std::vector<std::uint8_t> bytes = readFont("UbuntuMono-R.ttf");
    const Font font = Font::parse(bytes).value();
    const float width = font.advance(font.glyphFor(U'M'), 16.0f);
    for (const char32_t c : std::u32string_view(U"il.;MWçãR0")) CHECK(font.advance(font.glyphFor(c), 16.0f) == width);
}

TEST(malformed_utf8_never_runs_off_the_end) {
    const std::vector<std::string> cases{"\xC3", "\xE2\x82", "\xF0\x9F\x98", "\xFF", "a\xC3", "\xC0\x80"};
    for (const std::string& text : cases) {
        std::size_t at = 0;
        std::size_t steps = 0;
        while (at < text.size() && steps < 20) {
            nextCharacter(text, at);
            ++steps;
        }
        CHECK(at <= text.size());
        CHECK(steps < 20);
    }
}

TEST(a_damaged_font_never_crashes_the_reader) {
    std::mt19937 random(5);
    const std::string_view letters = "AéçWgQ@";
    for (int round = 0; round < 400; ++round) {
        std::vector<std::uint8_t> bytes = regularBytes();
        if (round % 3 == 0) bytes.resize(random() % bytes.size());
        const int damage = 1 + static_cast<int>(random() % 200);
        for (int i = 0; i < damage && !bytes.empty(); ++i) bytes[random() % (round % 2 == 0 ? std::min<std::size_t>(bytes.size(), 4096) : bytes.size())] = static_cast<std::uint8_t>(random());
        const std::optional<Font> font = Font::parse(bytes);
        if (!font) continue;
        std::size_t at = 0;
        while (at < letters.size()) {
            const std::uint16_t glyph = font->glyphFor(nextCharacter(letters, at));
            font->advance(glyph, 20.0f);
            font->rasterize(glyph, 20.0f);
        }
    }
    CHECK(true);
}

int main() { return check::runAll(); }
