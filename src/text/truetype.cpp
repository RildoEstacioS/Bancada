#include "text/truetype.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bancada::text {

namespace {

constexpr int kMaxCompositeDepth = 6;
constexpr std::size_t kMaxPoints = 20000;
constexpr float kMaxPixels = 512.0f;

// Adds up, cell by cell, how much of each pixel a straight edge covers. Summing a row from left to right
// afterwards gives each pixel's exact coverage, so there's no supersampling and no jagged edges.
class Coverage {
public:
    Coverage(int width, int height) : width_(width), height_(height), cells_(static_cast<std::size_t>((width + 2) * height), 0.0f) {}

    void line(float x0, float y0, float x1, float y1) {
        if (y0 == y1) return;
        const float direction = y0 < y1 ? 1.0f : -1.0f;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        const float slope = (x1 - x0) / (y1 - y0);
        const int firstRow = std::max(0, static_cast<int>(std::floor(y0)));
        const int lastRow = std::min(height_, static_cast<int>(std::ceil(y1)));
        float x = y0 < 0.0f ? x0 - y0 * slope : x0;
        for (int row = firstRow; row < lastRow; ++row) {
            const auto top = static_cast<float>(row);
            const float height = std::min(top + 1.0f, y1) - std::max(top, y0);
            const float nextX = x + slope * height;
            span(row, x, nextX, height * direction);
            x = nextX;
        }
    }

    GlyphBitmap finish(int left, int top) const {
        GlyphBitmap out;
        out.width = width_;
        out.height = height_;
        out.left = left;
        out.top = top;
        out.coverage.resize(static_cast<std::size_t>(width_ * height_));
        const int stride = width_ + 2;
        for (int row = 0; row < height_; ++row) {
            float sum = 0.0f;
            for (int column = 0; column < width_; ++column) {
                sum += cells_[static_cast<std::size_t>(row * stride + column)];
                const float amount = std::min(std::abs(sum), 1.0f);
                out.coverage[static_cast<std::size_t>(row * width_ + column)] = static_cast<std::uint8_t>(amount * 255.0f + 0.5f);
            }
        }
        return out;
    }

private:
    void add(int row, int column, float amount) {
        if (column < 0) column = 0;
        if (column > width_) column = width_;
        cells_[static_cast<std::size_t>(row * (width_ + 2) + column)] += amount;
    }

    // One row of an edge going from x to nextX, with its signed height.
    void span(int row, float x, float nextX, float d) {
        const float low = std::min(x, nextX);
        const float high = std::max(x, nextX);
        const float lowFloor = std::floor(low);
        const auto first = static_cast<int>(lowFloor);
        const float highCeil = std::ceil(high);
        const auto last = static_cast<int>(highCeil);
        if (last <= first + 1) {
            const float mean = 0.5f * (x + nextX) - lowFloor;
            add(row, first, d - d * mean);
            add(row, first + 1, d * mean);
            return;
        }
        const float perUnit = 1.0f / (high - low);
        const float lowPart = low - lowFloor;
        const float firstArea = 0.5f * perUnit * (1.0f - lowPart) * (1.0f - lowPart);
        const float highPart = high - highCeil + 1.0f;
        const float lastArea = 0.5f * perUnit * highPart * highPart;
        add(row, first, d * firstArea);
        if (last == first + 2) {
            add(row, first + 1, d * (1.0f - firstArea - lastArea));
        } else {
            const float second = perUnit * (1.5f - lowPart);
            add(row, first + 1, d * (second - firstArea));
            for (int column = first + 2; column < last - 1; ++column) add(row, column, d * perUnit);
            const float reached = second + static_cast<float>(last - first - 3) * perUnit;
            add(row, last - 1, d * (1.0f - reached - lastArea));
        }
        add(row, last, d * lastArea);
    }

    int width_;
    int height_;
    std::vector<float> cells_;
};

}  // namespace

std::optional<Font::Range> Font::table(const char tag[4]) const {
    const std::uint16_t count = u16(4);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t record = 12 + 16 * i;
        if (record + 16 > file_.size()) return std::nullopt;
        if (std::memcmp(file_.data() + record, tag, 4) != 0) continue;
        const std::size_t offset = u32(record + 8);
        const std::size_t length = u32(record + 12);
        if (offset > file_.size() || length > file_.size() - offset) return std::nullopt;
        return Range{offset, length};
    }
    return std::nullopt;
}

std::optional<Font> Font::parse(std::span<const std::uint8_t> file) {
    Font font;
    font.file_ = file;
    if (file.size() < 12) return std::nullopt;
    const std::uint32_t version = font.u32(0);
    if (version != 0x00010000 && version != 0x74727565) return std::nullopt;

    const auto head = font.table("head");
    const auto hhea = font.table("hhea");
    const auto hmtx = font.table("hmtx");
    const auto maxp = font.table("maxp");
    const auto cmap = font.table("cmap");
    const auto loca = font.table("loca");
    const auto glyf = font.table("glyf");
    if (!head || !hhea || !hmtx || !maxp || !cmap || !loca || !glyf) return std::nullopt;
    if (head->length < 54 || hhea->length < 36 || maxp->length < 6 || cmap->length < 4) return std::nullopt;

    font.unitsPerEm_ = font.u16(head->offset + 18);
    font.longOffsets_ = font.i16(head->offset + 50) != 0;
    font.ascender_ = font.i16(hhea->offset + 4);
    font.descender_ = font.i16(hhea->offset + 6);
    font.lineGap_ = font.i16(hhea->offset + 8);
    font.metricsCount_ = font.u16(hhea->offset + 34);
    font.glyphCount_ = font.u16(maxp->offset + 4);
    font.hmtx_ = *hmtx;
    font.loca_ = *loca;
    font.glyf_ = *glyf;
    if (font.unitsPerEm_ == 0 || font.glyphCount_ == 0 || font.metricsCount_ == 0) return std::nullopt;

    // Prefers a table of all of Unicode, then one of the part most text uses.
    int bestRank = 0;
    const std::uint16_t subtables = font.u16(cmap->offset + 2);
    for (std::size_t i = 0; i < subtables; ++i) {
        const std::size_t record = cmap->offset + 4 + 8 * i;
        const std::uint16_t platform = font.u16(record);
        const std::uint16_t encoding = font.u16(record + 2);
        const std::size_t offset = cmap->offset + font.u32(record + 4);
        const std::uint16_t format = font.u16(offset);
        int rank = 0;
        if (format == 12 && (platform == 0 || (platform == 3 && encoding == 10))) rank = 2;
        if (format == 4 && (platform == 0 || (platform == 3 && encoding == 1))) rank = 1;
        if (rank > bestRank) {
            bestRank = rank;
            font.cmap_ = offset;
            font.cmapFormat_ = format;
        }
    }
    if (bestRank == 0) return std::nullopt;
    return font;
}

std::uint16_t Font::glyphFor(char32_t character) const {
    if (cmapFormat_ == 12) {
        const std::uint32_t groups = u32(cmap_ + 12);
        std::size_t low = 0;
        std::size_t high = groups;
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2;
            const std::size_t group = cmap_ + 16 + 12 * middle;
            if (group + 12 > file_.size()) return 0;
            const std::uint32_t start = u32(group);
            const std::uint32_t end = u32(group + 4);
            if (character < start) {
                high = middle;
            } else if (character > end) {
                low = middle + 1;
            } else {
                const std::uint32_t glyph = u32(group + 8) + (character - start);
                return glyph < static_cast<std::uint32_t>(glyphCount_) ? static_cast<std::uint16_t>(glyph) : std::uint16_t{0};
            }
        }
        return 0;
    }

    if (character > 0xFFFF) return 0;
    const auto code = static_cast<std::uint16_t>(character);
    const std::size_t segments = u16(cmap_ + 6) / 2u;
    const std::size_t ends = cmap_ + 14;
    const std::size_t starts = ends + 2 * segments + 2;
    const std::size_t deltas = starts + 2 * segments;
    const std::size_t rangeOffsets = deltas + 2 * segments;
    for (std::size_t i = 0; i < segments; ++i) {
        if (u16(ends + 2 * i) < code) continue;
        const std::uint16_t start = u16(starts + 2 * i);
        if (start > code) return 0;
        const std::uint16_t delta = u16(deltas + 2 * i);
        const std::uint16_t rangeOffset = u16(rangeOffsets + 2 * i);
        std::uint16_t glyph = 0;
        if (rangeOffset == 0) {
            glyph = static_cast<std::uint16_t>(code + delta);
        } else {
            const std::uint16_t stored = u16(rangeOffsets + 2 * i + rangeOffset + 2 * static_cast<std::size_t>(code - start));
            glyph = stored == 0 ? std::uint16_t{0} : static_cast<std::uint16_t>(stored + delta);
        }
        return glyph < glyphCount_ ? glyph : std::uint16_t{0};
    }
    return 0;
}

float Font::advance(std::uint16_t glyph, float pixels) const {
    const int index = std::min<int>(glyph, metricsCount_ - 1);
    return static_cast<float>(u16(hmtx_.offset + 4 * static_cast<std::size_t>(index))) * pixels / static_cast<float>(unitsPerEm_);
}

float Font::ascent(float pixels) const { return static_cast<float>(ascender_) * pixels / static_cast<float>(unitsPerEm_); }
float Font::descent(float pixels) const { return -static_cast<float>(descender_) * pixels / static_cast<float>(unitsPerEm_); }
float Font::lineHeight(float pixels) const {
    return static_cast<float>(ascender_ - descender_ + lineGap_) * pixels / static_cast<float>(unitsPerEm_);
}

Font::Range Font::glyphData(std::uint16_t glyph) const {
    if (glyph >= glyphCount_) return {};
    const std::size_t index = glyph;
    std::size_t start = 0;
    std::size_t end = 0;
    if (longOffsets_) {
        if ((index + 2) * 4 > loca_.length) return {};
        start = u32(loca_.offset + index * 4);
        end = u32(loca_.offset + (index + 1) * 4);
    } else {
        if ((index + 2) * 2 > loca_.length) return {};
        start = std::size_t{u16(loca_.offset + index * 2)} * 2;
        end = std::size_t{u16(loca_.offset + (index + 1) * 2)} * 2;
    }
    if (end <= start || end > glyf_.length) return {};
    return {glyf_.offset + start, end - start};
}

void Font::appendSimple(const Range& data, int contours, Outline& out) const {
    const std::size_t limit = data.offset + data.length;
    std::size_t at = data.offset + 10;
    const std::size_t firstPoint = out.points.size();
    std::vector<std::size_t> ends;
    std::size_t previous = 0;
    for (int i = 0; i < contours; ++i) {
        const std::size_t end = std::size_t{u16(at)} + 1;
        if (end <= previous) return;
        ends.push_back(end);
        previous = end;
        at += 2;
    }
    const std::size_t count = ends.empty() ? 0 : ends.back();
    if (count > kMaxPoints) return;
    at += std::size_t{2} + u16(at);  // skips the hinting program

    std::vector<std::uint8_t> flags;
    flags.reserve(count);
    while (flags.size() < count) {
        if (at >= limit) return;
        const std::uint8_t flag = u8(at++);
        flags.push_back(flag);
        if ((flag & 8) != 0) {
            std::size_t repeats = u8(at++);
            while (repeats-- > 0 && flags.size() < count) flags.push_back(flag);
        }
    }

    const auto readAxis = [&](std::uint8_t shortBit, std::uint8_t sameBit, std::vector<int>& values) {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if ((flags[i] & shortBit) != 0) {
                const int delta = u8(at++);
                value += (flags[i] & sameBit) != 0 ? delta : -delta;
            } else if ((flags[i] & sameBit) == 0) {
                value += i16(at);
                at += 2;
            }
            values[i] = value;
        }
    };
    std::vector<int> xs(count);
    std::vector<int> ys(count);
    readAxis(2, 16, xs);
    readAxis(4, 32, ys);
    if (at > limit + 3) return;  // ran off the end: padding allows a little, but not this

    for (std::size_t i = 0; i < count; ++i) {
        out.points.push_back({static_cast<float>(xs[i]), static_cast<float>(ys[i]), (flags[i] & 1) != 0});
    }
    for (const std::size_t end : ends) out.contourEnds.push_back(firstPoint + end);
}

void Font::appendComposite(const Range& data, Outline& out, int depth) const {
    const std::size_t limit = data.offset + data.length;
    std::size_t at = data.offset + 10;
    constexpr std::uint16_t kWords = 0x1, kOffsets = 0x2, kScale = 0x8, kMore = 0x20, kXyScale = 0x40, kMatrix = 0x80;
    std::uint16_t flags = kMore;
    while ((flags & kMore) != 0 && at + 4 <= limit) {
        flags = u16(at);
        const std::uint16_t component = u16(at + 2);
        at += 4;
        float dx = 0.0f;
        float dy = 0.0f;
        if ((flags & kWords) != 0) {
            if ((flags & kOffsets) != 0) {
                dx = static_cast<float>(i16(at));
                dy = static_cast<float>(i16(at + 2));
            }
            at += 4;
        } else {
            if ((flags & kOffsets) != 0) {
                dx = static_cast<float>(static_cast<std::int8_t>(u8(at)));
                dy = static_cast<float>(static_cast<std::int8_t>(u8(at + 1)));
            }
            at += 2;
        }
        float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
        const auto fixed = [&](std::size_t where) { return static_cast<float>(i16(where)) / 16384.0f; };
        if ((flags & kScale) != 0) {
            a = d = fixed(at);
            at += 2;
        } else if ((flags & kXyScale) != 0) {
            a = fixed(at);
            d = fixed(at + 2);
            at += 4;
        } else if ((flags & kMatrix) != 0) {
            a = fixed(at);
            b = fixed(at + 2);
            c = fixed(at + 4);
            d = fixed(at + 6);
            at += 8;
        }
        const std::size_t first = out.points.size();
        appendOutline(component, out, depth + 1);
        for (std::size_t i = first; i < out.points.size(); ++i) {
            Point& p = out.points[i];
            const float x = p.x * a + p.y * c + dx;
            const float y = p.x * b + p.y * d + dy;
            p.x = x;
            p.y = y;
        }
    }
}

void Font::appendOutline(std::uint16_t glyph, Outline& out, int depth) const {
    if (depth > kMaxCompositeDepth || out.points.size() > kMaxPoints) return;
    const Range data = glyphData(glyph);
    if (data.length < 10) return;
    const int contours = i16(data.offset);
    if (contours >= 0) {
        appendSimple(data, contours, out);
    } else {
        appendComposite(data, out, depth);
    }
}

GlyphBitmap Font::rasterize(std::uint16_t glyph, float pixels) const {
    Outline outline;
    appendOutline(glyph, outline, 0);
    if (outline.points.empty()) return {};
    pixels = std::clamp(pixels, 1.0f, kMaxPixels);
    const float scale = pixels / static_cast<float>(unitsPerEm_);

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (Point& p : outline.points) {
        p.x *= scale;
        p.y *= -scale;  // bitmaps count downwards
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const auto left = static_cast<int>(std::floor(minX));
    const auto top = static_cast<int>(std::floor(minY));
    const int width = static_cast<int>(std::ceil(maxX)) - left;
    const int height = static_cast<int>(std::ceil(maxY)) - top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return {};

    Coverage coverage(width, height);
    const auto offsetX = static_cast<float>(left);
    const auto offsetY = static_cast<float>(top);
    const auto edge = [&](const Point& a, const Point& b) { coverage.line(a.x - offsetX, a.y - offsetY, b.x - offsetX, b.y - offsetY); };
    const auto curve = [&](const Point& a, const Point& control, const Point& b) {
        const float bend = std::abs(a.x - 2.0f * control.x + b.x) + std::abs(a.y - 2.0f * control.y + b.y);
        const int steps = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 2.0f))), 1, 24);
        Point last = a;
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float u = 1.0f - t;
            const Point next{u * u * a.x + 2.0f * u * t * control.x + t * t * b.x, u * u * a.y + 2.0f * u * t * control.y + t * t * b.y, true};
            edge(last, next);
            last = next;
        }
    };

    std::size_t begin = 0;
    for (const std::size_t end : outline.contourEnds) {
        const std::size_t count = end - begin;
        if (count >= 2 && end <= outline.points.size()) {
            const auto at = [&](std::size_t i) -> const Point& { return outline.points[begin + i % count]; };
            const auto middle = [](const Point& a, const Point& b) { return Point{(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f, true}; };
            // A contour may start between two control points, where the real starting point is implied.
            std::size_t first = 0;
            while (first < count && !at(first).onCurve) ++first;
            const bool allControls = first == count;
            Point start = allControls ? middle(at(0), at(1)) : at(first);
            if (allControls) first = 0;
            Point pen = start;
            for (std::size_t step = 1; step <= count; ++step) {
                const Point& p = at(first + step);
                if (p.onCurve) {
                    edge(pen, p);
                    pen = p;
                    continue;
                }
                const Point& following = at(first + step + 1);
                Point target = following.onCurve ? following : middle(p, following);
                if (step == count) target = start;
                curve(pen, p, target);
                pen = target;
                if (following.onCurve) ++step;
            }
            if (pen.x != start.x || pen.y != start.y) edge(pen, start);
        }
        begin = end;
    }
    return coverage.finish(left, -top);
}

char32_t nextCharacter(std::span<const char> text, std::size_t& at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned first = byte(at++);
    if (first < 0x80) return first;
    std::size_t length = 0;
    char32_t value = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        length = 1;
        value = first & 0x1Fu;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 2;
        value = first & 0x0Fu;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 3;
        value = first & 0x07u;
    } else {
        return 0xFFFD;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (at >= text.size() || (byte(at) & 0xC0u) != 0x80u) return 0xFFFD;
        value = (value << 6) | (byte(at++) & 0x3Fu);
    }
    return value;
}

}  // namespace bancada::text
