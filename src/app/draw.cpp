#include "app/draw.hpp"


#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

#include "app/text.hpp"
#include "asm/assembler.hpp"
#include "asm/mips.hpp"
#include "editor/computer16.hpp"
#include "editor/computer_view.hpp"
#include "editor/course.hpp"
#include "editor/gallery.hpp"
#include "editor/lessons.hpp"
#include "electric/units.hpp"
#include "i18n/i18n.hpp"
#include "text/truetype.hpp"

namespace bancada::app {

namespace {

using namespace bancada::editor;

struct Color {
    Uint8 r, g, b;
};

// A dark green circuit board, copper traces and one teal accent. Every colour on screen comes from here.
constexpr Color kBackground{12, 28, 24};
constexpr Color kGridDot{30, 58, 50};
constexpr Color kWireOff{130, 98, 58};
constexpr Color kWireOn{255, 190, 92};
constexpr Color kWireFloating{84, 98, 93};
constexpr Color kWireConflict{240, 84, 84};
constexpr Color kWirePreview{94, 222, 190};
constexpr Color kBusOff{48, 98, 104};
constexpr Color kBusOn{82, 208, 228};
constexpr Color kSelected{120, 190, 255};
constexpr Color kLabelFocus{255, 184, 80};  // every label with the name of the one picked
constexpr Color kBodyGate{16, 38, 33};
constexpr Color kGateLine{226, 232, 220};
constexpr Color kBodyEdge{62, 84, 78};
constexpr Color kBodyInput{30, 48, 70};
constexpr Color kBodyBox{26, 48, 60};
// The program's colour: the ROM that holds it, the editor that writes it and every button that opens it.
constexpr Color kToneProgram{255, 186, 84};
constexpr Color kBodyProgram{70, 52, 24};
Color mixed(Color a, Color b, float t);  // below
constexpr Color kBodyProbe{8, 18, 16};
constexpr Color kBodyChip{48, 40, 78};
constexpr Color kBodyClock{74, 56, 28};
constexpr Color kKnobOff{70, 40, 46};
constexpr Color kKnobOn{86, 226, 150};
constexpr Color kLedOff{86, 28, 32};
constexpr Color kLedOn{255, 70, 62};
constexpr Color kLedShine{255, 214, 208};
constexpr Color kPin{210, 206, 184};
constexpr Color kGhostGood{94, 222, 190};
constexpr Color kGhostBad{240, 84, 84};
constexpr Color kPanel{17, 24, 23};
constexpr Color kPanelRaised{25, 34, 32};
constexpr Color kPanelButton{32, 43, 41};
constexpr Color kPanelHover{44, 58, 55};
constexpr Color kPanelActive{24, 104, 88};
constexpr Color kPanelRow{28, 76, 66};
constexpr Color kRule{38, 50, 47};
constexpr Color kText{232, 238, 233};
constexpr Color kTextDim{150, 168, 160};
constexpr Color kTextOff{82, 98, 92};
constexpr Color kWarning{255, 168, 96};

constexpr float kUiText = 13.0f;

void setColor(SDL_Renderer* renderer, Color color, Uint8 alpha = 255) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
}

// Rectangles of one colour, sent in one call: this is what keeps huge boards drawable.
class Batch {
public:
    void add(float x, float y, float w, float h) { rects_.push_back({x, y, w, h}); }
    void flush(SDL_Renderer* renderer, Color color) {
        if (rects_.empty()) return;
        setColor(renderer, color);
        SDL_RenderFillRects(renderer, rects_.data(), static_cast<int>(rects_.size()));
        rects_.clear();
    }

private:
    std::vector<SDL_FRect> rects_;
};

struct View {
    const Camera& camera;
    float minX, minY, maxX, maxY;  // what's on screen, in board units

    bool sees(float left, float top, float right, float bottom) const {
        return right >= minX && left <= maxX && bottom >= minY && top <= maxY;
    }
    bool sees(const Rect& r) const {
        return sees(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom));
    }
};

void addPath(Batch& batch, const Camera& camera, const std::vector<Point>& path, float thickness) {
    const float half = thickness / 2.0f;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const Vec2 a = camera.toScreen(static_cast<float>(path[i].x), static_cast<float>(path[i].y));
        const Vec2 b = camera.toScreen(static_cast<float>(path[i + 1].x), static_cast<float>(path[i + 1].y));
        const float left = std::min(a.x, b.x) - half;
        const float top = std::min(a.y, b.y) - half;
        batch.add(left, top, std::abs(a.x - b.x) + thickness, std::abs(a.y - b.y) + thickness);
    }
}

void drawGrid(SDL_Renderer* renderer, const View& view, int width, int height) {
    const Camera& camera = view.camera;
    int step = 1;
    while (camera.cellPixels * static_cast<float>(step) < 8.0f) step *= 2;
    if (step > 32) return;
    const int startX = static_cast<int>(std::floor(view.minX / static_cast<float>(step))) * step;
    const int startY = static_cast<int>(std::floor(view.minY / static_cast<float>(step))) * step;
    Batch dots;
    const float size = camera.cellPixels >= 12.0f ? 2.0f : 1.0f;
    for (int y = startY;; y += step) {
        const float sy = camera.toScreen(0, static_cast<float>(y)).y;
        if (sy > static_cast<float>(height)) break;
        for (int x = startX;; x += step) {
            const float sx = camera.toScreen(static_cast<float>(x), 0).x;
            if (sx > static_cast<float>(width)) break;
            dots.add(sx, sy, size, size);
        }
    }
    dots.flush(renderer, kGridDot);
}

// Triangles sent in one call, each vertex with its own colour, drawn in the order they were added. The
// renderer has no anti-aliasing of its own, so every shape carries a one-pixel fringe that fades to nothing.
class Mesh {
public:
    void addFan(const std::vector<Vec2>& ring, Vec2 centre, Color color, float centreAlpha = 1.0f, float ringAlpha = 1.0f) {
        const int start = static_cast<int>(vertices_.size());
        vertices_.push_back(vertex(centre, color, centreAlpha));
        for (const Vec2& point : ring) vertices_.push_back(vertex(point, color, ringAlpha));
        const int count = static_cast<int>(ring.size());
        for (int i = 0; i < count; ++i) {
            indices_.push_back(start);
            indices_.push_back(start + 1 + i);
            indices_.push_back(start + 1 + (i + 1) % count);
        }
    }

    void addLine(Vec2 a, Vec2 b, float thickness, Color color, float opacity = 1.0f) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0f) return;
        const float tx = dx / length;
        const float ty = dy / length;
        const float core = std::max(thickness / 2.0f - 0.5f, 0.0f);
        const float outer = core + 1.0f;
        const float alpha = opacity * std::min(thickness, 1.0f);
        // Stretched a little past both ends, so that corners of a stroke close up.
        const float reach = thickness / 2.0f;
        const Vec2 from{a.x - tx * reach, a.y - ty * reach};
        const Vec2 to{b.x + tx * reach, b.y + ty * reach};
        const int start = static_cast<int>(vertices_.size());
        for (const Vec2 end : {from, to}) {
            vertices_.push_back(vertex({end.x - ty * outer, end.y + tx * outer}, color, 0.0f));
            vertices_.push_back(vertex({end.x - ty * core, end.y + tx * core}, color, alpha));
            vertices_.push_back(vertex({end.x + ty * core, end.y - tx * core}, color, alpha));
            vertices_.push_back(vertex({end.x + ty * outer, end.y - tx * outer}, color, 0.0f));
        }
        for (int strip = 0; strip < 3; ++strip) {
            for (int offset : {0, 1, 5, 0, 5, 4}) indices_.push_back(start + strip + offset);
        }
    }

    void addDisc(Vec2 centre, float radius, Color color, float alpha = 1.0f) {
        std::vector<Vec2> ring;
        std::vector<Vec2> normals;
        const int sides = std::clamp(static_cast<int>(radius * 3.0f), 10, 48);
        for (int i = 0; i < sides; ++i) {
            const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(sides);
            normals.push_back({std::cos(angle), std::sin(angle)});
            ring.push_back({centre.x + radius * normals.back().x, centre.y + radius * normals.back().y});
        }
        addShape(ring, normals, centre, color, alpha);
    }

    void addRoundRect(const SDL_FRect& r, float radius, Color color, float alpha = 1.0f) {
        std::vector<Vec2> ring;
        std::vector<Vec2> normals;
        roundOutline(r, radius, ring, normals);
        addShape(ring, normals, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, color, alpha);
    }

    void addRoundFrame(const SDL_FRect& r, float radius, float thickness, Color color) {
        std::vector<Vec2> ring;
        std::vector<Vec2> normals;
        roundOutline(r, radius, ring, normals);
        for (std::size_t i = 0; i < ring.size(); ++i) addLine(ring[i], ring[(i + 1) % ring.size()], thickness, color);
    }

    std::size_t size() const { return vertices_.size(); }

    void flush(SDL_Renderer* renderer) {
        if (indices_.empty()) return;
        SDL_RenderGeometry(renderer, nullptr, vertices_.data(), static_cast<int>(vertices_.size()), indices_.data(),
                           static_cast<int>(indices_.size()));
        vertices_.clear();
        indices_.clear();
    }

private:
    static SDL_Vertex vertex(Vec2 at, Color color, float alpha) {
        SDL_Vertex out{};
        out.position = {at.x, at.y};
        out.color = {static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                     static_cast<float>(color.b) / 255.0f, alpha};
        return out;
    }

    static void roundOutline(const SDL_FRect& r, float radius, std::vector<Vec2>& ring, std::vector<Vec2>& normals) {
        radius = std::min({radius, r.w / 2.0f, r.h / 2.0f});
        const int steps = std::clamp(static_cast<int>(radius), 2, 10);
        const std::array<Vec2, 4> corners{Vec2{r.x + r.w - radius, r.y + radius}, Vec2{r.x + r.w - radius, r.y + r.h - radius},
                                          Vec2{r.x + radius, r.y + r.h - radius}, Vec2{r.x + radius, r.y + radius}};
        for (int corner = 0; corner < 4; ++corner) {
            for (int i = 0; i <= steps; ++i) {
                const float angle = (static_cast<float>(corner) - 1.0f + static_cast<float>(i) / static_cast<float>(steps)) * 3.14159265f / 2.0f;
                const Vec2 normal{std::cos(angle), std::sin(angle)};
                normals.push_back(normal);
                ring.push_back({corners[static_cast<std::size_t>(corner)].x + radius * normal.x, corners[static_cast<std::size_t>(corner)].y + radius * normal.y});
            }
        }
    }

    // A convex shape: solid out to half a pixel inside its edge, and fading to nothing half a pixel outside.
    void addShape(const std::vector<Vec2>& ring, const std::vector<Vec2>& normals, Vec2 centre, Color color, float alpha) {
        const int start = static_cast<int>(vertices_.size());
        const int count = static_cast<int>(ring.size());
        vertices_.push_back(vertex(centre, color, alpha));
        for (std::size_t i = 0; i < ring.size(); ++i) {
            vertices_.push_back(vertex({ring[i].x - normals[i].x * 0.5f, ring[i].y - normals[i].y * 0.5f}, color, alpha));
            vertices_.push_back(vertex({ring[i].x + normals[i].x * 0.5f, ring[i].y + normals[i].y * 0.5f}, color, 0.0f));
        }
        for (int i = 0; i < count; ++i) {
            const int inner = start + 1 + 2 * i;
            const int outer = inner + 1;
            const int nextInner = start + 1 + 2 * ((i + 1) % count);
            const int nextOuter = nextInner + 1;
            for (int index : {start, inner, nextInner, inner, outer, nextOuter, inner, nextOuter, nextInner}) indices_.push_back(index);
        }
    }

    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

// Where a part is on screen: turns its own coordinates, in unscaled cells, into pixels.
struct Frame {
    Vec2 origin;
    float cell;
    std::uint8_t rotation;
    float width;
    float height;

    Vec2 place(Vec2 p) const {
        Vec2 turned = p;
        switch (rotation & 3) {
            case 1: turned = {height - p.y, p.x}; break;
            case 2: turned = {width - p.x, height - p.y}; break;
            case 3: turned = {p.y, width - p.x}; break;
            default: break;
        }
        return {origin.x + turned.x * cell, origin.y + turned.y * cell};
    }

    std::vector<Vec2> place(const std::vector<Vec2>& points) const {
        std::vector<Vec2> out;
        out.reserve(points.size());
        for (const Vec2& point : points) out.push_back(place(point));
        return out;
    }
};

Frame frameOf(const Part& part, Vec2 topLeft, float zoom) {
    return {topLeft, zoom * static_cast<float>(part.scale), part.rotation, static_cast<float>(baseWidth(part)),
            static_cast<float>(baseHeight(part))};
}

constexpr float kPi = 3.14159265f;

std::vector<Vec2> circlePoints(Vec2 centre, float radius, int sides = 20) {
    std::vector<Vec2> ring;
    ring.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        ring.push_back({centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)});
    }
    return ring;
}

void addLoop(Mesh& mesh, const std::vector<Vec2>& points, float thickness, Color color) {
    for (std::size_t i = 0; i < points.size(); ++i) mesh.addLine(points[i], points[(i + 1) % points.size()], thickness, color);
}

// A gate's symbol in its own cells, facing right, with its pins at the left and right edges.
struct GateSymbol {
    std::vector<Vec2> body;                  // a closed outline
    std::vector<std::vector<Vec2>> strokes;  // open lines: pin leads, and the XOR's second arc
    bool hasBubble = false;
    Vec2 bubble;
    Vec2 centre;       // a point the whole body can be seen from, for filling it
    Vec2 label;        // where its name goes
    float labelWidth;  // how much room the body has for it, in cells
};

// How big a name can be drawn to stay inside its symbol, or 0 if it wouldn't be readable.
float gateNameScale(const GateSymbol& symbol, const char* name, float cell) {
    const float fit = symbol.labelWidth * cell / (8.5f * static_cast<float>(std::strlen(name)));
    const float wanted = std::min(fit, cell / 11.0f);
    return wanted >= 0.72f ? wanted : 0.0f;
}

constexpr float kGateBack = 0.75f;
constexpr float kGateTip = 3.55f;
constexpr float kBubbleRadius = 0.28f;
constexpr float kBackDepth = 0.8f;   // how far an OR's hollow back reaches in

// Adds a curve's points after its first one, which is already there.
void addCurve(std::vector<Vec2>& out, Vec2 from, Vec2 control, Vec2 to) {
    constexpr int kSteps = 14;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps;
        const float u = 1.0f - t;
        out.push_back({u * u * from.x + 2 * u * t * control.x + t * t * to.x,
                       u * u * from.y + 2 * u * t * control.y + t * t * to.y});
    }
}

GateSymbol makeSymbol(PartKind kind, std::uint8_t inputs) {
    const Part part = makePart(kind, {}, 0, inputs);
    const auto height = static_cast<float>(baseHeight(part));
    const float top = 0.5f;
    const float bottom = height - 0.5f;
    const float middle = height / 2.0f;

    GateSymbol symbol;
    symbol.hasBubble = kind == PartKind::Nand || kind == PartKind::Nor || kind == PartKind::Xnor || kind == PartKind::Not;
    const float tip = symbol.hasBubble ? kGateTip - 2 * kBubbleRadius : kGateTip;
    symbol.bubble = {tip + kBubbleRadius, middle};
    symbol.centre = {1.9f, middle};
    // Inside the body at mid height, from its back to its tip, leaving a margin on both sides.
    const auto setLabel = [&](float from, float to) {
        symbol.label = {(from + to) / 2.0f, middle};
        symbol.labelWidth = (to - from) * 0.96f;
    };
    const float outputFrom = symbol.hasBubble ? tip + 2 * kBubbleRadius : tip;
    symbol.strokes.push_back({{outputFrom, middle}, {4.0f, middle}});

    // Input pins, as the board has them. reach says how far right each lead goes at a given height.
    const auto addLeads = [&](auto reach) {
        for (unsigned pin = 0; pin < part.inputs; ++pin) {
            const Point at = pinPosition(part, pin);
            const auto y = static_cast<float>(at.y);
            symbol.strokes.push_back({{0.0f, y}, {reach(y), y}});
        }
    };
    // The hollow back of an OR at a given height, from a curve whose ends are at x.
    const auto backAt = [&](float x, float y) {
        const float t = (bottom - y) / (bottom - top);
        return x + 2.0f * t * (1.0f - t) * kBackDepth;
    };
    const auto orBack = [&](float x) {
        std::vector<Vec2> curve{{x, bottom}};
        addCurve(curve, {x, bottom}, {x + kBackDepth, middle}, {x, top});
        return curve;
    };

    switch (kind) {
        case PartKind::Not:
            symbol.body = {{kGateBack, top}, {tip, middle}, {kGateBack, bottom}};
            symbol.centre = {1.5f, middle};
            setLabel(kGateBack, kGateBack + (tip - kGateBack) * 0.72f);  // the triangle narrows: stay at the wide end
            addLeads([](float) { return kGateBack; });
            break;
        case PartKind::And:
        case PartKind::Nand: {
            constexpr float kRadius = 1.5f;
            const float centre = tip - kRadius;
            const float spread = (bottom - top) / 2.0f;
            symbol.body.push_back({kGateBack, top});
            constexpr int kSteps = 20;
            for (int i = 0; i <= kSteps; ++i) {
                const float angle = -kPi / 2.0f + kPi * static_cast<float>(i) / kSteps;
                symbol.body.push_back({centre + kRadius * std::cos(angle), middle + spread * std::sin(angle)});
            }
            symbol.body.push_back({kGateBack, bottom});
            setLabel(kGateBack, tip);
            addLeads([](float) { return kGateBack; });
            break;
        }
        default: {  // the OR family
            const bool exclusive = kind == PartKind::Xor || kind == PartKind::Xnor;
            const float back = exclusive ? 0.95f : 0.6f;
            symbol.centre = {2.0f, middle};
            setLabel(back + kBackDepth / 2.0f, tip);
            symbol.body.push_back({back, top});
            addCurve(symbol.body, {back, top}, {2.2f, top - 0.05f}, {tip, middle});
            addCurve(symbol.body, {tip, middle}, {2.2f, bottom + 0.05f}, {back, bottom});
            const std::vector<Vec2> hollow = orBack(back);
            symbol.body.insert(symbol.body.end(), hollow.begin() + 1, hollow.end() - 1);
            if (exclusive) {
                symbol.strokes.push_back(orBack(0.55f));
                addLeads([&](float y) { return backAt(0.55f, y); });
            } else {
                addLeads([&](float y) { return backAt(back, y); });
            }
            break;
        }
    }
    return symbol;
}

const GateSymbol& symbolOf(PartKind kind, std::uint8_t inputs) {
    static std::map<std::pair<PartKind, std::uint8_t>, GateSymbol> symbols;
    const auto key = std::make_pair(kind, inputs);
    auto found = symbols.find(key);
    if (found == symbols.end()) found = symbols.emplace(key, makeSymbol(kind, inputs)).first;
    return found->second;
}

float strokeFor(float cell) { return std::max(1.25f, cell * 0.17f); }

void addGate(Mesh& mesh, const Part& part, const Frame& frame) {
    const GateSymbol& symbol = symbolOf(part.kind, part.inputs);
    const float thickness = strokeFor(frame.cell);

    for (const std::vector<Vec2>& stroke : symbol.strokes) {
        const std::vector<Vec2> line = frame.place(stroke);
        for (std::size_t i = 0; i + 1 < line.size(); ++i) mesh.addLine(line[i], line[i + 1], thickness, kGateLine);
    }
    const std::vector<Vec2> body = frame.place(symbol.body);
    mesh.addFan(body, frame.place(symbol.centre), kBodyGate);
    addLoop(mesh, body, thickness, kGateLine);

    if (symbol.hasBubble) {
        const std::vector<Vec2> circle = frame.place(circlePoints(symbol.bubble, kBubbleRadius, 16));
        mesh.addFan(circle, frame.place(symbol.bubble), kBodyGate);
        addLoop(mesh, circle, thickness, kGateLine);
    }
}

// A round LED with a lead going to its pin, a highlight and, when lit, a glow around it.
// A rectangle in the part's own cells, turned with the part.
void addCellRect(Mesh& mesh, const Frame& frame, float x0, float y0, float x1, float y1, Color color, float alpha = 1.0f) {
    const std::vector<Vec2> corners = frame.place(std::vector<Vec2>{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    mesh.addFan(corners, frame.place(Vec2{(x0 + x1) / 2.0f, (y0 + y1) / 2.0f}), color, alpha, alpha);
}

// Seven bars and a point on a dark face, the way a calculator draws digits: each lit by its pin.
void addDisplay7(Mesh& mesh, Mesh& glow, const Frame& frame, const std::array<bool, 8>& lit) {
    addCellRect(mesh, frame, 0.6f, 0.2f, 7.8f, 15.8f, Color{24, 20, 22});
    static constexpr float kBars[8][4] = {
        {2.9f, 1.9f, 5.9f, 2.7f},   {6.0f, 2.8f, 6.8f, 7.6f},   {6.0f, 8.4f, 6.8f, 13.2f}, {2.9f, 13.3f, 5.9f, 14.1f},
        {2.0f, 8.4f, 2.8f, 13.2f},  {2.0f, 2.8f, 2.8f, 7.6f},   {2.9f, 7.6f, 5.9f, 8.4f},  {7.0f, 13.3f, 7.6f, 14.1f},
    };
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& b = kBars[i];
        if (lit[i]) addCellRect(glow, frame, b[0] - 0.5f, b[1] - 0.5f, b[2] + 0.5f, b[3] + 0.5f, kLedOn, 0.35f);
        addCellRect(mesh, frame, b[0], b[1], b[2], b[3], lit[i] ? kLedOn : Color{70, 34, 36});
    }
}

// The LED matrix: a dot for every bit of every row. The terminal: its text, line by line, on a dark screen.
template <typename Labels>
void drawScreenBox(Mesh& mesh, Mesh& glow, Labels& labels, const Part& part, const Frame& frame, const BoardCircuit& circuit) {
    const auto width = static_cast<float>(baseWidth(part));
    const auto height = static_cast<float>(baseHeight(part));
    if (isScreenBox(part, "led_matrix")) {
        const float left = 3.0f;
        addCellRect(mesh, frame, left, 0.5f, width - 0.5f, height - 0.5f, Color{24, 20, 22});
        const unsigned columns = std::min(pinBits(part, 0), 16u);
        const float step = std::min((width - left - 1.0f) / static_cast<float>(columns), (height - 1.0f) / 8.0f);
        for (unsigned row = 0; row < 8; ++row) {
            const std::uint64_t bits = circuit.pinValue(part.id, row);
            for (unsigned column = 0; column < columns; ++column) {
                // Bit 0 on the right, the way the number is written.
                const Vec2 centre{width - 1.0f - step * (static_cast<float>(column) + 0.5f), 1.0f + step * (static_cast<float>(row) + 0.5f)};
                const bool lit = (bits >> column) & 1u;
                const Vec2 at = frame.place(centre);
                if (lit) glow.addFan(frame.place(circlePoints(centre, step * 0.7f, 16)), at, kLedOn, 0.4f, 0.0f);
                mesh.addFan(frame.place(circlePoints(centre, step * 0.36f, 12)), at, lit ? kLedOn : Color{70, 34, 36});
            }
        }
        return;
    }
    // Four lines of sixteen: a character 10 starts a new line, and only the last four lines show.
    addCellRect(mesh, frame, 5.0f, 0.6f, width - 0.6f, height - 0.6f, Color{10, 24, 16});
    const auto count = static_cast<unsigned>(circuit.pinValue(part.id, 4));
    std::vector<std::string> lines(1);
    for (unsigned i = 0; i < count; ++i) {
        const auto c = static_cast<char>(circuit.memoryWord(std::array<PartId, 1>{part.id}, i) & 0x7Fu);
        if (c == '\n' || c == '\r' || lines.back().size() >= 16) lines.emplace_back();
        if (c >= 32 && c < 127) lines.back() += c;
    }
    const std::size_t first = lines.size() > 4 ? lines.size() - 4 : 0;
    const float lineHeight = (height - 2.0f) / 4.0f;
    for (std::size_t i = first; i < lines.size(); ++i) {
        const Vec2 at = frame.place(Vec2{(5.0f + width - 0.6f) / 2.0f, 1.0f + lineHeight * (static_cast<float>(i - first) + 0.5f)});
        labels.push_back({at.x, at.y, std::clamp(frame.cell / 11.0f, 0.6f, 1.6f), lines[i] + (i + 1 == lines.size() ? "_" : ""), Color{120, 255, 160}, Face::Mono});
    }
}

// A keyboard: a few rows of keys, lit while one is down.
void addKeyboard(Mesh& mesh, const Frame& frame, bool down) {
    addCellRect(mesh, frame, 0.2f, 0.3f, 7.8f, 3.7f, Color{50, 56, 64});
    for (int row = 0; row < 3; ++row) {
        for (int key = 0; key < 7 - row; ++key) {
            const float x = 0.6f + static_cast<float>(row) * 0.4f + static_cast<float>(key) * 0.98f;
            const float y = 0.7f + static_cast<float>(row) * 0.95f;
            addCellRect(mesh, frame, x, y, x + 0.8f, y + 0.75f, down && key == 3 && row == 1 ? kKnobOn : Color{130, 140, 150});
        }
    }
}

// A push button: a square key that lights while it's held.
void addButton(Mesh& mesh, Mesh& glow, const Frame& frame, bool held) {
    addCellRect(mesh, frame, 0.15f, 0.1f, 2.85f, 1.9f, kBodyInput);
    const Vec2 centre{1.5f, 1.0f};
    const Vec2 at = frame.place(centre);
    if (held) glow.addFan(frame.place(circlePoints(centre, 1.2f, 24)), at, kKnobOn, 0.4f, 0.0f);
    mesh.addFan(frame.place(circlePoints(centre, 0.62f)), at, held ? kKnobOn : Color{120, 130, 140});
    addLoop(mesh, frame.place(circlePoints(centre, 0.62f)), std::max(1.0f, frame.cell * 0.08f), kBodyEdge);
}

void addLed(Mesh& mesh, Mesh& glow, const Frame& frame, bool on) {
    const Vec2 centre{1.75f, 1.0f};
    const float radius = 0.9f;
    const float thickness = strokeFor(frame.cell);
    mesh.addLine(frame.place({0.0f, 1.0f}), frame.place({centre.x - radius, 1.0f}), thickness, kGateLine);

    const Vec2 at = frame.place(centre);
    const std::vector<Vec2> ring = frame.place(circlePoints(centre, radius));
    if (on) glow.addFan(frame.place(circlePoints(centre, radius * 2.2f, 28)), at, kLedOn, 0.55f, 0.0f);
    mesh.addFan(ring, at, on ? kLedOn : kLedOff);
    addLoop(mesh, ring, std::max(1.0f, frame.cell * 0.1f), on ? kLedShine : kGateLine);

    // The little reflection that makes it read as a dome and not a flat disc. It stays in the same place
    // on screen when the part turns, the way light from above would.
    const float shine = frame.cell * 0.24f;
    const Vec2 highlight{at.x - frame.cell * 0.28f, at.y - frame.cell * 0.28f};
    mesh.addFan(circlePoints(highlight, shine, 10), highlight, on ? kLedShine : kLedOn, on ? 0.95f : 0.5f, 0.0f);
}

// A wire on screen: straight runs joined by small curves.
void addSmoothPath(Mesh& mesh, const Camera& camera, const std::vector<Point>& path, float thickness, Color color, float opacity) {
    std::vector<Vec2> points;
    points.reserve(path.size());
    for (const Point& p : path) points.push_back(camera.toScreen(static_cast<float>(p.x), static_cast<float>(p.y)));
    const float wanted = camera.cellPixels * 0.5f;
    const auto towards = [](Vec2 from, Vec2 to, float distance, float length) {
        return Vec2{from.x + (to.x - from.x) / length * distance, from.y + (to.y - from.y) / length * distance};
    };
    Vec2 start = points.front();
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const Vec2 corner = points[i];
        const float before = std::hypot(points[i - 1].x - corner.x, points[i - 1].y - corner.y);
        const float after = std::hypot(points[i + 1].x - corner.x, points[i + 1].y - corner.y);
        if (before <= 0.0f || after <= 0.0f) continue;
        const float radius = std::min({wanted, before / 2.0f, after / 2.0f});
        const Vec2 enter = towards(corner, points[i - 1], radius, before);
        const Vec2 leave = towards(corner, points[i + 1], radius, after);
        mesh.addLine(start, enter, thickness, color, opacity);
        Vec2 last = enter;
        constexpr int kSteps = 6;
        for (int step = 1; step <= kSteps; ++step) {
            const float t = static_cast<float>(step) / kSteps;
            const float u = 1.0f - t;
            const Vec2 next{u * u * enter.x + 2 * u * t * corner.x + t * t * leave.x, u * u * enter.y + 2 * u * t * corner.y + t * t * leave.y};
            mesh.addLine(last, next, thickness, color, opacity);
            last = next;
        }
        start = leave;
    }
    mesh.addLine(start, points.back(), thickness, color, opacity);
}

Color voltsColor(double volts) {
    if (volts < -0.05) return kBusOff;
    const auto t = static_cast<float>(std::clamp(volts / 5.0, 0.0, 1.0));
    const auto mix = [&](Uint8 a, Uint8 b) { return static_cast<Uint8>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t); };
    return {mix(kWireOff.r, kWireOn.r), mix(kWireOff.g, kWireOn.g), mix(kWireOff.b, kWireOn.b)};
}

// An electric part, drawn as on a schematic. The reading is empty in the library, where nothing runs.
void addElectric(Mesh& mesh, Mesh& glow, const Part& part, const Frame& frame, const std::optional<PartReading>& reading, bool closed) {
    const float thickness = strokeFor(frame.cell);
    const bool burnt = reading && reading->burnt;
    const Color ink = burnt ? kWarning : kGateLine;
    const auto line = [&](std::initializer_list<Vec2> points, float weight = 1.0f) {
        Vec2 last{};
        bool first = true;
        for (const Vec2 p : points) {
            const Vec2 next = frame.place(p);
            if (!first) mesh.addLine(last, next, thickness * weight, ink);
            last = next;
            first = false;
        }
    };
    const auto solid = [&](std::initializer_list<Vec2> points, Color fill) {
        std::vector<Vec2> ring;
        Vec2 centre{0.0f, 0.0f};
        for (const Vec2 p : points) {
            ring.push_back(frame.place(p));
            centre.x += ring.back().x / static_cast<float>(points.size());
            centre.y += ring.back().y / static_cast<float>(points.size());
        }
        mesh.addFan(ring, centre, fill);
        addLoop(mesh, ring, thickness, ink);
    };
    const auto arrow = [&](Vec2 from, Vec2 to) {
        const float length = std::hypot(to.x - from.x, to.y - from.y);
        const float ux = (to.x - from.x) / length;
        const float uy = (to.y - from.y) / length;
        const Vec2 back{to.x - ux * 0.42f, to.y - uy * 0.42f};
        solid({to, {back.x - uy * 0.22f, back.y + ux * 0.22f}, {back.x + uy * 0.22f, back.y - ux * 0.22f}}, ink);
    };
    const auto ring = [&](Vec2 centre, float radius, Color fill) {
        const Vec2 at = frame.place(centre);
        const std::vector<Vec2> points = circlePoints(at, radius * frame.cell, 28);
        mesh.addFan(points, at, fill);
        addLoop(mesh, points, thickness, ink);
    };
    const bool on = reading && std::string_view(reading->state) != "desligado" && std::abs(reading->amps) > 1e-6;
    switch (part.kind) {
        case PartKind::Resistor:
            line({{0, 1}, {0.7f, 1}, {0.92f, 0.55f}, {1.35f, 1.45f}, {1.78f, 0.55f}, {2.22f, 1.45f}, {2.65f, 0.55f}, {3.08f, 1.45f}, {3.3f, 1}, {4, 1}});
            break;
        case PartKind::Capacitor:
            line({{0, 1}, {1.7f, 1}});
            line({{2.3f, 1}, {4, 1}});
            line({{1.7f, 0.3f}, {1.7f, 1.7f}}, 1.7f);
            line({{2.3f, 0.3f}, {2.3f, 1.7f}}, 1.7f);
            break;
        case PartKind::Battery:
            line({{0, 1}, {1.7f, 1}});
            line({{2.3f, 1}, {4, 1}});
            line({{1.7f, 0.15f}, {1.7f, 1.85f}});
            line({{2.3f, 0.6f}, {2.3f, 1.4f}}, 2.2f);
            line({{0.85f, 0.45f}, {1.35f, 0.45f}}, 0.8f);
            line({{1.1f, 0.2f}, {1.1f, 0.7f}}, 0.8f);
            break;
        case PartKind::Diode:
        case PartKind::Light: {
            const float level = reading ? static_cast<float>(reading->light) : 0.0f;
            if (part.kind == PartKind::Light && level > 0.02f && !burnt) {
                const Vec2 at = frame.place({2.0f, 1.0f});
                glow.addFan(circlePoints(at, frame.cell * (1.2f + 1.6f * level), 28), at, kLedOn, 0.65f * level, 0.0f);
            }
            line({{0, 1}, {1.4f, 1}});
            line({{2.6f, 1}, {4, 1}});
            Color fill = kBodyGate;
            if (part.kind == PartKind::Light) fill = burnt ? Color{40, 30, 30} : level > 0.02f ? kLedOn : kLedOff;
            solid({{1.4f, 0.35f}, {1.4f, 1.65f}, {2.6f, 1.0f}}, fill);
            line({{2.6f, 0.35f}, {2.6f, 1.65f}});
            if (part.kind == PartKind::Light) {
                line({{2.0f, 0.35f}, {2.55f, -0.2f}}, 0.7f);
                line({{2.45f, 0.5f}, {3.0f, -0.05f}}, 0.7f);
            }
            break;
        }
        case PartKind::BatteryCell: {
            // A battery as it looks: a cell lying down, its + cap and silver end on the left, the flat - end on
            // the right. Turned, its boxes stay boxes, so they're placed corner to corner.
            const auto box = [&](float x0, float y0, float x1, float y1) {
                const Vec2 a = frame.place({x0, y0});
                const Vec2 b = frame.place({x1, y1});
                return SDL_FRect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
            };
            const auto mark = [&](Vec2 from, Vec2 to, Color color) { mesh.addLine(frame.place(from), frame.place(to), thickness, color); };
            line({{0, 1}, {0.38f, 1}});
            line({{3.8f, 1}, {4, 1}});
            const float round = frame.cell * 0.22f;
            const SDL_FRect body = box(0.6f, 0.34f, 3.8f, 1.66f);
            mesh.addRoundRect(box(0.36f, 0.74f, 0.64f, 1.26f), frame.cell * 0.06f, kPin);
            mesh.addRoundRect(body, round, Color{56, 78, 132});
            mesh.addRoundRect(box(0.6f, 0.34f, 1.45f, 1.66f), round, Color{196, 200, 206});
            mesh.addRoundFrame(body, round, thickness, ink);
            mesh.addLine(frame.place({1.6f, 0.56f}), frame.place({3.55f, 0.56f}), thickness, Color{120, 146, 200}, 0.7f);
            const Color dark{34, 36, 44};
            mark({0.82f, 1.0f}, {1.22f, 1.0f}, dark);
            mark({1.02f, 0.8f}, {1.02f, 1.2f}, dark);
            mark({3.2f, 1.0f}, {3.55f, 1.0f}, kText);
            break;
        }
        case PartKind::LedBulb: {
            // An LED as it looks: a red dome on a rim, standing on two metal legs. The + leg, A, is marked.
            const float level = reading ? static_cast<float>(reading->light) : 0.0f;
            const bool lit = level > 0.02f && !burnt;
            const Vec2 centre = frame.place({2.0f, 1.3f});
            if (lit) glow.addFan(circlePoints(centre, frame.cell * (1.3f + 1.7f * level), 28), centre, kLedOn, 0.7f * level, 0.0f);
            const Color metal = burnt ? kWarning : kPin;
            const auto leg = [&](std::initializer_list<Vec2> points) {
                Vec2 last{};
                bool first = true;
                for (const Vec2 p : points) {
                    const Vec2 next = frame.place(p);
                    if (!first) mesh.addLine(last, next, thickness * 1.1f, metal);
                    last = next;
                    first = false;
                }
            };
            leg({{1.6f, 2.3f}, {1.6f, 3.0f}, {0.0f, 3.0f}});
            leg({{2.4f, 2.3f}, {2.4f, 2.62f}, {2.78f, 3.0f}, {4.0f, 3.0f}});
            const Color dome = burnt ? Color{58, 48, 48} : lit ? kLedOn : Color{122, 36, 42};
            const Color rim = burnt ? Color{44, 36, 36} : Color{static_cast<Uint8>(dome.r * 3 / 4), static_cast<Uint8>(dome.g * 3 / 4), static_cast<Uint8>(dome.b * 3 / 4)};
            std::vector<Vec2> base{frame.place({1.0f, 2.02f}), frame.place({2.88f, 2.02f}), frame.place({2.88f, 2.34f}), frame.place({1.0f, 2.34f})};
            mesh.addFan(base, frame.place({1.94f, 2.18f}), rim);
            addLoop(mesh, base, thickness, ink);
            std::vector<Vec2> shell;
            for (int i = 0; i <= 14; ++i) {
                const float angle = kPi + kPi * static_cast<float>(i) / 14.0f;
                shell.push_back(frame.place({2.0f + 0.85f * std::cos(angle), 1.25f + 0.85f * std::sin(angle)}));
            }
            shell.push_back(frame.place({2.85f, 2.04f}));
            shell.push_back(frame.place({1.15f, 2.04f}));
            mesh.addFan(shell, frame.place({2.0f, 1.5f}), dome);
            addLoop(mesh, shell, thickness, ink);
            mesh.addDisc(frame.place({1.62f, 0.98f}), std::max(1.2f, frame.cell * 0.17f), kLedShine, lit ? 0.95f : 0.6f);
            const float plus = 0.2f;
            line({{0.75f - plus, 2.45f}, {0.75f + plus, 2.45f}}, 0.8f);
            line({{0.75f, 2.45f - plus}, {0.75f, 2.45f + plus}}, 0.8f);
            break;
        }
        case PartKind::Switch: {
            line({{0, 1}, {1, 1}});
            line({{3, 1}, {4, 1}});
            const Vec2 left = frame.place({1, 1});
            const Vec2 right = frame.place({3, 1});
            mesh.addDisc(left, std::max(1.6f, frame.cell * 0.14f), ink);
            mesh.addDisc(right, std::max(1.6f, frame.cell * 0.14f), ink);
            if (closed) {
                line({{1, 1}, {3, 1}}, 1.2f);
            } else {
                line({{1, 1}, {2.8f, 0.25f}}, 1.2f);
            }
            break;
        }
        case PartKind::Ground:
            line({{1, 0}, {1, 0.9f}});
            line({{0.15f, 0.9f}, {1.85f, 0.9f}});
            line({{0.45f, 1.3f}, {1.55f, 1.3f}});
            line({{0.75f, 1.7f}, {1.25f, 1.7f}});
            break;
        case PartKind::Npn:
        case PartKind::Pnp:
            ring({2.1f, 2.0f}, 1.55f, on ? Color{40, 66, 52} : kBodyGate);
            line({{0, 2}, {1.2f, 2}});
            line({{1.2f, 1.2f}, {1.2f, 2.8f}}, 1.8f);
            line({{1.2f, 1.6f}, {3, 0.75f}, {3, 0}});
            line({{1.2f, 2.4f}, {3, 3.25f}, {3, 4}});
            if (part.kind == PartKind::Npn) {
                arrow({1.2f, 2.4f}, {2.75f, 3.13f});
            } else {
                arrow({3.0f, 3.25f}, {1.45f, 2.52f});
            }
            break;
        case PartKind::Nmos:
        case PartKind::Pmos:
            ring({2.1f, 2.0f}, 1.55f, on ? Color{40, 66, 52} : kBodyGate);
            if (part.kind == PartKind::Pmos) {
                line({{0, 2}, {0.62f, 2}});
                const Vec2 bubble = frame.place({0.82f, 2.0f});
                const std::vector<Vec2> circle = circlePoints(bubble, frame.cell * 0.2f, 14);
                mesh.addFan(circle, bubble, kBodyGate);
                addLoop(mesh, circle, thickness, ink);
            } else {
                line({{0, 2}, {1.0f, 2}});
            }
            line({{1.05f, 1.2f}, {1.05f, 2.8f}}, 1.4f);
            line({{1.4f, 1.0f}, {1.4f, 3.0f}}, 1.4f);
            line({{1.4f, 1.4f}, {3, 1.4f}, {3, 0}});
            line({{1.4f, 2.6f}, {3, 2.6f}, {3, 4}});
            if (part.kind == PartKind::Nmos) {
                arrow({2.6f, 2.6f}, {1.6f, 2.6f});
            } else {
                arrow({1.5f, 2.6f}, {2.5f, 2.6f});
            }
            break;
        default: break;
    }
}

struct Label {
    float x = 0;  // the centre
    float y = 0;
    float scale = 1;
    std::string text;
    Color color = kText;
    Face face = Face::Regular;
};

std::unique_ptr<TextRenderer>& textSlot() {
    static std::unique_ptr<TextRenderer> slot;
    return slot;
}

TextRenderer& textFor(SDL_Renderer* renderer) {
    if (textSlot() == nullptr) textSlot() = std::make_unique<TextRenderer>(renderer);
    return *textSlot();
}

SDL_FColor floatColor(Color color) {
    return {static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f, static_cast<float>(color.b) / 255.0f, 1.0f};
}

// Every text passes here on its way to the screen and comes out in the chosen language. The texts are
// written in Portuguese, the catalog's keys; one put together with numbers in it is translated where
// it's put together. Monospaced text is code and numbers, and stays as it is.
std::string_view shown(Face face, std::string_view text) { return face == Face::Mono ? text : i18n::tr(text); }

// x is the left edge and y the baseline.
void write(SDL_Renderer* renderer, Face face, float size, float x, float y, std::string_view text, Color color) {
    textFor(renderer).draw(face, size, x, y, shown(face, text), floatColor(color));
}

float textWidth(SDL_Renderer* renderer, Face face, float size, std::string_view text) { return textFor(renderer).measure(face, size, shown(face, text)); }

std::vector<std::string> wrapped(SDL_Renderer* renderer, Face face, float size, std::string_view text, float width);  // below
float drawXrayLegend(SDL_Renderer* renderer, const SDL_FRect& area);  // below: the short names under the X-ray, and the room they take

// Cuts a text to fit a width, a whole character at a time, and says so with an ellipsis.
std::string fitted(SDL_Renderer* renderer, Face face, float size, std::string_view text, float width) {
    text = shown(face, text);
    if (textWidth(renderer, face, size, text) <= width) return std::string(text);
    std::size_t end = text.size();
    while (end > 0) {
        do {
            --end;
        } while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80);
        std::string shorter(text.substr(0, end));
        shorter += "\xE2\x80\xA6";
        if (textWidth(renderer, face, size, shorter) <= width) return shorter;
    }
    return {};
}

void drawLabels(SDL_Renderer* renderer, const std::vector<Label>& labels) {
    for (const Label& label : labels) {
        const float size = label.scale * kUiText;
        const float width = textWidth(renderer, label.face, size, label.text);
        write(renderer, label.face, size, label.x - width / 2.0f, label.y + size * 0.36f, label.text, label.color);
    }
}

bool inside(const SDL_FRect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

void fillRound(SDL_Renderer* renderer, const SDL_FRect& r, float radius, Color color, float alpha = 1.0f) {
    Mesh mesh;
    mesh.addRoundRect(r, radius, color, alpha);
    mesh.flush(renderer);
}

void frameRound(SDL_Renderer* renderer, const SDL_FRect& r, float radius, float thickness, Color color) {
    Mesh mesh;
    mesh.addRoundFrame(r, radius, thickness, color);
    mesh.flush(renderer);
}

void fillRect(SDL_Renderer* renderer, const SDL_FRect& r, Color color) {
    setColor(renderer, color);
    SDL_RenderFillRect(renderer, &r);
}

// Our own icons, drawn in a 24-unit square with lines and arcs.
void addIcon(Mesh& mesh, Icon icon, Vec2 centre, float size, Color color) {
    const float unit = size / 24.0f;
    const auto at = [&](float x, float y) { return Vec2{centre.x + (x - 12.0f) * unit, centre.y + (y - 12.0f) * unit}; };
    const float stroke = std::max(1.4f, size * 0.085f);
    const auto path = [&](std::initializer_list<std::pair<float, float>> points) {
        Vec2 last{};
        bool first = true;
        for (const auto& [x, y] : points) {
            const Vec2 next = at(x, y);
            if (!first) mesh.addLine(last, next, stroke, color);
            last = next;
            first = false;
        }
    };
    const auto arc = [&](float cx, float cy, float radius, float from, float to) {
        constexpr int kSteps = 18;
        Vec2 last = at(cx + radius * std::cos(from), cy + radius * std::sin(from));
        for (int i = 1; i <= kSteps; ++i) {
            const float angle = from + (to - from) * static_cast<float>(i) / kSteps;
            const Vec2 next = at(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
            mesh.addLine(last, next, stroke, color);
            last = next;
        }
    };
    const auto solid = [&](std::initializer_list<std::pair<float, float>> points) {
        std::vector<Vec2> ring;
        float sumX = 0.0f;
        float sumY = 0.0f;
        for (const auto& [x, y] : points) {
            ring.push_back(at(x, y));
            sumX += ring.back().x;
            sumY += ring.back().y;
        }
        mesh.addFan(ring, {sumX / static_cast<float>(ring.size()), sumY / static_cast<float>(ring.size())}, color);
        for (std::size_t i = 0; i < ring.size(); ++i) mesh.addLine(ring[i], ring[(i + 1) % ring.size()], 1.0f, color);
    };
    constexpr float kPi2 = 3.14159265f;
    switch (icon) {
        case Icon::Trash:
            path({{5, 7}, {19, 7}});
            path({{10, 7}, {10, 4.5f}, {14, 4.5f}, {14, 7}});
            path({{7, 7}, {8, 20}, {16, 20}, {17, 7}});
            path({{10.5f, 11}, {10.5f, 16.5f}});
            path({{13.5f, 11}, {13.5f, 16.5f}});
            break;
        case Icon::Copy:
            path({{9, 9}, {20, 9}, {20, 20}, {9, 20}, {9, 9}});
            path({{6, 15}, {4, 15}, {4, 4}, {15, 4}, {15, 6}});
            break;
        case Icon::Cut:
            arc(7, 18, 2.8f, 0, 2 * kPi2);
            arc(17, 18, 2.8f, 0, 2 * kPi2);
            path({{8.6f, 15.8f}, {17, 4}});
            path({{15.4f, 15.8f}, {7, 4}});
            break;
        case Icon::Paste:
            path({{9, 5}, {6, 5}, {6, 21}, {18, 21}, {18, 5}, {15, 5}});
            path({{9, 3.5f}, {15, 3.5f}, {15, 7}, {9, 7}, {9, 3.5f}});
            break;
        case Icon::Rotate:
            arc(12, 12, 7.5f, -1.2f, 4.2f);
            path({{14.5f, 2.5f}, {14.9f, 5.1f}, {12.4f, 6.3f}});
            break;
        case Icon::Reset:
            arc(12, 12, 7.5f, 4.34f, -1.06f + 0.0f);
            path({{9.5f, 2.5f}, {9.1f, 5.1f}, {11.6f, 6.3f}});
            break;
        case Icon::Undo:
            path({{9, 5}, {4.5f, 9.5f}, {9, 14}});
            path({{4.5f, 9.5f}, {14, 9.5f}});
            arc(14, 15, 5.5f, -kPi2 / 2.0f, kPi2 / 2.0f);
            path({{14, 20.5f}, {8, 20.5f}});
            break;
        case Icon::Redo:
            path({{15, 5}, {19.5f, 9.5f}, {15, 14}});
            path({{19.5f, 9.5f}, {10, 9.5f}});
            arc(10, 15, 5.5f, -kPi2 / 2.0f, -3.0f * kPi2 / 2.0f);
            path({{10, 20.5f}, {16, 20.5f}});
            break;
        case Icon::Play: solid({{8, 5}, {19, 12}, {8, 19}}); break;
        case Icon::Pause:
            solid({{7, 5}, {10.5f, 5}, {10.5f, 19}, {7, 19}});
            solid({{13.5f, 5}, {17, 5}, {17, 19}, {13.5f, 19}});
            break;
        case Icon::Step:
            solid({{6, 5}, {15, 12}, {6, 19}});
            path({{18, 5}, {18, 19}});
            break;
        case Icon::Plus:
            path({{12, 6}, {12, 18}});
            path({{6, 12}, {18, 12}});
            break;
        case Icon::Minus: path({{6, 12}, {18, 12}}); break;
        case Icon::ChevronRight: path({{9, 6}, {15, 12}, {9, 18}}); break;
        case Icon::ChevronDown: path({{6, 9}, {12, 15}, {18, 9}}); break;
        case Icon::Search:
            arc(10.5f, 10.5f, 6.0f, 0, 2 * kPi2);
            path({{15, 15}, {20, 20}});
            break;
        case Icon::Home:
            path({{3.5f, 11.5f}, {12, 4}, {20.5f, 11.5f}});
            path({{6.5f, 9.5f}, {6.5f, 20}, {17.5f, 20}, {17.5f, 9.5f}});
            path({{10.5f, 20}, {10.5f, 15}, {13.5f, 15}, {13.5f, 20}});
            break;
        case Icon::Save:  // a floppy disk: what "save" has looked like for forty years
            path({{4.5f, 4.5f}, {16.5f, 4.5f}, {19.5f, 7.5f}, {19.5f, 19.5f}, {4.5f, 19.5f}, {4.5f, 4.5f}});
            path({{8, 4.5f}, {8, 9}, {15, 9}, {15, 4.5f}});
            path({{8, 19.5f}, {8, 14}, {16, 14}, {16, 19.5f}});
            break;
        case Icon::Folder:
            path({{3.5f, 7}, {3.5f, 19}, {20.5f, 19}, {20.5f, 9}, {11.5f, 9}, {9.5f, 6}, {3.5f, 6}, {3.5f, 7}});
            break;
        case Icon::None: break;
    }
}

void drawIcon(SDL_Renderer* renderer, Icon icon, Vec2 centre, float size, Color color) {
    Mesh mesh;
    addIcon(mesh, icon, centre, size, color);
    mesh.flush(renderer);
}

// Lower case and without accents, for searching. Only the letters Portuguese needs are folded.
std::string folded(std::string_view text) {
    static constexpr std::string_view kFrom = "\xC3\xA1\xC3\xA0\xC3\xA2\xC3\xA3\xC3\xA9\xC3\xAA\xC3\xAD\xC3\xB3\xC3\xB4\xC3\xB5\xC3\xBA\xC3\xA7"
                                              "\xC3\x81\xC3\x80\xC3\x82\xC3\x83\xC3\x89\xC3\x8A\xC3\x8D\xC3\x93\xC3\x94\xC3\x95\xC3\x9A\xC3\x87";
    static constexpr std::string_view kTo = "aaaaeeiooouc" "aaaaeeiooouc";
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        bool replaced = false;
        if (static_cast<unsigned char>(text[i]) == 0xC3 && i + 1 < text.size()) {
            for (std::size_t k = 0; k < kTo.size(); ++k) {
                if (kFrom[k * 2 + 1] == text[i + 1]) {
                    out += kTo[k];
                    ++i;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    }
    return out;
}

// Whether a part's name has what was typed in it, in Portuguese or in the language on screen.
bool named(std::string_view name, const std::string& wanted) {
    return folded(name).find(wanted) != std::string::npos || folded(i18n::tr(name)).find(wanted) != std::string::npos;
}

}  // namespace

void releaseDrawing() { textSlot().reset(); }

std::vector<PaletteGroup> paletteGroups(const Session& session, const std::string& filter, const LibraryLimit* limit) {
    static const std::vector<PaletteGroup> library = [] {
        const auto item = [](PartKind kind, BoxId box = kNoBox) {
            PaletteItem out;
            out.kind = kind;
            out.box = box;
            out.name = partName(makePart(kind, {}, 0, 0, 1, box));
            return out;
        };
        std::vector<PaletteGroup> all;
        // Electricity first, the battery at its head: everything the game teaches starts there.
        all.push_back({"Eletrônica",
                       {item(PartKind::BatteryCell), item(PartKind::Battery), item(PartKind::Ground), item(PartKind::Resistor),
                        item(PartKind::LedBulb), item(PartKind::Light),
                        item(PartKind::Diode),
                        item(PartKind::Switch), item(PartKind::Capacitor), item(PartKind::Npn), item(PartKind::Pnp), item(PartKind::Nmos),
                        item(PartKind::Pmos)}});
        all.push_back({"Portas", {item(PartKind::And), item(PartKind::Or), item(PartKind::Xor), item(PartKind::Nand),
                                  item(PartKind::Nor), item(PartKind::Xnor), item(PartKind::Not)}});
        all.push_back({"Entrada e saída", {item(PartKind::Toggle), item(PartKind::Button), item(PartKind::Keyboard), item(PartKind::Clock),
                                           item(PartKind::Led), item(PartKind::Probe), item(PartKind::Display7)}});
        all.push_back({"Fios", {item(PartKind::Tunnel), item(PartKind::Splitter), item(PartKind::Joiner), item(PartKind::Constant)}});
        for (BoxId id = 1; id <= boxCount(); ++id) {
            const std::string group = boxSpec(id).group;
            auto found = std::find_if(all.begin(), all.end(), [&](const PaletteGroup& g) { return g.name == group; });
            if (found == all.end()) {
                all.push_back({group, {}});
                found = all.end() - 1;
            }
            found->items.push_back(item(PartKind::Box, id));
        }
        const auto example = [](const char* name, Example which) {
            PaletteItem out;
            out.kind = PartKind::Chip;
            out.name = name;
            out.example = which;
            return out;
        };
        all.push_back({"Exemplos",
                       {example("LED e resistor", Example::LedCircuit), example("NAND de transistores", Example::TransistorNand),
                        example("Pisca-pisca", Example::Blinker), example("Computador pronto", Example::Computer), example(kCpuName, Example::Cpu),
                        example("Ver todos os exemplos", Example::Gallery)}});
        return all;
    }();

    if (limit != nullptr) {
        std::vector<PaletteGroup> groups;
        for (const PaletteGroup& group : library) {
            PaletteGroup kept{group.name, {}};
            for (const PaletteItem& item : group.items) {
                if (item.example != Example::None) continue;
                const bool allowed = item.kind == PartKind::Box
                                         ? std::find(limit->boxes.begin(), limit->boxes.end(), item.box) != limit->boxes.end()
                                         : std::find(limit->kinds.begin(), limit->kinds.end(), item.kind) != limit->kinds.end();
                const bool matches = filter.empty() || named(item.name, folded(filter));
                if (allowed && matches) kept.items.push_back(item);
            }
            if (!kept.items.empty() || !filter.empty()) groups.push_back(std::move(kept));
        }
        // A search keeps the same folders as without one, so empty ones stay; without a search they go.
        if (!filter.empty()) {
            std::vector<PaletteGroup> unfiltered = paletteGroups(session, {}, limit);
            std::erase_if(groups, [&](const PaletteGroup& g) {
                return std::none_of(unfiltered.begin(), unfiltered.end(), [&](const PaletteGroup& u) { return u.name == g.name; });
            });
        }
        return groups;
    }

    std::vector<PaletteGroup> groups = library;
    PaletteGroup project{"Circuitos do projeto", {}};
    for (const CircuitEntry& entry : session.circuits()) {
        PaletteItem row;
        row.kind = PartKind::Chip;
        row.chip = entry.id;
        row.name = entry.name;
        row.circuit = true;
        project.items.push_back(std::move(row));
    }
    PaletteItem add;
    add.name = "+ Novo chip";
    add.newChip = true;
    project.items.push_back(std::move(add));
    groups.push_back(std::move(project));
    if (!filter.empty()) {
        const std::string wanted = folded(filter);
        for (PaletteGroup& group : groups) {
            std::erase_if(group.items, [&](const PaletteItem& item) { return !named(item.name, wanted); });
        }
    }
    return groups;
}

SDL_FRect paletteFolderRect(int group) {
    constexpr float kLeft = 10.0f;
    constexpr float kWidth = (kPanelWidth - 2.0f * kLeft - (kFolderColumns - 1) * kFolderGap) / kFolderColumns;
    const int column = group % kFolderColumns;
    const int row = group / kFolderColumns;
    return {kLeft + static_cast<float>(column) * (kWidth + kFolderGap), kPaletteTop + static_cast<float>(row) * (kFolderHeight + kFolderGap), kWidth,
            kFolderHeight};
}

float paletteListTop(const std::vector<PaletteGroup>& groups) {
    const int rows = (static_cast<int>(groups.size()) + kFolderColumns - 1) / kFolderColumns;
    return kPaletteTop + static_cast<float>(rows) * (kFolderHeight + kFolderGap) + 8.0f;
}

std::vector<PaletteHit> paletteShown(const std::vector<PaletteGroup>& groups, const PaletteView& view) {
    std::vector<PaletteHit> shown;
    for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
        if (view.filter.empty() && g != view.folder) continue;
        for (int i = 0; i < static_cast<int>(groups[static_cast<std::size_t>(g)].items.size()); ++i) shown.push_back({g, i});
    }
    return shown;
}

namespace {

SDL_FRect tileAt(const std::vector<PaletteGroup>& groups, const PaletteView& view, std::size_t index) {
    const auto column = static_cast<float>(index % 2);
    const auto row = static_cast<float>(index / 2);
    return {10.0f + column * (kTileWidth + kTileGap), paletteListTop(groups) + row * (kTileHeight + kTileGap) - view.scroll, kTileWidth, kTileHeight};
}

}  // namespace

SDL_FRect paletteTileRect(const std::vector<PaletteGroup>& groups, const PaletteView& view, int group, int item) {
    const std::vector<PaletteHit> shown = paletteShown(groups, view);
    for (std::size_t k = 0; k < shown.size(); ++k) {
        if (shown[k].group == group && shown[k].item == item) return tileAt(groups, view, k);
    }
    return {0, 0, 0, 0};
}

float paletteContentHeight(const std::vector<PaletteGroup>& groups, const PaletteView& view) {
    const std::size_t rows = (paletteShown(groups, view).size() + 1) / 2;
    return static_cast<float>(rows) * (kTileHeight + kTileGap);
}

PaletteHit paletteAt(const std::vector<PaletteGroup>& groups, const PaletteView& view, float x, float y, int height) {
    for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
        if (inside(paletteFolderRect(g), x, y)) return {g, -1};
    }
    if (y < paletteListTop(groups) || y > static_cast<float>(height) - 46.0f) return {};
    const std::vector<PaletteHit> shown = paletteShown(groups, view);
    for (std::size_t k = 0; k < shown.size(); ++k) {
        if (inside(tileAt(groups, view, k), x, y)) return shown[k];
    }
    return {};
}

float toolbarButtonX(int index) {
    float x = kPanelWidth + 10.0f;
    for (int i = 0; i < index; ++i) x += kToolbar[i].width + 4.0f + kToolbar[i].gapAfter;
    return x;
}

int toolbarAt(float x, float y) {
    if (y < 6.0f || y > kToolbarHeight - 4.0f) return -1;
    for (int i = 0; i < static_cast<int>(std::size(kToolbar)); ++i) {
        const float left = toolbarButtonX(i);
        if (x >= left && x < left + kToolbar[i].width) return i;
    }
    return -1;
}

namespace {

constexpr float kTableRowHeight = 18.0f;
constexpr float kInspectorTop = 36.0f;
constexpr float kInspectorRow = 30.0f;

float inputColumn(const TruthTable& table, std::size_t i) {
    return std::max(24.0f, 8.0f * static_cast<float>(hexDigits(table.inputBits[i])) + 12.0f);
}

float outputColumn(const TruthTable& table, std::size_t i) {
    return std::max(24.0f, 8.0f * static_cast<float>(hexDigits(table.outputBits[i])) + 12.0f);
}

float tableTop(const Session& session) {
    const std::vector<PanelControl> controls = panelControls(session, 0);
    if (!session.inspected()) return 0.0f;
    float bottom = 44.0f;
    for (const PanelControl& control : controls) bottom = std::max(bottom, control.rect.y + control.rect.h + 12.0f);
    return bottom;
}

}  // namespace

float rightPanelWidth(const Session& session) {
    const TruthTable& table = session.truthTable();
    if (table.status != TruthTable::Status::Ready) return 224.0f;
    float columns = 48.0f;
    for (std::size_t i = 0; i < table.inputBits.size(); ++i) columns += inputColumn(table, i);
    for (std::size_t i = 0; i < table.outputBits.size(); ++i) columns += outputColumn(table, i);
    return std::clamp(columns, 224.0f, 470.0f);
}

std::vector<PanelControl> panelControls(const Session& session, float left) {
    std::vector<PanelControl> controls;
    const std::optional<Inspected> shown = session.inspected();
    if (!shown) return controls;
    const Part& part = shown->part;
    float y = kInspectorTop;
    const auto stepper = [&](PanelButton down, PanelButton up) {
        controls.push_back({down, {left + 104.0f, y, 28.0f, 24.0f}});
        controls.push_back({up, {left + 168.0f, y, 28.0f, 24.0f}});
        y += kInspectorRow;
    };
    if (minInputs(part.kind) != maxInputs(part.kind)) stepper(PanelButton::InputsDown, PanelButton::InputsUp);
    if (hasWidth(part)) stepper(PanelButton::BitsDown, PanelButton::BitsUp);
    if (hasAddress(part)) stepper(PanelButton::AddressDown, PanelButton::AddressUp);
    stepper(PanelButton::SizeDown, PanelButton::SizeUp);
    if (!shown->placing) {
        const auto action = [&](PanelButton button) {
            controls.push_back({button, {left + 14.0f, y, 182.0f, 24.0f}});
            y += kInspectorRow;
        };
        if (part.kind == PartKind::Toggle || isOutputPart(part.kind) || part.kind == PartKind::Tunnel || isLed(part.kind)) {
            action(PanelButton::EditName);
        }
        if (part.kind == PartKind::Constant || valueQuantity(part.kind)) action(PanelButton::EditValue);
        if (part.kind == PartKind::Box && boxSpec(part.box).hasContents) {
            action(PanelButton::EditProgram);
            action(PanelButton::EditContents);
        }
        if (part.kind == PartKind::Chip) action(PanelButton::OpenChip);
        if (canLookInside(part)) action(PanelButton::LookInside);
    }
    return controls;
}

float boardTopFor(bool strip) { return kToolbarHeight + (strip ? kLessonStripHeight : 0.0f); }

std::vector<TrailPiece> trailPieces(SDL_Renderer* renderer, const Session& session, float left, float top) {
    std::vector<TrailPiece> pieces;
    if (session.currentCircuit() == 0) return pieces;
    float x = left + 12.0f;
    const float y = top + 10.0f;
    pieces.push_back({-1, {x, y, textWidth(renderer, Face::Medium, kUiText, "Voltar") + 40.0f, 30.0f}});
    x += pieces.back().rect.w + 12.0f;
    const std::vector<CircuitEntry> trail = session.trail();
    for (std::size_t i = 0; i < trail.size(); ++i) {
        const float w = textWidth(renderer, Face::Medium, kUiText, trail[i].name) + 16.0f;
        pieces.push_back({static_cast<int>(i), {x, y, w, 30.0f}});
        x += w + 18.0f;
    }
    return pieces;
}

// A part opens when the gallery shows how it's made, down to gates; a splitter or a joiner shows its bits.
bool canLookInside(const Part& part) { return isBundle(part.kind) || galleryInsideOf(part).item != nullptr; }

SDL_FRect panelButtonRect(const Session& session, PanelButton button, float panelLeft) {
    for (const PanelControl& control : panelControls(session, panelLeft)) {
        if (control.button == button) return control.rect;
    }
    return {};
}

PanelButton rightPanelAt(const Session& session, float x, float y, int width) {
    const float left = static_cast<float>(width) - rightPanelWidth(session);
    for (const PanelControl& control : panelControls(session, left)) {
        const SDL_FRect& r = control.rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return control.button;
    }
    return PanelButton::None;
}

int truthRowsVisible(const Session& session, int height) {
    const float room = static_cast<float>(height) - tableTop(session) - 76.0f - 40.0f;
    return std::max(1, static_cast<int>(room / kTableRowHeight));
}

namespace {

// How much current each electric wire carries, worked out from what goes in and out at the pins. A net's
// wires form a tree, so peeling it from the leaves inwards gives every wire's share. Ground takes the rest.
std::unordered_map<WireId, double> wireCurrents(const Board& board, const BoardCircuit& circuit) {
    std::unordered_map<WireId, double> out;
    struct Edge {
        WireId wire;
        int a;
        int b;
        bool alive = true;
    };
    std::vector<Edge> edges;
    std::vector<double> inject;
    std::vector<char> absorbs;
    std::vector<std::vector<std::size_t>> touching;
    std::unordered_map<std::uint64_t, int> index;
    const auto vertexOf = [&](const Terminal& t) {
        const std::uint64_t key = t.kind == Terminal::Kind::Pin ? (std::uint64_t{t.id} << 8 | t.pin) : (std::uint64_t{1} << 62 | t.id);
        const auto [found, isNew] = index.emplace(key, static_cast<int>(inject.size()));
        if (isNew) {
            double amps = 0.0;
            char absorbing = 0;
            if (t.kind == Terminal::Kind::Pin) {
                const Part* part = board.part(t.id);
                if (part != nullptr && part->kind == PartKind::Ground) {
                    absorbing = 1;
                } else {
                    amps = circuit.pinAmps(t.id, t.pin);
                }
            }
            inject.push_back(amps);
            absorbs.push_back(absorbing);
            touching.emplace_back();
        }
        return found->second;
    };
    const auto join = [&](WireId wire, int a, int b) {
        touching[static_cast<std::size_t>(a)].push_back(edges.size());
        touching[static_cast<std::size_t>(b)].push_back(edges.size());
        edges.push_back({wire, a, b});
    };
    for (const Wire& wire : board.wires()) {
        if (!circuit.wireVolts(wire.id)) continue;
        join(wire.id, vertexOf(wire.from), vertexOf(wire.to));
    }
    std::unordered_map<std::string, int> firstLabel;
    for (const Part& part : board.parts()) {
        if (part.kind != PartKind::Tunnel) continue;
        const auto found = index.find(std::uint64_t{part.id} << 8);
        if (found == index.end()) continue;
        const auto [first, isNew] = firstLabel.emplace(part.label, found->second);
        if (!isNew) join(0, first->second, found->second);
    }
    std::vector<std::size_t> degree;
    for (const auto& list : touching) degree.push_back(list.size());
    std::vector<int> leaves;
    for (std::size_t v = 0; v < degree.size(); ++v) {
        if (degree[v] == 1 && absorbs[v] == 0) leaves.push_back(static_cast<int>(v));
    }
    while (!leaves.empty()) {
        const auto v = static_cast<std::size_t>(leaves.back());
        leaves.pop_back();
        if (degree[v] != 1) continue;
        for (const std::size_t e : touching[v]) {
            Edge& edge = edges[e];
            if (!edge.alive) continue;
            edge.alive = false;
            const auto other = static_cast<std::size_t>(edge.a == static_cast<int>(v) ? edge.b : edge.a);
            if (edge.wire != 0) out[edge.wire] = edge.a == static_cast<int>(v) ? inject[v] : -inject[v];
            inject[other] += inject[v];
            --degree[v];
            --degree[other];
            if (degree[other] == 1 && absorbs[other] == 0) leaves.push_back(static_cast<int>(other));
            break;
        }
    }
    return out;
}

}  // namespace

// The label picked on its own, if a label is all that's selected: the board then shows every label with
// its name, and the wires of that signal, so the player sees where it goes on the whole board.
const Part* focusedLabel(const Session& session) {
    const std::vector<PartId>& selected = session.selectedParts();
    if (selected.size() != 1 || !session.selectedWires().empty()) return nullptr;
    const Part* part = session.board().part(selected[0]);
    return part != nullptr && part->kind == PartKind::Tunnel ? part : nullptr;
}

std::string describeReading(const Part& part, const PartReading& r) {
    using electric::formatSi;
    using i18n::format;
    using i18n::tr;
    std::string name(tr(partName(part)));
    if (const std::optional<electric::Quantity> quantity = valueQuantity(part.kind)) name += " " + electric::formatQuantity(part.value, *quantity);
    std::string text = name + ": ";
    switch (part.kind) {
        case PartKind::Ground: return std::string(tr("Terra: 0 V, a referência de todas as tensões"));
        case PartKind::Battery:
        case PartKind::BatteryCell: text += format("fornece {}, {}", {formatSi(r.amps, "A"), formatSi(r.power, "W")}); break;
        case PartKind::Light:
        case PartKind::LedBulb:
            text += format("{}, {}, brilho {}%", {formatSi(r.volts, "V"), formatSi(r.amps, "A"), std::to_string(static_cast<int>(std::lround(r.light * 100)))});
            break;
        case PartKind::Switch: text = format("Chave {}: {}", {tr(r.state), formatSi(std::abs(r.amps), "A")}); break;
        case PartKind::Npn:
        case PartKind::Pnp:
            text += format("{}. Base-emissor {}, base {}, coletor {}", {tr(r.state), formatSi(r.volts, "V"), formatSi(r.control, "A"), formatSi(r.amps, "A")});
            break;
        case PartKind::Nmos:
        case PartKind::Pmos: text += format("{}. Porta-fonte {}, dreno {}", {tr(r.state), formatSi(r.volts, "V"), formatSi(r.amps, "A")}); break;
        case PartKind::Capacitor: text += formatSi(r.volts, "V") + ", " + formatSi(r.amps, "A"); break;
        default: text += formatSi(r.volts, "V") + ", " + formatSi(r.amps, "A") + ", " + formatSi(r.power, "W"); break;
    }
    if (r.burnt) text += tr(isBattery(part.kind) ? ". Curto-circuito!" : ". Corrente demais: um de verdade queimaria.");
    return text;
}

void drawBoard(SDL_Renderer* renderer, const Session& session, const Camera& camera, int width, int height) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);

    const Vec2 topLeft = camera.toBoard(0, 0);
    const Vec2 bottomRight = camera.toBoard(static_cast<float>(width), static_cast<float>(height));
    const View view{camera, topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    drawGrid(renderer, view, width, height);

    const Board& board = session.board();
    const BoardCircuit& circuit = session.circuit();
    const float zoom = camera.cellPixels;
    const float wireWidth = std::max(1.0f, zoom / 5.0f);
    const bool smooth = zoom >= 5.0f;

    Batch wiresOff, wiresOn, wiresFloating, wiresConflict, wiresSelected, busesOff, busesOn;
    const float busWidth = std::max(2.0f, zoom / 3.0f);

    // The net under the mouse lights up, and everything else steps back.
    std::optional<std::uint32_t> hoverNet;
    if (session.mode() == Mode::Idle) {
        const Drop over = session.hovered();
        if (over.kind == Drop::Kind::Pin) hoverNet = circuit.netOfPin(over.id, over.pin);
        if (over.kind == Drop::Kind::Wire) hoverNet = circuit.netOfWire(over.id);
        if (over.kind == Drop::Kind::Joint) {
            if (const Wire* wire = board.wireAt(over.at)) hoverNet = circuit.netOfWire(wire->id);
        }
    }
    const Part* focus = focusedLabel(session);
    const std::optional<std::uint32_t> focusNet = focus != nullptr ? circuit.netOfPin(focus->id, 0) : std::nullopt;
    Mesh wireHalos;
    Mesh wireLines;
    const std::vector<WireId>& selectedWires = session.selectedWires();
    for (const Wire& wire : board.wires()) {
        float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
        for (const Point& p : wire.path) {
            left = std::min(left, static_cast<float>(p.x));
            right = std::max(right, static_cast<float>(p.x));
            top = std::min(top, static_cast<float>(p.y));
            bottom = std::max(bottom, static_cast<float>(p.y));
        }
        if (!view.sees(left, top, right, bottom)) continue;
        const bool bus = circuit.wireBits(wire.id) > 1;
        Batch* target = bus ? &busesOff : &wiresOff;
        Color color = bus ? kBusOff : kWireOff;
        if (circuit.wireConflicted(wire.id)) {
            target = &wiresConflict;
            color = kWireConflict;
        } else if (circuit.wireOn(wire.id)) {
            target = bus ? &busesOn : &wiresOn;
            color = bus ? kBusOn : kWireOn;
        } else if (circuit.wireFloating(wire.id)) {
            target = &wiresFloating;
            color = kWireFloating;
        }
        if (const std::optional<double> volts = circuit.wireVolts(wire.id)) color = voltsColor(*volts);
        if (smooth) {
            const std::optional<std::uint32_t> net = circuit.netOfWire(wire.id);
            const bool lit = hoverNet && net == hoverNet;
            const bool focused = focusNet && net == focusNet;
            const float thickness = bus ? busWidth : wireWidth;
            if (focused) addSmoothPath(wireHalos, camera, wire.path, thickness + 6.0f, kLabelFocus, 0.55f);
            if (lit) addSmoothPath(wireHalos, camera, wire.path, thickness + 5.0f, kWirePreview, 0.45f);
            if (!selectedWires.empty() && std::find(selectedWires.begin(), selectedWires.end(), wire.id) != selectedWires.end()) {
                addSmoothPath(wireHalos, camera, wire.path, thickness + 4.0f, kSelected, 1.0f);
            }
            const bool dimmed = (hoverNet && !lit) || (focusNet && !hoverNet && !focused);
            addSmoothPath(wireLines, camera, wire.path, thickness, color, dimmed ? 0.32f : 1.0f);
            continue;
        }
        if (!selectedWires.empty() && std::find(selectedWires.begin(), selectedWires.end(), wire.id) != selectedWires.end()) {
            addPath(wiresSelected, camera, wire.path, wireWidth + 4.0f);
        }
        addPath(*target, camera, wire.path, bus ? busWidth : wireWidth);
    }
    wireHalos.flush(renderer);
    wireLines.flush(renderer);
    if (smooth && circuit.hasAnalog()) {
        // Current: dots moving along each wire, faster as it grows, the way it really flows.
        const std::unordered_map<WireId, double> amps = wireCurrents(board, circuit);
        const float now = static_cast<float>(SDL_GetTicks()) / 1000.0f;
        Mesh flow;
        for (const Wire& wire : board.wires()) {
            const auto found = amps.find(wire.id);
            if (found == amps.end() || std::abs(found->second) < 1e-6) continue;
            const float speed = std::clamp(18.0f * static_cast<float>(std::log10(std::abs(found->second) / 1e-6)), 8.0f, 110.0f);
            constexpr float kSpacing = 18.0f;
            std::vector<Vec2> points;
            for (const Point& p : wire.path) points.push_back(camera.toScreen(static_cast<float>(p.x), static_cast<float>(p.y)));
            if (found->second < 0.0) std::reverse(points.begin(), points.end());
            float travelled = std::fmod(now * speed, kSpacing);
            float start = 0.0f;
            for (std::size_t i = 0; i + 1 < points.size(); ++i) {
                const float length = std::hypot(points[i + 1].x - points[i].x, points[i + 1].y - points[i].y);
                while (travelled < start + length) {
                    const float t = (travelled - start) / length;
                    flow.addDisc({points[i].x + (points[i + 1].x - points[i].x) * t, points[i].y + (points[i + 1].y - points[i].y) * t},
                                 std::max(1.6f, zoom * 0.11f), kLedShine, 0.85f);
                    travelled += kSpacing;
                }
                start += length;
            }
        }
        flow.flush(renderer);
    }
    wiresSelected.flush(renderer, kSelected);
    busesOff.flush(renderer, kBusOff);
    busesOn.flush(renderer, kBusOn);
    wiresOff.flush(renderer, kWireOff);
    wiresFloating.flush(renderer, kWireFloating);
    wiresOn.flush(renderer, kWireOn);
    wiresConflict.flush(renderer, kWireConflict);

    // Dots where wires actually join. A crossing with no dot is not a connection.
    if (zoom >= 3.0f) {
        Batch dots;
        Mesh roundDots;
        const float size = std::max(3.0f, wireWidth * 2.4f);
        for (const Joint& joint : board.joints()) {
            const auto x = static_cast<float>(joint.position.x);
            const auto y = static_cast<float>(joint.position.y);
            if (!view.sees(x, y, x, y)) continue;
            const Vec2 at = camera.toScreen(x, y);
            if (smooth) {
                roundDots.addDisc(at, size / 2.0f, kPin);
            } else {
                dots.add(at.x - size / 2, at.y - size / 2, size, size);
            }
        }
        dots.flush(renderer, kPin);
        roundDots.flush(renderer);
    }

    Batch edges, bodiesGate, bodiesInput, bodiesBox, bodiesProgram, bodiesChip, bodiesClock, bodiesProbe, knobsOff, knobsOn, pins, selection, focusFrames;
    Mesh shapes;
    Mesh glow;
    Mesh roundBodies;
    Mesh roundTops;
    std::vector<Label> labels;
    const bool rounded = zoom >= 6.0f;
    const std::vector<PartId>& selectedParts = session.selectedParts();
    const std::unordered_map<PartId, std::string>& names = session.truthTable().labels;
    const float inset = zoom >= 6.0f ? 1.0f : 0.0f;
    const bool details = zoom >= 3.0f;
    const bool showPins = zoom >= 5.0f;
    const bool showSymbols = zoom >= 4.0f;  // further out they'd be mush: everything becomes plain blocks

    for (const Part& part : board.parts()) {
        const Rect bounds = boundsOf(part);
        if (!view.sees(bounds)) continue;
        const Vec2 at = camera.toScreen(static_cast<float>(bounds.left), static_cast<float>(bounds.top));
        const float w = static_cast<float>(bounds.right - bounds.left) * zoom;
        const float h = static_cast<float>(bounds.bottom - bounds.top) * zoom;
        if (focus != nullptr && part.kind == PartKind::Tunnel && part.id != focus->id && part.label == focus->label) {
            if (rounded) {
                roundBodies.addRoundFrame({at.x - 4, at.y - 4, w + 8, h + 8}, 8.0f, 2.5f, kLabelFocus);
            } else {
                focusFrames.add(at.x - 3, at.y - 3, w + 6, h + 6);
            }
        }
        if (!selectedParts.empty() && std::find(selectedParts.begin(), selectedParts.end(), part.id) != selectedParts.end()) {
            if (rounded) {
                roundBodies.addRoundFrame({at.x - 3, at.y - 3, w + 6, h + 6}, 8.0f, 2.0f, kSelected);
            } else {
                selection.add(at.x - 3, at.y - 3, w + 6, h + 6);
            }
        }
        const Frame frame = frameOf(part, at, zoom);
        const bool gate = isGate(part.kind);

        if (gate && showSymbols) {
            addGate(shapes, part, frame);
            const GateSymbol& symbol = symbolOf(part.kind, part.inputs);
            const float nameScale = gateNameScale(symbol, partName(part), frame.cell);
            if (nameScale > 0.0f) {
                const Vec2 centre = frame.place(symbol.label);
                labels.push_back({centre.x, centre.y, nameScale, partName(part), kGateLine});
            }
        } else if (part.kind == PartKind::Led && showSymbols) {
            addLed(shapes, glow, frame, circuit.outputOn(part.id));
        } else if (part.kind == PartKind::Display7 && showSymbols) {
            std::array<bool, 8> lit{};
            for (unsigned pin = 0; pin < 8; ++pin) lit[pin] = circuit.pinValue(part.id, pin) != 0;
            addDisplay7(shapes, glow, frame, lit);
        } else if (part.kind == PartKind::Keyboard && showSymbols) {
            addKeyboard(shapes, frame, (session.toggleValue(part.id) & 0x80u) != 0);
        } else if (part.kind == PartKind::Button && showSymbols) {
            addButton(shapes, glow, frame, session.toggleValue(part.id) != 0);
        } else if (isElectric(part.kind) && showSymbols) {
            const std::optional<PartReading> reading = circuit.reading(part.id);
            addElectric(shapes, glow, part, frame, reading, session.toggleOn(part.id));
            if (zoom >= 5.0f) {
                if (const std::optional<electric::Quantity> quantity = valueQuantity(part.kind)) {
                    labels.push_back({at.x + w * 0.5f, at.y - 7.0f, 0.82f, electric::formatQuantity(part.value, *quantity), kTextDim});
                }
                if (reading && reading->burnt) labels.push_back({at.x + w * 0.5f, at.y + h + 8.0f, 0.85f, isBattery(part.kind) ? "curto!" : "queimou", kWarning});
            }
        } else if (part.kind == PartKind::Tunnel && zoom >= 3.0f) {
            // A tag pointing at its pin.
            const auto length = static_cast<float>(baseWidth(part));
            const std::vector<Vec2> tag = frame.place({{0.25f, 0.2f}, {length - 1.0f, 0.2f}, {length, 1.0f}, {length - 1.0f, 1.8f}, {0.25f, 1.8f}});
            const bool lit = hoverNet && circuit.netOfPin(part.id, 0) == hoverNet;
            shapes.addFan(tag, frame.place({length / 2.0f, 1.0f}), lit ? kPanelActive : kPanelRaised);
            addLoop(shapes, tag, 1.2f, lit ? kWirePreview : kBodyEdge);
            if (zoom >= 4.5f) labels.push_back({at.x + w * 0.5f, at.y + h * 0.5f, std::clamp(frame.cell / 13.0f, 0.72f, 1.4f), part.label, lit ? kText : kWirePreview});
        } else if (rounded && !isBundle(part.kind)) {
            const bool pill = part.kind == PartKind::Toggle && part.width == 1;
            const float radius = pill ? std::min(w, h) / 2.0f : std::min(frame.cell * 0.55f, 9.0f);
            Color fill = kBodyProbe;
            if (part.kind == PartKind::Toggle) fill = kBodyInput;
            if (part.kind == PartKind::Box) fill = kBodyBox;
            const bool rom = part.kind == PartKind::Box && part.box == boxFromKey("rom");
            if (rom) fill = kBodyProgram;
            if (part.kind == PartKind::Chip) fill = kBodyChip;
            if (part.kind == PartKind::Clock) fill = kBodyClock;
            if (gate) fill = kBodyGate;
            const SDL_FRect r{at.x, at.y, w, h};
            roundBodies.addRoundRect(r, radius, fill);
            roundBodies.addRoundFrame(r, radius, rom ? 1.6f : 1.0f, rom ? kToneProgram : kBodyEdge);
            if (part.kind == PartKind::Chip) roundBodies.addDisc(frame.place({static_cast<float>(baseWidth(part)) / 2.0f, 0.0f}), frame.cell * 0.42f, kBackground);
        } else {
            if (inset > 0) edges.add(at.x, at.y, w, h);
            Batch* body = &bodiesProbe;
            if (gate) body = &bodiesGate;
            if (part.kind == PartKind::Toggle) body = &bodiesInput;
            if (part.kind == PartKind::Box) body = part.box == boxFromKey("rom") ? &bodiesProgram : &bodiesBox;
            if (part.kind == PartKind::Chip) body = &bodiesChip;
            if (part.kind == PartKind::Clock) body = &bodiesClock;
            if (isBundle(part.kind)) body = &edges;
            body->add(at.x + inset, at.y + inset, w - 2 * inset, h - 2 * inset);
        }

        if (details && part.kind == PartKind::Toggle) {
            const std::uint64_t value = session.toggleValue(part.id);
            if (part.width == 1 && rounded) {
                // A slide switch: the knob sits at whichever end is on.
                const bool wide = w >= h;
                const float reach = (std::max(w, h) - std::min(w, h)) / 2.0f;
                const float shift = value != 0 ? reach : -reach;
                const Vec2 centre{at.x + w / 2.0f + (wide ? shift : 0.0f), at.y + h / 2.0f + (wide ? 0.0f : shift)};
                roundTops.addDisc(centre, std::min(w, h) * 0.36f, value != 0 ? kKnobOn : kTextOff);
            } else if (part.width == 1) {
                Batch& knob = value != 0 ? knobsOn : knobsOff;
                knob.add(at.x + w * 0.25f, at.y + h * 0.25f, w * 0.5f, h * 0.5f);
            } else {
                // One lamp per bit, the highest first. They're upright, so a turned switch turns too.
                for (int bit = 0; bit < part.width; ++bit) {
                    const int fromTop = part.width - 1 - bit;
                    const auto column = static_cast<float>(fromTop % 8);
                    const auto row = static_cast<float>(fromTop / 8);
                    const Vec2 a = frame.place({0.8f + 2.0f * column, 0.4f + 2.0f * row});
                    const Vec2 b = frame.place({2.2f + 2.0f * column, 1.6f + 2.0f * row});
                    const bool lit = ((value >> bit) & 1) != 0;
                    const SDL_FRect lamp{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
                    if (rounded) {
                        roundTops.addRoundRect(lamp, 3.0f, lit ? kKnobOn : kKnobOff);
                    } else {
                        Batch& knob = lit ? knobsOn : knobsOff;
                        knob.add(lamp.x, lamp.y, lamp.w, lamp.h);
                    }
                }
            }
        }
        if ((part.kind == PartKind::Probe || part.kind == PartKind::Constant) && frame.cell >= 6.0f) {
            const std::uint64_t value = part.kind == PartKind::Constant ? part.value : circuit.outputValue(part.id);
            char text[20];
            std::snprintf(text, sizeof text, "%0*llX", static_cast<int>(hexDigits(part.width)), static_cast<unsigned long long>(value));
            labels.push_back({at.x + w * 0.5f, at.y + h * 0.5f, std::max(0.9f, frame.cell * 1.15f / 13.0f), text,
                              part.kind == PartKind::Constant ? kText : value != 0 ? kKnobOn : kTextDim, Face::Mono});
        }
        if (part.kind == PartKind::Clock && frame.cell >= 6.0f) {
            // A square wave, bright while the clock is high.
            const bool on = circuit.pinOn(part.id, 0);
            const float u = std::min(w, h) * 0.26f;
            const Vec2 c{at.x + w * 0.5f, at.y + h * 0.5f};
            const std::array<Vec2, 6> wave{Vec2{c.x - 2.0f * u, c.y + u}, Vec2{c.x - u, c.y + u}, Vec2{c.x - u, c.y - u},
                                           Vec2{c.x + u, c.y - u},        Vec2{c.x + u, c.y + u}, Vec2{c.x + 2.0f * u, c.y + u}};
            for (std::size_t i = 0; i + 1 < wave.size(); ++i) shapes.addLine(wave[i], wave[i + 1], 1.6f, on ? kWireOn : kWarning, on ? 1.0f : 0.6f);
        }
        if (isBundle(part.kind) && frame.cell >= 6.0f) {
            char text[8];
            std::snprintf(text, sizeof text, "%d", part.width);
            labels.push_back({at.x + w * 0.5f, at.y + h * 0.5f, 1.0f, text, kBackground});
        }
        const bool screen = isScreenBox(part, "led_matrix") || isScreenBox(part, "terminal");
        if (screen && showSymbols) drawScreenBox(shapes, glow, labels, part, frame, circuit);
        if ((part.kind == PartKind::Box || part.kind == PartKind::Chip) && frame.cell >= 5.0f) {
            // Zoomed out there's no room for the name among the pins, so it goes over the top of the part. A
            // screen keeps its middle for its picture.
            const bool roomInside = frame.cell >= 11.0f && !screen;
            const float textScale = std::clamp(frame.cell / 13.0f, 0.85f, 1.6f);
            labels.push_back({at.x + w * 0.5f, roomInside ? at.y + h * 0.5f : at.y - 7.0f, textScale, partName(part), kText});
            if (part.kind == PartKind::Box && part.box == boxFromKey("rom") && roomInside) {
                labels.push_back({at.x + w * 0.5f, at.y + h * 0.5f + 15.0f * textScale, textScale * 0.78f, "programa", kToneProgram});
            }
            const Vec2 middle{at.x + w * 0.5f, at.y + h * 0.5f};
            const unsigned count = pinCount(part);
            const float pinScale = std::clamp(frame.cell / 13.0f, 0.74f, 1.0f);
            for (unsigned pin = 0; pin < count; ++pin) {
                const Point p = pinPosition(part, pin);
                const Vec2 s = camera.toScreen(static_cast<float>(p.x), static_cast<float>(p.y));
                const char* name = pinName(part, pin);
                const bool sideways = std::abs(s.x - middle.x) / std::max(w, 1.0f) > std::abs(s.y - middle.y) / std::max(h, 1.0f);
                // Each side has half the part. A name too long for its half gets smaller, rather than
                // running into the name on the other side, as "QualImm" did into "R".
                float scale = pinScale;
                const float full = textWidth(renderer, Face::Regular, kUiText, name);
                const float room = (sideways ? w * 0.5f : w) - frame.cell * 0.2f - 9.0f;
                if (full * scale > room && full > 0.0f) scale = std::max(0.5f, room / full);
                const float reach = 5.0f + full * scale / 2.0f + frame.cell * 0.2f;
                const float x = sideways ? s.x + (s.x < middle.x ? reach : -reach) : s.x;
                const float y = sideways ? s.y : s.y + (s.y < middle.y ? 10.0f : -10.0f);
                labels.push_back({x, y, scale, name, kTextDim});
            }
        }

        if (zoom >= 5.0f) {
            const auto name = names.find(part.id);
            if (name != names.end()) {
                // Outputs stand in columns, so their names go beside them, clear of the neighbour above.
                const bool beside = isOutputPart(part.kind) && part.rotation == 0;
                const float half = 4.0f * static_cast<float>(std::string_view(name->second).size());
                labels.push_back({beside ? at.x + w + 6.0f + half : at.x + w * 0.5f, beside ? at.y + h * 0.5f : at.y - 8.0f, 1.0f, name->second, kWirePreview});
            }
        }

        if (showPins) {
            const unsigned count = pinCount(part);
            const float size = std::max(3.0f, zoom * 0.4f);
            for (unsigned pin = 0; pin < count; ++pin) {
                const Point p = pinPosition(part, pin);
                const Vec2 s = camera.toScreen(static_cast<float>(p.x), static_cast<float>(p.y));
                if (rounded) {
                    roundTops.addDisc(s, std::max(1.8f, zoom * 0.17f), kPin);
                } else {
                    pins.add(s.x - size / 2, s.y - size / 2, size, size);
                }
            }
        }
    }
    focusFrames.flush(renderer, kLabelFocus);
    selection.flush(renderer, kSelected);
    edges.flush(renderer, kBodyEdge);
    bodiesGate.flush(renderer, kBodyEdge);
    bodiesInput.flush(renderer, kBodyInput);
    bodiesBox.flush(renderer, kBodyBox);
    bodiesProgram.flush(renderer, kBodyProgram);
    bodiesChip.flush(renderer, kBodyChip);
    bodiesClock.flush(renderer, kBodyClock);
    bodiesProbe.flush(renderer, kBodyProbe);
    roundBodies.flush(renderer);
    knobsOff.flush(renderer, kKnobOff);
    knobsOn.flush(renderer, kKnobOn);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    glow.flush(renderer);
    shapes.flush(renderer);
    roundTops.flush(renderer);
    pins.flush(renderer, kPin);
    drawLabels(renderer, labels);

    const Drop hovered = session.hovered();
    if (session.mode() == Mode::Idle || session.mode() == Mode::Wiring) {
        if (hovered.kind != Drop::Kind::Empty) {
            const Vec2 at = camera.toScreen(static_cast<float>(hovered.at.x), static_cast<float>(hovered.at.y));
            const float size = std::max(8.0f, zoom * 0.8f);
            const SDL_FRect ring{at.x - size / 2, at.y - size / 2, size, size};
            setColor(renderer, kWirePreview);
            SDL_RenderRect(renderer, &ring);
        }
    }

    if (session.mode() == Mode::Wiring) {
        Batch preview;
        addPath(preview, camera, session.wirePreview(), wireWidth);
        preview.flush(renderer, kWirePreview);
    }

    if (session.mode() == Mode::Placing) {
        const Part part = session.placingPart();
        const Rect bounds = boundsOf(part);
        const Vec2 at = camera.toScreen(static_cast<float>(bounds.left), static_cast<float>(bounds.top));
        const SDL_FRect ghost{at.x, at.y, static_cast<float>(bounds.right - bounds.left) * zoom,
                              static_cast<float>(bounds.bottom - bounds.top) * zoom};
        setColor(renderer, session.placingFits() ? kGhostGood : kGhostBad, 70);
        SDL_RenderFillRect(renderer, &ghost);
        if (isGate(part.kind) && zoom >= 4.0f) {
            Mesh mesh;
            addGate(mesh, part, frameOf(part, at, zoom));
            mesh.flush(renderer);
        }
            setColor(renderer, session.placingFits() ? kGhostGood : kGhostBad);
        SDL_RenderRect(renderer, &ghost);
    }

    if (session.mode() == Mode::Pasting) {
        const Rect box = session.pasteBox();
        const Vec2 a = camera.toScreen(static_cast<float>(box.left), static_cast<float>(box.top));
        const Vec2 b = camera.toScreen(static_cast<float>(box.right), static_cast<float>(box.bottom));
        const SDL_FRect rect{a.x - 3, a.y - 3, b.x - a.x + 6, b.y - a.y + 6};
        setColor(renderer, session.pasteFits() ? kGhostGood : kGhostBad);
        SDL_RenderRect(renderer, &rect);
    }

    if (session.mode() == Mode::BoxSelect) {
        const Rect box = session.selectionBox();
        const Vec2 a = camera.toScreen(static_cast<float>(box.left), static_cast<float>(box.top));
        const Vec2 b = camera.toScreen(static_cast<float>(box.right), static_cast<float>(box.bottom));
        const SDL_FRect rect{a.x, a.y, b.x - a.x, b.y - a.y};
        setColor(renderer, kSelected);
        SDL_RenderRect(renderer, &rect);
    }
}

namespace {

// The folder's name on its button, short enough for a third of the panel.
const char* folderLabel(const std::string& name) {
    static constexpr const char* kShort[][2] = {
        {"Entrada e saída", "E / S"}, {"Eletrônica", "Elétrica"}, {"Aritmética", "Cálculo"}, {"Circuitos do projeto", "Projeto"},
    };
    for (const auto& pair : kShort) {
        if (name == pair[0]) return pair[1];
    }
    return name.c_str();
}

// What a ready-made box says on its tile: a short name, the way a chip is marked.
const char* boxMark(BoxId box) {
    static constexpr const char* kMarks[][2] = {
        {"half_adder", "½ SOMA"}, {"full_adder", "SOMA"},   {"adder_n", "SOMA N"},     {"sub_n", "SUB N"},       {"cmp_n", "CMP N"},
        {"adder4", "SOMA 4"},     {"full_subtractor", "SUB"}, {"comparator", "CMP"},    {"alu_n", "ULA N"},       {"alu1", "ULA 1"},
        {"alu4", "ULA 4"},        {"mux2_n", "MUX N"},      {"mux4_n", "MUX4 N"},      {"mux2", "MUX"},          {"mux4", "MUX4"},
        {"demux2", "DEMUX"},      {"decoder2", "DEC"},      {"encoder4", "COD"},       {"register", "REG"},      {"counter", "CONT"},
        {"shift_register", "DESL"}, {"ram", "RAM"},         {"rom", "ROM"},            {"regfile", "R0–R7"},     {"extender", "EXT"},
        {"shifter", "<< >>"},     {"sr_latch", "SR"},       {"d_latch", "LATCH D"},    {"d_flip_flop", "FF D"},  {"t_flip_flop", "FF T"},
        {"jk_flip_flop", "FF JK"}, {"buffer", "BUF"},        {"parity_odd", "PAR"},       {"parity_even", "ÍMPAR"},  {"bit_select", "SEL BIT"},
        {"priority4", "PRIO"},    {"multiplier", "MUL N"}, {"divider", "DIV N"},      {"negator", "NEG N"},    {"bit_count", "CONTA 1"},
        {"bit_find", "ACHA 1"},   {"decoder7", "7 SEG"},    {"random", "ACASO"},    {"led_matrix", "MATRIZ"}, {"terminal", "TERMINAL"},
    };
    const std::string_view key = boxSpec(box).key;
    for (const auto& pair : kMarks) {
        if (key == pair[0]) return pair[1];
    }
    return "";
}

// A chip drawn small: a body, a few pins on each side and its mark in the middle.
void drawMiniChip(SDL_Renderer* renderer, Mesh& lines, Vec2 centre, const char* mark, Color body, Color edge) {
    const SDL_FRect r{centre.x - 24.0f, centre.y - 15.0f, 48.0f, 30.0f};
    for (int i = 0; i < 3; ++i) {
        const float y = r.y + 7.0f + static_cast<float>(i) * 8.0f;
        lines.addLine({r.x - 6.0f, y}, {r.x, y}, 1.4f, kPin);
        lines.addLine({r.x + r.w, y}, {r.x + r.w + 6.0f, y}, 1.4f, kPin);
    }
    fillRound(renderer, r, 5, body);
    frameRound(renderer, r, 5, 1, edge);
    const float size = std::strlen(mark) > 5 ? 9.5f : 11.0f;
    const float w = textWidth(renderer, Face::Medium, size, mark);
    write(renderer, Face::Medium, size, centre.x - w / 2.0f, centre.y + size * 0.36f, mark, kText);
}

// The picture on a tile, centred in the tile's upper part.
void drawTileGlyph(SDL_Renderer* renderer, Mesh& icons, const PaletteItem& item, Vec2 centre, bool current) {
    if (item.newChip) {
        drawIcon(renderer, Icon::Plus, centre, 26, kWirePreview);
        return;
    }
    if (item.circuit) {
        drawMiniChip(renderer, icons, centre, item.chip == 0 ? "PLACA" : "CHIP", item.chip == 0 ? kBodyBox : kBodyChip, current ? kWirePreview : kBodyEdge);
        return;
    }
    if (item.example != Example::None) {
        const SDL_FRect r{centre.x - 24.0f, centre.y - 15.0f, 48.0f, 30.0f};
        fillRound(renderer, r, 6, kBodyChip);
        frameRound(renderer, r, 6, 1, kWirePreview);
        drawIcon(renderer, Icon::Play, centre, 18, kWirePreview);
        return;
    }
    const Part part = makePart(item.kind, {}, 0, 0, 1, item.box);
    if (item.kind == PartKind::Box) {
        drawMiniChip(renderer, icons, centre, boxMark(item.box), kBodyBox, kBodyEdge);
        return;
    }
    // Gates and electric parts are drawn the way the board draws them, at a size that fits the tile.
    if (isGate(item.kind) || isElectric(item.kind)) {
        const float w = static_cast<float>(baseWidth(part));
        const float h = static_cast<float>(baseHeight(part));
        const float cell = std::min(44.0f / w, 32.0f / h);
        const Frame frame = frameOf(part, {centre.x - w * cell / 2.0f, centre.y - h * cell / 2.0f}, cell);
        if (isGate(item.kind)) {
            addGate(icons, part, frame);
        } else {
            Mesh glow;
            addElectric(icons, glow, part, frame, std::nullopt, true);
        }
        return;
    }
    switch (item.kind) {
        case PartKind::Led: {
            Mesh led;
            led.addDisc(centre, 15.0f, kLedOn, 0.18f);
            led.addDisc(centre, 9.5f, kLedOn);
            led.addDisc({centre.x - 3.0f, centre.y - 3.0f}, 2.6f, kLedShine);
            led.flush(renderer);
            break;
        }
        case PartKind::Keyboard: {
            fillRound(renderer, {centre.x - 22.0f, centre.y - 12.0f, 44.0f, 24.0f}, 4, Color{50, 56, 64});
            for (int row = 0; row < 3; ++row) {
                for (int key = 0; key < 5; ++key) {
                    fillRound(renderer, {centre.x - 18.0f + static_cast<float>(row) * 2.0f + static_cast<float>(key) * 7.0f, centre.y - 9.0f + static_cast<float>(row) * 6.5f, 5.0f, 4.5f}, 1,
                              Color{130, 140, 150});
                }
            }
            break;
        }
        case PartKind::Button: {
            fillRound(renderer, {centre.x - 16.0f, centre.y - 12.0f, 32.0f, 24.0f}, 6, kBodyInput);
            Mesh key;
            key.addDisc(centre, 8.0f, kKnobOn);
            key.flush(renderer);
            break;
        }
        case PartKind::Display7: {
            const SDL_FRect r{centre.x - 13.0f, centre.y - 19.0f, 26.0f, 38.0f};
            fillRound(renderer, r, 4, Color{24, 20, 22});
            const auto bar = [&](float x, float y, float w, float h) { fillRound(renderer, {centre.x + x, centre.y + y, w, h}, 1, kLedOn); };
            bar(-6, -15, 12, 3);  // a
            bar(6, -13, 3, 11);   // b
            bar(6, 2, 3, 11);     // c
            bar(-6, 12, 12, 3);   // d
            bar(-6, -1.5f, 12, 3);  // g
            break;
        }
        case PartKind::Toggle: {
            fillRound(renderer, {centre.x - 18.0f, centre.y - 10.0f, 36.0f, 20.0f}, 10, kBodyInput);
            Mesh knob;
            knob.addDisc({centre.x + 8.0f, centre.y}, 7.0f, kKnobOn);
            knob.flush(renderer);
            break;
        }
        case PartKind::Probe: {
            const SDL_FRect r{centre.x - 22.0f, centre.y - 12.0f, 44.0f, 24.0f};
            fillRound(renderer, r, 5, kBodyProbe);
            frameRound(renderer, r, 5, 1, kBodyEdge);
            const float w = textWidth(renderer, Face::Mono, 14, "01");
            write(renderer, Face::Mono, 14, centre.x - w / 2.0f, centre.y + 5.0f, "01", kKnobOn);
            break;
        }
        case PartKind::Clock: {
            const SDL_FRect r{centre.x - 20.0f, centre.y - 13.0f, 40.0f, 26.0f};
            fillRound(renderer, r, 5, kBodyClock);
            const float u = 6.0f;
            const std::array<Vec2, 6> wave{Vec2{centre.x - 2.0f * u, centre.y + u}, Vec2{centre.x - u, centre.y + u}, Vec2{centre.x - u, centre.y - u},
                                           Vec2{centre.x + u, centre.y - u},        Vec2{centre.x + u, centre.y + u}, Vec2{centre.x + 2.0f * u, centre.y + u}};
            for (std::size_t i = 0; i + 1 < wave.size(); ++i) icons.addLine(wave[i], wave[i + 1], 2.0f, kWireOn);
            break;
        }
        case PartKind::Constant: {
            const SDL_FRect r{centre.x - 18.0f, centre.y - 12.0f, 36.0f, 24.0f};
            fillRound(renderer, r, 5, kBodyProbe);
            frameRound(renderer, r, 5, 1, kBodyEdge);
            const float w = textWidth(renderer, Face::Mono, 14, "1F");
            write(renderer, Face::Mono, 14, centre.x - w / 2.0f, centre.y + 5.0f, "1F", kText);
            break;
        }
        case PartKind::Tunnel: {
            const std::vector<Vec2> tag{{centre.x - 20.0f, centre.y - 10.0f}, {centre.x + 10.0f, centre.y - 10.0f}, {centre.x + 20.0f, centre.y},
                                        {centre.x + 10.0f, centre.y + 10.0f}, {centre.x - 20.0f, centre.y + 10.0f}};
            icons.addFan(tag, centre, kPanelRaised);
            addLoop(icons, tag, 1.4f, kWirePreview);
            break;
        }
        case PartKind::Splitter:
        case PartKind::Joiner: {
            // A thick bus on one side fanning out to thin wires on the other.
            const bool split = item.kind == PartKind::Splitter;
            const float bar = split ? centre.x - 4.0f : centre.x + 4.0f;
            fillRound(renderer, {bar - 3.0f, centre.y - 15.0f, 6.0f, 30.0f}, 2, kBodyEdge);
            icons.addLine({split ? bar - 18.0f : bar + 3.0f, centre.y}, {split ? bar - 3.0f : bar + 18.0f, centre.y}, 4.0f, kBusOn);
            for (int i = 0; i < 4; ++i) {
                const float y = centre.y - 12.0f + static_cast<float>(i) * 8.0f;
                icons.addLine({split ? bar + 3.0f : bar - 18.0f, y}, {split ? bar + 18.0f : bar - 3.0f, y}, 1.4f, kWireOn);
            }
            break;
        }
        default: break;
    }
}

void drawPalette(SDL_Renderer* renderer, const Session& session, const std::vector<PaletteGroup>& groups, const PaletteView& view, int height, float mouseX, float mouseY) {
    const PaletteHit hit = mouseX >= 0.0f && mouseX < kPanelWidth ? paletteAt(groups, view, mouseX, mouseY, height) : PaletteHit{};

    // The folders. With a search none is chosen: the tiles come from all of them.
    for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
        const SDL_FRect r = paletteFolderRect(g);
        const bool chosen = view.filter.empty() && g == view.folder;
        const bool empty = groups[static_cast<std::size_t>(g)].items.empty();
        fillRound(renderer, r, 7, chosen ? kPanelActive : hit.group == g && hit.item < 0 ? kPanelHover : kPanelButton);
        const char* label = folderLabel(groups[static_cast<std::size_t>(g)].name);
        const float w = textWidth(renderer, Face::Medium, 11.5f, label);
        write(renderer, Face::Medium, 11.5f, r.x + (r.w - w) / 2.0f, r.y + 17.0f, label, chosen ? kText : empty ? kTextOff : kTextDim);
    }

    const float top = paletteListTop(groups);
    const SDL_Rect clip{0, static_cast<int>(top) - 2, static_cast<int>(kPanelWidth), height - static_cast<int>(top) - 44};
    SDL_SetRenderClipRect(renderer, &clip);
    const std::vector<PaletteHit> shown = paletteShown(groups, view);
    if (shown.empty()) {
        write(renderer, Face::Regular, 12.5f, 14.0f, top + 20.0f, view.filter.empty() ? "Pasta vazia." : "Nenhuma peça com esse nome.", kTextDim);
    }
    Mesh icons;
    for (const PaletteHit& place : shown) {
        const PaletteItem& item = groups[static_cast<std::size_t>(place.group)].items[static_cast<std::size_t>(place.item)];
        const SDL_FRect r = paletteTileRect(groups, view, place.group, place.item);
        if (r.y + r.h < top - 2.0f || r.y > static_cast<float>(height)) continue;
        const bool hovered = hit.group == place.group && hit.item == place.item;
        const bool current = item.circuit && item.chip == session.currentCircuit();
        const bool placing = !item.circuit && !item.newChip && item.example == Example::None && session.mode() == Mode::Placing &&
                             session.placingKind() == item.kind && session.placingPart().box == item.box;
        fillRound(renderer, r, 9, placing || current ? kPanelActive : hovered ? kPanelHover : kPanelRaised);
        if (hovered || item.newChip) frameRound(renderer, r, 9, 1, item.newChip ? kBodyEdge : kWirePreview);

        drawTileGlyph(renderer, icons, item, {r.x + r.w / 2.0f, r.y + 27.0f}, current);
        icons.flush(renderer);

        // The name, on two lines when one isn't enough: "Banco de registradores" shouldn't lose its end.
        const Color color = item.example != Example::None || item.newChip ? kWirePreview : kText;
        constexpr float kSize = 11.5f;
        const float room = r.w - 10.0f;
        std::string first = item.name;
        std::string second;
        if (textWidth(renderer, Face::Regular, kSize, first) > room) {
            const std::size_t space = first.rfind(' ', first.size());
            std::size_t cut = std::string::npos;
            for (std::size_t at = first.find(' '); at != std::string::npos; at = first.find(' ', at + 1)) {
                if (textWidth(renderer, Face::Regular, kSize, std::string_view(first).substr(0, at)) <= room) cut = at;
            }
            if (cut == std::string::npos) cut = space;
            if (cut != std::string::npos) {
                second = fitted(renderer, Face::Regular, kSize, std::string_view(first).substr(cut + 1), room);
                first.resize(cut);
            }
            first = fitted(renderer, Face::Regular, kSize, first, room);
        }
        const float lineY = second.empty() ? r.y + r.h - 12.0f : r.y + r.h - 20.0f;
        write(renderer, Face::Regular, kSize, r.x + (r.w - textWidth(renderer, Face::Regular, kSize, first)) / 2.0f, lineY, first, color);
        if (!second.empty()) {
            write(renderer, Face::Regular, kSize, r.x + (r.w - textWidth(renderer, Face::Regular, kSize, second)) / 2.0f, lineY + 13.0f, second, color);
        }
    }
    icons.flush(renderer);
    // More below: the list fades out over its last rows instead of stopping at a cut.
    const float bottom = static_cast<float>(height) - 46.0f;
    float lowest = 0.0f;
    for (const PaletteHit& place : shown) {
        const SDL_FRect r = paletteTileRect(groups, view, place.group, place.item);
        lowest = std::max(lowest, r.y + r.h);
    }
    if (lowest > bottom) {
        for (int i = 0; i < 14; ++i) {
            fillRound(renderer, {0.0f, bottom - 28.0f + 2.0f * static_cast<float>(i), kPanelWidth - 1.0f, 2.0f}, 0, kPanel,
                      static_cast<float>(i + 1) / 14.0f);
        }
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

void drawToolbar(SDL_Renderer* renderer, const Session& session, int width, float rightEdge, float mouseX, float mouseY) {
    fillRect(renderer, {kPanelWidth, 0, static_cast<float>(width) - kPanelWidth, kToolbarHeight}, kPanel);
    fillRect(renderer, {kPanelWidth, kToolbarHeight - 1.0f, static_cast<float>(width) - kPanelWidth, 1.0f}, kRule);

    const bool selected = !session.selectedParts().empty() || !session.selectedWires().empty();
    const bool idle = session.mode() == Mode::Idle;
    const int hoveredTool = toolbarAt(mouseX, mouseY);
    for (int i = 0; i < static_cast<int>(std::size(kToolbar)); ++i) {
        const ToolButton& button = kToolbar[i];
        const float x = toolbarButtonX(i);
        if (x + button.width > rightEdge) break;
        bool enabled = true;
        Icon icon = button.icon;
        char speedText[24] = "";
        if (button.sim == SimButton::None) {
            switch (button.key) {
                case Key::Delete:
                case Key::Copy:
                case Key::Cut: enabled = selected && idle; break;
                case Key::Paste: enabled = session.canPaste(); break;
                case Key::Rotate: enabled = (selected && idle) || session.mode() == Mode::Placing; break;
                case Key::Undo: enabled = session.undoSteps() > 0; break;
                case Key::Redo: enabled = session.redoSteps() > 0; break;
                default: break;
            }
        } else {
            enabled = session.hasClock() || button.sim == SimButton::Slower || button.sim == SimButton::Faster ||
                      button.sim == SimButton::SpeedLabel || button.sim == SimButton::Reset || button.sim == SimButton::Home ||
                      button.sim == SimButton::Save || button.sim == SimButton::Open;
            if (button.sim == SimButton::RunPause && session.running()) icon = Icon::Pause;
            if (button.sim == SimButton::SpeedLabel) {
                const std::uint64_t hz = session.speed();
                if (hz == 0) {
                    std::snprintf(speedText, sizeof speedText, "m\xC3\xA1ximo");
                } else if (hz >= 1024) {
                    std::snprintf(speedText, sizeof speedText, "%llu kHz", static_cast<unsigned long long>(hz / 1024));
                } else {
                    std::snprintf(speedText, sizeof speedText, "%llu Hz", static_cast<unsigned long long>(hz));
                }
            }
        }
        const SDL_FRect rect{x, 7, button.width, kToolbarHeight - 14};
        if (button.sim == SimButton::SpeedLabel) {
            const float textW = textWidth(renderer, Face::Regular, kUiText, speedText);
            write(renderer, Face::Regular, kUiText, x + (button.width - textW) / 2.0f, 27, speedText, kTextDim);
            continue;
        }
        const bool running = button.sim == SimButton::RunPause && session.running();
        if (running) {
            fillRound(renderer, rect, 7, kPanelActive);
        } else if (hoveredTool == i && enabled) {
            fillRound(renderer, rect, 7, kPanelHover);
        }
        drawIcon(renderer, icon, {x + button.width / 2.0f, kToolbarHeight / 2.0f}, 20, enabled ? kText : kTextOff);
    }
}

void drawTooltip(SDL_Renderer* renderer, float mouseX, float mouseY) {
    const int tool = toolbarAt(mouseX, mouseY);
    if (tool < 0 || kToolbar[tool].label[0] == '\0') return;
    const char* label = kToolbar[tool].label;
    const float w = textWidth(renderer, Face::Regular, 12.5f, label) + 18.0f;
    const SDL_FRect box{std::max(kPanelWidth + 6.0f, toolbarButtonX(tool) + kToolbar[tool].width / 2.0f - w / 2.0f), kToolbarHeight + 2.0f, w, 24.0f};
    fillRound(renderer, box, 6, kPanelRaised);
    frameRound(renderer, box, 6, 1, kBodyEdge);
    write(renderer, Face::Regular, 12.5f, box.x + 9.0f, box.y + 17.0f, label, kText);
}

void drawCheckMark(SDL_Renderer* renderer, Vec2 centre, float size, Color color);  // below

// For a lesson that asks for a table: what each output of one row should show, by the lesson's names, or
// nothing for an output it doesn't ask about or a row whose inputs aren't all on the board.
std::vector<std::optional<std::uint64_t>> goalRow(const Lesson& lesson, const TruthTable& table, int row) {
    std::vector<std::optional<std::uint64_t>> goal(table.outputs.size());
    std::vector<std::uint64_t> values;
    unsigned remaining = static_cast<unsigned>(table.totalInputBits);
    std::vector<std::uint64_t> shown;
    for (std::size_t i = 0; i < table.inputs.size(); ++i) {
        remaining -= table.inputBits[i];
        shown.push_back((static_cast<std::uint64_t>(row) >> remaining) & ((std::uint64_t{1} << table.inputBits[i]) - 1));
    }
    for (const char* name : lesson.inputs) {
        const auto found = std::find(table.inputNames.begin(), table.inputNames.end(), name);
        if (found == table.inputNames.end()) return goal;
        values.push_back(shown[static_cast<std::size_t>(found - table.inputNames.begin())]);
    }
    for (std::size_t o = 0; o < lesson.outputs.size(); ++o) {
        const auto found = std::find(table.outputNames.begin(), table.outputNames.end(), lesson.outputs[o]);
        if (found == table.outputNames.end()) continue;
        std::uint64_t wanted = 0;
        if (lesson.expect != nullptr) {
            wanted = lesson.expect(values, o);
        } else {
            std::size_t index = 0;
            for (const std::uint64_t v : values) index = index * 2 + (v & 1u);
            const std::string_view rows(lesson.table);
            const std::size_t at = index * lesson.outputs.size() + o;
            if (at >= rows.size()) continue;
            wanted = rows[at] == '1' ? 1u : 0u;
        }
        goal[static_cast<std::size_t>(found - table.outputNames.begin())] = wanted;
    }
    return goal;
}

// Whether a row's output cell shows the number wanted.
bool cellShows(const std::string& cell, std::uint64_t wanted) {
    if (cell.empty() || cell[0] == '~') return false;
    return std::strtoull(cell.c_str(), nullptr, 16) == wanted;
}

void drawRightPanel(SDL_Renderer* renderer, const Session& session, int width, int height, int scroll, float mouseX, float mouseY,
                    const Lesson* goal) {
    const float panelWidth = rightPanelWidth(session);
    const float left = static_cast<float>(width) - panelWidth;
    fillRect(renderer, {left, 0, panelWidth, static_cast<float>(height)}, kPanel);
    fillRect(renderer, {left, 0, 1.0f, static_cast<float>(height)}, kRule);

    char text[64];
    const std::optional<Inspected> shown = session.inspected();
    if (shown) {
        const Part& part = shown->part;
        write(renderer, Face::Medium, 14, left + 14, 24, fitted(renderer, Face::Medium, 14, shown->name, panelWidth - 28.0f), kText);
        for (const PanelControl& control : panelControls(session, left)) {
            const SDL_FRect& r = control.rect;
            const bool programs = control.button == PanelButton::EditProgram;
            fillRound(renderer, r, 6, programs ? (inside(r, mouseX, mouseY) ? mixed(kBodyProgram, kToneProgram, 0.35f) : kBodyProgram)
                                      : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
            if (programs) frameRound(renderer, r, 6, 1.2f, kToneProgram);
            const auto row = [&](const char* label, int value) {
                write(renderer, Face::Regular, kUiText, left + 14, r.y + 17, label, kTextDim);
                std::snprintf(text, sizeof text, "%d", value);
                const float valueWidth = textWidth(renderer, Face::Medium, kUiText, text);
                write(renderer, Face::Medium, kUiText, r.x + r.w + (left + 168.0f - r.x - r.w - valueWidth) / 2.0f, r.y + 17, text, kText);
                drawIcon(renderer, Icon::Minus, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, 15, kText);
            };
            const auto plus = [&] { drawIcon(renderer, Icon::Plus, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, 15, kText); };
            const auto caption = [&](const char* label) {
                const std::string shortened = fitted(renderer, Face::Regular, kUiText, label, r.w - 12.0f);
                const float captionWidth = textWidth(renderer, Face::Regular, kUiText, shortened);
                write(renderer, Face::Regular, kUiText, r.x + (r.w - captionWidth) / 2.0f, r.y + 17, shortened, kText);
            };
            switch (control.button) {
                case PanelButton::InputsDown: row(isBundle(part.kind) ? "Ramos" : "Entradas", part.inputs); break;
                case PanelButton::BitsDown: row("Bits", part.width); break;
                case PanelButton::AddressDown: row(boxSpec(part.box).addressName, part.address); break;
                case PanelButton::SizeDown: row("Tamanho", part.scale); break;
                case PanelButton::InputsUp:
                case PanelButton::BitsUp:
                case PanelButton::AddressUp:
                case PanelButton::SizeUp: plus(); break;
                case PanelButton::EditName: caption(part.label.empty() ? "Dar nome" : part.label.c_str()); break;
                case PanelButton::EditValue: {
                    const std::optional<electric::Quantity> quantity = valueQuantity(part.kind);
                    caption(quantity ? i18n::format("Valor: {}", {electric::formatQuantity(part.value, *quantity)}).c_str() : "Editar valor");
                    break;
                }
                case PanelButton::EditContents: caption("Editar conte\xC3\xBA" "do"); break;
                case PanelButton::EditProgram: caption("Programar a ROM"); break;
                case PanelButton::OpenChip: caption("Abrir chip"); break;
                case PanelButton::LookInside: caption("Ver por dentro"); break;
                default: break;
            }
        }
        const float ruleY = tableTop(session) - 6.0f;
        fillRect(renderer, {left + 12, ruleY, panelWidth - 24, 1.0f}, kRule);
    }

    const float top = tableTop(session);
    write(renderer, Face::Medium, 14, left + 14, top + 24, "Tabela-verdade", kText);
    const TruthTable& table = session.truthTable();
    // In a lesson, how many rows already do what it asks.
    if (goal != nullptr && table.status == TruthTable::Status::Ready) {
        int right = 0;
        int checked = 0;
        for (int r = 0; r < static_cast<int>(table.rows.size()); ++r) {
            const std::vector<std::optional<std::uint64_t>> wanted = goalRow(*goal, table, r);
            bool any = false;
            bool ok = true;
            std::size_t at = 0;
            for (std::size_t i = 0; i < table.outputs.size(); ++i) {
                const std::size_t digits = hexDigits(table.outputBits[i]);
                const std::string cell = table.rows[static_cast<std::size_t>(r)].substr(at, digits);
                at += digits;
                if (!wanted[i]) continue;
                any = true;
                ok = ok && cellShows(cell, *wanted[i]);
            }
            checked += any ? 1 : 0;
            right += any && ok ? 1 : 0;
        }
        if (checked > 0) {
            const std::string count = i18n::format("{} de {} certas", {std::to_string(right), std::to_string(checked)});
            const float countWidth = textWidth(renderer, Face::Medium, 12.0f, count);
            write(renderer, Face::Medium, 12.0f, left + panelWidth - 14.0f - countWidth, top + 24, count, right == checked ? kKnobOn : kWarning);
        }
    }

    const auto message = [&](const char* first, const char* second) {
        write(renderer, Face::Regular, kUiText, left + 14, top + 50, first, kTextDim);
        write(renderer, Face::Regular, kUiText, left + 14, top + 68, second, kTextDim);
    };
    switch (table.status) {
        case TruthTable::Status::NothingToShow:
            message("Coloque entradas e", "uma sa\xC3\xAD" "da na placa.");
            break;
        case TruthTable::Status::TooManyInputs:
            std::snprintf(text, sizeof text, i18n::tr("Mais de %zu bits de entrada:"), kMaxTableInputs);
            message(text, "a tabela seria grande demais.");
            break;
        case TruthTable::Status::BoardTooBig:
            message("Placa muito grande", "para calcular agora.");
            break;
        case TruthTable::Status::Ready: {
            const float headerY = top + 52;
            const float firstRowY = headerY + 12;
            bool crowded = false;
            for (std::size_t i = 0; i < table.inputs.size(); ++i) {
                crowded = crowded || textWidth(renderer, Face::Medium, 12, table.inputNames[i]) > inputColumn(table, i) - 2.0f;
            }
            for (std::size_t i = 0; i < table.outputs.size(); ++i) {
                crowded = crowded || textWidth(renderer, Face::Medium, 12, table.outputNames[i]) > outputColumn(table, i) - 2.0f;
            }
            std::size_t headed = 0;
            const auto heading = [&](float at, float column, const std::string& name) {
                const bool raised = crowded && headed++ % 2 == 1;
                const std::string shortened = crowded ? fitted(renderer, Face::Medium, 12, name, 2.0f * column - 4.0f) : name;
                write(renderer, Face::Medium, 12, at, raised ? headerY - 12.0f : headerY, shortened, kWirePreview);
            };
            std::vector<float> inputX;
            float x = left + 16;
            for (std::size_t i = 0; i < table.inputs.size(); ++i) {
                inputX.push_back(x);
                heading(x, inputColumn(table, i), table.inputNames[i]);
                x += inputColumn(table, i);
            }
            const float split = x;
            x += 14;
            std::vector<float> outputX;
            for (std::size_t i = 0; i < table.outputs.size(); ++i) {
                outputX.push_back(x);
                heading(x, outputColumn(table, i), table.outputNames[i]);
                x += outputColumn(table, i);
            }

            const int visible = truthRowsVisible(session, height);
            const int rows = static_cast<int>(table.rows.size());
            const int current = session.currentRow();
            const SDL_FRect splitLine{split, headerY - 10, 1, static_cast<float>(std::min(rows, visible)) * kTableRowHeight + 22.0f};
            fillRect(renderer, splitLine, kRule);
            for (int r = scroll; r < rows && r < scroll + visible; ++r) {
                const float y = firstRowY + static_cast<float>(r - scroll) * kTableRowHeight;
                if (r == current) fillRound(renderer, {left + 8, y - 1, panelWidth - 16, kTableRowHeight}, 5, kPanelRow);
                const float baseline = y + 13.0f;
                unsigned remaining = static_cast<unsigned>(table.totalInputBits);
                for (std::size_t i = 0; i < table.inputs.size(); ++i) {
                    remaining -= table.inputBits[i];
                    const unsigned value = (static_cast<unsigned>(r) >> remaining) & ((1u << table.inputBits[i]) - 1);
                    char cell[20];
                    std::snprintf(cell, sizeof cell, "%0*X", static_cast<int>(hexDigits(table.inputBits[i])), value);
                    write(renderer, Face::Mono, 14, inputX[i], baseline, cell, value != 0 ? kText : kTextDim);
                }
                const std::string& values = table.rows[static_cast<std::size_t>(r)];
                const std::vector<std::optional<std::uint64_t>> wanted =
                    goal != nullptr ? goalRow(*goal, table, r) : std::vector<std::optional<std::uint64_t>>(table.outputs.size());
                bool checked = false;
                bool rowRight = true;
                std::size_t at = 0;
                for (std::size_t i = 0; i < table.outputs.size(); ++i) {
                    const std::size_t digits = hexDigits(table.outputBits[i]);
                    const std::string cell = values.substr(at, digits);
                    at += digits;
                    const bool zero = cell.find_first_not_of('0') == std::string::npos;
                    const bool wrong = wanted[i].has_value() && !cellShows(cell, *wanted[i]);
                    checked = checked || wanted[i].has_value();
                    rowRight = rowRight && !wrong;
                    const Color color = wrong ? kWireConflict : cell[0] == '~' ? kWarning : zero ? kTextDim : kKnobOn;
                    write(renderer, Face::Mono, 14, outputX[i], baseline, cell, color);
                }
                // Against the lesson's table: a tick for a right row, a red dot for a wrong one.
                if (checked) {
                    const Vec2 mark{left + panelWidth - 16.0f, y + kTableRowHeight / 2.0f - 1.0f};
                    if (rowRight) {
                        drawCheckMark(renderer, mark, 10.0f, kKnobOn);
                    } else {
                        Mesh dot;
                        dot.addDisc(mark, 3.5f, kWireConflict);
                        dot.flush(renderer);
                    }
                }
            }
            const float below = static_cast<float>(height) - 58.0f;
            if (rows > visible) {
                std::snprintf(text, sizeof text, i18n::tr("%d a %d de %d"), scroll + 1, std::min(rows, scroll + visible), rows);
                write(renderer, Face::Regular, 12, left + 14, below, text, kTextDim);
            }
            if (table.hasMemory) write(renderer, Face::Regular, 12, left + 14, below + 18, "Cada linha come\xC3\xA7" "a com a mem\xC3\xB3ria zerada", kWarning);
            break;
        }
    }

    const Board& board = session.board();
    std::snprintf(text, sizeof text, i18n::tr("%zu pe\xC3\xA7" "as, %zu fios"), board.parts().size(), board.wires().size());
    write(renderer, Face::Regular, 12, left + 14, static_cast<float>(height) - 14.0f, text, kTextDim);
}

constexpr Color kCodeWrongLine{70, 32, 34};
constexpr Color kCodeSelection{36, 84, 100};
constexpr float kProblemRow = 17.0f;
constexpr std::size_t kProblemsShown = 4;
constexpr float kCodeText = 16.0f;

// The colour of each kind of instruction: the X-ray paints the instruction going through the CPU in it,
// and the code editor's list of instructions uses the same, so the two read alike.
constexpr Color kToneAlu{92, 168, 255};
constexpr Color kToneNumber{94, 222, 190};
constexpr Color kToneMemory{196, 140, 255};
constexpr Color kToneBranch{255, 168, 96};
constexpr Color kToneJump{255, 118, 176};
constexpr Color kToneWorld{134, 222, 112};
constexpr Color kToneHalt{240, 84, 84};

// The list beside the code. A row with no meaning heads a group, in the group's colour.
struct CodeReference {
    const char* code;
    const char* meaning;
    Color tone;
};

constexpr CodeReference kCodeReference[] = {
    {"Registradores", "", kWirePreview},
    {"R0 a R7", "R0 vale sempre 0", kWirePreview},
    {"; texto", "comentário", kWirePreview},
    {"nome:", "marca um lugar", kWirePreview},
    {"Contas", "", kToneAlu},
    {"ADD Rd, Rs, Rt", "Rd = Rs + Rt", kToneAlu},
    {"SUB Rd, Rs, Rt", "Rd = Rs - Rt", kToneAlu},
    {"AND Rd, Rs, Rt", "bit a bit", kToneAlu},
    {"OR  Rd, Rs, Rt", "bit a bit", kToneAlu},
    {"XOR Rd, Rs, Rt", "bit a bit", kToneAlu},
    {"NOT Rd, Rs", "inverte os bits", kToneAlu},
    {"SHL Rd, Rs", "dobra", kToneAlu},
    {"SHR Rd, Rs", "metade", kToneAlu},
    {"ADDI Rd, Rs, n", "soma n, de -32 a 31", kToneAlu},
    {"MOV Rd, Rs", "copia", kToneAlu},
    {"Números", "", kToneNumber},
    {"LI  Rd, n", "n de 0 a 511", kToneNumber},
    {"LUI Rd, n", "n vezes 128", kToneNumber},
    {"LIW Rd, n", "n de 0 a 65535", kToneNumber},
    {"Memória", "", kToneMemory},
    {"LD Rd, n(Rs)", "lê da RAM", kToneMemory},
    {"ST Rd, n(Rs)", "grava na RAM", kToneMemory},
    {"Caminho", "", kToneBranch},
    {"BEQ Ra, Rb, x", "vai a x se iguais", kToneBranch},
    {"BNE Ra, Rb, x", "vai a x se diferentes", kToneBranch},
    {"JMP x", "vai a x", kToneJump},
    {"CALL x", "vai a x; volta em R7", kToneJump},
    {"RET", "volta", kToneJump},
    {"JR Rs", "vai aonde Rs manda", kToneJump},
    {"Mundo", "", kToneWorld},
    {"IN Rd", "lê as chaves", kToneWorld},
    {"OUT Rs", "mostra na saída", kToneWorld},
    {"NOP", "não faz nada", kTextDim},
    {"HLT", "para o computador", kToneHalt},
};

std::size_t characters(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Labels, instructions, numbers and comments, each in its colour.
void drawCodeLine(SDL_Renderer* renderer, const std::string& line, float x, float baseline, float size = kCodeText, float charWidth = kCodeCharWidth) {
    const std::string_view view(line);
    const std::size_t comment = std::min(line.find_first_of(";#"), line.size());
    const std::size_t colon = view.substr(0, comment).find(':');
    const auto piece = [&](std::size_t from, std::size_t to, Color color) {
        if (from >= to) return;
        write(renderer, Face::Mono, size, x + charWidth * static_cast<float>(characters(view.substr(0, from))), baseline, view.substr(from, to - from), color);
    };
    std::size_t at = colon == std::string_view::npos ? 0 : colon + 1;
    piece(0, at, kWirePreview);
    while (at < comment && line[at] == ' ') ++at;
    std::size_t end = at;
    while (end < comment && line[end] != ' ') ++end;
    piece(at, end, kBusOn);
    std::size_t word = end;
    while (word < comment) {
        std::size_t stop = word;
        while (stop < comment && std::strchr(" ,()", line[stop]) == nullptr) ++stop;
        if (stop == word) {
            ++word;
            continue;
        }
        const char first = line[word];
        const bool number = std::isdigit(static_cast<unsigned char>(first)) != 0 || first == '-';
        piece(word, stop, number ? kWarning : kText);
        word = stop;
    }
    piece(end, end, kText);
    for (std::size_t i = end; i < comment; ++i) {
        if (std::strchr(",()", line[i]) != nullptr) piece(i, i + 1, kTextDim);
    }
    piece(comment, line.size(), kTextDim);
}

void drawButton(SDL_Renderer* renderer, const SDL_FRect& rect, const char* label, Color fill) {
    fillRound(renderer, rect, 7, fill);
    const float width = textWidth(renderer, Face::Medium, kUiText, label);
    write(renderer, Face::Medium, kUiText, rect.x + (rect.w - width) / 2.0f, rect.y + rect.h / 2.0f + 5.0f, label, kText);
}

void drawXrayPlan(SDL_Renderer* renderer, const ComputerState& state, const SDL_FRect& plan, int stage, float progress);

// How close the diagrams are seen: set from the interface each frame, for the X-rays and the panel.
XrayZoom gZoom;

// The diagram's scale and corner for a plan of this size, with the zoom and the pan: fitted at zoom 1.
float zoomedScale(const SDL_FRect& plan, float w, float h) { return std::min((plan.w - 20.0f) / w, (plan.h - 20.0f) / h) * gZoom.zoom; }
Vec2 zoomedOrigin(const SDL_FRect& plan, float w, float h, float scale) {
    return {plan.x + (plan.w - w * scale) / 2.0f + gZoom.panX, plan.y + (plan.h - h * scale) / 2.0f + gZoom.panY};
}

void drawZoomButtons(SDL_Renderer* renderer, const SDL_FRect& plan, float mouseX, float mouseY);  // below

// The panel: the whole instruction at once on the same datapath as the X-ray, every value where it
// arrived. The program, the registers and the memory are in the debugger beside it.
// The step's explanation, in a column of its own beside the diagram: as long as it needs to be.
void drawStepColumn(SDL_Renderer* renderer, const SDL_FRect& side, int stage, const std::string& text, Color tone) {
    fillRound(renderer, side, 10, kPanel);
    frameRound(renderer, side, 10, 1.0f, mixed(kPanel, tone, 0.5f));
    const float x = side.x + 16.0f;
    const float width = side.w - 32.0f;
    char heading[64];
    std::snprintf(heading, sizeof heading, "%d  %s", stage + 1, i18n::tr(kTraceStageNames[stage]));
    write(renderer, Face::Medium, 16.0f, x, side.y + 28.0f, heading, tone);
    float y = side.y + 56.0f;
    const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 14.5f, text, width);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (y > side.y + side.h - 12.0f) break;
        const bool last = y + 21.0f > side.y + side.h - 12.0f && i + 1 < lines.size();
        write(renderer, Face::Regular, 14.5f, x, y, last ? fitted(renderer, Face::Regular, 14.5f, lines[i] + " …", width) : lines[i], kText);
        y += 21.0f;
    }
}

void drawComputerPanel(SDL_Renderer* renderer, const ComputerState& state, const SDL_FRect& area, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const SDL_FRect next = panelNextButton(area);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Step, {next.x + 20.0f, next.y + next.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, next.x + 36.0f, next.y + 21.0f, "Próxima instrução", kText);
    const float sentenceX = next.x + next.w + 18.0f;
    write(renderer, Face::Regular, 15, sentenceX, next.y + 22.0f, fitted(renderer, Face::Regular, 15, explainInstruction(state), area.x + area.w - sentenceX - 16.0f), kText);
    drawXrayPlan(renderer, state, panelPlanArea(area), kTraceStages - 1, 100.0f);
    drawZoomButtons(renderer, panelPlanArea(area), mouseX, mouseY);
}

// --- The X-ray: the instruction about to run going through the CPU, with the value on each wire ------
// Drawn on a 980 by 450 plan scaled to fit. Each block and wire belongs to one of the six steps, and
// lights up when the animation reaches it, if this instruction uses it at all.

// The colours of an instruction's fields: the strip of bits, the help, and the values on the wires that
// come from each field share them, so a number can be followed from the word to where it goes.
constexpr Color kFieldOp{232, 92, 92};
constexpr Color kFieldRd{160, 150, 255};
constexpr Color kFieldRs{110, 226, 140};
constexpr Color kFieldRt{96, 200, 236};
constexpr Color kFieldFunc{244, 156, 84};
constexpr Color kFieldImm{240, 210, 80};
constexpr Color kFieldUnused{92, 104, 112};

enum XrayBlock : int { kXNext, kXPc, kXInc, kXRom, kXDecoder, kXControl, kXRegisters, kXExtender, kXMuxB, kXAlu, kXBranch, kXOutput, kXRam, kXSwitches, kXWriteBack, kXMuxA, kXZero };

struct XrayBlockShape {
    XrayBlock block;
    int stage;
    SDL_FRect rect;
    const char* name;
    const char* detail;
};

constexpr XrayBlockShape kXrayBlocks[] = {
    // Próximo PC is a multiplexer, its inputs coming in on top; PC + 1 an adder, drawn like the ALU.
    {kXNext, 5, {16, 112, 92, 40}, "Próximo PC", ""},     {kXPc, 0, {34, 196, 56, 44}, "PC", ""},
    {kXInc, 0, {136, 114, 40, 36}, "PC + 1", ""},         {kXRom, 0, {136, 184, 92, 84}, "ROM", "instruções"},
    {kXDecoder, 1, {262, 164, 96, 160}, "Decodificador", ""}, {kXControl, 1, {262, 380, 110, 46}, "Controle", ""},
    {kXRegisters, 2, {400, 164, 140, 140}, "Registradores", ""}, {kXExtender, 1, {400, 326, 140, 34}, "Extensor", ""},
    {kXMuxB, 2, {574, 232, 26, 76}, "", ""},               {kXAlu, 2, {626, 176, 74, 124}, "ULA", ""},
    {kXBranch, 5, {626, 98, 74, 36}, "Alvo do desvio", "PC+1+imm"},      {kXOutput, 2, {740, 104, 96, 36}, "Saída", ""},
    {kXRam, 3, {740, 220, 100, 88}, "RAM", "dados"},       {kXSwitches, 4, {740, 336, 100, 30}, "Chaves", ""},
    {kXWriteBack, 4, {884, 176, 26, 150}, "", ""},
    // In front of the ALU's A: the register, or a 0 that is always there, as in the real CPU's circuit.
    {kXMuxA, 2, {574, 166, 26, 48}, "", ""},             {kXZero, 2, {558, 172, 14, 16}, "0", ""},
};

enum XrayWire : int {
    kWPcRom, kWPcInc, kWRomDecoder, kWDecoderControl, kWDecoderA, kWDecoderB, kWDecoderWrite, kWDecoderExtender, kWAAlu, kWBMux,
    kWExtenderMux, kWMuxAlu, kWZero, kWAOutput, kWAddress, kWStoreValue, kWLoaded, kWAluBack, kWLinkBack, kWSwitchesBack,
    kWBackRegisters, kWIncNext, kWIncBranch, kWImmBranch, kWBranchNext, kWJump, kWJumpRegister, kWNextPc,
    kWSelectB, kWZeroA, kWSelectBack,  // the control's lines: what each multiplexer picks
    kWMuxAAlu,
    kWSelectNext,  // the control's line to the Next PC multiplexer: branch if equal, if different, jump
    // The rest of the control's lines: the ALU's operation, writing a register, which immediate, writing the RAM,
    // writing the output.
    kWAluOp, kWRegWrite, kWImmSelect, kWMemWrite, kWOutWrite,
};

// The control's lines, drawn in a colour of their own, as the textbooks do: they carry orders, not data.
constexpr Color kToneControl{112, 166, 255};

bool xrayControlLine(XrayWire wire) {
    switch (wire) {
        case kWSelectB:
        case kWZeroA:
        case kWSelectBack:
        case kWSelectNext:
        case kWAluOp:
        case kWRegWrite:
        case kWImmSelect:
        case kWMemWrite:
        case kWOutWrite: return true;
        default: return false;
    }
}

struct XrayWireShape {
    XrayWire wire;
    int stage;
    std::vector<Vec2> points;
    Vec2 label;  // where the value's tag goes
};

const std::vector<XrayWireShape>& xrayWires() {
    static const std::vector<XrayWireShape> wires{
        {kWPcRom, 0, {{90, 218}, {136, 218}}, {113, 232}},
        {kWPcInc, 0, {{100, 218}, {100, 140}, {136, 140}}, {-100, 0}},
        {kWRomDecoder, 0, {{228, 226}, {262, 226}}, {245, 212}},
        {kWDecoderControl, 1, {{310, 324}, {310, 380}}, {310, 352}},
        {kWDecoderA, 1, {{358, 190}, {400, 190}}, {379, 190}},
        {kWDecoderB, 1, {{358, 214}, {400, 214}}, {379, 214}},
        {kWDecoderWrite, 1, {{358, 238}, {400, 238}}, {379, 238}},
        {kWDecoderExtender, 1, {{358, 300}, {380, 300}, {380, 343}, {400, 343}}, {380, 321}},
        {kWAAlu, 2, {{540, 196}, {574, 196}}, {562, 196}},
        {kWMuxAAlu, 2, {{600, 190}, {626, 190}}, {613, 190}},
        {kWBMux, 2, {{540, 248}, {574, 248}}, {556, 262}},
        {kWExtenderMux, 2, {{540, 343}, {556, 343}, {556, 292}, {574, 292}}, {556, 318}},
        {kWMuxAlu, 2, {{600, 270}, {626, 270}}, {613, 284}},
        // Z, from the ALU's bottom edge down to the Control, whose gates join it with the kind of branch.
        {kWZero, 2, {{663, 283}, {663, 436}, {317, 436}, {317, 426}}, {663, 340}},
        {kWAOutput, 2, {{540, 196}, {552, 196}, {552, 122}, {740, 122}}, {700, 122}},
        {kWAddress, 3, {{716, 238}, {716, 250}, {740, 250}}, {716, 266}},
        {kWStoreValue, 3, {{562, 248}, {562, 380}, {728, 380}, {728, 290}, {740, 290}}, {650, 380}},
        {kWLoaded, 3, {{840, 264}, {884, 264}}, {862, 250}},
        {kWAluBack, 4, {{700, 238}, {716, 238}, {716, 200}, {884, 200}}, {800, 200}},
        {kWLinkBack, 4, {{176, 132}, {188, 132}, {188, 84}, {870, 84}, {870, 300}, {884, 300}}, {800, 84}},  // from PC + 1, like the branch's
        {kWSwitchesBack, 4, {{840, 351}, {858, 351}, {858, 316}, {884, 316}}, {-100, 0}},
        {kWBackRegisters, 4, {{910, 250}, {940, 250}, {940, 152}, {470, 152}, {470, 164}}, {760, 152}},
        {kWIncNext, 5, {{176, 132}, {188, 132}, {188, 100}, {90, 100}, {90, 112}}, {124, 100}},
        {kWIncBranch, 5, {{176, 132}, {188, 132}, {188, 84}, {600, 84}, {600, 108}, {626, 108}}, {450, 84}},  // from PC + 1 itself
        {kWImmBranch, 5, {{540, 335}, {618, 335}, {618, 124}, {626, 124}}, {618, 310}},  // from the extender, up to the branch adder
        {kWBranchNext, 5, {{700, 116}, {716, 116}, {716, 26}, {46, 26}, {46, 112}}, {420, 26}},
        {kWJump, 5, {{340, 164}, {340, 44}, {66, 44}, {66, 112}}, {220, 44}},
        {kWJumpRegister, 5, {{540, 196}, {548, 196}, {548, 14}, {28, 14}, {28, 112}}, {300, 14}},  // from the A port itself
        {kWNextPc, 5, {{62, 152}, {62, 196}}, {62, 174}},
        {kWSelectB, 1, {{372, 392}, {587, 392}, {587, 302}}, {480, 392}},
        {kWZeroA, 1, {{372, 402}, {610, 402}, {610, 224}, {587, 224}, {587, 210}}, {610, 375}},  // up to MUX A's selection
        {kWSelectBack, 1, {{372, 412}, {897, 412}, {897, 320}}, {897, 370}},
        // From the control, around the left, into the side of the Next PC multiplexer: branch or not, jump.
        {kWSelectNext, 5, {{262, 416}, {8, 416}, {8, 132}, {22, 132}}, {150, 416}},
        // From the control's right side up to the ALU's bottom edge: which operation to do.
        {kWAluOp, 1, {{372, 422}, {682, 422}, {682, 274}}, {682, 340}},
        // Around the extender, up into the registers: write the value coming back.
        {kWRegWrite, 1, {{372, 386}, {390, 386}, {390, 314}, {470, 314}, {470, 304}}, {430, 314}},
        // From the control's top into the extender: which of the instruction's numbers to take.
        {kWImmSelect, 1, {{350, 380}, {350, 370}, {470, 370}, {470, 360}}, {410, 370}},
        // Along the bottom and up beside the switches, into the RAM: write it on the clock.
        {kWMemWrite, 1, {{340, 426}, {340, 442}, {848, 442}, {848, 322}, {800, 322}, {800, 308}}, {600, 442}},
        // Along the bottom, up the right edge and over into the output: keep this number.
        {kWOutWrite, 1, {{330, 426}, {330, 448}, {960, 448}, {960, 96}, {790, 96}, {790, 104}}, {960, 300}},
    };
    return wires;
}

// A value on a wire, in the base the X-ray is showing.
std::string xrayHex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", static_cast<unsigned>(value));
    return text;
}
std::string xrayNumber(std::uint16_t value) { return gZoom.hex ? xrayHex(value) : std::to_string(value); }
std::string xraySigned(std::uint16_t value) { return gZoom.hex ? xrayHex(value) : std::to_string(static_cast<std::int16_t>(value)); }

// Whether this instruction uses a wire at all, and what's on it.
bool xrayWireUsed(const InstructionTrace& t, XrayWire wire) {
    using WriteBack = InstructionTrace::WriteBack;
    using Next = InstructionTrace::Next;
    switch (wire) {
        case kWPcRom:
        case kWPcInc:
        case kWRomDecoder:
        case kWDecoderControl:
        case kWNextPc: return true;
        case kWDecoderA: return t.readsA;
        case kWDecoderB: return t.readsB;
        case kWDecoderWrite: return t.writeBack != WriteBack::None;
        case kWDecoderExtender: return t.usesImmediate && t.op != assembly::kOpJmp && t.op != assembly::kOpCall;
        case kWAAlu: return t.usesAlu && !t.aluZeroesA;
        case kWMuxAAlu: return t.usesAlu;
        case kWSelectB: return xrayWireUsed(t, kWMuxAlu);
        case kWZeroA: return t.usesAlu;
        case kWSelectBack: return t.writeBack != InstructionTrace::WriteBack::None;
        case kWSelectNext: return t.branches || t.next == Next::Jump || t.next == Next::Register;
        case kWAluOp: return t.usesAlu;
        case kWRegWrite: return t.writeBack != WriteBack::None;
        case kWImmSelect: return xrayWireUsed(t, kWDecoderExtender);
        case kWMemWrite: return t.writesMemory;
        case kWOutWrite: return t.shows;
        case kWBMux: return t.usesAlu && !t.aluTakesImmediate && t.readsB;
        case kWExtenderMux: return t.aluTakesImmediate;
        case kWMuxAlu: return t.usesAlu && (t.aluTakesImmediate || t.readsB);
        case kWZero: return t.branches;
        case kWAOutput: return t.shows;
        case kWAddress: return t.readsMemory || t.writesMemory;
        case kWStoreValue: return t.writesMemory;
        case kWLoaded: return t.readsMemory;
        case kWAluBack: return t.writeBack == WriteBack::Alu;
        case kWLinkBack: return t.writeBack == WriteBack::Link;
        case kWSwitchesBack: return t.writeBack == WriteBack::Switches;
        case kWBackRegisters: return t.writeBack != WriteBack::None;
        case kWIncNext: return t.next == Next::Plain;
        case kWIncBranch:
        case kWImmBranch: return t.branches;
        case kWBranchNext: return t.branches && t.branchTaken;
        case kWJump: return t.next == Next::Jump;
        case kWJumpRegister: return t.next == Next::Register;
    }
    return false;
}

std::string xrayWireValue(const InstructionTrace& t, XrayWire wire) {
    const bool signedImmediate = std::string_view(t.immediateField) == "imm6";
    const std::string immediate = signedImmediate ? xraySigned(t.immediate) : xrayNumber(t.immediate);
    switch (wire) {
        case kWPcRom: return xrayNumber(t.pc);
        case kWRomDecoder: {
            char text[10];
            std::snprintf(text, sizeof text, "0x%04X", static_cast<unsigned>(t.word));  // the word, in hexadecimal
            return text;
        }
        case kWDecoderControl: return "op " + std::to_string(t.op);
        case kWDecoderA: return "R" + std::to_string(t.registerA);
        case kWDecoderB: return "R" + std::to_string(t.registerB);
        case kWDecoderWrite: return "R" + std::to_string(t.writtenRegister);
        case kWDecoderExtender:
        case kWExtenderMux:
        case kWImmBranch: return "imm " + immediate;  // the number written in the instruction
        case kWAAlu:
        case kWMuxAAlu: return xrayNumber(t.aluA);
        case kWSelectB: return t.aluTakesImmediate ? "UsaImm 1" : "UsaImm 0";
        case kWZeroA: return t.aluZeroesA ? "ZeraA 1" : "ZeraA 0";
        case kWSelectBack:
            switch (t.writeBack) {
                case InstructionTrace::WriteBack::Alu: return i18n::tr("escolhe ULA");
                case InstructionTrace::WriteBack::Memory: return i18n::tr("escolhe RAM");
                case InstructionTrace::WriteBack::Link: return i18n::tr("escolhe PC+1");
                case InstructionTrace::WriteBack::Switches: return i18n::tr("escolhe chaves");
                case InstructionTrace::WriteBack::None: break;
            }
            return "";
        case kWBMux: return xrayNumber(t.valueB);
        case kWMuxAlu: return t.aluTakesImmediate ? "imm " + immediate : xrayNumber(t.aluB);
        case kWZero: return t.aluResult == 0 ? "Z = 1" : "Z = 0";
        case kWSelectNext:
            if (t.branches) return i18n::tr(t.branchTaken ? "desvia" : "não desvia");
            if (t.next == InstructionTrace::Next::Jump) return i18n::tr("salta");
            if (t.next == InstructionTrace::Next::Register) return i18n::tr("salta p/ reg");
            return "";
        case kWAOutput: return xrayNumber(t.shown);
        case kWAluOp: {
            // The operation by the name the instructions give it.
            static constexpr const char* kNames[] = {"AND", "OR", "XOR", "ADD", "SUB", "NOT", "SHL", "SHR"};
            unsigned code = assembly::kAdd;
            if (t.op == assembly::kOpAlu) code = t.func;
            if (t.op == assembly::kOpBeq || t.op == assembly::kOpBne) code = assembly::kSub;
            return std::string("OpULA ") + kNames[code & 7u];
        }
        case kWRegWrite: return "RegEsc 1";
        case kWImmSelect: {
            const std::string_view field(t.immediateField);
            return std::string("QualImm ") + (field == "imm6" ? "imm6" : field == "imm9" ? "imm9" : "ImmH");
        }
        case kWMemWrite: return "MemEsc 1";
        case kWOutWrite: return "SaiEsc 1";
        case kWAddress: return i18n::tr("end ") + xrayNumber(t.address);
        case kWStoreValue:
        case kWLoaded: return xrayNumber(t.memoryValue);
        case kWAluBack: return xrayNumber(t.aluResult);
        case kWLinkBack: return i18n::tr("volta ") + xrayNumber(t.pcPlusOne);  // the address to come back to
        case kWIncNext:
        case kWIncBranch: return "PC+1 " + xrayNumber(t.pcPlusOne);  // the address of the next instruction
        case kWSwitchesBack: return xrayNumber(t.writtenValue);
        // The tags use the monospaced font, which has no arrows: words instead.
        case kWBackRegisters: return "R" + std::to_string(t.writtenRegister) + " = " + xrayNumber(t.writtenValue);
        case kWBranchNext: return i18n::tr("alvo ") + xrayNumber(t.branchTarget);
        case kWJump:
        case kWJumpRegister: return i18n::tr("alvo ") + xrayNumber(t.nextPc);
        case kWNextPc: return xrayNumber(t.nextPc);
        case kWPcInc: break;
    }
    return "";
}

// The value on a wire that comes straight from a field of the instruction wears that field's colour.
Color xrayWireColor(const InstructionTrace& t, XrayWire wire, Color tone) {
    switch (wire) {
        case kWDecoderControl: return kFieldOp;
        case kWDecoderA: return kFieldRs;
        case kWDecoderB: return t.branches || t.op == assembly::kOpSt ? kFieldRd : kFieldRt;
        case kWDecoderWrite: return kFieldRd;
        case kWDecoderExtender:
        case kWExtenderMux:
        case kWImmBranch:
        case kWJump: return kFieldImm;
        case kWMuxAlu: return t.aluTakesImmediate ? kFieldImm : tone;
        default: return xrayControlLine(wire) ? kToneControl : tone;
    }
}

// A multiplexer's other inputs: they reach it, but it isn't taking them this time.
bool xrayWireOffered(const InstructionTrace& t, XrayWire wire) {
    const bool choosingB = xrayWireUsed(t, kWMuxAlu);
    const bool writing = t.writeBack != InstructionTrace::WriteBack::None;
    switch (wire) {
        case kWBMux:
        case kWExtenderMux: return choosingB;
        case kWAAlu: return t.usesAlu;
        case kWAluBack:
        case kWLoaded:
        case kWLinkBack:
        case kWSwitchesBack: return writing;
        default: return false;
    }
}

bool xrayBlockUsed(const InstructionTrace& t, XrayBlock block) {
    switch (block) {
        case kXRegisters: return t.readsA || t.readsB || t.writeBack != InstructionTrace::WriteBack::None;
        case kXExtender: return xrayWireUsed(t, kWDecoderExtender);
        case kXMuxB: return xrayWireUsed(t, kWMuxAlu);
        case kXMuxA: return t.usesAlu;
        case kXZero: return t.aluZeroesA;
        case kXAlu: return t.usesAlu;
        case kXBranch: return t.branches;
        case kXOutput: return t.shows;
        case kXRam: return t.readsMemory || t.writesMemory;
        case kXSwitches: return t.writeBack == InstructionTrace::WriteBack::Switches;
        case kXWriteBack: return t.writeBack != InstructionTrace::WriteBack::None;
        default: return true;
    }
}

// The line of the program the instruction came from, without its comment, or the disassembly.
std::string xraySourceLine(const ComputerState& state, const InstructionTrace& trace) {
    if (state.source.empty() || state.pc >= state.lineOfWord.size()) return trace.text;
    int line = state.lineOfWord[state.pc];
    std::size_t start = 0;
    while (--line > 0 && start != std::string::npos) {
        start = state.source.find('\n', start);
        if (start != std::string::npos) ++start;
    }
    if (start == std::string::npos) return trace.text;
    std::string text = state.source.substr(start, state.source.find('\n', start) - start);
    if (const std::size_t comment = text.find(';'); comment != std::string::npos) text.resize(comment);
    std::string tidy;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!tidy.empty() && tidy.back() != ' ') tidy += ' ';
        } else {
            tidy += c;
        }
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy.empty() ? trace.text : tidy;
}

// Each kind of instruction has its colour, on the wires it lights, its blocks and its line in the program.
Color instructionTone(const InstructionTrace& t) {
    if (t.halts) return kToneHalt;
    if (t.notAnInstruction) return kBodyEdge;
    switch (t.op) {
        case assembly::kOpAlu: return kToneAlu;
        case assembly::kOpAddi:
        case assembly::kOpLi:
        case assembly::kOpLui: return kToneNumber;
        case assembly::kOpLd:
        case assembly::kOpSt: return kToneMemory;
        case assembly::kOpBeq:
        case assembly::kOpBne: return kToneBranch;
        case assembly::kOpJmp:
        case assembly::kOpCall:
        case assembly::kOpJr: return kToneJump;
        default: return kToneWorld;
    }
}

const char* instructionKind(const InstructionTrace& t) {
    if (t.halts) return "parada";
    if (t.notAnInstruction) return "não é instrução";
    switch (t.op) {
        case assembly::kOpAlu: return "conta na ULA";
        case assembly::kOpAddi:
        case assembly::kOpLi:
        case assembly::kOpLui: return "número da instrução";
        case assembly::kOpLd:
        case assembly::kOpSt: return "memória";
        case assembly::kOpBeq:
        case assembly::kOpBne: return "desvio";
        case assembly::kOpJmp:
        case assembly::kOpCall:
        case assembly::kOpJr: return "salto";
        default: return "entrada e saída";
    }
}

Color mixed(Color a, Color b, float t) {
    const auto blend = [&](Uint8 x, Uint8 y) { return static_cast<Uint8>(std::lround(static_cast<float>(x) * (1.0f - t) + static_cast<float>(y) * t)); };
    return {blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b)};
}

// A point a distance along a path of screen points, stopping at its end.
Vec2 alongPath(const std::vector<Vec2>& points, float distance) {
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[i + 1];
        const float piece = std::hypot(b.x - a.x, b.y - a.y);
        if (distance <= piece && piece > 0.0f) return {a.x + (b.x - a.x) * distance / piece, a.y + (b.y - a.y) * distance / piece};
        distance -= piece;
    }
    return points.back();
}

// The datapath: every block and wire of the CPU, the ones this instruction uses lit in its colour up to
// the step shown. Each value travels along its wire as a little block during its step, and then stays
// where it arrived, just before the block it went into: what leaves each block, and what comes in.
void drawXrayPlan(SDL_Renderer* renderer, const ComputerState& state, const SDL_FRect& plan, int stage, float progress) {
    const InstructionTrace trace = traceInstruction(state);
    const bool running = !state.halted || traceInstruction(state).halts;  // the HLT itself still shows its way
    const Color tone = instructionTone(trace);
    fillRound(renderer, plan, 10, kPanel);
    const float scale = zoomedScale(plan, 980.0f, 450.0f);
    const Vec2 origin = zoomedOrigin(plan, 980.0f, 450.0f, scale);
    const SDL_Rect clip{static_cast<int>(plan.x), static_cast<int>(plan.y), static_cast<int>(plan.w), static_cast<int>(plan.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const auto at = [&](Vec2 p) { return Vec2{origin.x + p.x * scale, origin.y + p.y * scale}; };
    const float text = std::clamp(12.5f * scale, 9.5f, 15.0f * gZoom.zoom);
    const float tagSize = std::clamp(11.5f * scale, 9.0f, 13.5f * gZoom.zoom);
    const float travel = std::clamp(progress / 1.1f, 0.0f, 1.0f);

    Mesh unused;
    Mesh waiting;
    Mesh lit;
    struct Packet {
        Vec2 where;
        std::string text;
        bool moving;
        bool now;
        Color color;
    };
    std::vector<Packet> packets;
    for (const XrayWireShape& wire : xrayWires()) {
        const bool used = running && xrayWireUsed(trace, wire.wire);
        const bool on = used && wire.stage <= stage;
        const bool offered = running && !used && xrayWireOffered(trace, wire.wire);
        Mesh& mesh = offered ? waiting : !used ? unused : on ? lit : waiting;
        std::vector<Vec2> points;
        float length = 0.0f;
        for (const Vec2 p : wire.points) points.push_back(at(p));
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            length += std::hypot(points[i + 1].x - points[i].x, points[i + 1].y - points[i].y);
            if (offered) {
                mesh.addLine(points[i], points[i + 1], 1.8f, kTextDim, 0.75f);
                continue;
            }
            const Color lineTone = xrayControlLine(wire.wire) ? kToneControl : tone;
            mesh.addLine(points[i], points[i + 1], on ? 2.8f : 1.4f, on ? lineTone : kBodyEdge, !used ? 0.2f : on ? 1.0f : 0.9f);
        }
        if (!on) continue;
        const std::string value = xrayWireValue(trace, wire.wire);
        if (value.empty()) continue;
        const float half = (textWidth(renderer, Face::Mono, tagSize, value) + 10.0f) / 2.0f;
        // The word from the ROM is the one value worth seeing on a wire too short for its tag: above the wire.
        if (half * 2.0f > length && wire.wire == kWRomDecoder) {
            const Vec2 middle = alongPath(points, length / 2.0f);
            const bool now = wire.stage == stage;
            packets.push_back({{middle.x, middle.y - tagSize * 1.7f}, value, now && travel < 1.0f, now, xrayWireColor(trace, wire.wire, tone)});
            continue;
        }
        // A wire shorter than its tag, between a multiplexer and the ALU, keeps the value its input already shows.
        if (half * 2.0f > length) continue;
        float parked = std::max(0.0f, length - half - 5.0f);
        if (wire.wire == kWZeroA || wire.wire == kWImmBranch || wire.wire == kWZero || wire.wire == kWSelectB || wire.wire == kWSelectNext ||
            wire.wire == kWAluOp || wire.wire == kWRegWrite || wire.wire == kWImmSelect || wire.wire == kWMemWrite || wire.wire == kWOutWrite) {
            // These end among the ALU's multiplexers, where there's no room: the tag waits at its own spot on the way.
            float along = 0.0f;
            for (std::size_t i = 0; i + 1 < wire.points.size(); ++i) {
                const Vec2 a = wire.points[i];
                const Vec2 b = wire.points[i + 1];
                const bool onIt = (a.x == b.x && wire.label.x == a.x && wire.label.y >= std::min(a.y, b.y) && wire.label.y <= std::max(a.y, b.y)) ||
                                  (a.y == b.y && wire.label.y == a.y && wire.label.x >= std::min(a.x, b.x) && wire.label.x <= std::max(a.x, b.x));
                if (onIt) {
                    parked = (along + std::abs(wire.label.x - a.x) + std::abs(wire.label.y - a.y)) * scale;
                    break;
                }
                along += std::abs(b.x - a.x) + std::abs(b.y - a.y);
            }
        }
        const bool now = wire.stage == stage;
        const bool moving = now && travel < 1.0f;
        packets.push_back({alongPath(points, moving ? parked * travel : parked), value, moving, now, xrayWireColor(trace, wire.wire, tone)});
    }
    unused.flush(renderer);
    waiting.flush(renderer);
    lit.flush(renderer);

    Mesh shapes;
    for (const XrayBlockShape& shape : kXrayBlocks) {
        const bool used = running && xrayBlockUsed(trace, shape.block);
        const bool on = used && shape.stage <= stage;
        const bool now = on && shape.stage == stage;
        const Vec2 corner = at({shape.rect.x, shape.rect.y});
        const SDL_FRect r{corner.x, corner.y, shape.rect.w * scale, shape.rect.h * scale};
        const Color fill = on ? mixed(kPanelRaised, tone, now ? 0.34f : 0.2f) : used ? kPanelRaised : kBackground;
        const Color edge = on ? tone : used ? kBodyEdge : kRule;
        const float frame = now ? 2.4f : on ? 1.4f : 1.0f;
        const Color ink = on ? kText : used ? kTextDim : kTextOff;
        if (shape.block == kXMuxA || shape.block == kXMuxB || shape.block == kXWriteBack) {
            const float dip = 10.0f * scale;
            const std::vector<Vec2> outline{{r.x, r.y}, {r.x + r.w, r.y + dip}, {r.x + r.w, r.y + r.h - dip}, {r.x, r.y + r.h}};
            shapes.addFan(outline, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, fill);
            addLoop(shapes, outline, frame, edge);
            const float small = std::clamp(10.5f * scale, 8.5f, 12.0f);
            if (shape.block != kXWriteBack) {
                shapes.flush(renderer);
                // The two in front of the ALU: named inside, by the input they feed, with nothing above to tangle with the wires.
                const float tiny = std::clamp(8.5f * scale, 7.5f, 10.0f);
                const float muxWidth = textWidth(renderer, Face::Medium, tiny, "MUX");
                write(renderer, Face::Medium, tiny, r.x + (r.w - muxWidth) / 2.0f, r.y + r.h / 2.0f - 1.0f * scale, "MUX", ink);
                const char* letter = shape.block == kXMuxA ? "A" : "B";
                const float letterWidth = textWidth(renderer, Face::Medium, small, letter);
                write(renderer, Face::Medium, small, r.x + (r.w - letterWidth) / 2.0f, r.y + r.h / 2.0f + small * 1.1f, letter, ink);
                continue;
            }
            // Too narrow for a name inside: it goes above, with what it chooses over it.
            const char* what = "o que volta";
            const float nameWidth = textWidth(renderer, Face::Medium, small, "MUX");
            write(renderer, Face::Medium, small, r.x + (r.w - nameWidth) / 2.0f, r.y - 6.0f * scale, "MUX", ink);
            // What it chooses, above the name, in short lines so it stays clear of the blocks beside it.
            const std::vector<std::string> whatLines = wrapped(renderer, Face::Regular, small * 0.9f, what, 64.0f * scale);
            for (std::size_t i = 0; i < whatLines.size(); ++i) {
                const float lineWidth = textWidth(renderer, Face::Regular, small * 0.9f, whatLines[i]);
                const float lineY = r.y - 6.0f * scale - small * 1.25f * static_cast<float>(whatLines.size() - i);
                write(renderer, Face::Regular, small * 0.9f, r.x + (r.w - lineWidth) / 2.0f, lineY, whatLines[i], kTextDim);
            }
            continue;
        }
        // The two adders, drawn as adders: the ALU's shape, a + inside, and their name under them.
        if (shape.block == kXInc || shape.block == kXBranch) {
            const std::vector<Vec2> outline{{r.x, r.y},
                                            {r.x + r.w, r.y + r.h * 0.28f},
                                            {r.x + r.w, r.y + r.h * 0.72f},
                                            {r.x, r.y + r.h},
                                            {r.x, r.y + r.h * 0.6f},
                                            {r.x + r.w * 0.22f, r.y + r.h * 0.5f},
                                            {r.x, r.y + r.h * 0.4f}};
            shapes.addFan(outline, {r.x + r.w * 0.62f, r.y + r.h * 0.5f}, fill);
            addLoop(shapes, outline, frame, edge);
            shapes.flush(renderer);
            const char* sign = shape.block == kXInc ? "+1" : "+";
            const float size = std::clamp(15.0f * scale, 11.0f, 19.0f);
            const float w = textWidth(renderer, Face::Medium, size, sign);
            write(renderer, Face::Medium, size, r.x + r.w * 0.6f - w / 2.0f, r.y + r.h * 0.5f + size * 0.36f, sign, on ? tone : ink);
            const float small = std::clamp(10.0f * scale, 8.5f, 11.5f);
            const float nameWidth = textWidth(renderer, Face::Medium, small, shape.name);
            write(renderer, Face::Medium, small, r.x + r.w * 0.5f - nameWidth / 2.0f, r.y + r.h + small * 1.35f, shape.name, ink);
            if (shape.detail[0] != '\0') {
                const float detailWidth = textWidth(renderer, Face::Mono, small * 0.95f, shape.detail);
                write(renderer, Face::Mono, small * 0.95f, r.x + r.w * 0.5f - detailWidth / 2.0f, r.y + r.h + small * 2.6f, shape.detail, kTextDim);
            }
            continue;
        }
        // The next PC is a multiplexer: four inputs on top, the PC under it, and Z at its side.
        if (shape.block == kXNext) {
            const float in = 14.0f * scale;
            const std::vector<Vec2> outline{{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w - in, r.y + r.h}, {r.x + in, r.y + r.h}};
            shapes.addFan(outline, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, fill);
            addLoop(shapes, outline, frame, edge);
            shapes.flush(renderer);
            const float small = std::clamp(10.0f * scale, 8.5f, 11.5f);
            const float muxWidth = textWidth(renderer, Face::Medium, small, "MUX");
            write(renderer, Face::Medium, small, r.x + (r.w - muxWidth) / 2.0f, r.y + r.h * 0.45f, "MUX", ink);
            const float nameWidth = textWidth(renderer, Face::Medium, small, shape.name);
            write(renderer, Face::Medium, small, r.x + (r.w - nameWidth) / 2.0f, r.y + r.h * 0.45f + small * 1.3f, shape.name, ink);
            continue;
        }
        if (shape.block == kXAlu) {
            const std::vector<Vec2> outline{{r.x, r.y},
                                            {r.x + r.w, r.y + r.h * 0.28f},
                                            {r.x + r.w, r.y + r.h * 0.72f},
                                            {r.x, r.y + r.h},
                                            {r.x, r.y + r.h * 0.6f},
                                            {r.x + r.w * 0.22f, r.y + r.h * 0.5f},
                                            {r.x, r.y + r.h * 0.4f}};
            shapes.addFan(outline, {r.x + r.w * 0.62f, r.y + r.h * 0.5f}, fill);
            addLoop(shapes, outline, frame, edge);
            shapes.flush(renderer);
            const char* operation = on ? trace.aluOperation : "";  // the ALU learns its operation in Executa
            const float size = std::clamp(20.0f * scale, 13.0f, 24.0f);
            const float w = textWidth(renderer, Face::Medium, size, operation);
            write(renderer, Face::Medium, size, r.x + r.w * 0.6f - w / 2.0f, r.y + r.h * 0.5f + size * 0.36f, operation, on ? tone : ink);
            const float nameWidth = textWidth(renderer, Face::Medium, text, shape.name);
            write(renderer, Face::Medium, text, r.x + r.w * 0.6f - nameWidth / 2.0f, r.y + r.h * 0.5f - size * 0.9f, shape.name, ink);
            continue;
        }
        shapes.flush(renderer);
        fillRound(renderer, r, 7, fill);
        frameRound(renderer, r, 7, frame, edge);

        if (shape.block == kXDecoder && on) {  // the word is only cut into fields in Decodifica
            const float x = r.x + 8.0f * scale;
            write(renderer, Face::Medium, text, x, r.y + 20.0f * scale, shape.name, ink);
            char line[48];
            float y = r.y + 80.0f * scale;
            const auto field = [&](const char* format, auto value) {
                std::snprintf(line, sizeof line, format, value);
                write(renderer, Face::Mono, std::clamp(11.5f * scale, 9.0f, 13.0f), x, y, line, ink);
                y += 17.0f * scale;
            };
            field("op %u", trace.op);
            field("rd %u", trace.rd);
            field("rs %u", trace.rs);
            if (trace.usesImmediate) {
                const std::string value = std::string_view(trace.immediateField) == "imm6" ? xraySigned(trace.immediate) : xrayNumber(trace.immediate);
                field("imm %s", value.c_str());
            } else {
                field("rt %u", trace.rt);
            }
            continue;
        }
        const bool tall = r.h > 60.0f * scale;
        const float nameWidth = textWidth(renderer, Face::Medium, text, shape.name);
        const float nameY = tall ? r.y + r.h / 2.0f - 4.0f * scale : r.y + r.h / 2.0f + text * 0.36f;
        write(renderer, Face::Medium, text, r.x + (r.w - nameWidth) / 2.0f, nameY, shape.name, ink);
        if (shape.detail[0] != '\0' && tall) {
            const float w = textWidth(renderer, Face::Regular, text * 0.9f, shape.detail);
            write(renderer, Face::Regular, text * 0.9f, r.x + (r.w - w) / 2.0f, nameY + text * 1.3f, shape.detail, kTextDim);
        }
        if (shape.block == kXRegisters) {
            const float small = std::clamp(10.5f * scale, 8.5f, 12.0f);
            write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 194}).y, "lê A", kTextDim);
            write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 218}).y, "lê B", kTextDim);
            write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 242}).y, "escreve", kTextDim);
            const float a = textWidth(renderer, Face::Regular, small, "A");
            write(renderer, Face::Regular, small, r.x + r.w - a - 5.0f * scale, at({0, 200}).y, "A", kTextDim);
            write(renderer, Face::Regular, small, r.x + r.w - a - 5.0f * scale, at({0, 252}).y, "B", kTextDim);
            const float d = textWidth(renderer, Face::Regular, small, "valor");
            write(renderer, Face::Regular, small, at({470, 0}).x - d / 2.0f, r.y + 14.0f * scale, "valor", kTextDim);
        }
        if (shape.block == kXControl && on) {
            std::string signals;
            std::string row = std::string(trace.name) + ":";  // the row of its ROM the op picked
            const auto add = [&](const char* name) { signals += signals.empty() ? std::string(i18n::tr(name)) : std::string(" · ") + i18n::tr(name); };
            if (trace.writeBack != InstructionTrace::WriteBack::None) add("escreve reg");
            if (trace.aluZeroesA) add("ZeraA");
            if (trace.aluTakesImmediate) add("usa imm");
            if (trace.readsMemory) add("lê RAM");
            if (trace.writesMemory) add("grava RAM");
            if (trace.branches) add(trace.op == assembly::kOpBeq ? "se igual" : "se diferente");
            if (trace.next == InstructionTrace::Next::Jump) add("salta");
            if (trace.next == InstructionTrace::Next::Register) add("salta p/ reg");
            if (trace.shows) add("saída");
            if (trace.halts) add("para");
            const float small = std::clamp(10.0f * scale, 8.5f, 11.5f);
            write(renderer, Face::Medium, small, r.x + 6.0f * scale, r.y + 14.0f * scale, row, tone);
            write(renderer, Face::Regular, small, r.x + 6.0f * scale, r.y + r.h - 8.0f * scale,
                  fitted(renderer, Face::Regular, small, signals.empty() ? "nada" : signals, r.w - 12.0f * scale), on ? tone : kTextDim);
        }
        if (shape.block == kXOutput) {
            const std::string value = std::to_string(state.output);
            const float small = std::clamp(11.0f * scale, 9.0f, 13.0f);
            const float w = textWidth(renderer, Face::Mono, small, value);
            write(renderer, Face::Mono, small, r.x + r.w - w - 6.0f * scale, r.y + r.h / 2.0f + small * 0.36f, value, kTextDim);
        }
    }
    shapes.flush(renderer);

    // The values: travelling in the instruction's colour, or arrived, in a frame of it.
    for (const Packet& packet : packets) {
        const float w = textWidth(renderer, Face::Mono, tagSize, packet.text) + 10.0f;
        const SDL_FRect box{packet.where.x - w / 2.0f, packet.where.y - tagSize * 0.8f, w, tagSize * 1.6f};
        fillRound(renderer, box, 5, packet.moving ? packet.color : packet.now ? mixed(kPanelRaised, packet.color, 0.35f) : kPanelRaised);
        frameRound(renderer, box, 5, packet.now ? 1.6f : 1.0f, packet.color);
        write(renderer, Face::Mono, tagSize, box.x + 5.0f, packet.where.y + tagSize * 0.36f, packet.text, packet.moving ? kBackground : kText);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

// --- What an instruction is made of: its 16 bits, field by field, and the same fields in assembly ------

struct WordField {
    const char* name;  // "op", "rd", "imm6"... or "" for bits nobody reads
    int high;
    int low;
    Color color;
};


std::vector<WordField> wordFields(const InstructionTrace& t) {
    namespace a = assembly;
    const WordField op{"op", 15, 12, kFieldOp};
    switch (t.op) {
        case a::kOpAlu:
            if (t.func >= a::kNot) return {op, {"rd", 11, 9, kFieldRd}, {"rs", 8, 6, kFieldRs}, {"", 5, 3, kFieldUnused}, {"func", 2, 0, kFieldFunc}};
            return {op, {"rd", 11, 9, kFieldRd}, {"rs", 8, 6, kFieldRs}, {"rt", 5, 3, kFieldRt}, {"func", 2, 0, kFieldFunc}};
        case a::kOpAddi:
        case a::kOpLd:
        case a::kOpSt:
        case a::kOpBeq:
        case a::kOpBne: return {op, {"rd", 11, 9, kFieldRd}, {"rs", 8, 6, kFieldRs}, {"imm6", 5, 0, kFieldImm}};
        case a::kOpLi:
        case a::kOpLui: return {op, {"rd", 11, 9, kFieldRd}, {"imm9", 8, 0, kFieldImm}};
        case a::kOpJmp:
        case a::kOpCall: return {op, {"imm12", 11, 0, kFieldImm}};
        case a::kOpJr:
        case a::kOpOut: return {op, {"", 11, 9, kFieldUnused}, {"rs", 8, 6, kFieldRs}, {"", 5, 0, kFieldUnused}};
        case a::kOpIn: return {op, {"rd", 11, 9, kFieldRd}, {"", 8, 0, kFieldUnused}};
        default: return {op, {"", 11, 0, kFieldUnused}};
    }
}

struct AsmToken {
    std::string text;
    Color color;
};

// The instruction in assembly, each piece in the colour of the field it comes from.
std::vector<AsmToken> asmTokens(const InstructionTrace& t) {
    namespace a = assembly;
    const auto r = [](unsigned index) { return "R" + std::to_string(index); };
    const std::string imm6 = std::to_string(static_cast<std::int16_t>(static_cast<std::uint16_t>((t.word & 32u) != 0 ? (t.word & 63u) | 0xFFC0u : t.word & 63u)));
    const AsmToken name{t.name, kFieldOp};
    const AsmToken comma{", ", kTextDim};
    switch (t.op) {
        case a::kOpAlu:
            if (t.func >= a::kNot) return {name, {" ", kText}, {r(t.rd), kFieldRd}, comma, {r(t.rs), kFieldRs}};
            return {name, {" ", kText}, {r(t.rd), kFieldRd}, comma, {r(t.rs), kFieldRs}, comma, {r(t.rt), kFieldRt}};
        case a::kOpAddi:
        case a::kOpBeq:
        case a::kOpBne: return {name, {" ", kText}, {r(t.rd), kFieldRd}, comma, {r(t.rs), kFieldRs}, comma, {imm6, kFieldImm}};
        case a::kOpLd:
        case a::kOpSt: return {name, {" ", kText}, {r(t.rd), kFieldRd}, comma, {imm6, kFieldImm}, {"(", kTextDim}, {r(t.rs), kFieldRs}, {")", kTextDim}};
        case a::kOpLi:
        case a::kOpLui: return {name, {" ", kText}, {r(t.rd), kFieldRd}, comma, {std::to_string(t.word & 511u), kFieldImm}};
        case a::kOpJmp:
        case a::kOpCall: return {name, {" ", kText}, {std::to_string(t.word & 4095u), kFieldImm}};
        case a::kOpJr:
        case a::kOpOut: return {name, {" ", kText}, {r(t.rs), kFieldRs}};
        case a::kOpIn: return {name, {" ", kText}, {r(t.rd), kFieldRd}};
        default: return {name};
    }
}

constexpr float kWordStripHeight = 58.0f;

void drawWordStrip(SDL_Renderer* renderer, const InstructionTrace& t, const SDL_FRect& area, bool decoded) {
    const float cell = std::clamp((area.w - 340.0f) / 19.0f, 14.0f, 22.0f);
    const float gap = 4.0f;
    float x = area.x;
    const float y = area.y;
    const float bitsTop = y + 18.0f;
    // Fetched but not decoded yet: sixteen bits and nothing more. Which are op, rd or imm is the decoder's job.
    if (!decoded) {
        char word[8];
        std::snprintf(word, sizeof word, "%04X", static_cast<unsigned>(t.word));
        write(renderer, Face::Medium, 11.5f, x, y + 10.0f, i18n::format("A palavra que a ROM entregou: 0x{}, ainda sem campos", {word}), kTextDim);
        const SDL_FRect box{x, bitsTop, cell * 16.0f, 22.0f};
        fillRound(renderer, box, 4, mixed(kBackground, kTextDim, 0.18f));
        frameRound(renderer, box, 4, 1.0f, kTextDim);
        for (int bit = 15; bit >= 0; --bit) {
            const char digit[2] = {((t.word >> bit) & 1u) != 0 ? '1' : '0', '\0'};
            const float cx = x + cell * static_cast<float>(15 - bit) + cell / 2.0f;
            write(renderer, Face::Mono, 13.0f, cx - textWidth(renderer, Face::Mono, 13.0f, digit) / 2.0f, bitsTop + 16.0f, digit, digit[0] == '1' ? kText : kTextDim);
        }
        return;
    }
    write(renderer, Face::Medium, 11.5f, x, y + 10.0f, "A instrução em binário, campo por campo", kTextDim);
    for (const WordField& field : wordFields(t)) {
        const int count = field.high - field.low + 1;
        const float w = cell * static_cast<float>(count);
        const SDL_FRect box{x, bitsTop, w, 22.0f};
        fillRound(renderer, box, 4, mixed(kBackground, field.color, field.name[0] == '\0' ? 0.25f : 0.38f));
        frameRound(renderer, box, 4, 1.0f, field.color);
        for (int bit = field.high; bit >= field.low; --bit) {
            const char digit[2] = {((t.word >> bit) & 1u) != 0 ? '1' : '0', '\0'};
            const float cx = x + cell * static_cast<float>(field.high - bit) + cell / 2.0f;
            write(renderer, Face::Mono, 13.0f, cx - textWidth(renderer, Face::Mono, 13.0f, digit) / 2.0f, bitsTop + 16.0f, digit, kText);
        }
        // Under the bits, the field's name and its value.
        std::string label;
        if (field.name[0] == '\0') {
            label = i18n::tr("não usado");
        } else {
            const unsigned value = (t.word >> field.low) & ((1u << count) - 1u);
            label = std::string(field.name) + " = " + std::to_string(value);
            if (std::string_view(field.name) == "imm6" && value >= 32u) label += " (" + std::to_string(static_cast<int>(value) - 64) + ")";
        }
        const float labelWidth = textWidth(renderer, Face::Mono, 10.5f, label);
        write(renderer, Face::Mono, 10.5f, x + (w - std::min(labelWidth, w)) / 2.0f, bitsTop + 36.0f,
              labelWidth > w ? fitted(renderer, Face::Mono, 10.5f, label, w) : label, field.color);
        x += w + gap;
    }
    // And the same instruction as it's written, in the same colours.
    x += 18.0f;
    write(renderer, Face::Medium, 18.0f, x, bitsTop + 17.0f, "=", kTextDim);
    x += 22.0f;
    for (const AsmToken& token : asmTokens(t)) {
        write(renderer, Face::Mono, 17.0f, x, bitsTop + 17.0f, token.text, token.color);
        x += textWidth(renderer, Face::Mono, 17.0f, token.text);
    }
}

void drawXray(SDL_Renderer* renderer, const ComputerState& state, const SDL_FRect& area, int stage, float progress, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const InstructionTrace trace = traceInstruction(state);
    const bool running = !state.halted || traceInstruction(state).halts;  // the HLT itself still shows its way
    const Color tone = instructionTone(trace);

    // The step button, what's running, and what kind of instruction it is.
    const SDL_FRect next = panelNextButton(area);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Step, {next.x + 20.0f, next.y + next.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, next.x + 36.0f, next.y + 21.0f, "Próxima instrução", kText);
    const float titleX = next.x + next.w + 18.0f;
    const std::string title = running ? i18n::format("Endereço {}:  {}", {std::to_string(state.pc), xraySourceLine(state, trace)})
                                      : std::string(i18n::tr("O computador parou. Zerar começa de novo."));
    const char* kind = instructionKind(trace);
    const float kindWidth = running ? textWidth(renderer, Face::Medium, 12.5f, kind) + 22.0f : 0.0f;
    const std::string shown = fitted(renderer, Face::Medium, 16, title, area.x + area.w - titleX - 30.0f - kindWidth);
    write(renderer, Face::Medium, 16, titleX, next.y + 23.0f, shown, kText);
    if (running) {
        const float x = titleX + textWidth(renderer, Face::Medium, 16, shown) + 14.0f;
        const SDL_FRect chip{x, next.y + 6.0f, kindWidth, 22.0f};
        fillRound(renderer, chip, 11, mixed(kPanelRaised, tone, 0.3f));
        frameRound(renderer, chip, 11, 1.2f, tone);
        write(renderer, Face::Medium, 12.5f, chip.x + 11.0f, chip.y + 15.5f, kind, kText);
    }

    // The six steps: done, now and still to come.
    for (int s = 0; s < kTraceStages; ++s) {
        const SDL_FRect r = xrayStageRect(area, s);
        const bool now = running && s == stage;
        const bool done = running && s < stage;
        fillRound(renderer, r, 7, now ? mixed(kPanelRaised, tone, 0.45f) : inside(r, mouseX, mouseY) ? kPanelHover : done ? mixed(kPanelRaised, tone, 0.18f) : kPanelButton);
        if (now) frameRound(renderer, r, 7, 1.4f, tone);
        char label[32];
        std::snprintf(label, sizeof label, "%d  %s", s + 1, i18n::tr(kTraceStageNames[s]));
        const float w = textWidth(renderer, Face::Medium, 12.5f, label);
        write(renderer, Face::Medium, 12.5f, r.x + (r.w - w) / 2.0f, r.y + 19.0f, label, now || done ? kText : kTextDim);
    }
    const XrayParts parts = xrayParts(area);
    if (running) {
        drawStepColumn(renderer, parts.side, stage, explainStage(trace, stage, gZoom.hex), tone);
        drawWordStrip(renderer, trace, parts.strip, stage >= 1);
    }
    drawXrayLegend(renderer, parts.legend);
    drawXrayPlan(renderer, state, parts.plan, stage, progress);
    drawZoomButtons(renderer, parts.plan, mouseX, mouseY);
}

// What the short names on the X-ray mean, under it: the fields of an instruction.
// Small buttons in the diagram's corner: farther, closer, back to the whole diagram, and the base of the numbers.
void drawZoomButtons(SDL_Renderer* renderer, const SDL_FRect& plan, float mouseX, float mouseY) {
    for (int i = 0; i < 3; ++i) {
        static constexpr const char* kLabels[] = {"-", "+", "1:1"};
        const SDL_FRect r = zoomButtonRect(plan, i);
        fillRound(renderer, r, 6, inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
        frameRound(renderer, r, 6, 1.0f, kRule);
        const float size = i == 2 ? 11.5f : 16.0f;
        write(renderer, Face::Medium, size, r.x + (r.w - textWidth(renderer, Face::Medium, size, kLabels[i])) / 2.0f, r.y + 18.5f, kLabels[i], kText);
    }
    // Dec | Hex: the half lit is the base the numbers are in now; a click switches.
    const SDL_FRect r = zoomButtonRect(plan, 3);
    fillRound(renderer, r, 6, inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
    frameRound(renderer, r, 6, 1.0f, kRule);
    const float half = r.w / 2.0f;
    fillRound(renderer, {r.x + (gZoom.hex ? half : 0.0f) + 2.0f, r.y + 2.0f, half - 4.0f, r.h - 4.0f}, 5, mixed(kPanelButton, kToneProgram, 0.45f));
    const char* labels[] = {"Dec", "Hex"};
    for (int i = 0; i < 2; ++i) {
        const float w = textWidth(renderer, Face::Medium, 11.5f, labels[i]);
        const bool lit = (i == 1) == gZoom.hex;
        write(renderer, Face::Medium, 11.5f, r.x + half * static_cast<float>(i) + (half - w) / 2.0f, r.y + 17.5f, labels[i], lit ? kText : kTextDim);
    }
}

float drawXrayLegend(SDL_Renderer* renderer, const SDL_FRect& area) {
    const float size = 12.5f;
    const std::vector<std::string> lines =
        wrapped(renderer, Face::Regular, size,
                "Siglas: op = o código que diz qual é a instrução · rd = o registrador que recebe o resultado · rs, rt = os registradores "
                "lidos · imm = o imediato, um número escrito dentro da própria instrução, como o 10 de LI R1, 10 (imm6, imm9 e imm12 dizem "
                "quantos bits ele ocupa)",
                area.w);
    float y = area.y + area.h - 6.0f - 17.0f * static_cast<float>(std::min<std::size_t>(lines.size(), 3) - 1);
    for (std::size_t i = 0; i < lines.size() && i < 3; ++i, y += 17.0f) write(renderer, Face::Regular, size, area.x, y, lines[i], kTextDim);
    return 57.0f;
}

// --- The debugger -----------------------------------------------------------------------------------

constexpr float kDebugRow = 18.0f;
// The first button runs and pauses, the way the play button of any player does: one button, two faces.
constexpr int kDebugButtonCount = 4;
constexpr const char* kDebugButtons[] = {"Rodar", "Instrução", "Etapa", "Zerar"};
constexpr DebugAction kDebugActions[] = {DebugAction::Continue, DebugAction::Step, DebugAction::Stage, DebugAction::Reset};

// Where everything in the debugger is, shared by drawing and clicking.
struct DebugLayout {
    SDL_FRect buttons[kDebugButtonCount];
    SDL_FRect program;
    SDL_FRect registers;
    SDL_FRect memory;
    std::vector<std::string> lines;
    std::vector<int> addressOfLine;  // -1 for a line with no code, or for the rest of a line too long for one row
    std::vector<int> sourceOfLine;   // the program line each row shows a piece of
    int current = -1;                // the row the line about to run starts on
    int currentSource = -1;          // and that line
    int first = 0;                   // the first line shown
    int rows = 1;
};

// Characters, not bytes: the code is drawn one fixed-width cell per character.
std::size_t codeCells(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The first count characters of text, whole.
std::size_t byteOfCell(std::string_view text, std::size_t count) {
    std::size_t at = 0;
    for (std::size_t cells = 0; at < text.size() && cells < count; ++cells) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
    }
    return at;
}

// A program line too wide for the debugger, in pieces that fit. A comment goes on below itself, each piece
// starting with ; under the first one, so that it keeps the comment's colour.
std::vector<std::string> wrapCodeLine(const std::string& line, std::size_t width) {
    if (width < 16 || codeCells(line) <= width) return {line};
    const std::size_t semicolon = line.find_first_of(";#");
    if (semicolon == std::string::npos) {
        std::vector<std::string> pieces;
        std::string_view rest = line;
        while (!rest.empty()) {
            const std::size_t cut = byteOfCell(rest, width);
            pieces.emplace_back(rest.substr(0, cut));
            rest.remove_prefix(cut);
        }
        return pieces;
    }
    const std::string code = line.substr(0, semicolon);
    const std::size_t column = codeCells(code);
    const bool beside = column + 18 <= width;  // room for the comment beside the code, or it goes below
    const std::size_t indent = beside ? column : 2;
    std::vector<std::string> words;
    {
        std::size_t start = semicolon + 1;
        while (start < line.size()) {
            const std::size_t space = line.find(' ', start);
            const std::size_t end = space == std::string::npos ? line.size() : space;
            if (end > start) words.push_back(line.substr(start, end - start));
            start = end + 1;
        }
    }
    std::vector<std::string> pieces;
    const char mark = line[semicolon];
    std::string current = beside ? code + mark : code;
    if (!beside) {
        pieces.push_back(code);
        current = std::string(indent, ' ') + mark;
    }
    for (const std::string& word : words) {
        if (codeCells(current) + 1 + codeCells(word) > width && codeCells(current) > indent + 1) {
            pieces.push_back(current);
            current = std::string(indent, ' ') + mark;
        }
        current += " " + word;
    }
    pieces.push_back(current);
    return pieces;
}

DebugLayout debugLayout(const ComputerState& state, const SDL_FRect& area) {
    DebugLayout layout;
    const float gap = 6.0f;
    const float w = (area.w - 24.0f - static_cast<float>(kDebugButtonCount - 1) * gap) / static_cast<float>(kDebugButtonCount);
    for (int i = 0; i < kDebugButtonCount; ++i) layout.buttons[i] = {area.x + 12.0f + static_cast<float>(i) * (w + gap), area.y + 40.0f, w, 46.0f};
    layout.memory = {area.x + 8.0f, area.y + area.h - 8.0f - 100.0f, area.w - 16.0f, 100.0f};
    layout.registers = {area.x + 8.0f, layout.memory.y - 8.0f - 120.0f, area.w - 16.0f, 120.0f};
    layout.program = {area.x + 8.0f, area.y + 96.0f, area.w - 16.0f, layout.registers.y - 8.0f - (area.y + 96.0f)};
    layout.rows = std::max(1, static_cast<int>((layout.program.h - 30.0f) / kDebugRow));

    if (!state.source.empty()) {
        std::size_t start = 0;
        while (start <= state.source.size()) {
            const std::size_t end = state.source.find('\n', start);
            layout.lines.push_back(state.source.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        layout.addressOfLine.assign(layout.lines.size(), -1);
        for (std::size_t address = 0; address < state.lineOfWord.size(); ++address) {
            const int line = state.lineOfWord[address] - 1;
            if (line >= 0 && line < static_cast<int>(layout.lines.size()) && layout.addressOfLine[static_cast<std::size_t>(line)] < 0) {
                layout.addressOfLine[static_cast<std::size_t>(line)] = static_cast<int>(address);
            }
        }
        if (state.pc < state.lineOfWord.size()) layout.current = state.lineOfWord[state.pc] - 1;
        // Lines too wide for the panel go on in the rows below, the address only on the first.
        const auto width = static_cast<std::size_t>(std::max(0.0f, (layout.program.w - 60.0f - 18.0f) / 6.4f));
        std::vector<std::string> rows;
        std::vector<int> addresses;
        std::vector<int> sources;
        int currentRow = -1;
        for (std::size_t line = 0; line < layout.lines.size(); ++line) {
            if (static_cast<int>(line) == layout.current) currentRow = static_cast<int>(rows.size());
            const std::vector<std::string> pieces = wrapCodeLine(layout.lines[line], width);
            for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                rows.push_back(pieces[piece]);
                addresses.push_back(piece == 0 ? layout.addressOfLine[line] : -1);
                sources.push_back(static_cast<int>(line));
            }
        }
        layout.currentSource = layout.current;
        layout.current = currentRow;
        layout.lines = std::move(rows);
        layout.addressOfLine = std::move(addresses);
        layout.sourceOfLine = std::move(sources);
    } else {
        const int from = std::max(0, static_cast<int>(state.pc) - layout.rows / 3);
        for (int address = from; address < from + layout.rows; ++address) {
            layout.lines.push_back(assembly::disassemble(address == state.pc ? state.instruction : 0, static_cast<std::size_t>(address)));
            layout.addressOfLine.push_back(address);
        }
        layout.current = static_cast<int>(state.pc) - from;
        layout.currentSource = layout.current;
        for (int row = 0; row < static_cast<int>(layout.lines.size()); ++row) layout.sourceOfLine.push_back(row);
    }
    layout.first = std::clamp(layout.current - layout.rows / 3, 0, std::max(0, static_cast<int>(layout.lines.size()) - layout.rows));
    return layout;
}

}  // namespace

DebugHit debuggerAt(const ComputerState& state, const SDL_FRect& area, float x, float y) {
    const DebugLayout layout = debugLayout(state, area);
    for (int i = 0; i < kDebugButtonCount; ++i) {
        if (inside(layout.buttons[i], x, y)) return {kDebugActions[i], 0};
    }
    if (inside(layout.program, x, y)) {
        const int row = static_cast<int>((y - layout.program.y - 28.0f) / kDebugRow);
        const int line = layout.first + row;
        if (row >= 0 && line < static_cast<int>(layout.lines.size()) && layout.addressOfLine[static_cast<std::size_t>(line)] >= 0) {
            return {DebugAction::Breakpoint, static_cast<std::uint16_t>(layout.addressOfLine[static_cast<std::size_t>(line)])};
        }
    }
    return {};
}

void drawDebugger(SDL_Renderer* renderer, const ComputerState& state, const DebugView& view, const SDL_FRect& area, float mouseX, float mouseY) {
    const DebugLayout layout = debugLayout(state, area);
    const InstructionTrace trace = traceInstruction(state);
    const Color tone = instructionTone(trace);
    fillRect(renderer, area, kPanel);
    fillRect(renderer, {area.x + area.w - 1.0f, area.y, 1.0f, area.h}, kRule);

    write(renderer, Face::Medium, 15, area.x + 14.0f, area.y + 26.0f, "Depurador", kText);
    const char* status = state.halted ? "Parado no HLT" : view.running ? "Rodando" : "Pausado";
    const float statusWidth = textWidth(renderer, Face::Medium, 12.5f, status);
    write(renderer, Face::Medium, 12.5f, area.x + area.w - 14.0f - statusWidth, area.y + 26.0f, status,
          state.halted ? kWarning : view.running ? kKnobOn : kTextDim);

    constexpr Icon kIcons[] = {Icon::Play, Icon::Step, Icon::ChevronRight, Icon::Reset};
    for (int i = 0; i < kDebugButtonCount; ++i) {
        const SDL_FRect r = layout.buttons[i];
        // Running, the first button pauses; stopped at the end, it starts over.
        const bool pauses = i == 0 && view.running;
        const Icon icon = pauses ? Icon::Pause : i == 0 && state.halted ? Icon::Reset : kIcons[i];
        const char* label = pauses ? "Pausar" : i == 0 && state.halted ? "De novo" : kDebugButtons[i];
        fillRound(renderer, r, 8, i == 0 ? (inside(r, mouseX, mouseY) ? kWirePreview : kPanelActive) : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
        const Color ink = i == 0 && inside(r, mouseX, mouseY) ? kBackground : kText;
        drawIcon(renderer, icon, {r.x + r.w / 2.0f, r.y + 16.0f}, 16, ink);
        const float w = textWidth(renderer, Face::Regular, 11, label);
        write(renderer, Face::Regular, 11, r.x + (r.w - w) / 2.0f, r.y + 38.0f, label, i == 0 ? ink : kTextDim);
    }

    // The program, as in an editor: breakpoints in the margin, the line about to run marked.
    const SDL_FRect program = layout.program;
    fillRound(renderer, program, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, program.x + 10.0f, program.y + 18.0f, "Programa", kTextDim);
    write(renderer, Face::Regular, 11, program.x + 76.0f, program.y + 18.0f, "número = endereço na ROM", kTextOff);
    const SDL_Rect clip{static_cast<int>(program.x), static_cast<int>(program.y), static_cast<int>(program.w), static_cast<int>(program.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const std::vector<std::uint16_t> none;
    const std::vector<std::uint16_t>& breakpoints = view.breakpoints != nullptr ? *view.breakpoints : none;
    for (int row = 0; row < layout.rows; ++row) {
        const int line = layout.first + row;
        if (line >= static_cast<int>(layout.lines.size())) break;
        const float y = program.y + 28.0f + static_cast<float>(row) * kDebugRow;
        const int address = layout.addressOfLine[static_cast<std::size_t>(line)];
        // Every row of the line about to run is marked, and the arrow stands on its first.
        const bool here = layout.sourceOfLine[static_cast<std::size_t>(line)] == layout.currentSource && layout.currentSource >= 0 && !state.halted;
        const bool arrowHere = line == layout.current && !state.halted;
        if (here) fillRound(renderer, {program.x + 4.0f, y, program.w - 8.0f, kDebugRow}, 4, mixed(kBackground, tone, 0.35f));
        if (address >= 0 && inside({program.x, y, program.w, kDebugRow}, mouseX, mouseY) && !here) {
            fillRound(renderer, {program.x + 4.0f, y, program.w - 8.0f, kDebugRow}, 4, kPanelRaised);
        }
        const bool stop = address >= 0 && std::find(breakpoints.begin(), breakpoints.end(), static_cast<std::uint16_t>(address)) != breakpoints.end();
        if (stop) {
            Mesh dot;
            dot.addDisc({program.x + 14.0f, y + kDebugRow / 2.0f}, 5.0f, kWireConflict);
            dot.flush(renderer);
        }
        if (arrowHere) {
            Mesh arrow;
            const Vec2 tip{program.x + 26.0f, y + kDebugRow / 2.0f};
            arrow.addFan({{tip.x - 7.0f, tip.y - 5.0f}, tip, {tip.x - 7.0f, tip.y + 5.0f}}, {tip.x - 4.0f, tip.y}, tone);
            arrow.flush(renderer);
        }
        // Each instruction by its address in the ROM, the number the PC, the X-ray and CALL use; a line with
        // no instruction has none.
        if (address >= 0) {
            char number[12];
            std::snprintf(number, sizeof number, "%d", address);
            const float numberWidth = textWidth(renderer, Face::Mono, 11.5f, number);
            write(renderer, Face::Mono, 11.5f, program.x + 52.0f - numberWidth, y + 13.0f, number, here ? kText : kWirePreview);
        }
        drawCodeLine(renderer, layout.lines[static_cast<std::size_t>(line)], program.x + 60.0f, y + 13.0f, 12.5f, 6.4f);
    }
    SDL_SetRenderClipRect(renderer, nullptr);

    // The registers, in two columns; the ones the last instruction changed stand out, as in an IDE.
    const SDL_FRect registers = layout.registers;
    fillRound(renderer, registers, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, registers.x + 10.0f, registers.y + 18.0f, "Registradores", kTextDim);
    const float column = (registers.w - 20.0f) / 2.0f;
    const auto cell = [&](float x, float y, const char* name, std::uint16_t value, bool changed, float width) {
        if (changed) fillRound(renderer, {x - 4.0f, y, width, kDebugRow}, 4, Color{92, 84, 30});
        write(renderer, Face::Medium, 12, x, y + 13.0f, name, kText);
        char text[16];
        std::snprintf(text, sizeof text, "%04X", static_cast<unsigned>(value));
        const float hexX = x + std::max(30.0f, textWidth(renderer, Face::Medium, 12, name) + 6.0f);
        write(renderer, Face::Mono, 12, hexX, y + 13.0f, text, value != 0 ? kKnobOn : kTextOff);
        std::snprintf(text, sizeof text, "%u", static_cast<unsigned>(value));
        const float w = textWidth(renderer, Face::Regular, 12, text);
        write(renderer, Face::Regular, 12, x + width - 12.0f - w, y + 13.0f, text, changed ? kText : kTextDim);
    };
    for (unsigned r = 0; r < 8; ++r) {
        char name[4];
        std::snprintf(name, sizeof name, "R%u", r);
        const float x = registers.x + 14.0f + static_cast<float>(r / 4) * column;
        const float y = registers.y + 26.0f + static_cast<float>(r % 4) * kDebugRow;
        cell(x, y, name, state.registers[r], view.hasBefore && view.before[r] != state.registers[r], column - 6.0f);
    }
    cell(registers.x + 14.0f, registers.y + 26.0f + 4.0f * kDebugRow + 2.0f, "PC", state.pc, false, column - 6.0f);
    cell(registers.x + 14.0f + column, registers.y + 26.0f + 4.0f * kDebugRow + 2.0f, "Saída", state.output, false, column - 6.0f);

    // The first words of memory, the same way.
    const SDL_FRect memory = layout.memory;
    fillRound(renderer, memory, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, memory.x + 10.0f, memory.y + 18.0f, "Memória", kTextDim);
    for (std::size_t address = 0; address < state.memory.size() && address < 8; ++address) {
        char name[8];
        std::snprintf(name, sizeof name, "[%zu]", address);
        const float x = memory.x + 14.0f + static_cast<float>(address / 4) * column;
        const float y = memory.y + 26.0f + static_cast<float>(address % 4) * kDebugRow;
        const bool changed = view.hasBefore && address < view.memoryBefore.size() && view.memoryBefore[address] != state.memory[address];
        cell(x, y, name, state.memory[address], changed, column - 6.0f);
    }
}

namespace {

void dimScreen(SDL_Renderer* renderer, int width, int height) {
    const SDL_FRect all{0, 0, static_cast<float>(width), static_cast<float>(height)};
    setColor(renderer, {4, 10, 9}, 170);
    SDL_RenderFillRect(renderer, &all);
}

void drawDialog(SDL_Renderer* renderer, const SDL_FRect& box) {
    fillRound(renderer, box, 12, kPanelRaised);
    frameRound(renderer, box, 12, 1, kBodyEdge);
}

}  // namespace

namespace {

// --- The help: every instruction, what it does, an example, and how its 16 bits are laid out ----------

enum class Format : std::uint8_t { R3, R2, I6, I9, J, Rs, Rd, None, Pseudo, Text };

struct InstructionHelp {
    const char* syntax;  // a group's name, when meaning is empty
    const char* meaning;
    const char* example;
    Format format;
    Color tone;
    const char* effect = "";  // what the example does, with numbers: the registers before, an arrow, and after
};

constexpr InstructionHelp kInstructionHelp[] = {
    {"Contas", "", "", Format::Text, kToneAlu},
    {"ADD Rd, Rs, Rt", "Soma Rs com Rt e guarda o resultado em Rd.", "ADD R3, R1, R2", Format::R3, kToneAlu, "R1 = 3, R2 = 4  →  R3 = 7"},
    {"SUB Rd, Rs, Rt", "Subtrai Rt de Rs e guarda em Rd. Abaixo de zero, o número dá a volta: 0 - 1 vira 65535.", "SUB R3, R1, R2", Format::R3, kToneAlu, "R1 = 9, R2 = 4  →  R3 = 5"},
    {"AND Rd, Rs, Rt", "E bit a bit: cada bit de Rd é 1 só onde Rs e Rt têm 1. Serve para isolar bits.", "AND R1, R1, R2", Format::R3, kToneAlu, "R1 = 1100, R2 = 1010  →  R1 = 1000"},
    {"OR Rd, Rs, Rt", "OU bit a bit: cada bit de Rd é 1 onde Rs ou Rt têm 1. Serve para ligar bits.", "OR R1, R1, R2", Format::R3, kToneAlu, "R1 = 1100, R2 = 1010  →  R1 = 1110"},
    {"XOR Rd, Rs, Rt", "OU exclusivo bit a bit: 1 onde os bits de Rs e Rt são diferentes.", "XOR R1, R1, R2", Format::R3, kToneAlu, "R1 = 1100, R2 = 1010  →  R1 = 0110"},
    {"NOT Rd, Rs", "Inverte cada bit de Rs: os 0 viram 1 e os 1 viram 0.", "NOT R2, R1", Format::R2, kToneAlu, "R1 = 0x000F  →  R2 = 0xFFF0"},
    {"SHL Rd, Rs", "Desloca os bits de Rs uma casa para a esquerda, entrando um 0: multiplica por 2.", "SHL R1, R1", Format::R2, kToneAlu, "R1 = 5  →  R1 = 10"},
    {"SHR Rd, Rs", "Desloca os bits de Rs uma casa para a direita: divide por 2, sem resto.", "SHR R1, R1", Format::R2, kToneAlu, "R1 = 10  →  R1 = 5"},
    {"ADDI Rd, Rs, n", "Soma o número n, de -32 a 31, a Rs e guarda em Rd. É o jeito de contar e descontar.", "ADDI R1, R1, -1", Format::I6, kToneAlu, "R1 = 8  →  R1 = 7"},
    {"MOV Rd, Rs", "Copia Rs para Rd. Não é uma instrução própria: o montador escreve ADD Rd, Rs, R0.", "MOV R2, R1", Format::Pseudo, kToneAlu, "R1 = 42  →  R2 = 42"},
    {"Números", "", "", Format::Text, kToneNumber},
    {"LI Rd, n", "Põe o número n, de 0 a 511, em Rd. O número fica escrito na própria instrução.", "LI R1, 10", Format::I9, kToneNumber, "→  R1 = 10"},
    {"LUI Rd, n", "Põe n × 128 em Rd: a parte de cima de um número grande, de 128 em 128.", "LUI R1, 4", Format::I9, kToneNumber, "→  R1 = 512, que é 4 × 128"},
    {"LIW Rd, n", "Põe qualquer número de 0 a 65535 em Rd. O montador troca por um LUI e alguns ADDI, então ocupa mais de uma palavra.",
     "LIW R1, 1000", Format::Pseudo, kToneNumber, "→  R1 = 1000"},
    {"Memória", "", "", Format::Text, kToneMemory},
    {"LD Rd, n(Rs)", "Lê a palavra da RAM no endereço Rs + n e guarda em Rd. Sem n, lê do endereço que está em Rs.", "LD R2, 0(R1)", Format::I6, kToneMemory, "R1 = 5, memória[5] = 99  →  R2 = 99"},
    {"ST Rd, n(Rs)", "Grava o valor de Rd na RAM, no endereço Rs + n.", "ST R2, 1(R1)", Format::I6, kToneMemory, "R1 = 5, R2 = 7  →  memória[6] = 7"},
    {"Caminho", "", "", Format::Text, kToneBranch},
    {"BEQ Ra, Rb, lugar", "Se Ra for igual a Rb, pula para o lugar; se não, segue para a instrução seguinte. O lugar fica a no máximo 32 instruções "
     "para trás ou 31 para a frente: o número guardado é a distância.", "BEQ R1, R0, fim", Format::I6, kToneBranch, "R1 = 0  →  vai para fim"},
    {"BNE Ra, Rb, lugar", "Se Ra for diferente de Rb, pula para o lugar. É o que faz um laço repetir.", "BNE R1, R0, laço", Format::I6, kToneBranch, "R1 = 3  →  volta para laço"},
    {"JMP lugar", "Pula para o lugar, sempre. Alcança qualquer endereço de 0 a 4095.", "JMP início", Format::J, kToneJump, "→  vai para início"},
    {"CALL lugar", "Chama uma sub-rotina: pula para o lugar e guarda em R7 o endereço da instrução seguinte, para poder voltar.", "CALL triplica",
     Format::J, kToneJump, "→  R7 = o endereço seguinte, e vai para triplica"},
    {"RET", "Volta de uma sub-rotina, para o endereço guardado em R7. O montador escreve JR R7.", "RET", Format::Pseudo, kToneJump, "R7 = 12  →  volta para o endereço 12"},
    {"JR Rs", "Pula para o endereço que está em Rs.", "JR R7", Format::Rs, kToneJump, "R7 = 12  →  vai para o endereço 12"},
    {"Mundo", "", "", Format::Text, kToneWorld},
    {"IN Rd", "Lê o número que as chaves formam e guarda em Rd.", "IN R1", Format::Rd, kToneWorld, "chaves em 5  →  R1 = 5"},
    {"OUT Rs", "Mostra o valor de Rs na saída, onde ele fica até o próximo OUT.", "OUT R2", Format::Rs, kToneWorld, "R2 = 19  →  a saída mostra 19"},
    {"NOP", "Não faz nada: ocupa uma palavra e passa adiante. O montador escreve ADD R0, R0, R0.", "NOP", Format::Pseudo, kTextDim, "nada muda; só passa o tempo de uma instrução"},
    {"HLT", "Para o computador. O PC fica parado nesta instrução.", "HLT", Format::None, kToneHalt, "o computador para; Parou acende"},
    {"Escrita", "", "", Format::Text, kWirePreview},
    {"R0 a R7", "Os oito registradores. R0 vale sempre 0, e o que se grava nele se perde; R7 recebe o endereço de volta do CALL.", "ADD R1, R0, R0",
     Format::Text, kWirePreview, "R0 é sempre 0  →  R1 = 0"},
    {"nome:", "Um rótulo: dá nome ao endereço da instrução seguinte, para BEQ, BNE, JMP e CALL usarem.", "laço:", Format::Text, kWirePreview},
    {"; texto", "Um comentário: tudo depois do ponto e vírgula é ignorado pelo montador.", "; conta até 10", Format::Text, kWirePreview},
    {".word n", "Grava o número n direto na ROM, sem ser instrução: para guardar dados junto com o programa.", ".word 42", Format::Text, kWirePreview},
};

constexpr CodeReference kMipsReference[] = {
    {"Registradores", "", kWirePreview},
    {"$zero", "vale sempre 0", kWirePreview},
    {"$t0 a $t9", "para contas", kWirePreview},
    {"$s0 a $s7", "para guardar", kWirePreview},
    {"$a0 $v0", "argumento, resultado", kWirePreview},
    {"$sp $ra", "pilha, volta", kWirePreview},
    {"# texto", "comentário", kWirePreview},
    {"Contas", "", kToneAlu},
    {"add $d, $s, $t", "$d = $s + $t", kToneAlu},
    {"sub $d, $s, $t", "$d = $s - $t", kToneAlu},
    {"addi $t, $s, n", "$t = $s + n", kToneAlu},
    {"and or xor nor", "bit a bit", kToneAlu},
    {"andi ori xori", "com um número", kToneAlu},
    {"slt $d, $s, $t", "1 se $s < $t", kToneAlu},
    {"sll srl sra", "desloca n casas", kToneAlu},
    {"move $d, $s", "copia", kToneAlu},
    {"Números", "", kToneNumber},
    {"li $t, n", "põe n em $t", kToneNumber},
    {"lui $t, n", "n nos bits de cima", kToneNumber},
    {"la $t, x", "o endereço de x", kToneNumber},
    {"Memória", "", kToneMemory},
    {"lw $t, n($s)", "lê da RAM", kToneMemory},
    {"sw $t, n($s)", "grava na RAM", kToneMemory},
    {".data .text", "dados, programa", kToneMemory},
    {"x: .word n", "uma palavra", kToneMemory},
    {"Caminho", "", kToneBranch},
    {"beq $s, $t, x", "vai a x se iguais", kToneBranch},
    {"bne $s, $t, x", "vai a x se diferentes", kToneBranch},
    {"blt bgt ble bge", "compara e vai", kToneBranch},
    {"j x", "vai a x", kToneJump},
    {"jal x", "vai a x; volta em $ra", kToneJump},
    {"jr $ra", "volta", kToneJump},
    {"Mundo: syscall", "", kToneWorld},
    {"li $v0, 1", "mostra o número $a0", kToneWorld},
    {"li $v0, 11", "mostra o caractere", kToneWorld},
    {"li $v0, 5", "lê as chaves", kToneWorld},
    {"li $v0, 10", "termina", kToneHalt},
};

constexpr InstructionHelp kMipsHelp[] = {
    {"Contas", "", "", Format::Text, kToneAlu},
    {"add $d, $s, $t", "Soma $s com $t e guarda em $d. addu faz o mesmo: aqui não há aviso de estouro.", "add $t2, $t0, $t1", Format::Text, kToneAlu, "$t0 = 3, $t1 = 4  →  $t2 = 7"},
    {"sub $d, $s, $t", "Subtrai $t de $s e guarda em $d.", "sub $t2, $t0, $t1", Format::Text, kToneAlu, "$t0 = 9, $t1 = 4  →  $t2 = 5"},
    {"and, or, xor, nor", "Bit a bit: E, OU, OU exclusivo e NÃO-OU de $s com $t, em $d.", "and $t2, $t0, $t1", Format::Text, kToneAlu, "$t0 = 0xC, $t1 = 0xA  →  $t2 = 0x8"},
    {"slt $d, $s, $t", "Põe 1 em $d se $s for menor que $t, com sinal; senão, 0. sltu compara sem sinal.", "slt $t2, $t0, $t1", Format::Text, kToneAlu, "$t0 = 2, $t1 = 5  →  $t2 = 1"},
    {"sll, srl, sra $d, $t, n", "Desloca $t n casas: sll para a esquerda (vezes 2^n), srl para a direita entrando zeros, sra copiando o sinal.",
     "sll $t1, $t0, 2", Format::Text, kToneAlu, "$t0 = 3  →  $t1 = 12"},
    {"addi $t, $s, n", "Soma o número n, de -32768 a 32767, a $s e guarda em $t.", "addi $t0, $t0, 1", Format::Text, kToneAlu, "$t0 = 9  →  $t0 = 10"},
    {"andi, ori, xori $t, $s, n", "Bit a bit com o número n, de 0 a 65535.", "andi $t1, $t0, 0xFF", Format::Text, kToneAlu, "$t0 = 0x1234  →  $t1 = 0x34"},
    {"slti $t, $s, n", "Põe 1 em $t se $s for menor que n.", "slti $t0, $a0, 2", Format::Text, kToneAlu, "$a0 = 1  →  $t0 = 1"},
    {"move $d, $s", "Copia $s para $d. O montador escreve addu $d, $zero, $s.", "move $a0, $t2", Format::Pseudo, kToneAlu, "$t2 = 42  →  $a0 = 42"},
    {"Números", "", "", Format::Text, kToneNumber},
    {"li $t, n", "Põe qualquer número de 32 bits em $t. Um número pequeno vira uma instrução; um grande, lui e ori.", "li $t0, 100000", Format::Pseudo, kToneNumber, "→  $t0 = 100000"},
    {"lui $t, n", "Põe n nos 16 bits de cima de $t, com zeros embaixo.", "lui $t0, 0x1001", Format::Text, kToneNumber, "→  $t0 = 0x10010000"},
    {"la $t, rótulo", "Põe em $t o endereço do rótulo, com lui e ori: para percorrer um vetor do .data.", "la $t0, vetor", Format::Pseudo, kToneNumber, "→  $t0 = 0x10010000, onde vetor começa"},
    {"Memória", "", "", Format::Text, kToneMemory},
    {"lw $t, n($s)", "Lê a palavra da RAM no endereço $s + n e guarda em $t. Os endereços contam bytes: as palavras vão de 4 em 4.",
     "lw $t1, 4($t0)", Format::Text, kToneMemory, "$t0 = 0x10010000  →  $t1 = a 2ª palavra do .data"},
    {"sw $t, n($s)", "Grava $t na RAM, no endereço $s + n.", "sw $ra, 0($sp)", Format::Text, kToneMemory, "$sp = 0x7FFFEFFC  →  a pilha guarda $ra"},
    {"lw $t, rótulo", "Lê a palavra que está no rótulo do .data.", "lw $t1, n", Format::Pseudo, kToneMemory, "→  $t1 = o valor guardado em n"},
    {"Caminho", "", "", Format::Text, kToneBranch},
    {"beq $s, $t, rótulo", "Se $s for igual a $t, desvia para o rótulo; senão, segue.", "beq $t0, $zero, fim", Format::Text, kToneBranch, "$t0 = 0  →  vai para fim"},
    {"bne $s, $t, rótulo", "Se $s for diferente de $t, desvia. É o que faz um laço repetir.", "bne $t0, $t1, laco", Format::Text, kToneBranch, "$t0 = 3, $t1 = 5  →  volta para laco"},
    {"blt, bgt, ble, bge", "Compara e desvia: menor, maior, menor ou igual, maior ou igual. O montador escreve slt e um beq ou bne, usando $at.",
     "bge $t3, $t1, fim", Format::Pseudo, kToneBranch, "$t3 = 7, $t1 = 5  →  vai para fim"},
    {"j rótulo", "Salta para o rótulo, sempre.", "j laco", Format::Text, kToneJump, "→  vai para laco"},
    {"jal rótulo", "Chama uma função: salta e guarda em $ra o endereço da instrução seguinte.", "jal fib", Format::Text, kToneJump, "→  $ra = o endereço seguinte, e vai para fib"},
    {"jr $s", "Salta para o endereço em $s. jr $ra volta de uma função.", "jr $ra", Format::Text, kToneJump, "$ra = 0x00400010  →  volta para lá"},
    {"Mundo: syscall", "", "", Format::Text, kToneWorld},
    {"syscall", "Pede algo de fora da CPU, pelo código em $v0: 1 mostra o número em $a0, 11 mostra o caractere em $a0, 5 lê as chaves para $v0, "
     "10 termina o programa.", "syscall", Format::Text, kToneWorld, "$v0 = 1, $a0 = 42  →  a saída mostra 42"},
    {"Escrita", "", "", Format::Text, kWirePreview},
    {".data e .text", "O que vem depois de .data vai para a RAM, a partir de 0x10010000; depois de .text, para a ROM, a partir de 0x00400000.",
     ".data", Format::Text, kWirePreview},
    {".word a, b, c", "Palavras de dados, uma depois da outra. .space n reserva n bytes zerados.", "vetor: .word 1, 2, 3", Format::Text, kWirePreview},
    {"nome:", "Um rótulo: dá nome ao endereço seguinte, de uma instrução ou de um dado.", "laco:", Format::Text, kWirePreview},
    {"# texto", "Um comentário: o resto da linha é ignorado.", "# soma o vetor", Format::Text, kWirePreview},
};

struct FormatPiece {
    const char* name;
    int bits;
    Color color;
};

std::vector<FormatPiece> formatPieces(Format format) {
    const FormatPiece op{"op", 4, kFieldOp};
    const FormatPiece rd{"rd", 3, kFieldRd};
    const FormatPiece rs{"rs", 3, kFieldRs};
    switch (format) {
        case Format::R3: return {op, rd, rs, {"rt", 3, kFieldRt}, {"func", 3, kFieldFunc}};
        case Format::R2: return {op, rd, rs, {"", 3, kFieldUnused}, {"func", 3, kFieldFunc}};
        case Format::I6: return {op, rd, rs, {"imm6", 6, kFieldImm}};
        case Format::I9: return {op, rd, {"imm9", 9, kFieldImm}};
        case Format::J: return {op, {"imm12", 12, kFieldImm}};
        case Format::Rs: return {op, {"", 3, kFieldUnused}, rs, {"", 6, kFieldUnused}};
        case Format::Rd: return {op, rd, {"", 9, kFieldUnused}};
        case Format::None: return {op, {"", 12, kFieldUnused}};
        default: return {};
    }
}

// The help: a header that stays, with a chip for each group of instructions to jump to, and under it every
// instruction: how it's written, what it does, its example with the numbers before and after, and the
// example's word, assembled for real, field by field in the X-ray's colours.
constexpr float kHelpHeader = 118.0f;
constexpr float kHelpChipsY = 80.0f;

std::span<const InstructionHelp> helpRows(bool mips) {
    return mips ? std::span<const InstructionHelp>(kMipsHelp) : std::span<const InstructionHelp>(kInstructionHelp);
}

float helpBitsWidth(bool mips) { return mips ? 352.0f : 256.0f; }

float helpMeaningWidth(int width, bool mips) { return std::max(220.0f, static_cast<float>(width) - 48.0f - 270.0f - helpBitsWidth(mips) - 40.0f); }

// The words an example becomes, as the editor assembles it; none for a line that isn't an instruction. The
// labels the examples use are put after it, so that branches and jumps have somewhere to go.
const std::vector<std::uint32_t>& helpWords(const InstructionHelp& row, bool mips) {
    static std::unordered_map<const InstructionHelp*, std::vector<std::uint32_t>> cache;
    if (const auto found = cache.find(&row); found != cache.end()) return found->second;
    std::vector<std::uint32_t>& words = cache[&row];
    const std::string example(row.example);
    const bool code = !example.empty() && example[0] != '.' && example[0] != '#' && example[0] != ';' && example.find(':') == std::string::npos &&
                      (mips || row.format != Format::Text);
    if (!code) return words;
    if (mips) {
        const mips::Program program = mips::assemble(".data\nvetor: .word 0\nn: .word 0\n.text\n" + example + "\nlaco:\nfim:\nfib: nop\n");
        if (program.ok() && program.text.size() > 1) words.assign(program.text.begin(), program.text.end() - 1);
    } else {
        const assembly::Result result = assembly::assemble(example + "\nfim:\nlaço:\ninício:\ntriplica: HLT\n");
        if (result.errors.empty() && result.words.size() > 1) words.assign(result.words.begin(), result.words.end() - 1);
    }
    return words;
}

bool helpPseudo(const InstructionHelp& row) { return row.format == Format::Pseudo; }

float helpRowHeight(SDL_Renderer* renderer, const InstructionHelp& row, float meaningWidth, bool mips) {
    if (row.meaning[0] == '\0') return 46.0f;
    float text = static_cast<float>(wrapped(renderer, Face::Regular, 14.0f, row.meaning, meaningWidth).size()) * 20.0f;
    if (row.effect[0] != '\0') text += 22.0f;
    const bool bits = !helpWords(row, mips).empty() && !helpPseudo(row);
    return std::max(text + 22.0f, bits ? 66.0f : 54.0f);
}

// Where every row is, from the top of the list, and the rows that start groups.
struct HelpLayout {
    std::vector<float> tops;
    std::vector<std::size_t> groups;  // indexes of the rows that name a group
    float height = 0.0f;
};

HelpLayout helpLayout(SDL_Renderer* renderer, int width, bool mips) {
    HelpLayout layout;
    float y = 0.0f;
    const std::span<const InstructionHelp> rows = helpRows(mips);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        layout.tops.push_back(y);
        if (rows[i].meaning[0] == '\0') layout.groups.push_back(i);
        y += helpRowHeight(renderer, rows[i], helpMeaningWidth(width, mips), mips);
    }
    layout.height = y + 40.0f;
    return layout;
}

// The chips of the groups, in the header, left to right.
std::vector<SDL_FRect> helpChips(SDL_Renderer* renderer, bool mips) {
    std::vector<SDL_FRect> chips;
    float x = 24.0f;
    for (const InstructionHelp& row : helpRows(mips)) {
        if (row.meaning[0] != '\0') continue;
        const float w = textWidth(renderer, Face::Medium, 12.5f, row.syntax) + 26.0f;
        chips.push_back({x, kCodeBarHeight + kHelpChipsY, w, 28.0f});
        x += w + 8.0f;
    }
    return chips;
}

// The group whose rows are at the top of the list.
std::size_t helpCurrentGroup(const HelpLayout& layout, float scroll) {
    std::size_t current = 0;
    for (std::size_t g = 0; g < layout.groups.size(); ++g) {
        if (layout.tops[layout.groups[g]] <= scroll + 8.0f) current = g;
    }
    return current;
}

// One word's bits, in the boxes of its fields, the name of each field under it.
void drawHelpBits(SDL_Renderer* renderer, std::uint32_t word, bool mips, const InstructionHelp& row, float x, float y) {
    struct Field {
        const char* name;
        int bits;
        Color color;
    };
    std::vector<Field> fields;
    if (mips) {
        const std::uint32_t op = word >> 26;
        if (op == mips::kOpSpecial) {
            fields = {{"op", 6, kFieldOp}, {"rs", 5, kFieldRs}, {"rt", 5, kFieldRt}, {"rd", 5, kFieldRd}, {"shamt", 5, kFieldUnused}, {"funct", 6, kFieldFunc}};
        } else if (op == mips::kOpJ || op == mips::kOpJal) {
            fields = {{"op", 6, kFieldOp}, {"alvo", 26, kFieldImm}};
        } else {
            fields = {{"op", 6, kFieldOp}, {"rs", 5, kFieldRs}, {"rt", 5, kFieldRt}, {"imm", 16, kFieldImm}};
        }
    } else {
        for (const FormatPiece& piece : formatPieces(row.format)) fields.push_back({piece.name, piece.bits, piece.color});
    }
    const int total = mips ? 32 : 16;
    const float cell = helpBitsWidth(mips) / static_cast<float>(total);
    int bit = total - 1;
    for (const Field& field : fields) {
        const float w = cell * static_cast<float>(field.bits);
        const SDL_FRect box{x, y, w - 2.0f, 22.0f};
        const bool unused = field.name[0] == '\0';
        fillRound(renderer, box, 3, mixed(kBackground, field.color, unused ? 0.16f : 0.3f));
        frameRound(renderer, box, 3, 1.0f, field.color);
        for (int b = 0; b < field.bits; ++b, --bit) {
            const char digit[2] = {((word >> bit) & 1u) != 0 ? '1' : '0', '\0'};
            const float size = std::min(12.0f, cell * 1.15f);
            const float cx = x + cell * (static_cast<float>(b) + 0.5f) - 1.0f;
            write(renderer, Face::Mono, size, cx - textWidth(renderer, Face::Mono, size, digit) / 2.0f, box.y + 15.5f, digit, digit[0] == '1' ? kText : kTextDim);
        }
        const char* label = unused ? "-" : field.name;
        const float labelWidth = textWidth(renderer, Face::Mono, 10.5f, label);
        if (labelWidth <= w) write(renderer, Face::Mono, 10.5f, box.x + (box.w - labelWidth) / 2.0f, box.y + 36.0f, label, field.color);
        x += w;
    }
}

void drawCodeHelp(SDL_Renderer* renderer, int width, int height, float scroll, bool mips) {
    const SDL_FRect area{0.0f, kCodeBarHeight, static_cast<float>(width), static_cast<float>(height) - kCodeBarHeight};
    fillRect(renderer, area, kBackground);
    const HelpLayout layout = helpLayout(renderer, width, mips);
    const std::span<const InstructionHelp> rows = helpRows(mips);
    const float left = 24.0f;
    const float meaningX = left + 270.0f;
    const float meaningWidth = helpMeaningWidth(width, mips);
    const float bitsX = meaningX + meaningWidth + 40.0f;

    // The list, under the header.
    const float listTop = area.y + kHelpHeader;
    const SDL_Rect clip{0, static_cast<int>(listTop), width, static_cast<int>(area.y + area.h - listTop)};
    SDL_SetRenderClipRect(renderer, &clip);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const InstructionHelp& row = rows[i];
        const float y = listTop + layout.tops[i] - scroll;
        const float rowHeight = helpRowHeight(renderer, row, meaningWidth, mips);
        if (y + rowHeight < listTop || y > area.y + area.h) continue;
        if (row.meaning[0] == '\0') {
            fillRound(renderer, {left, y + 18.0f, 4.0f, 18.0f}, 2, row.tone);
            write(renderer, Face::Medium, 17.0f, left + 14.0f, y + 33.0f, row.syntax, row.tone);
            fillRect(renderer, {left, y + rowHeight - 2.0f, static_cast<float>(width) - 48.0f, 1.0f}, kRule);
            continue;
        }
        const std::vector<std::uint32_t>& words = helpWords(row, mips);
        write(renderer, Face::Mono, 14.5f, left, y + 24.0f, row.syntax, row.tone);
        std::string example = row.example;
        if (words.size() == 1 && !helpPseudo(row)) {
            char hex[16];
            std::snprintf(hex, sizeof hex, mips ? "   0x%08X" : "   0x%04X", static_cast<unsigned>(words[0]));
            example += hex;
        }
        write(renderer, Face::Mono, 12.5f, left, y + 44.0f, fitted(renderer, Face::Mono, 12.5f, example, 260.0f), kTextDim);
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 14.0f, row.meaning, meaningWidth);
        float lineY = y + 24.0f;
        for (const std::string& line : lines) {
            write(renderer, Face::Regular, 14.0f, meaningX, lineY, line, kText);
            lineY += 20.0f;
        }
        if (row.effect[0] != '\0') {
            const float tag = textWidth(renderer, Face::Medium, 12.0f, "Exemplo") + 14.0f;
            const SDL_FRect chip{meaningX, lineY - 11.0f, tag, 19.0f};
            fillRound(renderer, chip, 9.5f, mixed(kBackground, kKnobOn, 0.18f));
            write(renderer, Face::Medium, 12.0f, chip.x + 7.0f, chip.y + 14.0f, "Exemplo", kKnobOn);
            // Before, an arrow drawn (the fonts have none), and after.
            const std::string effect(i18n::tr(row.effect));
            const std::size_t arrow = effect.find("→");
            const Color tone = mixed(kKnobOn, kText, 0.35f);
            float ex = meaningX + tag + 8.0f;
            const auto trimmed = [](std::string text) {
                while (!text.empty() && text.back() == ' ') text.pop_back();
                while (!text.empty() && text.front() == ' ') text.erase(0, 1);
                return text;
            };
            if (arrow == std::string::npos) {
                write(renderer, Face::Regular, 13.5f, ex, lineY + 2.0f, fitted(renderer, Face::Regular, 13.5f, effect, meaningWidth - tag - 8.0f), tone);
            } else {
                const std::string before = trimmed(effect.substr(0, arrow));
                const std::string after = trimmed(effect.substr(arrow + 3));
                if (!before.empty()) {
                    write(renderer, Face::Regular, 13.5f, ex, lineY + 2.0f, before, tone);
                    ex += textWidth(renderer, Face::Regular, 13.5f, before) + 10.0f;
                }
                const float ay = lineY - 2.5f;
                fillRect(renderer, {ex, ay - 0.75f, 14.0f, 1.5f}, kKnobOn);
                Mesh head;
                head.addFan({{ex + 12.0f, ay - 4.5f}, {ex + 19.0f, ay}, {ex + 12.0f, ay + 4.5f}}, {ex + 14.0f, ay}, kKnobOn);
                head.flush(renderer);
                ex += 28.0f;
                write(renderer, Face::Regular, 13.5f, ex, lineY + 2.0f, fitted(renderer, Face::Regular, 13.5f, after, meaningX + meaningWidth - ex), tone);
            }
        }
        if (helpPseudo(row) && !words.empty()) {
            // A pseudo-instruction: the real ones it becomes.
            write(renderer, Face::Regular, 12.5f, bitsX, y + 22.0f, words.size() == 1 ? "pseudo-instrução: o montador escreve" : "pseudo-instrução: vira estas", kTextDim);
            for (std::size_t w = 0; w < words.size() && w < 2; ++w) {
                const std::string real = mips ? mips::disassemble(words[w], mips::kTextBase + static_cast<std::uint32_t>(4 * w))
                                              : assembly::disassemble(static_cast<std::uint16_t>(words[w]), w);
                write(renderer, Face::Mono, 13.0f, bitsX, y + 42.0f + 18.0f * static_cast<float>(w), real, kText);
            }
        } else if (helpPseudo(row)) {
            write(renderer, Face::Regular, 12.5f, bitsX, y + 24.0f, "pseudo-instrução", kTextDim);
        } else if (!words.empty()) {
            drawHelpBits(renderer, words[0], mips, row, bitsX, y + 12.0f);
        }
        fillRect(renderer, {left, y + rowHeight - 1.0f, static_cast<float>(width) - 48.0f, 1.0f}, Color{30, 40, 44});
    }
    SDL_SetRenderClipRect(renderer, nullptr);

    // The header: what the help is, and the groups to jump to, the one in view lit.
    fillRect(renderer, {0.0f, area.y, static_cast<float>(width), kHelpHeader}, kBackground);
    write(renderer, Face::Medium, 22.0f, left, area.y + 36.0f, mips ? "Todas as instruções do MIPS" : "Todas as instruções", kText);
    const char* intro = mips ? "Cada instrução vira uma palavra de 32 bits. À direita, os bits do exemplo, campo por campo, nas cores do Raio-X."
                             : "Cada instrução vira uma palavra de 16 bits. À direita, os bits do exemplo, campo por campo, nas cores do Raio-X.";
    write(renderer, Face::Regular, 14.0f, left, area.y + 60.0f, fitted(renderer, Face::Regular, 14.0f, intro, static_cast<float>(width) - 48.0f), kTextDim);
    const std::vector<SDL_FRect> chips = helpChips(renderer, mips);
    const std::size_t current = helpCurrentGroup(layout, scroll);
    for (std::size_t g = 0; g < chips.size() && g < layout.groups.size(); ++g) {
        const InstructionHelp& group = rows[layout.groups[g]];
        const bool lit = g == current;
        fillRound(renderer, chips[g], 14, lit ? mixed(kBackground, group.tone, 0.3f) : kPanelButton);
        if (lit) frameRound(renderer, chips[g], 14, 1.2f, group.tone);
        write(renderer, Face::Medium, 12.5f, chips[g].x + 13.0f, chips[g].y + 18.5f, group.syntax, lit ? kText : group.tone);
    }
    fillRect(renderer, {0.0f, area.y + kHelpHeader - 1.0f, static_cast<float>(width), 1.0f}, kRule);
}

}  // namespace

float codeHelpMaxScroll(SDL_Renderer* renderer, int width, int height, bool mips) {
    const HelpLayout layout = helpLayout(renderer, width, mips);
    return std::max(0.0f, layout.height - (static_cast<float>(height) - kCodeBarHeight - kHelpHeader));
}

float codeHelpGroupAt(SDL_Renderer* renderer, int width, int height, bool mips, float x, float y) {
    const std::vector<SDL_FRect> chips = helpChips(renderer, mips);
    const HelpLayout layout = helpLayout(renderer, width, mips);
    for (std::size_t g = 0; g < chips.size() && g < layout.groups.size(); ++g) {
        if (inside(chips[g], x, y)) return std::min(layout.tops[layout.groups[g]], codeHelpMaxScroll(renderer, width, height, mips));
    }
    return -1.0f;
}

namespace {

void drawCodeBlocks32(SDL_Renderer* renderer, const CodeView& view, const SDL_FRect& area);  // with the 32-bit computer

constexpr float kBlock16 = 56.0f;
constexpr float kBlock32 = 60.0f;
constexpr float kBlockGap = 6.0f;

// The blocks' place under the tabs.
SDL_FRect codeBlocksArea(int width, int height) {
    const CodeLayout layout = codeLayout(width, height);
    const float referenceLeft = layout.problems.w;
    const float y = kCodeBarHeight + 62.0f;
    return {referenceLeft + 12.0f, y - 14.0f, static_cast<float>(width) - referenceLeft - 24.0f, static_cast<float>(height) - y};
}

int blocksThatFit(const SDL_FRect& area, bool mips) {
    return std::max(1, static_cast<int>((area.h - 8.0f) / ((mips ? kBlock32 : kBlock16) + kBlockGap)));
}

// When there are more blocks than room, a thin bar on the right says where the list is.
void drawBlocksBar(SDL_Renderer* renderer, const SDL_FRect& area, int first, int fit, int count) {
    if (count <= fit) return;
    const SDL_FRect track{area.x + area.w - 4.0f, area.y, 4.0f, area.h - 8.0f};
    fillRound(renderer, track, 2, kRule);
    const float h = std::max(24.0f, track.h * static_cast<float>(fit) / static_cast<float>(count));
    const float y = track.y + (track.h - h) * static_cast<float>(first) / static_cast<float>(count - fit);
    fillRound(renderer, {track.x, y, track.w, h}, 2, kTextDim);
}

// A bit in a field: a 1 bright and bold, a 0 faint, so the ones stand out at a glance.
void writeBit(SDL_Renderer* renderer, bool one, float cx, float baseline, float size) {
    const char* digit = one ? "1" : "0";
    const Face face = one ? Face::Medium : Face::Mono;
    write(renderer, face, size, cx - textWidth(renderer, face, size, digit) / 2.0f, baseline, digit, one ? kText : Color{112, 128, 132});
}

std::vector<int> codeTabs(const CodeView& view) {
    if (view.lesson >= 0) return {kCodeTabTask, kCodeTabCommands, kCodeTabBlocks};
    return {kCodeTabCommands, kCodeTabBlocks};
}

SDL_FRect codeTabRect(int width, int height, int index, int count) {
    const CodeLayout layout = codeLayout(width, height);
    const float left = layout.problems.w + 12.0f;
    const float room = static_cast<float>(width) - layout.problems.w - 24.0f;
    const float w = (room - 6.0f * static_cast<float>(count - 1)) / static_cast<float>(count);
    return {left + static_cast<float>(index) * (w + 6.0f), kCodeBarHeight + 10.0f, w, 30.0f};
}

// The program as blocks, one per word, in the order they sit in the ROM: each one's 16 bits by field, in
// the X-ray's colours, and the instruction they make. The one the computer is on is lit, and kept in view.
void drawCodeBlocks(SDL_Renderer* renderer, const CodeView& view, const SDL_FRect& area) {
    if (view.program.empty()) {
        for (const std::string& line : wrapped(renderer, Face::Regular, 13.0f, "Monte o programa com Ctrl+Enter: cada instrução aparece aqui como um bloco.",
                                               area.w)) {
            write(renderer, Face::Regular, 13.0f, area.x, area.y + 20.0f, line, kTextDim);
        }
        return;
    }
    constexpr float kBlock = kBlock16;
    const int count = static_cast<int>(view.program.size());
    const int fit = std::min(count, blocksThatFit(area, false));
    const int first = std::clamp(view.blocksFirst, 0, std::max(0, count - fit));
    drawBlocksBar(renderer, area, first, fit, count);
    const float blockW = count > fit ? area.w - 10.0f : area.w;
    float y = area.y;
    for (int i = first; i < count && i < first + fit; ++i) {
        const bool running = i == view.runningAddress;
        const SDL_FRect r{area.x, y, blockW, kBlock};
        fillRound(renderer, r, 8, running ? mixed(kPanelRaised, kToneProgram, 0.28f) : kPanelRaised);
        frameRound(renderer, r, 8, running ? 1.8f : 1.0f, running ? kToneProgram : kBodyEdge);
        ComputerState state;
        state.instruction = view.program[static_cast<std::size_t>(i)];
        state.pc = static_cast<std::uint16_t>(i);
        const InstructionTrace trace = traceInstruction(state);
        // The address, and the instruction in the colours of its fields.
        const std::string address = std::to_string(i);
        write(renderer, Face::Mono, 12.0f, r.x + 10.0f, r.y + 20.0f, address, running ? kToneProgram : kTextOff);
        float x = r.x + 40.0f;
        for (const AsmToken& token : asmTokens(trace)) {
            write(renderer, Face::Mono, 13.5f, x, r.y + 20.0f, token.text, token.color);
            x += textWidth(renderer, Face::Mono, 13.5f, token.text);
        }
        if (i < static_cast<int>(view.lines.size())) {
            const std::string line = i18n::format("linha {}", {std::to_string(view.lines[static_cast<std::size_t>(i)])});
            const float w = textWidth(renderer, Face::Regular, 11.0f, line);
            write(renderer, Face::Regular, 11.0f, r.x + r.w - 10.0f - w, r.y + 19.0f, line, kTextOff);
        }
        // The bits, field by field.
        const float cell = (r.w - 50.0f) / 16.0f;
        float bx = r.x + 40.0f;
        for (const WordField& field : wordFields(trace)) {
            const int bits = field.high - field.low + 1;
            const SDL_FRect box{bx, r.y + 29.0f, cell * static_cast<float>(bits) - 2.0f, 18.0f};
            fillRound(renderer, box, 3, mixed(kBackground, field.color, field.name[0] == '\0' ? 0.12f : 0.2f));
            frameRound(renderer, box, 3, 1.0f, field.color);
            for (int bit = field.high; bit >= field.low; --bit) {
                const float cx = bx + cell * static_cast<float>(field.high - bit) + cell / 2.0f - 1.0f;
                writeBit(renderer, ((trace.word >> bit) & 1u) != 0, cx, box.y + 13.5f, 11.5f);
            }
            bx += cell * static_cast<float>(bits);
        }
        y += kBlock + 6.0f;
    }
    if (view.runningAddress < 0) {
        y += 6.0f;
        for (const std::string& line : wrapped(renderer, Face::Regular, 12.5f,
                                               "Grave o programa (Ctrl+Enter) e rode: o bloco da instrução em execução acende aqui.", area.w)) {
            write(renderer, Face::Regular, 12.5f, area.x, y + 14.0f, line, kTextDim);
            y += 17.0f;
        }
    }
}

}  // namespace

int codeBlocksVisible(int width, int height, bool mips) { return blocksThatFit(codeBlocksArea(width, height), mips); }

bool overCodeBlocks(int width, int height, float x, float y) {
    const SDL_FRect area = codeBlocksArea(width, height);
    return x >= area.x && x < area.x + area.w + 12.0f && y >= area.y && y < area.y + area.h;
}

int codeTabAt(const CodeView& view, float x, float y, int width, int height) {
    const std::vector<int> tabs = codeTabs(view);
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (inside(codeTabRect(width, height, static_cast<int>(i), static_cast<int>(tabs.size())), x, y)) return tabs[i];
    }
    return -1;
}

CodeLayout codeLayout(int width, int height) {
    CodeLayout layout;
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    const float left = std::max(kCodeGutter + 80.0f, w - kCodeReferenceWidth);
    layout.text = {kCodeGutter + 8.0f, kCodeBarHeight + 8.0f, left - kCodeGutter - 16.0f, std::max(40.0f, h - kCodeBarHeight - kCodeProblemsHeight - 16.0f)};
    layout.problems = {0.0f, h - kCodeProblemsHeight, left, kCodeProblemsHeight};
    // Above the program's coloured line at the bar's foot, clear of the tabs under it.
    layout.assemble = {w - 282.0f, 5.0f, 176.0f, 28.0f};
    layout.help = {w - 470.0f, 5.0f, 178.0f, 28.0f};
    layout.close = {w - 96.0f, 5.0f, 84.0f, 28.0f};
    layout.save = {w - 562.0f, 5.0f, 84.0f, 28.0f};
    layout.open = {w - 654.0f, 5.0f, 84.0f, 28.0f};
    layout.lines = std::max(1, static_cast<int>(layout.text.h / kCodeLineHeight));
    layout.columns = std::max(1, static_cast<int>(layout.text.w / kCodeCharWidth));
    return layout;
}

int codeProblemAt(const CodeView& view, float y, int width, int height) {
    const CodeLayout layout = codeLayout(width, height);
    const float first = layout.problems.y + 32.0f;
    if (y < first) return -1;
    const auto row = static_cast<std::size_t>((y - first) / kProblemRow);
    return row < std::min(view.problems->size(), kProblemsShown) ? static_cast<int>(row) : -1;
}

void drawCode(SDL_Renderer* renderer, const CodeView& view, int width, int height) {
    const CodeLayout layout = codeLayout(width, height);
    const editor::TextEdit& text = *view.text;
    const auto fullHeight = static_cast<float>(height);
    const auto fullWidth = static_cast<float>(width);

    fillRect(renderer, {0.0f, 0.0f, fullWidth, fullHeight}, kBackground);
    fillRect(renderer, {0, kCodeBarHeight, kCodeGutter, fullHeight - kCodeBarHeight - kCodeProblemsHeight}, kPanel);

    const int last = std::min(text.lineCount(), view.scrollLine + layout.lines);
    std::vector<const char*> wrong(static_cast<std::size_t>(layout.lines), nullptr);
    for (const assembly::Error& problem : *view.problems) {
        const int row = problem.line - 1 - view.scrollLine;
        if (row >= 0 && row < layout.lines && wrong[static_cast<std::size_t>(row)] == nullptr) wrong[static_cast<std::size_t>(row)] = problem.message.c_str();
    }

    const auto rowY = [&](int line) { return layout.text.y + static_cast<float>(line - view.scrollLine) * kCodeLineHeight; };
    const auto columnX = [&](int column) { return layout.text.x + static_cast<float>(column - view.scrollColumn) * kCodeCharWidth; };

    const SDL_Rect area{static_cast<int>(layout.text.x), static_cast<int>(layout.text.y), static_cast<int>(layout.text.w), static_cast<int>(layout.text.h)};
    SDL_SetRenderClipRect(renderer, &area);
    for (int line = view.scrollLine; line < last; ++line) {
        const float y = rowY(line);
        if (line == view.runningLine) {
            fillRound(renderer, {layout.text.x - 4.0f, y, layout.text.w + 8.0f, kCodeLineHeight}, 4, mixed(kBackground, kToneProgram, 0.24f));
            fillRect(renderer, {layout.text.x - 8.0f, y + 2.0f, 3.0f, kCodeLineHeight - 4.0f}, kToneProgram);
        }
        if (wrong[static_cast<std::size_t>(line - view.scrollLine)] != nullptr) {
            fillRound(renderer, {layout.text.x - 4.0f, y, layout.text.w + 8.0f, kCodeLineHeight}, 4, kCodeWrongLine);
        }
        if (text.hasSelection() && line >= text.selectionBegin().line && line <= text.selectionEnd().line) {
            const editor::TextPosition begin = text.selectionBegin();
            const editor::TextPosition end = text.selectionEnd();
            const int from = line == begin.line ? text.displayColumn(begin) : 0;
            const int to = line == end.line ? text.displayColumn(end) : text.displayColumn({line, static_cast<int>(text.line(line).size())}) + 1;
            fillRect(renderer, {columnX(from), y, static_cast<float>(to - from) * kCodeCharWidth, kCodeLineHeight}, kCodeSelection);
        }
    }
    for (int line = view.scrollLine; line < last; ++line) {
        const float baseline = rowY(line) + 15.0f;
        drawCodeLine(renderer, text.line(line), columnX(0), baseline);
        const char* problem = wrong[static_cast<std::size_t>(line - view.scrollLine)];
        if (problem != nullptr) {
            const auto length = static_cast<int>(characters(text.line(line)));
            write(renderer, Face::Regular, kUiText, columnX(length + 3), baseline - 1.0f, problem, kWarning);
        }
    }
    const editor::TextPosition cursor = text.cursor();
    fillRect(renderer, {columnX(text.displayColumn(cursor)), rowY(cursor.line) + 1.0f, 2.0f, kCodeLineHeight - 2.0f}, kKnobOn);
    SDL_SetRenderClipRect(renderer, nullptr);

    for (int line = view.scrollLine; line < last; ++line) {
        char number[16];
        std::snprintf(number, sizeof number, "%d", line + 1);
        const float numberWidth = textWidth(renderer, Face::Mono, 13, number);
        write(renderer, Face::Mono, 13, kCodeGutter - 10.0f - numberWidth, rowY(line) + 14.0f, number, wrong[static_cast<std::size_t>(line - view.scrollLine)] != nullptr ? kWarning : kTextOff);
    }

    fillRect(renderer, layout.problems, kPanel);
    fillRect(renderer, {0, layout.problems.y, layout.problems.w, 1.0f}, kRule);
    const std::size_t problems = view.problems->size();
    char status[96];
    if (view.warning[0] != '\0') {
        std::snprintf(status, sizeof status, "%s", view.warning);
    } else if (problems == 0) {
        std::snprintf(status, sizeof status, i18n::tr("Tudo certo: %zu palavras, de %zu que cabem na ROM."), view.words, view.capacity);
    } else {
        std::snprintf(status, sizeof status, i18n::tr(problems == 1 ? "1 problema. Clique nele para ir até a linha." : "%zu problemas. Clique em um para ir até a linha."),
                      problems);
    }
    write(renderer, Face::Medium, kUiText, 14.0f, layout.problems.y + 22.0f, status, view.warning[0] != '\0' || problems != 0 ? kWarning : kKnobOn);
    const std::size_t shown = std::min(problems, kProblemsShown);
    for (std::size_t i = 0; i < shown; ++i) {
        const assembly::Error& problem = (*view.problems)[i];
        std::string row = problem.line > 0 ? i18n::format("Linha {}: {}", {std::to_string(problem.line), problem.message}) : problem.message;
        if (i + 1 == shown && problems > shown) row += i18n::format("   (e mais {})", {std::to_string(problems - shown)});
        write(renderer, Face::Regular, kUiText, 14.0f, layout.problems.y + 46.0f + static_cast<float>(i) * kProblemRow, row, kText);
    }

    const float referenceLeft = layout.problems.w;
    fillRect(renderer, {referenceLeft, kCodeBarHeight, fullWidth - referenceLeft, fullHeight - kCodeBarHeight}, kPanel);
    fillRect(renderer, {referenceLeft, kCodeBarHeight, 1.0f, fullHeight - kCodeBarHeight}, kRule);
    // Each group in its colour: a mark by its name, a stripe by each row, and the instruction itself in it.
    // The operands stay white, with the numbers to fill in (n, x) in the editor's colour for numbers.
    // The tabs: the task (in a lesson), the instructions, and the program as blocks.
    {
        const std::vector<int> tabs = codeTabs(view);
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            const SDL_FRect r = codeTabRect(width, height, static_cast<int>(i), static_cast<int>(tabs.size()));
            const bool chosen = tabs[i] == view.tab;
            fillRound(renderer, r, 7, chosen ? kPanelActive : kPanelButton);
            if (chosen) frameRound(renderer, r, 7, 1.2f, kWirePreview);
            const char* label = tabs[i] == kCodeTabTask ? "Tarefa" : tabs[i] == kCodeTabCommands ? "Comandos" : "Blocos";
            const float w = textWidth(renderer, Face::Medium, kUiText, label);
            write(renderer, Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 20.0f, label, chosen ? kText : kTextDim);
        }
    }
    float y = kCodeBarHeight + 62.0f;
    constexpr float kReferenceSize = 13.5f;
    const float charWidth = textWidth(renderer, Face::Mono, kReferenceSize, "M");
    if (view.tab == kCodeTabBlocks && view.mips) drawCodeBlocks32(renderer, view, {referenceLeft + 12.0f, y - 14.0f, fullWidth - referenceLeft - 24.0f, fullHeight - y});
    if (view.tab == kCodeTabBlocks && !view.mips) drawCodeBlocks(renderer, view, {referenceLeft + 12.0f, y - 14.0f, fullWidth - referenceLeft - 24.0f, fullHeight - y});
    // In a programming lesson, the column says what to do; every instruction is one click away, in Ajuda.
    if (view.tab == kCodeTabTask && view.lesson >= 0) {
        const Lesson& lesson = lessons()[static_cast<std::size_t>(view.lesson)];
        const float x = referenceLeft + 18.0f;
        const float room = fullWidth - referenceLeft - 36.0f;
        y += 4.0f;
        write(renderer, Face::Regular, 12.0f, x, y, fitted(renderer, Face::Regular, 12.0f, lessonActs()[static_cast<std::size_t>(lesson.act)].title, room),
              kTextDim);
        y += 26.0f;
        for (const std::string& line : wrapped(renderer, Face::Medium, 17.0f, lesson.title, room)) {
            write(renderer, Face::Medium, 17.0f, x, y, line, kText);
            y += 22.0f;
        }
        y += 6.0f;
        for (const std::string& line : wrapped(renderer, Face::Regular, 13.5f, lesson.story, room)) {
            write(renderer, Face::Regular, 13.5f, x, y, line, kText);
            y += 19.0f;
        }
        y += 10.0f;
        for (std::size_t step = 0; step < lesson.steps.size(); ++step) {
            const std::string number = std::to_string(step + 1) + ".";
            write(renderer, Face::Medium, 13.0f, x, y, number, kWirePreview);
            for (const std::string& line : wrapped(renderer, Face::Regular, 13.0f, lesson.steps[step], room - 22.0f)) {
                write(renderer, Face::Regular, 13.0f, x + 22.0f, y, line, kTextDim);
                y += 18.0f;
            }
            y += 4.0f;
        }
        y += 8.0f;
        fillRect(renderer, {x, y, room, 1.0f}, kRule);
        y += 22.0f;
        for (const std::string& line : wrapped(renderer, Face::Regular, 12.5f,
                                               "Ctrl+Enter monta e grava o programa na ROM, e o exercício é verificado sozinho. Ajuda, no alto, lista todas "
                                               "as instruções.",
                                               room)) {
            write(renderer, Face::Regular, 12.5f, x, y, line, kTextDim);
            y += 17.0f;
        }
        if (!view.lessonMessage.empty()) {
            y += 10.0f;
            const Color color = view.lessonPassed ? kKnobOn : view.lessonProblem ? kWarning : kWirePreview;
            for (const std::string& line : wrapped(renderer, Face::Medium, 13.5f, view.lessonMessage, room)) {
                write(renderer, Face::Medium, 13.5f, x, y, line, color);
                y += 19.0f;
            }
        }
    }
    for (const CodeReference& row : view.tab != kCodeTabCommands ? std::span<const CodeReference>{} : view.mips ? std::span<const CodeReference>(kMipsReference) : std::span<const CodeReference>(kCodeReference)) {
        if (row.meaning[0] == '\0') {
            y += 9.0f;
            fillRound(renderer, {referenceLeft + 14.0f, y - 9.0f, 9.0f, 9.0f}, 2, row.tone);
            write(renderer, Face::Medium, kUiText, referenceLeft + 30.0f, y, row.code, row.tone);
            y += 17.0f;
            continue;
        }
        fillRect(renderer, {referenceLeft + 14.0f, y - 12.0f, 3.0f, 15.0f}, row.tone);
        const std::string code(i18n::tr(row.code));
        const std::string_view instruction(code);
        const std::size_t space = std::min(code.find(' '), code.size());
        const float codeX = referenceLeft + 26.0f;
        write(renderer, Face::Mono, kReferenceSize, codeX, y, instruction.substr(0, space), row.tone);
        for (std::size_t i = space; i < code.size();) {
            std::size_t end = i;
            while (end < code.size() && std::strchr(" ,()", code[end]) == nullptr) ++end;
            if (end == i) {
                if (code[i] != ' ') write(renderer, Face::Mono, kReferenceSize, codeX + charWidth * static_cast<float>(i), y, instruction.substr(i, 1), kTextDim);
                ++i;
                continue;
            }
            const std::string_view word = instruction.substr(i, end - i);
            const bool blank = word == "n" || word == "x";
            write(renderer, Face::Mono, kReferenceSize, codeX + charWidth * static_cast<float>(i), y, word, blank ? kWarning : kText);
            i = end;
        }
        write(renderer, Face::Regular, 12.5f, referenceLeft + 162.0f, y, row.meaning, kTextDim);
        y += 17.0f;
    }

    fillRect(renderer, {0, 0, fullWidth, kCodeBarHeight}, kPanel);
    fillRect(renderer, {0, kCodeBarHeight - 1.0f, fullWidth, 1.0f}, kRule);
    fillRect(renderer, {0.0f, kCodeBarHeight - 3.0f, fullWidth, 3.0f}, kToneProgram);
    fillRound(renderer, {14.0f, 11.0f, 30.0f, 20.0f}, 4, kBodyProgram);
    frameRound(renderer, {14.0f, 11.0f, 30.0f, 20.0f}, 4, 1.2f, kToneProgram);
    write(renderer, Face::Medium, 9.5f, 17.5f, 24.5f, "ROM", kToneProgram);
    write(renderer, Face::Medium, 15, 54.0f, 29.0f, "Programando a ROM", kToneProgram);
    write(renderer, Face::Regular, 12.5f, 54.0f + textWidth(renderer, Face::Medium, 15, "Programando a ROM") + 18.0f, 29.0f,
          "Ctrl+Enter monta e grava  ·  Esc fecha", kTextDim);
    drawButton(renderer, layout.assemble, "Montar e gravar", problems == 0 ? kPanelActive : kPanelButton);
    drawButton(renderer, layout.close, "Fechar", kPanelButton);
    drawButton(renderer, layout.open, "Abrir", kPanelButton);
    drawButton(renderer, layout.save, "Salvar", kPanelButton);
    {
        const SDL_FRect r = layout.help;
        fillRound(renderer, r, 7, view.help ? kPanelActive : mixed(kPanel, kWirePreview, 0.16f));
        frameRound(renderer, r, 7, 1.2f, kWirePreview);
        if (view.help) {
            const char* label = "Voltar ao código";
            write(renderer, Face::Medium, kUiText, r.x + (r.w - textWidth(renderer, Face::Medium, kUiText, label)) / 2.0f, r.y + 19.0f, label, kText);
        } else {
            const Vec2 badge{r.x + 17.0f, r.y + r.h / 2.0f};
            Mesh disc;
            disc.addDisc(badge, 8.5f, kWirePreview);
            disc.flush(renderer);
            write(renderer, Face::Medium, 12.0f, badge.x - textWidth(renderer, Face::Medium, 12.0f, "?") / 2.0f, badge.y + 4.5f, "?", kBackground);
            write(renderer, Face::Medium, kUiText, r.x + 32.0f, r.y + 19.0f, "Ajuda das instruções", kText);
        }
    }
    if (view.help) drawCodeHelp(renderer, width, height, view.helpScroll, view.mips);
}

namespace {

// --- The home: a sheet of glass over the circuits, the lessons as the big way in, the rest beside them -----

struct HomeLayout {
    SDL_FRect panel;     // the glass
    SDL_FRect content;   // under the title
    SDL_FRect hero;      // the lessons
    SDL_FRect tiles[5];  // continue, new, examples, open, computers
};

constexpr HomeChoice kHomeTiles[5] = {HomeChoice::Continue, HomeChoice::New, HomeChoice::Example, HomeChoice::Open, HomeChoice::Computers};

HomeLayout homeLayout(int width, int height) {
    HomeLayout l;
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    const float panelW = std::min(w - 48.0f, 1060.0f);
    const float panelH = std::min(h - 84.0f, 500.0f);
    l.panel = {(w - panelW) / 2.0f, std::max(70.0f, (h - panelH) / 2.0f + 10.0f), panelW, panelH};
    const float pad = 34.0f;
    l.content = {l.panel.x + pad, l.panel.y + 124.0f, panelW - 2.0f * pad, panelH - 124.0f - 52.0f};
    const float heroW = std::max(300.0f, l.content.w * 0.4f);
    l.hero = {l.content.x, l.content.y, heroW, l.content.h};
    const float gap = 14.0f;
    const float gx = l.hero.x + heroW + gap;
    const float gw = l.content.x + l.content.w - gx;
    const float rowH = (l.content.h - 2.0f * gap) / 3.0f;
    const float colW = (gw - gap) / 2.0f;
    for (int i = 0; i < 4; ++i) {
        l.tiles[i] = {gx + static_cast<float>(i % 2) * (colW + gap), l.content.y + static_cast<float>(i / 2) * (rowH + gap), colW, rowH};
    }
    l.tiles[4] = {gx, l.content.y + 2.0f * (rowH + gap), gw, rowH};
    return l;
}

// The computers' page: one card for each computer, wide enough to say what it has, and a way back.
struct ComputerCard {
    HomeChoice choice;
    const char* title;
    const char* subtitle;
    const char* specs[4][2];  // what it has, as a small table: a name and its value
    bool ready;
};

constexpr ComputerCard kComputerCards[] = {
    {HomeChoice::Computer16, "16 bits", "O computador do curso, feito para aprender",
     {{"Registradores", "8 de 16 bits, R0 a R7"}, {"Instruções", "16 bits cada, 16 códigos"}, {"Memória", "ROM e RAM de 16 bits"},
      {"Para ver", "Painel e Raio-X, passo a passo"}},
     true},
    {HomeChoice::Computer32, "32 bits · MIPS", "Um conjunto de instruções de verdade",
     {{"Registradores", "32 de 32 bits, como $t0 e $sp"}, {"Instruções", "MIPS: formatos R, I e J"}, {"Memória", "endereços em bytes, .data e pilha"},
      {"Saída", "syscall: números e letras"}},
     true},
};
constexpr int kComputerCardCount = static_cast<int>(std::size(kComputerCards));

SDL_FRect computerCard(int index, int width, int height) {
    const HomeLayout home = homeLayout(width, height);
    const float gap = 20.0f;
    const float cardW = std::min(360.0f, (home.content.w - gap) / 2.0f);
    const float total = 2.0f * cardW + gap;
    const float left = home.content.x + (home.content.w - total) / 2.0f;
    return {left + static_cast<float>(index) * (cardW + gap), home.content.y + 34.0f, cardW, 218.0f};
}

// "Por dentro" can be closed behind the developer's password; while closed, a button asks for it.
bool gInsideOpen = true;  // open to everyone: every part can be seen from the inside

// Under each card: the computer seen whole, from the inside.
SDL_FRect computerInsideRect(int index, int width, int height) {
    const SDL_FRect card = computerCard(index, width, height);
    return {card.x, card.y + card.h + 10.0f, card.w, 38.0f};
}

SDL_FRect computersBackRect(int width, int height) {
    const SDL_FRect first = gInsideOpen ? computerInsideRect(0, width, height) : computerCard(0, width, height);
    return {first.x, first.y + first.h + 20.0f, 120.0f, 34.0f};
}

// Beside Voltar, while "Por dentro" is closed: the button that asks for the password.
SDL_FRect computersLockRect(int width, int height) {
    const SDL_FRect back = computersBackRect(width, height);
    return {back.x + back.w + 12.0f, back.y, 190.0f, back.h};
}

// --- The lesson list: the acts on the left, the chosen act's readings and exercises on the right -----

constexpr float kLessonCardHeight = 54.0f;  // the gallery shares these
constexpr float kLessonCardGap = 8.0f;
constexpr float kActListWidth = 300.0f;
constexpr float kActRowHeight = 50.0f;
constexpr float kListTop = 124.0f;
constexpr float kExerciseHeight = 54.0f;
constexpr float kReadingCardHeight = 70.0f;
constexpr float kPlannedCardHeight = 56.0f;

struct ListItem {
    LessonListKind kind;
    int index;
    SDL_FRect rect;
    const char* text = nullptr;  // for a heading: kind None
};

// What kind of exercise a lesson is, in a word and a colour: the list and the strip say it the same way.
const char* lessonKindName(const Lesson& lesson) {
    return lesson.check == LessonCheck::Answer ? "Perguntas" : lesson.check == LessonCheck::Program ? "Programa" : "Montar";
}

Color lessonKindColor(const Lesson& lesson) {
    return lesson.check == LessonCheck::Answer ? kWarning : lesson.check == LessonCheck::Program ? kToneProgram : kWirePreview;
}

// The acts share the column's height: as tall as kActRowHeight when they fit, shorter when there are more.
SDL_FRect actRowRect(int act, int height) {
    const float room = static_cast<float>(height) - kListTop - 34.0f + 5.0f;
    const float step = std::clamp(room / static_cast<float>(lessonActs().size()), 36.0f, kActRowHeight + 5.0f);
    return {24.0f, kListTop + static_cast<float>(act) * step, kActListWidth, step - 5.0f};
}

SDL_FRect lessonContentArea(int width, int height) {
    const float left = 24.0f + kActListWidth + 28.0f;
    return {left, kListTop, static_cast<float>(width) - left - 24.0f, static_cast<float>(height) - kListTop - 34.0f};
}

// Where everything of the chosen act goes, scrolled; contentHeight gets how tall it all is.
std::vector<ListItem> actContent(int width, int height, const LessonListView& view, float* contentHeight = nullptr) {
    const SDL_FRect area = lessonContentArea(width, height);
    const float start = area.y + 64.0f - view.scroll;
    float y = start;
    std::vector<ListItem> items;
    const int columns = std::max(1, static_cast<int>((area.w + 10.0f) / 270.0f));
    const float cardWidth = (area.w - 10.0f * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const auto exercises = [&](const std::vector<int>& lessonIndexes) {
        for (std::size_t i = 0; i < lessonIndexes.size(); ++i) {
            const int column = static_cast<int>(i) % columns;
            if (column == 0 && i > 0) y += kExerciseHeight + 10.0f;
            items.push_back({LessonListKind::Exercise, lessonIndexes[i],
                             {area.x + static_cast<float>(column) * (cardWidth + 10.0f), y, cardWidth, kExerciseHeight}});
        }
        if (!lessonIndexes.empty()) y += kExerciseHeight + 30.0f;
    };
    std::vector<int> loose = actLessons(view.act);
    const std::vector<Topic>& all = topics();
    for (std::size_t t = 0; t < all.size(); ++t) {
        if (all[t].act != view.act) continue;
        items.push_back({LessonListKind::Reading, static_cast<int>(t), {area.x, y, area.w, kReadingCardHeight}});
        y += kReadingCardHeight + 12.0f;
        std::vector<int> mine;
        for (const char* id : all[t].exercises) {
            const int lesson = lessonIndex(id);
            if (lesson < 0) continue;
            mine.push_back(lesson);
            loose.erase(std::remove(loose.begin(), loose.end(), lesson), loose.end());
        }
        exercises(mine);
    }
    if (!loose.empty()) {
        // An act whose readings are still to be written: its exercises on their own.
        items.push_back({LessonListKind::None, -1, {area.x, y, area.w, 24.0f}, "Exercícios"});
        y += 30.0f;
        exercises(loose);
    }
    // The readings still to come, after the ones there are.
    const std::vector<PlannedTopic>& planned = lessonActs()[static_cast<std::size_t>(view.act)].planned;
    if (!planned.empty()) {
        items.push_back({LessonListKind::None, -1, {area.x, y, area.w, 24.0f}, "A ser implementado"});
        y += 30.0f;
        for (std::size_t p = 0; p < planned.size(); ++p) {
            items.push_back({LessonListKind::Planned, static_cast<int>(p), {area.x, y, area.w, kPlannedCardHeight}});
            y += kPlannedCardHeight + 10.0f;
        }
        y += 20.0f;
    }
    if (contentHeight != nullptr) *contentHeight = y - start + 64.0f;
    return items;
}

bool visibleIn(const SDL_FRect& r, int width, int height) {
    const SDL_FRect area = lessonContentArea(width, height);
    return r.y + r.h > area.y && r.y < area.y + area.h;
}

// A padlock: the body and the shackle over it.
void drawLock(SDL_Renderer* renderer, Vec2 centre, float size, Color color) {
    const float w = size * 0.72f;
    const float h = size * 0.52f;
    fillRound(renderer, {centre.x - w / 2.0f, centre.y - h / 2.0f + size * 0.16f, w, h}, size * 0.1f, color);
    const float shackle = size * 0.46f;
    frameRound(renderer, {centre.x - shackle / 2.0f, centre.y - size * 0.46f, shackle, size * 0.62f}, shackle / 2.0f, std::max(1.6f, size * 0.11f), color);
}

SDL_FRect lessonBackRect() { return {24.0f, 24.0f, 110.0f, 34.0f}; }

SDL_FRect lessonResetRect(int width) { return {static_cast<float>(width) - 24.0f - 230.0f, 24.0f, 230.0f, 34.0f}; }
SDL_FRect lessonUnlockRect(int width) { return {static_cast<float>(width) - 24.0f - 230.0f - 12.0f - 190.0f, 24.0f, 190.0f, 34.0f}; }

// Text broken into lines no wider than width, at spaces.
std::vector<std::string> wrapped(SDL_Renderer* renderer, Face face, float size, std::string_view text, float width) {
    text = shown(face, text);
    std::vector<std::string> lines;
    std::string line;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view word = text.substr(start, end - start);
        const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && textWidth(renderer, face, size, candidate) > width) {
            lines.push_back(line);
            line = std::string(word);
        } else {
            line = candidate;
        }
        start = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void drawCheckMark(SDL_Renderer* renderer, Vec2 centre, float size, Color color) {
    Mesh mark;
    mark.addLine({centre.x - size * 0.45f, centre.y}, {centre.x - size * 0.1f, centre.y + size * 0.35f}, 2.2f, color);
    mark.addLine({centre.x - size * 0.1f, centre.y + size * 0.35f}, {centre.x + size * 0.5f, centre.y - size * 0.4f}, 2.2f, color);
    mark.flush(renderer);
}

void drawHelp(SDL_Renderer* renderer, int width, int height) {
    // A key (or a gesture) and what it does; a row with no key starts a group.
    struct Shortcut {
        const char* key;
        const char* does;
    };
    static constexpr Shortcut kLeft[] = {
        {nullptr, "Peças e fios"},
        {"Clique ou arraste", "põe a peça da biblioteca na placa"},
        {"Clique num pino", "começa ou termina um fio"},
        {"Clique no vazio", "fixa um ponto do fio"},
        {"Shift", "fio em L"},
        {"Enter", "deixa a ponta do fio solta"},
        {"Arraste um fio", "puxa um fio novo do meio dele"},
        {nullptr, "Editar"},
        {"R", "gira"},
        {"Delete", "apaga"},
        {"Ctrl+C  X  V", "copia, recorta, cola"},
        {"Ctrl+Z  Y", "desfaz, refaz"},
        {"+  -", "mais ou menos entradas"},
        {"[  ]", "tamanho"},
        {",  .", "bits"},
    };
    static constexpr Shortcut kRight[] = {
        {nullptr, "Vista"},
        {"Arraste no vazio", "seleciona"},
        {"Botão direito", "move a vista"},
        {"Roda", "zoom"},
        {"F", "enquadra tudo"},
        {"Clique duplo", "abre um chip"},
        {nullptr, "Simulação"},
        {"Espaço", "roda ou pausa o clock"},
        {"N", "um passo do clock"},
        {nullptr, "Arquivos e painéis"},
        {"Ctrl+S  O  N", "salva, abre, novo"},
        {"T", "mostra a tabela-verdade"},
        {"P", "perfis"},
        {"F1", "esta ajuda"},
    };
    dimScreen(renderer, width, height);
    const float boxWidth = std::min(780.0f, static_cast<float>(width) - 40.0f);
    const float rows = static_cast<float>(std::max(std::size(kLeft), std::size(kRight)));
    const float boxHeight = rows * 25.0f + 118.0f;
    const float x = (static_cast<float>(width) - boxWidth) / 2.0f;
    const float y = (static_cast<float>(height) - boxHeight) / 2.0f;
    drawDialog(renderer, {x, y, boxWidth, boxHeight});
    write(renderer, Face::Medium, 18, x + 24, y + 36, "Atalhos", kText);
    const char* close = "F1 ou Esc fecha";
    write(renderer, Face::Regular, 12.5f, x + boxWidth - 24.0f - textWidth(renderer, Face::Regular, 12.5f, close), y + 35, close, kTextOff);
    const float column = (boxWidth - 72.0f) / 2.0f;
    const float keyRoom = 132.0f;
    const auto list = [&](std::span<const Shortcut> items, float left) {
        float line = y + 70.0f;
        for (const Shortcut& item : items) {
            if (item.key == nullptr) {
                if (line > y + 75.0f) line += 6.0f;
                write(renderer, Face::Medium, 12.5f, left, line + 4.0f, item.does, kWirePreview);
                line += 24.0f;
                continue;
            }
            const std::string key = fitted(renderer, Face::Medium, 12.0f, item.key, keyRoom - 14.0f);
            const float keyWidth = textWidth(renderer, Face::Medium, 12.0f, key) + 14.0f;
            const SDL_FRect chip{left, line - 9.0f, keyWidth, 21.0f};
            fillRound(renderer, chip, 5, kPanelButton);
            fillRect(renderer, {chip.x + 3.0f, chip.y + chip.h - 2.0f, chip.w - 6.0f, 1.0f}, kRule);
            write(renderer, Face::Medium, 12.0f, chip.x + 7.0f, chip.y + 15.0f, key, kText);
            write(renderer, Face::Regular, 13.0f, left + keyRoom, line + 6.0f, fitted(renderer, Face::Regular, 13.0f, item.does, column - keyRoom), kTextDim);
            line += 25.0f;
        }
    };
    list(kLeft, x + 24.0f);
    list(kRight, x + 48.0f + column);
    const char* fonts = "Fontes Ubuntu, © Canonical Ltd. Ubuntu Font Licence 1.0: design.ubuntu.com/font";
    write(renderer, Face::Regular, 11.5f, x + 24, y + boxHeight - 18.0f, fitted(renderer, Face::Regular, 11.5f, fonts, boxWidth - 48.0f), kTextOff);
}

}  // namespace

SDL_FRect profileRow(std::size_t row, std::size_t rows, int width, int height) {
    const float boxHeight = static_cast<float>(rows) * kProfileRowHeight + 146.0f;
    const float left = (static_cast<float>(width) - kProfileBoxWidth) / 2.0f;
    const float top = (static_cast<float>(height) - boxHeight) / 2.0f;
    return {left + 12.0f, top + 44.0f + static_cast<float>(row) * kProfileRowHeight, kProfileBoxWidth - 24.0f,
            kProfileRowHeight - 4.0f};
}

void drawProfiles(SDL_Renderer* renderer, const ProfileList& list, int width, int height) {
    const std::size_t rows = list.names.size();
    const SDL_FRect first = profileRow(0, rows, width, height);
    const float boxHeight = static_cast<float>(rows) * kProfileRowHeight + 146.0f;
    const SDL_FRect box{first.x - 12.0f, first.y - 44.0f, kProfileBoxWidth, boxHeight};
    dimScreen(renderer, width, height);
    drawDialog(renderer, box);
    write(renderer, Face::Medium, 16, box.x + 16, box.y + 30, "Perfis nesta máquina", kText);
    const char* close = "Esc fecha";
    write(renderer, Face::Regular, 12.0f, box.x + box.w - 16.0f - textWidth(renderer, Face::Regular, 12.0f, close), box.y + 29, close, kTextOff);

    // Each player with the first letter of the name in a circle; the one playing now marked.
    for (std::size_t i = 0; i < rows; ++i) {
        const SDL_FRect row = profileRow(i, rows, width, height);
        const bool current = i == list.current;
        fillRound(renderer, row, 6, current ? kPanelActive : kPanelButton);
        const Vec2 badge{row.x + 15.0f, row.y + row.h / 2.0f};
        Mesh disc;
        disc.addDisc(badge, 9.0f, current ? kWirePreview : kPanelHover);
        disc.flush(renderer);
        const std::string initial = list.names[i].empty() ? "?" : list.names[i].substr(0, (static_cast<unsigned char>(list.names[i][0]) & 0x80) != 0 ? 2 : 1);
        write(renderer, Face::Medium, 11.0f, badge.x - textWidth(renderer, Face::Medium, 11.0f, initial) / 2.0f, badge.y + 4.0f, initial,
              current ? kBackground : kText);
        write(renderer, Face::Regular, kUiText, row.x + 32, row.y + 18, list.names[i], kText);
        if (current) {
            const char* tag = "jogando agora";
            write(renderer, Face::Regular, 11.5f, row.x + row.w - 10.0f - textWidth(renderer, Face::Regular, 11.5f, tag), row.y + 17.5f, tag, kTextDim);
        }
    }

    // A new one: its name typed in a field, Enter makes it.
    const float y = box.y + boxHeight - 88.0f;
    write(renderer, Face::Regular, 12.5f, box.x + 16, y + 12, "Novo perfil", kTextDim);
    const SDL_FRect field{box.x + 12.0f, y + 22.0f, box.w - 24.0f, 32.0f};
    fillRound(renderer, field, 7, kPanel);
    frameRound(renderer, field, 7, 1, list.typing.empty() ? kRule : kWirePreview);
    if (list.typing.empty()) {
        write(renderer, Face::Regular, kUiText, field.x + 10, field.y + 21, "Digite um nome", kTextOff);
    } else {
        write(renderer, Face::Regular, kUiText, field.x + 10, field.y + 21, list.typing + "_", kText);
    }
    write(renderer, Face::Regular, 12.0f, box.x + 16, y + 74, "Clique num perfil para trocar. Enter cria o novo.", kTextOff);
}

void drawPrompt(SDL_Renderer* renderer, const char* title, const std::string& text, int width, int height, const char* unit) {
    constexpr float kWidth = 540;
    constexpr float kHeight = 124;
    const float x = (static_cast<float>(width) - kWidth) / 2.0f;
    const float y = (static_cast<float>(height) - kHeight) / 2.0f;
    dimScreen(renderer, width, height);
    drawDialog(renderer, {x, y, kWidth, kHeight});
    write(renderer, Face::Medium, 15, x + 18, y + 30, title, kText);
    const SDL_FRect field{x + 16, y + 44, kWidth - 32, 34};
    fillRound(renderer, field, 7, kPanel);
    frameRound(renderer, field, 7, 1, kWirePreview);
    const float unitWidth = unit[0] != '\0' ? textWidth(renderer, Face::Medium, 15, unit) + 8.0f : 0.0f;
    std::string shown = text + "_";
    while (textWidth(renderer, Face::Mono, 15, shown) > field.w - 20.0f - unitWidth && shown.size() > 1) {
        shown.erase(0, 1);
        while (!shown.empty() && (static_cast<unsigned char>(shown[0]) & 0xC0) == 0x80) shown.erase(0, 1);
    }
    write(renderer, Face::Mono, 15, field.x + 10, field.y + 22, shown, kText);
    if (unit[0] != '\0') {
        const float after = field.x + 10.0f + textWidth(renderer, Face::Mono, 15, shown) + 4.0f;
        write(renderer, Face::Medium, 15, after, field.y + 22, unit, kWirePreview);
    }
    write(renderer, Face::Regular, 12.5f, x + 18, y + 104, "Enter confirma  ·  Esc cancela", kTextDim);
}

// The program button and the three views share the toolbar's free end: at full size when there's room, the
// program's button with its whole name; narrower when the window is, never over the speed buttons.
struct ViewButtons {
    SDL_FRect program;
    SDL_FRect views[kViewCount];
};

ViewButtons viewButtons(float rightEdge) {
    const int lastTool = static_cast<int>(std::size(kToolbar)) - 1;
    const float start = toolbarButtonX(lastTool) + kToolbar[lastTool].width + 10.0f;
    const float end = rightEdge - 10.0f;
    const float room = end - start;
    float view = 74.0f;
    float program = 124.0f;
    if (room < program + 8.0f + static_cast<float>(kViewCount) * (view + 2.0f)) {
        view = std::max(44.0f, (room - 8.0f) / static_cast<float>(kViewCount + 1) - 2.0f);
        program = view;
    }
    ViewButtons buttons;
    float x = end - static_cast<float>(kViewCount) * (view + 2.0f) + 2.0f;
    for (int which = 0; which < kViewCount; ++which) {
        buttons.views[which] = {x + static_cast<float>(which) * (view + 2.0f), 8.0f, view, 28.0f};
    }
    buttons.program = {x - 8.0f - program, 8.0f, program, 28.0f};
    return buttons;
}

SDL_FRect programSwitchRect(float rightEdge) { return viewButtons(rightEdge).program; }

SDL_FRect viewSwitchRect(float rightEdge, int which) { return viewButtons(rightEdge).views[which]; }

SDL_FRect panelNextButton(const SDL_FRect& area) { return {area.x + 14.0f, area.y + 12.0f, 182.0f, 34.0f}; }

SDL_FRect panelPlanArea(const SDL_FRect& area) { return {area.x + 10.0f, area.y + 58.0f, area.w - 20.0f, area.h - 68.0f}; }

XrayParts xrayParts(const SDL_FRect& area) {
    XrayParts p;
    const float sideW = std::clamp(area.w * 0.3f, 260.0f, 420.0f);
    const float top = area.y + 100.0f;
    p.side = {area.x + area.w - sideW - 10.0f, top, sideW, area.y + area.h - top - 10.0f};
    const float left = area.x + 10.0f;
    const float leftW = p.side.x - 12.0f - left;
    p.strip = {left + 4.0f, top, leftW - 8.0f, 58.0f};
    const float planTop = top + 64.0f;
    p.plan = {left, planTop, leftW, area.y + area.h - 62.0f - planTop};
    p.legend = {left + 4.0f, p.plan.y + p.plan.h, leftW - 8.0f, 60.0f};  // three lines of short names
    return p;
}

// The diagram's corner buttons: 0 farther, 1 closer, 2 the whole diagram, 3 the Dec | Hex switch, left of them.
SDL_FRect zoomButtonRect(const SDL_FRect& plan, int which) {
    if (which == 3) return {plan.x + plan.w - 10.0f - 3.0f * 34.0f - 82.0f, plan.y + 8.0f, 76.0f, 26.0f};
    return {plan.x + plan.w - 10.0f - static_cast<float>(3 - which) * 34.0f, plan.y + 8.0f, 30.0f, 26.0f};
}

SDL_FRect xrayStageRect(const SDL_FRect& area, int stage) {
    const float width = std::min(128.0f, (area.w - 28.0f - 5.0f * 6.0f) / 6.0f);
    return {area.x + 14.0f + static_cast<float>(stage) * (width + 6.0f), area.y + 58.0f, width, 30.0f};
}

// The language switch: one half for each language, each named in itself so anyone finds their own.
SDL_FRect homeLanguageRect(int width, i18n::Language language) {
    constexpr float kHalf = 110.0f;
    const float right = static_cast<float>(width) - 24.0f;
    return {right - kHalf * static_cast<float>(i18n::kLanguageCount - static_cast<int>(language)), 24.0f, kHalf, 34.0f};
}

void showInsideOnMenu(bool open) { gInsideOpen = open; }
SDL_FRect insideLockRect(int width, int height) { return computersLockRect(width, height); }

HomeChoice homeAt(float x, float y, int width, int height, bool computers) {
    if (inside(homeLanguageRect(width, i18n::Language::Portuguese), x, y)) return HomeChoice::Portuguese;
    if (inside(homeLanguageRect(width, i18n::Language::English), x, y)) return HomeChoice::English;
    if (computers) {
        for (int i = 0; i < kComputerCardCount; ++i) {
            if (inside(computerCard(i, width, height), x, y)) return kComputerCards[i].choice;
            if (gInsideOpen && inside(computerInsideRect(i, width, height), x, y)) return i == 0 ? HomeChoice::Inside16 : HomeChoice::Inside32;
        }
        if (!gInsideOpen && inside(computersLockRect(width, height), x, y)) return HomeChoice::InsideLock;
        return inside(computersBackRect(width, height), x, y) ? HomeChoice::Back : HomeChoice::None;
    }
    const HomeLayout home = homeLayout(width, height);
    if (inside(home.hero, x, y)) return HomeChoice::Lessons;
    for (int i = 0; i < 5; ++i) {
        if (inside(home.tiles[i], x, y)) return kHomeTiles[i];
    }
    return HomeChoice::None;
}

namespace {
SDL_Texture* figureTexture(SDL_Renderer* renderer, TheoryFigure (*make)(), int w, int h, bool redraw);  // below
void drawComputerCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY);
void drawHomeCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const HomeInfo& info);
void drawHomeLanguages(SDL_Renderer* renderer, int width, float mouseX, float mouseY);
}  // namespace

// Behind the menu: circuits from every act, faded, tilted a little and drifting slowly, like prints pinned
// on a workbench.
void drawHomeBackdrop(SDL_Renderer* renderer, int width, int height) {
    static constexpr float kTilt[] = {-6.0f, 4.0f, -3.0f, 5.0f, -5.0f, 3.0f, -4.0f, 6.0f, -2.0f};
    static constexpr float kNudgeX[] = {-0.10f, 0.05f, 0.12f, -0.06f, 0.0f, 0.08f, -0.12f, 0.02f, 0.10f};
    static constexpr float kNudgeY[] = {0.06f, -0.10f, 0.04f, 0.10f, -0.04f, -0.08f, 0.02f, 0.12f, -0.06f};
    const std::vector<TheoryFigure (*)()>& figures = backdropFigures();
    const float cellW = static_cast<float>(width) / 3.0f;
    const float cellH = static_cast<float>(height) / 3.0f;
    const float seconds = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    for (std::size_t i = 0; i < figures.size() && i < 9; ++i) {
        SDL_Texture* texture = figureTexture(renderer, figures[i], 720, 440, false);
        if (texture == nullptr) continue;
        const auto column = static_cast<float>(i % 3);
        const auto row = static_cast<float>(i / 3);
        const float w = cellW * 1.3f;
        const float h = w * 440.0f / 720.0f;
        const float phase = static_cast<float>(i) * 1.7f;
        const float cx = cellW * (column + 0.5f + kNudgeX[i]) + std::sin(seconds * 0.07f + phase) * 14.0f;
        const float cy = cellH * (row + 0.5f + kNudgeY[i]) + std::cos(seconds * 0.05f + phase) * 10.0f;
        const SDL_FRect at{cx - w / 2.0f, cy - h / 2.0f, w, h};
        SDL_SetTextureAlphaMod(texture, 62);
        SDL_RenderTextureRotated(renderer, texture, nullptr, &at, static_cast<double>(kTilt[i] + std::sin(seconds * 0.04f + phase) * 0.8f), nullptr,
                                 SDL_FLIP_NONE);
        SDL_SetTextureAlphaMod(texture, 255);
    }
}

// A chip, the game's mark: a body with a notch, and pins down both sides.
void drawChipMark(SDL_Renderer* renderer, Vec2 centre, float size) {
    const SDL_FRect body{centre.x - size * 0.34f, centre.y - size * 0.5f, size * 0.68f, size};
    for (int pin = 0; pin < 4; ++pin) {
        const float y = body.y + size * (0.16f + 0.23f * static_cast<float>(pin));
        fillRound(renderer, {body.x - size * 0.16f, y, size * 0.2f, size * 0.09f}, 1.5f, kWirePreview);
        fillRound(renderer, {body.x + body.w - size * 0.04f, y, size * 0.2f, size * 0.09f}, 1.5f, kWirePreview);
    }
    fillRound(renderer, body, size * 0.1f, mixed(kPanelRaised, kWirePreview, 0.25f));
    frameRound(renderer, body, size * 0.1f, 2.0f, kWirePreview);
    fillRound(renderer, {centre.x - size * 0.09f, body.y - size * 0.05f, size * 0.18f, size * 0.12f}, size * 0.06f, kBackground);
    fillRound(renderer, {body.x + size * 0.12f, body.y + size * 0.14f, size * 0.1f, size * 0.1f}, size * 0.05f, kWirePreview);
}

void drawHome(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const HomeInfo& info, bool computers) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    drawHomeBackdrop(renderer, width, height);
    const HomeLayout home = homeLayout(width, height);
    // A sheet of dark glass under the menu, so the circuits behind never get in the way of reading.
    fillRound(renderer, home.panel, 24, kBackground, 0.9f);
    frameRound(renderer, home.panel, 24, 1.0f, mixed(kBackground, kWirePreview, 0.35f));
    const float x = home.panel.x + 34.0f;
    drawChipMark(renderer, {x + 22.0f, home.panel.y + 60.0f}, 44.0f);
    write(renderer, Face::Medium, 44, x + 60.0f, home.panel.y + 76.0f, "Bancada", kText);
    write(renderer, Face::Regular, 16, x + 62.0f, home.panel.y + 100.0f, "Do LED ao computador, montando cada peça", kWirePreview);
    if (computers) {
        drawComputerCards(renderer, width, height, mouseX, mouseY);
    } else {
        drawHomeCards(renderer, width, height, mouseX, mouseY, info);
    }
    // The keys, on a strip of the same glass, so the circuits behind don't cross the words.
    const char* keys = computers ? "Esc: voltar ao menu" : "Esc ou Enter: continuar  ·  F1 na bancada: atalhos";
    const float keysY = std::min(static_cast<float>(height) - 12.0f, home.panel.y + home.panel.h + 26.0f);
    fillRound(renderer, {home.panel.x, keysY - 17.0f, textWidth(renderer, Face::Regular, 12, keys) + 20.0f, 24.0f}, 8, kBackground, 0.9f);
    write(renderer, Face::Regular, 12, home.panel.x + 10.0f, keysY, keys, kTextDim);
    // Who made it, on the right of the same line.
    const std::string author = i18n::format("Feito por {}  ·  {}  ·  {}", {"Rildo de Cássio Estácio da Silva", "rces.ecomp@poli.br", "github.com/RildoEstacioS"});
    const float authorWidth = textWidth(renderer, Face::Regular, 12, author);
    const float authorX = home.panel.x + home.panel.w - authorWidth - 10.0f;
    fillRound(renderer, {authorX - 10.0f, keysY - 17.0f, authorWidth + 20.0f, 24.0f}, 8, kBackground, 0.9f);
    write(renderer, Face::Regular, 12, authorX, keysY, author, kTextDim);
    drawHomeLanguages(renderer, width, mouseX, mouseY);
}

namespace {

void drawComputerCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY) {
    const SDL_FRect first = computerCard(0, width, height);
    write(renderer, Face::Medium, 18, first.x, first.y - 14.0f, "Escolha um computador para programar", kText);
    for (int i = 0; i < kComputerCardCount; ++i) {
        const ComputerCard& card = kComputerCards[i];
        const SDL_FRect r = computerCard(i, width, height);
        const bool hovered = inside(r, mouseX, mouseY) && card.ready;
        fillRound(renderer, r, 14, hovered ? mixed(kPanelRaised, kToneProgram, 0.12f) : kPanelRaised);
        frameRound(renderer, r, 14, hovered ? 1.8f : 1.0f, hovered ? kToneProgram : kBodyEdge);
        drawChipMark(renderer, {r.x + 34.0f, r.y + 38.0f}, 34.0f);
        write(renderer, Face::Medium, 20, r.x + 64.0f, r.y + 36.0f, card.title, kText);
        write(renderer, Face::Regular, 12.5f, r.x + 64.0f, r.y + 56.0f, fitted(renderer, Face::Regular, 12.5f, card.subtitle, r.w - 80.0f), kTextDim);
        // What it has, a row each.
        float y = r.y + 92.0f;
        for (const auto& spec : card.specs) {
            fillRect(renderer, {r.x + 20.0f, y - 15.0f, r.w - 40.0f, 1.0f}, kRule);
            write(renderer, Face::Regular, 12.5f, r.x + 20.0f, y, spec[0], kTextDim);
            write(renderer, Face::Regular, 12.5f, r.x + 128.0f, y, fitted(renderer, Face::Regular, 12.5f, spec[1], r.w - 148.0f), kText);
            y += 24.0f;
        }
        const char* open = "Abrir e programar  ›";
        const float openWidth = textWidth(renderer, Face::Medium, 13.0f, open);
        write(renderer, Face::Medium, 13.0f, r.x + r.w - 20.0f - openWidth, r.y + r.h - 16.0f, open, hovered ? kText : kToneProgram);
    }
    for (int i = 0; i < kComputerCardCount && gInsideOpen; ++i) {
        const SDL_FRect r = computerInsideRect(i, width, height);
        const bool over = inside(r, mouseX, mouseY);
        fillRound(renderer, r, 10, over ? mixed(kPanelRaised, kSelected, 0.16f) : kPanelButton);
        frameRound(renderer, r, 10, 1.0f, over ? kSelected : kRule);
        const char* text = "Por dentro: portas, NANDs e transistores";
        const float w = textWidth(renderer, Face::Medium, kUiText, text);
        write(renderer, Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 24.0f, fitted(renderer, Face::Medium, kUiText, text, r.w - 16.0f), over ? kText : kTextDim);
    }
    if (!gInsideOpen) {
        const SDL_FRect lock = computersLockRect(width, height);
        const bool over = inside(lock, mouseX, mouseY);
        fillRound(renderer, lock, 8, over ? kPanelHover : kPanelButton);
        const float w = textWidth(renderer, Face::Medium, kUiText, "Por dentro (senha)");
        write(renderer, Face::Medium, kUiText, lock.x + (lock.w - w) / 2.0f, lock.y + 22.0f, "Por dentro (senha)", over ? kText : kTextDim);
    }
    const SDL_FRect back = computersBackRect(width, height);
    const bool hovered = inside(back, mouseX, mouseY);
    fillRound(renderer, back, 8, hovered ? kPanelHover : kPanelButton);
    const float w = textWidth(renderer, Face::Medium, kUiText, "Voltar");
    write(renderer, Face::Medium, kUiText, back.x + (back.w - w) / 2.0f, back.y + 22.0f, "Voltar", kText);
}

// The small pictures on the home's tiles, each in its tile's colour.
enum class HomeIcon : std::uint8_t { Continue, New, Examples, Open, Computers };

void drawHomeIcon(SDL_Renderer* renderer, HomeIcon icon, Vec2 c, Color color) {
    Mesh mesh;
    switch (icon) {
        case HomeIcon::Continue:
            mesh.addFan({{c.x - 6.0f, c.y - 9.0f}, {c.x + 9.0f, c.y}, {c.x - 6.0f, c.y + 9.0f}}, {c.x - 1.0f, c.y}, color);
            mesh.flush(renderer);
            return;
        case HomeIcon::New:
            mesh.addLine({c.x - 9.0f, c.y}, {c.x + 9.0f, c.y}, 3.0f, color);
            mesh.addLine({c.x, c.y - 9.0f}, {c.x, c.y + 9.0f}, 3.0f, color);
            mesh.flush(renderer);
            return;
        case HomeIcon::Examples:
            for (int i = 0; i < 4; ++i) {
                const float dx = i % 2 == 0 ? -9.5f : 1.5f;
                const float dy = i < 2 ? -9.5f : 1.5f;
                fillRound(renderer, {c.x + dx, c.y + dy, 8.0f, 8.0f}, 2.0f, i == 3 ? mixed(kPanelRaised, color, 0.5f) : color);
            }
            return;
        case HomeIcon::Open:
            fillRound(renderer, {c.x - 11.0f, c.y - 9.0f, 9.0f, 5.0f}, 2.0f, color);
            fillRound(renderer, {c.x - 11.0f, c.y - 6.0f, 22.0f, 15.0f}, 3.0f, color);
            fillRound(renderer, {c.x - 8.0f, c.y - 2.0f, 16.0f, 2.0f}, 1.0f, mixed(color, kBackground, 0.45f));
            return;
        case HomeIcon::Computers: drawChipMark(renderer, c, 22.0f); return;
    }
}

// A tile: its icon in a rounded square of its colour, the name and a line under it; lit by the mouse.
void drawHomeTile(SDL_Renderer* renderer, const SDL_FRect& r, HomeIcon icon, Color tone, const char* title, const char* line, float mouseX, float mouseY) {
    const bool hovered = inside(r, mouseX, mouseY);
    fillRound(renderer, r, 14, hovered ? mixed(kPanelRaised, tone, 0.12f) : kPanelRaised, 0.96f);
    frameRound(renderer, r, 14, hovered ? 1.8f : 1.0f, hovered ? tone : kBodyEdge);
    const float iconSize = std::min(46.0f, r.h - 28.0f);
    const SDL_FRect badge{r.x + 16.0f, r.y + (r.h - iconSize) / 2.0f, iconSize, iconSize};
    fillRound(renderer, badge, 11, mixed(kBackground, tone, 0.2f));
    frameRound(renderer, badge, 11, 1.0f, mixed(kBackground, tone, 0.55f));
    drawHomeIcon(renderer, icon, {badge.x + badge.w / 2.0f, badge.y + badge.h / 2.0f}, tone);
    const float tx = badge.x + badge.w + 14.0f;
    const float room = r.x + r.w - tx - 30.0f;
    write(renderer, Face::Medium, 16, tx, r.y + r.h / 2.0f - 2.0f, fitted(renderer, Face::Medium, 16, title, room), kText);
    write(renderer, Face::Regular, 12.5f, tx, r.y + r.h / 2.0f + 17.0f, fitted(renderer, Face::Regular, 12.5f, line, room), kTextDim);
    drawIcon(renderer, Icon::ChevronRight, {r.x + r.w - 20.0f, r.y + r.h / 2.0f}, 16, hovered ? tone : kTextOff);
}

void drawHomeCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const HomeInfo& info) {
    const HomeLayout home = homeLayout(width, height);
    const Color green = kKnobOn;

    // The lessons: what the course is, how far the player got, and the act that comes next.
    const SDL_FRect hero = home.hero;
    const bool heroHover = inside(hero, mouseX, mouseY);
    fillRound(renderer, hero, 18, mixed(kPanelRaised, green, heroHover ? 0.2f : 0.13f));
    frameRound(renderer, hero, 18, heroHover ? 2.0f : 1.4f, mixed(kBodyEdge, green, heroHover ? 1.0f : 0.7f));
    const float hx = hero.x + 24.0f;
    const float hw = hero.w - 48.0f;
    write(renderer, Face::Medium, 12.5f, hx, hero.y + 32.0f, "AULAS", green);
    float y = hero.y + 64.0f;
    for (const std::string& line : wrapped(renderer, Face::Medium, 24, "Da eletricidade ao sistema operacional", hw)) {
        write(renderer, Face::Medium, 24, hx, y, line, kText);
        y += 30.0f;
    }
    for (const std::string& line : wrapped(renderer, Face::Regular, 13.5f, info.about, hw)) {
        write(renderer, Face::Regular, 13.5f, hx, y + 2.0f, line, kTextDim);
        y += 19.0f;
    }
    // The progress, as a bar.
    const float barY = hero.y + hero.h - 104.0f;
    write(renderer, Face::Regular, 13, hx, barY - 10.0f, info.lessonsLine, kText);
    const float share = info.total > 0 ? static_cast<float>(info.done) / static_cast<float>(info.total) : 0.0f;
    const std::string percent = std::to_string(static_cast<int>(std::lround(share * 100.0f))) + "%";
    write(renderer, Face::Medium, 13, hx + hw - textWidth(renderer, Face::Medium, 13, percent), barY - 10.0f, percent, green);
    fillRound(renderer, {hx, barY, hw, 8.0f}, 4, mixed(kBackground, green, 0.18f));
    if (share > 0.0f) fillRound(renderer, {hx, barY, std::max(8.0f, hw * share), 8.0f}, 4, green);
    if (!info.next.empty()) {
        write(renderer, Face::Regular, 13, hx, barY + 30.0f, fitted(renderer, Face::Regular, 13, i18n::format("Próximo: {}", {info.next}), hw), kTextDim);
    }
    const SDL_FRect button{hx, hero.y + hero.h - 52.0f, std::min(hw, 220.0f), 36.0f};
    fillRound(renderer, button, 9, heroHover ? green : mixed(kPanelRaised, green, 0.55f));
    const char* go = info.done == 0 ? "Começar as aulas" : "Continuar as aulas";
    drawIcon(renderer, Icon::Play, {button.x + 20.0f, button.y + button.h / 2.0f}, 14, heroHover ? kBackground : kText);
    write(renderer, Face::Medium, 14, button.x + 36.0f, button.y + 23.0f, go, heroHover ? kBackground : kText);

    drawHomeTile(renderer, home.tiles[0], HomeIcon::Continue, kWireOn, "Continuar", "Volta ao último circuito", mouseX, mouseY);
    drawHomeTile(renderer, home.tiles[1], HomeIcon::New, kBusOn, "Novo circuito", "Uma bancada vazia", mouseX, mouseY);
    drawHomeTile(renderer, home.tiles[2], HomeIcon::Examples, Color{176, 150, 255}, "Exemplos", "Circuitos e programas", mouseX, mouseY);
    drawHomeTile(renderer, home.tiles[3], HomeIcon::Open, Color{150, 182, 206}, "Abrir arquivo", "Um projeto salvo", mouseX, mouseY);
    drawHomeTile(renderer, home.tiles[4], HomeIcon::Computers, kToneProgram, "Editor de código", "Escreva, salve, abra e rode programas", mouseX, mouseY);
    // The two computers, as badges on the right of their tile.
    const SDL_FRect wide = home.tiles[4];
    float bx = wide.x + wide.w - 44.0f;
    for (const char* badge : {"32 bits · MIPS", "16 bits"}) {
        const float w = textWidth(renderer, Face::Medium, 12, badge) + 20.0f;
        bx -= w;
        if (bx < wide.x + wide.w * 0.55f) break;
        const SDL_FRect pill{bx, wide.y + wide.h / 2.0f - 12.0f, w, 24.0f};
        fillRound(renderer, pill, 12, mixed(kBackground, kToneProgram, 0.18f));
        frameRound(renderer, pill, 12, 1.0f, mixed(kBackground, kToneProgram, 0.6f));
        write(renderer, Face::Medium, 12, pill.x + 10.0f, pill.y + 16.5f, badge, kToneProgram);
        bx -= 8.0f;
    }

    // What's still to come, in one quiet line at the foot of the glass.
    const float fy = home.panel.y + home.panel.h - 22.0f;
    write(renderer, Face::Medium, 12.5f, home.content.x, fy, "Em breve", kTextDim);
    write(renderer, Face::Regular, 12.5f, home.content.x + textWidth(renderer, Face::Medium, 12.5f, "Em breve") + 12.0f, fy,
          "Puzzles com estrelas  ·  Laboratório para medir e experimentar", kTextOff);
}

void drawHomeLanguages(SDL_Renderer* renderer, int width, float mouseX, float mouseY) {
    for (int l = 0; l < i18n::kLanguageCount; ++l) {
        const auto language = static_cast<i18n::Language>(l);
        const SDL_FRect r = homeLanguageRect(width, language);
        const bool chosen = i18n::language() == language;
        fillRound(renderer, r, 8, chosen ? kPanelActive : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
        if (chosen) frameRound(renderer, r, 8, 1.4f, kWirePreview);
        // The names stay as they are in any language: each is written in its own.
        const std::string_view name = i18n::nameOf(language);
        const float w = textFor(renderer).measure(Face::Medium, kUiText, name);
        textFor(renderer).draw(Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 22.0f, name, floatColor(chosen ? kText : kTextDim));
    }
}

}  // namespace

namespace {

// The examples look like the lessons: their folders down the left, the chosen folder's examples on the right,
// as wide cards with the whole title and what the example is.
constexpr float kGalleryCardHeight = 84.0f;
constexpr float kGalleryCardGap = 12.0f;

SDL_FRect galleryFolderRect(int group, int height) {
    const float room = static_cast<float>(height) - kListTop - 34.0f + 5.0f;
    const float step = std::clamp(room / static_cast<float>(galleryGroups().size()), 36.0f, kActRowHeight + 5.0f);
    return {24.0f, kListTop + static_cast<float>(group) * step, kActListWidth, step - 5.0f};
}

int galleryColumns(const SDL_FRect& area) { return area.w >= 820.0f ? 3 : area.w >= 520.0f ? 2 : 1; }

// The examples of a folder, in order, as indexes into galleryItems().
std::vector<int> galleryFolder(int group) {
    std::vector<int> items;
    for (int i = 0; i < static_cast<int>(galleryItems().size()); ++i) {
        if (galleryItems()[static_cast<std::size_t>(i)].group == group) items.push_back(i);
    }
    return items;
}

SDL_FRect galleryCardRect(int position, int width, int height) {
    const SDL_FRect area = lessonContentArea(width, height);
    const int columns = galleryColumns(area);
    const float cardWidth = (area.w - kGalleryCardGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const int column = position % columns;
    const int row = position / columns;
    return {area.x + static_cast<float>(column) * (cardWidth + kGalleryCardGap), area.y + 66.0f + static_cast<float>(row) * (kGalleryCardHeight + kGalleryCardGap),
            cardWidth, kGalleryCardHeight};
}

}  // namespace

int galleryAt(float x, float y, int width, int height, int group) {
    if (inside(lessonBackRect(), x, y)) return kGalleryBack;
    for (int g = 0; g < static_cast<int>(galleryGroups().size()); ++g) {
        if (inside(galleryFolderRect(g, height), x, y)) return kGalleryGroupFirst + g;
    }
    const std::vector<int> items = galleryFolder(group);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (inside(galleryCardRect(static_cast<int>(i), width, height), x, y)) return items[i];
    }
    return -1;
}

void drawGallery(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, int group) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const SDL_FRect back = lessonBackRect();
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Home, {back.x + 20.0f, back.y + back.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, back.x + 38.0f, back.y + 22.0f, "Início", kText);

    write(renderer, Face::Medium, 30, 24.0f, 96.0f, "Exemplos", kText);
    write(renderer, Face::Regular, 15, 24.0f + textWidth(renderer, Face::Medium, 30, "Exemplos") + 22.0f, 94.0f,
          i18n::format("Circuitos e programas prontos para abrir, mexer e entender  ·  {} exemplos", {std::to_string(galleryItems().size())}), kTextDim);

    // The folders, down the left.
    for (int g = 0; g < static_cast<int>(galleryGroups().size()); ++g) {
        const SDL_FRect r = galleryFolderRect(g, height);
        const bool chosen = g == group;
        fillRound(renderer, r, 10, chosen ? kPanelActive : inside(r, mouseX, mouseY) ? kPanelHover : kPanelRaised);
        if (chosen) frameRound(renderer, r, 10, 1.4f, kWirePreview);
        const std::vector<int> items = galleryFolder(g);
        const bool programs = !items.empty() && galleryItems()[static_cast<std::size_t>(items.front())].program >= 0;
        const SDL_FRect badge{r.x + 12.0f, r.y + (r.h - 24.0f) / 2.0f, 24.0f, 24.0f};
        fillRound(renderer, badge, 6, programs ? kBodyChip : kBodyBox);
        drawIcon(renderer, programs ? Icon::Step : Icon::Play, {badge.x + 12.0f, badge.y + 12.0f}, 13, kWirePreview);
        const float titleY = r.y + r.h * 0.42f;
        write(renderer, Face::Medium, 14.0f, r.x + 48.0f, titleY, fitted(renderer, Face::Medium, 14.0f, galleryGroups()[static_cast<std::size_t>(g)], r.w - 100.0f), kText);
        const std::string count = i18n::format(items.size() == 1 ? "{} exemplo" : "{} exemplos", {std::to_string(items.size())});
        write(renderer, Face::Regular, 11.5f, r.x + 48.0f, titleY + 17.0f, count, kTextDim);
    }

    // The chosen folder: its name, then a card for each example.
    const SDL_FRect area = lessonContentArea(width, height);
    write(renderer, Face::Medium, 22.0f, area.x, area.y + 26.0f, galleryGroups()[static_cast<std::size_t>(group)], kText);
    write(renderer, Face::Regular, 14.0f, area.x, area.y + 50.0f, "Clique num exemplo para abri-lo na bancada.", kTextDim);
    const std::vector<int> items = galleryFolder(group);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const GalleryItem& item = galleryItems()[static_cast<std::size_t>(items[i])];
        const SDL_FRect r = galleryCardRect(static_cast<int>(i), width, height);
        const bool hovered = inside(r, mouseX, mouseY);
        fillRound(renderer, r, 12, hovered ? kPanelHover : kPanelRaised);
        frameRound(renderer, r, 12, 1.0f, hovered ? kWirePreview : kBodyEdge);
        const SDL_FRect badge{r.x + 14.0f, r.y + 16.0f, 28.0f, 28.0f};
        fillRound(renderer, badge, 7, item.program >= 0 ? kBodyChip : kBodyBox);
        drawIcon(renderer, item.program >= 0 ? Icon::Step : Icon::Play, {badge.x + 14.0f, badge.y + 14.0f}, 14, kWirePreview);
        const float textX = r.x + 54.0f;
        const float room = r.w - 66.0f;
        write(renderer, Face::Medium, 15.0f, textX, r.y + 30.0f, fitted(renderer, Face::Medium, 15.0f, item.title, room), kText);
        const std::vector<std::string> line = wrapped(renderer, Face::Regular, 12.5f, item.line, room);
        for (std::size_t l = 0; l < line.size() && l < 2; ++l) {
            const std::string shown = l == 1 && line.size() > 2 ? fitted(renderer, Face::Regular, 12.5f, line[l] + " …", room) : line[l];
            write(renderer, Face::Regular, 12.5f, textX, r.y + 51.0f + 17.0f * static_cast<float>(l), shown, kTextDim);
        }
        if (item.versions.size() > 1) {
            const std::string versions = i18n::format("{} versões", {std::to_string(item.versions.size())});
            const float w = textWidth(renderer, Face::Medium, 11.0f, versions);
            write(renderer, Face::Medium, 11.0f, r.x + r.w - 14.0f - w, r.y + 30.0f, versions, kWirePreview);
        }
    }
    write(renderer, Face::Regular, 12, 24.0f, static_cast<float>(height) - 14.0f,
          "Abrir um exemplo guarda o que estava na bancada: Continuar, na tela inicial, traz de volta. Esc volta ao início.", kTextOff);
}

LessonListHit lessonListAt(float x, float y, int width, int height, const LessonListView& view) {
    if (inside(lessonBackRect(), x, y)) return {LessonListKind::Back, -1};
    if (inside(lessonResetRect(width), x, y)) return {LessonListKind::Reset, -1};
    if (inside(lessonUnlockRect(width), x, y)) return {LessonListKind::Unlock, -1};
    for (int a = 0; a < static_cast<int>(lessonActs().size()); ++a) {
        if (inside(actRowRect(a, height), x, y)) return {LessonListKind::Act, a};
    }
    const SDL_FRect area = lessonContentArea(width, height);
    if (!inside(area, x, y) || view.finished == nullptr || !(view.unlocked || actOpen(view.act, *view.finished))) return {};
    for (const ListItem& item : actContent(width, height, view)) {
        if (item.kind != LessonListKind::None && item.kind != LessonListKind::Planned && inside(item.rect, x, y)) return {item.kind, item.index};
    }
    return {};
}

float lessonListMaxScroll(int width, int height, const LessonListView& view) {
    float content = 0.0f;
    actContent(width, height, view, &content);
    return std::max(0.0f, content - lessonContentArea(width, height).h);
}

void drawLessonList(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const LessonListView& view) {
    static const std::vector<std::string> none;
    const std::vector<std::string>& finished = view.finished != nullptr ? *view.finished : none;
    const auto isDone = [&](std::string_view id) { return std::find(finished.begin(), finished.end(), id) != finished.end(); };
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const SDL_FRect back = lessonBackRect();
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Home, {back.x + 20.0f, back.y + back.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, back.x + 38.0f, back.y + 22.0f, "Início", kText);

    // Starting over takes two clicks: the first one says what the second will do.
    // The developer's switch, to look at any act without doing the ones before: nothing is marked done.
    const SDL_FRect unlock = lessonUnlockRect(width);
    fillRound(renderer, unlock, 8, view.unlocked ? kPanelActive : inside(unlock, mouseX, mouseY) ? kPanelHover : kPanelButton);
    if (view.unlocked) frameRound(renderer, unlock, 8, 1.4f, kWarning);
    drawLock(renderer, {unlock.x + 20.0f, unlock.y + unlock.h / 2.0f}, 15.0f, view.unlocked ? kWarning : kTextDim);
    write(renderer, Face::Medium, kUiText, unlock.x + 38.0f, unlock.y + 22.0f,
          view.unlocked ? "Dev: tudo aberto" : view.denied ? "Senha errada" : "Dev: abrir tudo",
          view.unlocked ? kText : view.denied ? kWarning : kTextDim);

    const SDL_FRect reset = lessonResetRect(width);
    fillRound(renderer, reset, 8, view.confirming ? Color{120, 48, 40} : inside(reset, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Reset, {reset.x + 20.0f, reset.y + reset.h / 2.0f}, 16, view.confirming ? kText : kTextDim);
    const char* resetLabel = view.confirming ? "Clique de novo para apagar" : "Zerar o progresso";
    write(renderer, Face::Medium, kUiText, reset.x + 38.0f, reset.y + 22.0f, resetLabel, view.confirming ? kText : kTextDim);
    if (view.confirming) {
        const char* warning = "Apaga as aulas feitas e as placas de todas as aulas deste jogador.";
        const float w = textWidth(renderer, Face::Regular, 12, warning);
        write(renderer, Face::Regular, 12, reset.x + reset.w - w, reset.y + reset.h + 18.0f, warning, kWarning);
    }

    std::size_t doneCount = 0;
    for (const Lesson& lesson : lessons()) doneCount += isDone(lesson.id) ? 1 : 0;
    write(renderer, Face::Medium, 30, 24.0f, 96.0f, "Aulas", kText);
    char progress[128];
    std::snprintf(progress, sizeof progress, i18n::tr("Do LED ao computador, um passo por vez  ·  %zu de %zu exercícios feitos"), doneCount,
                  lessons().size());
    write(renderer, Face::Regular, 15, 24.0f + textWidth(renderer, Face::Medium, 30, "Aulas") + 22.0f, 94.0f, progress, kTextDim);

    // The acts, down the left.
    for (int a = 0; a < static_cast<int>(lessonActs().size()); ++a) {
        const LessonAct& act = lessonActs()[static_cast<std::size_t>(a)];
        const SDL_FRect r = actRowRect(a, height);
        const bool open = view.unlocked || actOpen(a, finished);
        const bool chosen = a == view.act;
        const int total = static_cast<int>(actLessons(a).size());
        const int done = actDone(a, finished);
        fillRound(renderer, r, 10, chosen ? kPanelActive : inside(r, mouseX, mouseY) ? kPanelHover : kPanelRaised);
        if (chosen) frameRound(renderer, r, 10, 1.4f, kWirePreview);
        const Vec2 badge{r.x + 24.0f, r.y + r.h / 2.0f};
        if (!open) {
            drawLock(renderer, badge, 18.0f, kTextOff);
        } else if (done == total && total > 0) {
            fillRound(renderer, {badge.x - 13.0f, badge.y - 13.0f, 26.0f, 26.0f}, 13, kWirePreview);
            drawCheckMark(renderer, badge, 13.0f, kBackground);
        } else {
            fillRound(renderer, {badge.x - 13.0f, badge.y - 13.0f, 26.0f, 26.0f}, 13, kPanelButton);
            const std::string number = std::to_string(a + 1);
            const float w = textWidth(renderer, Face::Medium, 12.5f, number);
            write(renderer, Face::Medium, 12.5f, badge.x - w / 2.0f, badge.y + 4.5f, number, kText);
        }
        const std::string title(i18n::tr(act.title));
        const std::size_t colon = title.find(": ");
        const std::string name = colon == std::string::npos ? title : title.substr(colon + 2);
        const float textX = r.x + 48.0f;
        const float room = r.w - 48.0f - 52.0f;
        const float titleY = r.y + r.h * 0.42f;  // 21 in a row of 50
        write(renderer, Face::Medium, 14.0f, textX, titleY, fitted(renderer, Face::Medium, 14.0f, name, room), open ? kText : kTextOff);
        write(renderer, Face::Regular, 11.5f, textX, titleY + 17.0f, fitted(renderer, Face::Regular, 11.5f, act.line, room), open ? kTextDim : kTextOff);
        const std::string count = std::to_string(done) + "/" + std::to_string(total);
        const float countWidth = textWidth(renderer, Face::Medium, 12.0f, count);
        write(renderer, Face::Medium, 12.0f, r.x + r.w - 14.0f - countWidth, titleY, count, open ? kTextDim : kTextOff);
        // How far along, as a thin bar under the count.
        const SDL_FRect bar{r.x + r.w - 46.0f, titleY + 9.0f, 32.0f, 4.0f};
        fillRound(renderer, bar, 2, kPanelButton);
        if (total > 0 && done > 0) fillRound(renderer, {bar.x, bar.y, bar.w * static_cast<float>(done) / static_cast<float>(total), bar.h}, 2, kWirePreview);
    }

    // The chosen act: its name, then each reading and its exercises.
    const SDL_FRect area = lessonContentArea(width, height);
    const LessonAct& act = lessonActs()[static_cast<std::size_t>(view.act)];
    const bool open = view.unlocked || actOpen(view.act, finished);
    const SDL_Rect clip{static_cast<int>(area.x) - 2, static_cast<int>(area.y), static_cast<int>(area.w) + 4, static_cast<int>(area.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const float headY = area.y - view.scroll;
    write(renderer, Face::Medium, 22.0f, area.x, headY + 26.0f, act.title, kText);
    if (open) {
        write(renderer, Face::Regular, 14.0f, area.x, headY + 50.0f, act.line, kTextDim);
    } else {
        drawLock(renderer, {area.x + 9.0f, headY + 45.0f}, 16.0f, kWarning);
        const std::string before(i18n::tr(lessonActs()[static_cast<std::size_t>(view.act - 1)].title));
        write(renderer, Face::Medium, 14.0f, area.x + 24.0f, headY + 50.0f,
              i18n::format("Bloqueado: termine os exercícios do {} para abrir este ato.", {before.substr(0, before.find(':'))}), kWarning);
    }
    // How far the act has come, on the right of its name.
    const std::vector<int> actExercises = actLessons(view.act);
    int actFinished = 0;
    int nextExercise = -1;
    for (const int l : actExercises) {
        const bool done = isDone(lessons()[static_cast<std::size_t>(l)].id);
        actFinished += done ? 1 : 0;
        if (!done && nextExercise < 0 && open) nextExercise = l;
    }
    if (!actExercises.empty()) {
        const std::string count = i18n::format("{} de {} exercícios", {std::to_string(actFinished), std::to_string(actExercises.size())});
        const float countWidth = textWidth(renderer, Face::Medium, 13.0f, count);
        write(renderer, Face::Medium, 13.0f, area.x + area.w - countWidth, headY + 26.0f, count, actFinished == static_cast<int>(actExercises.size()) ? kKnobOn : kTextDim);
        const SDL_FRect bar{area.x + area.w - 160.0f, headY + 38.0f, 160.0f, 5.0f};
        fillRound(renderer, bar, 2.5f, kPanelButton);
        if (actFinished > 0) {
            fillRound(renderer, {bar.x, bar.y, bar.w * static_cast<float>(actFinished) / static_cast<float>(actExercises.size()), bar.h}, 2.5f, kWirePreview);
        }
    }
    int topicNumber = 0;
    int exerciseNumber = 0;
    for (const ListItem& item : actContent(width, height, view)) {
        if (!visibleIn(item.rect, width, height)) {
            if (item.kind == LessonListKind::Exercise) ++exerciseNumber;
            continue;
        }
        const SDL_FRect r = item.rect;
        const bool hovered = open && inside(r, mouseX, mouseY) && inside(area, mouseX, mouseY);
        if (item.kind == LessonListKind::None) {
            write(renderer, Face::Medium, 13.0f, r.x, r.y + 16.0f, item.text, kTextOff);
            continue;
        }
        if (item.kind == LessonListKind::Planned) {
            // A reading still to be written: there, but faded, and nothing to click.
            const PlannedTopic& next = act.planned[static_cast<std::size_t>(item.index)];
            fillRound(renderer, r, 12, kBackground);
            frameRound(renderer, r, 12, 1.0f, kRule);
            const float textX = r.x + 20.0f;
            const char* tag = "A ser implementado";
            const float tagWidth = textWidth(renderer, Face::Medium, 12.0f, tag) + 24.0f;
            const float room = r.w - tagWidth - 52.0f;
            write(renderer, Face::Medium, 15.0f, textX, r.y + 24.0f, fitted(renderer, Face::Medium, 15.0f, next.title, room), kTextDim);
            write(renderer, Face::Regular, 12.5f, textX, r.y + 43.0f, fitted(renderer, Face::Regular, 12.5f, next.line, room), kTextOff);
            const SDL_FRect tagRect{r.x + r.w - tagWidth - 16.0f, r.y + (r.h - 26.0f) / 2.0f, tagWidth, 26.0f};
            frameRound(renderer, tagRect, 13, 1.0f, kRule);
            write(renderer, Face::Medium, 12.0f, tagRect.x + 12.0f, tagRect.y + 17.5f, tag, kTextOff);
            continue;
        }
        if (item.kind == LessonListKind::Reading) {
            const Topic& topic = topics()[static_cast<std::size_t>(item.index)];
            topicNumber = item.index + 1;
            const bool read = isDone(readingId(topic));
            fillRound(renderer, r, 12, hovered ? kPanelHover : kPanel);
            frameRound(renderer, r, 12, 1.2f, hovered ? kWirePreview : open ? kBodyEdge : kRule);
            // A page with lines: the reading's mark.
            const SDL_FRect page{r.x + 16.0f, r.y + 17.0f, 30.0f, 36.0f};
            fillRound(renderer, page, 4, open ? kWirePreview : kTextOff);
            for (int line = 0; line < 4; ++line) {
                fillRect(renderer, {page.x + 6.0f, page.y + 8.0f + 6.0f * static_cast<float>(line), line == 3 ? 11.0f : 18.0f, 2.0f}, kPanel);
            }
            const float textX = r.x + 62.0f;
            const std::string title = i18n::format("Aula {}: {}", {std::to_string(topicNumber), i18n::tr(topic.title)});
            write(renderer, Face::Medium, 16.0f, textX, r.y + 30.0f, fitted(renderer, Face::Medium, 16.0f, title, r.w - 200.0f), open ? kText : kTextOff);
            write(renderer, Face::Regular, 13.0f, textX, r.y + 52.0f, fitted(renderer, Face::Regular, 13.0f, topic.line, r.w - 200.0f),
                  open ? kTextDim : kTextOff);
            const char* tag = read ? "Lida" : "Leitura";
            const float tagWidth = textWidth(renderer, Face::Medium, 12.0f, tag) + 24.0f;
            const SDL_FRect tagRect{r.x + r.w - tagWidth - 16.0f, r.y + (r.h - 26.0f) / 2.0f, tagWidth, 26.0f};
            fillRound(renderer, tagRect, 13, read ? kPanelActive : kPanelButton);
            write(renderer, Face::Medium, 12.0f, tagRect.x + 12.0f, tagRect.y + 17.5f, tag, read ? kText : kTextDim);
            continue;
        }
        const Lesson& lesson = lessons()[static_cast<std::size_t>(item.index)];
        const bool done = isDone(lesson.id);
        const bool next = item.index == nextExercise;
        ++exerciseNumber;
        fillRound(renderer, r, 10, hovered ? kPanelHover : next ? mixed(kPanelRaised, kWirePreview, 0.08f) : kPanelRaised);
        frameRound(renderer, r, 10, next ? 1.4f : 1.0f, hovered || next ? kWirePreview : done ? kPanelActive : open ? kBodyEdge : kRule);
        const SDL_FRect badge{r.x + 9.0f, r.y + 14.0f, 26.0f, 26.0f};
        if (!open) {
            drawLock(renderer, {badge.x + 13.0f, badge.y + 13.0f}, 15.0f, kTextOff);
        } else {
            fillRound(renderer, badge, 13, done ? kPanelActive : kPanelButton);
            if (done) {
                drawCheckMark(renderer, {badge.x + 13.0f, badge.y + 13.0f}, 13.0f, kText);
            } else {
                const std::string text = std::to_string(exerciseNumber);
                const float w = textWidth(renderer, Face::Medium, 12.5f, text);
                write(renderer, Face::Medium, 12.5f, badge.x + (badge.w - w) / 2.0f, badge.y + 18.0f, text, kTextDim);
            }
        }
        const float textX = r.x + 44.0f;
        const float room = r.w - 52.0f;
        write(renderer, Face::Medium, 11.0f, textX, r.y + 20.0f, lessonKindName(lesson), open ? lessonKindColor(lesson) : kTextOff);
        if (next) {
            const float nextWidth = textWidth(renderer, Face::Medium, 11.0f, "Próximo");
            write(renderer, Face::Medium, 11.0f, r.x + r.w - 12.0f - nextWidth, r.y + 20.0f, "Próximo", kWirePreview);
        }
        write(renderer, Face::Medium, 13.5f, textX, r.y + 39.0f, fitted(renderer, Face::Medium, 13.5f, lesson.title, room), open ? kText : kTextOff);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
    write(renderer, Face::Regular, 12, 24.0f, static_cast<float>(height) - 14.0f,
          "Leia a aula e faça os exercícios dela. Cada ato abre quando o anterior termina. Esc volta ao início.", kTextOff);
}

namespace {

// --- A reading -----------------------------------------------------------------------------------------
// A reading is one or more screens. A screen holds one idea: text, live circuits whose switches can be
// clicked, worked examples, common mistakes, a summary, and questions that must be answered right before
// the next screen opens. A reading without screens is one long page, scrolled.

constexpr float kReadingBarHeight = 64.0f;
constexpr float kReadingText = 16.0f;
constexpr float kReadingLine = 26.0f;
constexpr Color kReadingBody{214, 224, 232};
constexpr Color kReadingRight{86, 226, 150};
constexpr Color kReadingWrong{240, 120, 110};
constexpr Color kReadingExample{120, 190, 255};
// A block set apart from the text: its LED and trace, and where its text starts.
constexpr float kMarkTrace = 9.0f;     // from the column's left to the LED's centre and the trace
constexpr float kMarkIndent = 32.0f;   // from the column's left to the block's text

SDL_FRect readingBackRect() { return {24.0f, 15.0f, 160.0f, 34.0f}; }
SDL_FRect readingExercisesRect(int width) { return {static_cast<float>(width) - 24.0f - 230.0f, 15.0f, 230.0f, 34.0f}; }

float readingColumnWidth(int width) { return std::min(820.0f, static_cast<float>(width) - 96.0f); }

// A table's columns, as wide as their widest cell.
std::vector<std::vector<std::string>> tableCells(const char* text) {
    std::vector<std::vector<std::string>> rows;
    const std::string all(i18n::tr(text));
    std::size_t start = 0;
    while (start <= all.size()) {
        std::size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        std::vector<std::string> cells;
        const std::string row = all.substr(start, end - start);
        std::size_t from = 0;
        while (true) {
            const std::size_t bar = row.find('|', from);
            cells.push_back(row.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
            if (bar == std::string::npos) break;
            from = bar + 1;
        }
        rows.push_back(cells);
        start = end + 1;
    }
    return rows;
}

// A table row may start with a picture of what it names: "@npn:B Base (B)" is the NPN with its pin B
// marked, then the text "Base (B)". Parts are drawn the way the library draws them; wires as a short run.
struct CellPicture {
    std::string name;  // "" for none
    std::string pin;
};

CellPicture takePicture(std::string& cell) {
    CellPicture picture;
    if (cell.empty() || cell[0] != '@') return picture;
    const std::size_t space = cell.find(' ');
    const std::string tag = cell.substr(1, space == std::string::npos ? std::string::npos : space - 1);
    cell = space == std::string::npos ? std::string() : cell.substr(space + 1);
    const std::size_t colon = tag.find(':');
    picture.name = tag.substr(0, colon);
    if (colon != std::string::npos) picture.pin = tag.substr(colon + 1);
    return picture;
}

constexpr float kTableRow = 32.0f;
constexpr float kPictureRow = 54.0f;
constexpr float kPictureSlot = 64.0f;

struct TableShape {
    std::vector<std::vector<std::string>> rows;
    std::vector<CellPicture> pictures;  // one per row
    std::vector<float> heights;         // one per row
    bool pictured = false;
    float height = 0.0f;
};

TableShape tableShape(const char* text) {
    TableShape table;
    table.rows = tableCells(text);
    for (std::vector<std::string>& row : table.rows) {
        table.pictures.push_back(row.empty() ? CellPicture{} : takePicture(row[0]));
        if (!table.pictures.back().name.empty()) table.pictured = true;
    }
    for (const CellPicture& picture : table.pictures) {
        table.heights.push_back(picture.name.empty() ? kTableRow : kPictureRow);
        table.height += table.heights.back();
    }
    return table;
}

void drawCellPicture(SDL_Renderer* renderer, const CellPicture& picture, Vec2 centre) {
    struct Run {
        const char* name;
        Color color;
        float thickness;
    };
    static constexpr Run kRuns[] = {{"wire1", kWireOn, 3.0f}, {"wire0", kWireOff, 3.0f}, {"bus", kBusOn, 6.0f},
                                    {"bus1", kBusOn, 6.0f},   {"bus0", kBusOff, 6.0f}};
    for (const Run& run : kRuns) {
        if (picture.name != run.name) continue;
        Mesh mesh;
        mesh.addLine({centre.x - 20.0f, centre.y}, {centre.x + 20.0f, centre.y}, run.thickness, run.color);
        mesh.flush(renderer);
        return;
    }
    static const std::pair<const char*, PartKind> kParts[] = {
        {"and", PartKind::And}, {"or", PartKind::Or},   {"not", PartKind::Not},     {"nand", PartKind::Nand},
        {"nor", PartKind::Nor}, {"xor", PartKind::Xor}, {"xnor", PartKind::Xnor},   {"npn", PartKind::Npn},
        {"pnp", PartKind::Pnp}, {"nmos", PartKind::Nmos}, {"pmos", PartKind::Pmos}, {"led", PartKind::Led}, {"probe", PartKind::Probe}, {"input", PartKind::Toggle}};
    const auto found = std::find_if(std::begin(kParts), std::end(kParts), [&](const auto& p) { return picture.name == p.first; });
    if (found == std::end(kParts)) return;
    PaletteItem item;
    item.kind = found->second;
    Mesh icons;
    drawTileGlyph(renderer, icons, item, centre, false);
    icons.flush(renderer);
    if (picture.pin.empty()) return;
    // The pin the row is about, lit on the drawing, where the tile drew it.
    const Part part = makePart(item.kind, {}, 0, 0, 1, item.box);
    const float w = static_cast<float>(baseWidth(part));
    const float h = static_cast<float>(baseHeight(part));
    const float cell = std::min(44.0f / w, 32.0f / h);
    const Frame frame = frameOf(part, {centre.x - w * cell / 2.0f, centre.y - h * cell / 2.0f}, cell);
    for (unsigned pin = 0; pin < pinCount(part); ++pin) {
        if (picture.pin != pinName(part, pin)) continue;
        const Point at = pinPosition(part, pin);
        const Vec2 dot = frame.place(Vec2{static_cast<float>(at.x), static_cast<float>(at.y)});
        Mesh mark;
        mark.addFan(circlePoints(dot, 11.0f, 24), dot, kWirePreview, 0.45f, 0.0f);
        mark.addDisc(dot, 4.0f, kWirePreview);
        mark.flush(renderer);
    }
}

std::vector<std::string> readingLines(const char* text) {
    std::vector<std::string> lines;
    const std::string all(i18n::tr(text));
    std::size_t start = 0;
    while (start <= all.size()) {
        std::size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        lines.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

int chosenAt(const ReadingView& view, std::size_t block) {
    return view.answers != nullptr && block < view.answers->size() ? (*view.answers)[block] : -1;
}

// A question laid out: its prompt, its options, and what's said about the last answer.
struct QuestionShape {
    QuestionParts parts;
    std::vector<std::string> prompt;
    std::vector<std::vector<std::string>> options;
    std::vector<float> optionTops;  // from the block's top
    std::vector<float> optionHeights;
    std::vector<std::string> feedback;
    int chosen = -1;
    bool right = false;
    float height = 0.0f;
};

QuestionShape questionShape(SDL_Renderer* renderer, const TheoryBlock& block, int chosen, float column) {
    QuestionShape q;
    q.parts = questionParts(i18n::tr(block.text));
    q.prompt = wrapped(renderer, Face::Medium, 16.0f, q.parts.prompt, column - kMarkIndent);
    float y = 46.0f + static_cast<float>(q.prompt.size()) * 24.0f + 10.0f;
    for (const std::string& option : q.parts.options) {
        q.options.push_back(wrapped(renderer, Face::Regular, 15.0f, option, column - kMarkIndent - 60.0f));
        const float h = std::max(40.0f, static_cast<float>(q.options.back().size()) * 22.0f + 18.0f);
        q.optionTops.push_back(y);
        q.optionHeights.push_back(h);
        y += h + 8.0f;
    }
    q.chosen = chosen;
    q.right = chosen >= 0 && chosen == q.parts.right;
    if (chosen >= 0) {
        const std::string said = q.right ? q.parts.explain
                                         : (q.parts.hint.empty() ? std::string(i18n::tr("Não é essa. Pense de novo e tente outra.")) : q.parts.hint);
        q.feedback = wrapped(renderer, Face::Regular, 14.5f, said, column - kMarkIndent);
        y += 6.0f + static_cast<float>(q.feedback.size()) * 22.0f + 8.0f;
    }
    q.height = y + 4.0f;
    return q;
}

// Where an option of a question is, the question's top at y.
SDL_FRect questionOption(const QuestionShape& q, std::size_t option, float left, float y, float column) {
    return {left + kMarkIndent, y + q.optionTops[option], column - kMarkIndent, q.optionHeights[option]};
}

// A block set apart from the text is marked the way the board marks things: a small lit LED in the block's
// colour beside its label, and a trace from it down the side of what the block holds, ending in a joint.

void drawLitLed(SDL_Renderer* renderer, float cx, float cy, Color tone) {
    Mesh mesh;
    const Vec2 at{cx, cy};
    mesh.addFan(circlePoints(at, 19.0f, 32), at, tone, 0.38f, 0.0f);
    mesh.addFan(circlePoints(at, 7.0f, 24), at, tone);
    addLoop(mesh, circlePoints(at, 7.0f, 24), 1.2f, mixed(tone, kLedShine, 0.55f));
    const Vec2 shine{cx - 2.3f, cy - 2.3f};
    mesh.addFan(circlePoints(shine, 2.8f, 12), shine, Color{255, 255, 255}, 0.9f, 0.0f);
    mesh.flush(renderer);
}

// The LED, the label beside it and the trace down to `bottom`, for a block whose top is at y.
void drawBlockMark(SDL_Renderer* renderer, float left, float y, float bottom, const char* label, Color tone) {
    const float x = left + kMarkTrace;
    fillRect(renderer, {x - 1.0f, y + 30.0f, 2.0f, std::max(0.0f, bottom - y - 30.0f)}, mixed(kBackground, tone, 0.4f));
    fillRound(renderer, {x - 3.0f, bottom - 3.0f, 6.0f, 6.0f}, 3.0f, mixed(kBackground, tone, 0.6f));
    drawLitLed(renderer, x, y + 21.0f, tone);
    write(renderer, Face::Medium, 13.5f, left + kMarkIndent, y + 26.0f, label, tone);
}

// A rule worth remembering: its label, then its lines.
float tipHeight(SDL_Renderer* renderer, const TheoryBlock& block, float column) {
    return 46.0f + static_cast<float>(wrapped(renderer, Face::Regular, 15.5f, block.text, column - kMarkIndent).size()) * 24.0f + 4.0f;
}

// The blocks set apart from the text: a worked example, a common mistake, a summary. Measured or drawn.
float boxedBlock(SDL_Renderer* renderer, const TheoryBlock& block, float left, float y, float column, bool draw) {
    const std::vector<std::string> items = readingLines(block.text);
    const char* label = "";
    Color tone = kWirePreview;
    switch (block.kind) {
        case TheoryBlockKind::Example: label = "Exemplo resolvido"; tone = kReadingExample; break;
        case TheoryBlockKind::Mistake: label = "Erro comum"; tone = kReadingWrong; break;
        case TheoryBlockKind::Summary: label = "Resumo"; tone = kReadingRight; break;
        default: break;
    }
    const float inner = column - kMarkIndent;
    float h = 46.0f;
    struct Line {
        std::string text;
        float y;
        Face face;
        float x;
    };
    std::vector<Line> lines;
    std::vector<std::pair<float, std::string>> marks;  // a step's number, or a bullet ("" ), at a height
    for (std::size_t i = 0; i < items.size(); ++i) {
        const bool title = i == 0 && block.kind != TheoryBlockKind::Summary;
        const Face face = title ? Face::Medium : Face::Regular;
        const float size = title ? 16.0f : 15.0f;
        const float indent = 0.0f;  // a step's number or a point's dot sits on the trace, not in the text
        if (!title) {
            if (block.kind == TheoryBlockKind::Example) marks.push_back({h, std::to_string(i)});
            if (block.kind == TheoryBlockKind::Summary) marks.push_back({h, ""});
        }
        for (const std::string& line : wrapped(renderer, face, size, items[i], inner - indent)) {
            lines.push_back({line, h, face, indent});
            h += 23.0f;
        }
        h += title ? 8.0f : 6.0f;
    }
    if (!draw) return h;
    drawBlockMark(renderer, left, y, y + h - 4.0f, label, tone);
    const float x = left + kMarkTrace;
    for (const auto& [at, mark] : marks) {
        if (mark.empty()) {
            fillRound(renderer, {x - 4.0f, y + at + 3.0f, 8.0f, 8.0f}, 4.0f, tone);
        } else {
            // A step's number in a ring on the trace, like a joint with a name.
            const SDL_FRect badge{x - 11.0f, y + at - 8.0f, 22.0f, 22.0f};
            fillRound(renderer, badge, 11, kBackground);
            frameRound(renderer, badge, 11, 1.4f, tone);
            const float w = textWidth(renderer, Face::Medium, 12.0f, mark);
            write(renderer, Face::Medium, 12.0f, badge.x + (badge.w - w) / 2.0f, badge.y + 15.5f, mark, tone);
        }
    }
    for (const Line& line : lines) write(renderer, line.face, line.face == Face::Medium ? 16.0f : 15.0f, left + kMarkIndent + line.x, y + line.y + 7.0f, line.text, line.face == Face::Medium ? kText : kReadingBody);
    return h;
}

// Where the blocks of the screen on show are, from the top of the content; drawing and clicking share it.
struct ReadingLayout {
    std::vector<float> tops;  // one per block of the screen
    std::size_t first = 0, last = 0;
    int page = 0, pages = 1;
    float height = 0.0f;
    float buttonTop = 0.0f;
};

ReadingLayout readingLayout(SDL_Renderer* renderer, int width, const ReadingView& view) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    const float column = readingColumnWidth(width);
    const std::vector<std::pair<std::size_t, std::size_t>> pages = readingPages(topic);
    ReadingLayout layout;
    layout.pages = static_cast<int>(pages.size());
    layout.page = std::clamp(view.page, 0, layout.pages - 1);
    layout.first = pages[static_cast<std::size_t>(layout.page)].first;
    layout.last = pages[static_cast<std::size_t>(layout.page)].second;
    float y = layout.pages > 1 ? 128.0f : 92.0f;  // under the title, the line, and the screens' dots
    for (std::size_t b = layout.first; b < layout.last; ++b) {
        const TheoryBlock& block = topic.blocks[b];
        switch (block.kind) {
            case TheoryBlockKind::Page:
                y += 4.0f;
                layout.tops.push_back(y);
                y += static_cast<float>(wrapped(renderer, Face::Medium, 25.0f, block.text, column).size()) * 32.0f + 18.0f;
                break;
            case TheoryBlockKind::Heading:
                y += 24.0f;
                layout.tops.push_back(y);
                y += 34.0f;
                break;
            case TheoryBlockKind::Text:
                layout.tops.push_back(y);
                y += static_cast<float>(wrapped(renderer, Face::Regular, kReadingText, block.text, column).size()) * kReadingLine + 14.0f;
                break;
            case TheoryBlockKind::Tip:
                y += 6.0f;
                layout.tops.push_back(y);
                y += tipHeight(renderer, block, column) + 18.0f;
                break;
            case TheoryBlockKind::Table:
                y += 4.0f;
                layout.tops.push_back(y);
                y += tableShape(block.text).height + 22.0f;
                break;
            case TheoryBlockKind::Code: {
                y += 6.0f;
                layout.tops.push_back(y);
                const std::string code(i18n::tr(block.text));
                y += static_cast<float>(std::count(code.begin(), code.end(), '\n') + 1) * 21.0f + 30.0f + 16.0f;
                break;
            }
            case TheoryBlockKind::Word:
                y += 8.0f;
                layout.tops.push_back(y);
                y += kWordStripHeight + 6.0f +
                     static_cast<float>(wrapped(renderer, Face::Regular, 13.5f, block.text, column).size()) * 20.0f + 18.0f;
                break;
            case TheoryBlockKind::Figure:
                y += 8.0f;
                layout.tops.push_back(y);
                y += static_cast<float>(block.height) + 10.0f +
                     static_cast<float>(wrapped(renderer, Face::Regular, 13.5f, block.text, column).size()) * 20.0f + 18.0f;
                break;
            case TheoryBlockKind::Question:
                y += 8.0f;
                layout.tops.push_back(y);
                y += questionShape(renderer, block, chosenAt(view, b), column).height + 14.0f;
                break;
            case TheoryBlockKind::Example:
            case TheoryBlockKind::Mistake:
            case TheoryBlockKind::Summary:
                y += 8.0f;
                layout.tops.push_back(y);
                y += boxedBlock(renderer, block, 0.0f, 0.0f, column, false) + 16.0f;
                break;
        }
    }
    layout.buttonTop = y + 20.0f;
    layout.height = layout.buttonTop + 130.0f;
    return layout;
}

// Every question of the screen answered right: the next screen may open.
bool pageAnswered(SDL_Renderer* renderer, const ReadingView& view, const ReadingLayout& layout) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    for (std::size_t b = layout.first; b < layout.last; ++b) {
        if (topic.blocks[b].kind != TheoryBlockKind::Question) continue;
        if (chosenAt(view, b) != questionParts(i18n::tr(topic.blocks[b].text)).right) return false;
    }
    (void)renderer;
    return true;
}

// At the bottom: the screen before on the left, and on the right the next screen or, on the last, the exercises.
SDL_FRect readingPreviousButton(int width, const ReadingLayout& layout, float scroll) {
    const float left = (static_cast<float>(width) - readingColumnWidth(width)) / 2.0f;
    return {left, kReadingBarHeight + layout.buttonTop + 44.0f - scroll, 170.0f, 42.0f};
}
SDL_FRect readingBottomButton(int width, const ReadingLayout& layout, float scroll) {
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    return {left + column - 280.0f, kReadingBarHeight + layout.buttonTop + 44.0f - scroll, 280.0f, 42.0f};
}

// The figures: a live board each, drawn to its own texture, built once. Their switches can be clicked.
struct FigureCache {
    std::unique_ptr<Session> session;
    SDL_Texture* texture = nullptr;
    int w = 0;
    int h = 0;
    bool switches = false;  // it has switches to click
};

FigureCache& figureEntry(TheoryFigure (*make)()) {
    static std::map<TheoryFigure (*)(), FigureCache> cache;
    FigureCache& entry = cache[make];
    if (!entry.session) {
        TheoryFigure figure = make();
        entry.session = std::make_unique<Session>();
        entry.session->adoptProjectChips(std::move(figure.chips));
        entry.session->adopt(std::move(figure.board), figure.closed);
        if (!figure.values.empty()) entry.session->setToggleValues(figure.values);
        for (const Part& part : entry.session->board().parts()) entry.switches = entry.switches || part.kind == PartKind::Toggle || part.kind == PartKind::Switch;
    }
    return entry;
}

// The board fitted in a w by h picture, with a margin, never blown up past a comfortable size.
Camera figureCamera(const Session& session, int w, int h) {
    const Rect extent = session.board().extent();
    // Names stand to the right of the last parts and above the first ones: room for them too.
    const float boardW = static_cast<float>(extent.right - extent.left) + 7.0f;
    const float boardH = static_cast<float>(extent.bottom - extent.top) + 4.0f;
    Camera camera;
    camera.cellPixels = std::clamp(std::min(static_cast<float>(w) / boardW, static_cast<float>(h) / boardH), 3.0f, 16.0f);
    camera.originX = static_cast<float>(extent.left) - 1.5f - (static_cast<float>(w) / camera.cellPixels - boardW) / 2.0f;
    camera.originY = static_cast<float>(extent.top) - 2.5f - (static_cast<float>(h) / camera.cellPixels - boardH) / 2.0f;
    return camera;
}

SDL_Texture* figureTexture(SDL_Renderer* renderer, TheoryFigure (*make)(), int w, int h, bool redraw) {
    FigureCache& entry = figureEntry(make);
    const bool fresh = entry.texture == nullptr || entry.w != w || entry.h != h;
    if (fresh) {
        if (entry.texture != nullptr) SDL_DestroyTexture(entry.texture);
        entry.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        entry.w = w;
        entry.h = h;
    }
    if (entry.texture == nullptr) return nullptr;
    if (!fresh && !redraw) return entry.texture;
    const Camera camera = figureCamera(*entry.session, w, h);
    SDL_Texture* screen = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, entry.texture);
    drawBoard(renderer, *entry.session, camera, w, h);
    SDL_SetRenderTarget(renderer, screen);
    return entry.texture;
}

void drawFigure(SDL_Renderer* renderer, TheoryFigure (*make)(), const SDL_FRect& rect) {
    SDL_Texture* texture = figureTexture(renderer, make, static_cast<int>(rect.w), static_cast<int>(rect.h), true);
    if (texture != nullptr) SDL_RenderTexture(renderer, texture, nullptr, &rect);
}

}  // namespace

namespace {

// --- A part from the inside ------------------------------------------------------------------------------

constexpr float kPeekBar = 64.0f;
constexpr float kPeekNote = 80.0f;  // under the bar: how this version is built, and what can be done here

SDL_FRect peekBackRect() { return {24.0f, 15.0f, 120.0f, 34.0f}; }

// The version buttons, right to left from the bar's right edge.
std::vector<SDL_FRect> peekVersionRects(SDL_Renderer* renderer, int width, const PeekView& view) {
    std::vector<SDL_FRect> rects(view.versions.size());
    float right = static_cast<float>(width) - 24.0f;
    for (std::size_t i = view.versions.size(); i-- > 0;) {
        const float w = textWidth(renderer, Face::Medium, kUiText, view.versions[i]) + 32.0f;
        rects[i] = {right - w, 15.0f, w, 34.0f};
        right -= w + 8.0f;
    }
    return rects;
}

}  // namespace

SDL_FRect peekBoardArea(int width, int height) {
    return {16.0f, kPeekBar + kPeekNote, static_cast<float>(width) - 32.0f, static_cast<float>(height) - kPeekBar - kPeekNote - 16.0f};
}

Camera peekFit(const Session& session, int width, int height) {
    const SDL_FRect area = peekBoardArea(width, height);
    Camera camera = figureCamera(session, static_cast<int>(area.w), static_cast<int>(area.h));
    // A small part would fill the screen at the figures' largest size: keep it at a size a board would show.
    if (camera.cellPixels > 14.0f) camera.zoomAt(area.w / 2.0f, area.h / 2.0f, 14.0f / camera.cellPixels);
    return camera;
}

void drawPeek(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const PeekView& view) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const SDL_FRect area = peekBoardArea(width, height);
    if (view.session != nullptr && area.w > 1.0f && area.h > 1.0f) {
        static SDL_Texture* texture = nullptr;
        static int tw = 0, th = 0;
        const int w = static_cast<int>(area.w);
        const int h = static_cast<int>(area.h);
        if (texture == nullptr || tw != w || th != h) {
            if (texture != nullptr) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
            tw = w;
            th = h;
        }
        if (texture != nullptr) {
            SDL_Texture* screen = SDL_GetRenderTarget(renderer);
            SDL_SetRenderTarget(renderer, texture);
            drawBoard(renderer, *view.session, view.camera, w, h);
            SDL_SetRenderTarget(renderer, screen);
            const SDL_FRect at{area.x, area.y, static_cast<float>(w), static_cast<float>(h)};
            SDL_RenderTexture(renderer, texture, nullptr, &at);
        }
    }
    frameRound(renderer, area, 10, 1.2f, kBodyEdge);
    // Not a board to change: say so in its corner.
    const char* tag = "Só para ver";
    const float tagW = textWidth(renderer, Face::Medium, 12.0f, tag) + 20.0f;
    const SDL_FRect pill{area.x + area.w - tagW - 10.0f, area.y + 10.0f, tagW, 24.0f};
    fillRound(renderer, pill, 12, mixed(kPanel, kWirePreview, 0.25f));
    write(renderer, Face::Medium, 12.0f, pill.x + 10.0f, pill.y + 16.5f, tag, kWirePreview);

    // Under the bar: how it's built, and what can be done.
    const std::vector<std::string> how = wrapped(renderer, Face::Regular, 14.0f, view.how, static_cast<float>(width) - 48.0f);
    for (std::size_t l = 0; l < how.size() && l < 2; ++l) {
        const std::string line = l == 1 && how.size() > 2 ? how[l] + "…" : how[l];
        write(renderer, Face::Regular, 14.0f, 24.0f, kPeekBar + 22.0f + 21.0f * static_cast<float>(l), fitted(renderer, Face::Regular, 14.0f, line, static_cast<float>(width) - 48.0f), kReadingBody);
    }
    write(renderer, Face::Regular, 13.0f, 24.0f, kPeekBar + 66.0f,
          "Clique nas chaves para ver funcionar. Clique duplo numa peça para ver por dentro dela. Esc volta.", kTextDim);

    // The bar.
    fillRect(renderer, {0.0f, 0.0f, static_cast<float>(width), kPeekBar}, kPanel);
    fillRect(renderer, {0.0f, kPeekBar - 1.0f, static_cast<float>(width), 1.0f}, kRule);
    const SDL_FRect back = peekBackRect();
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    write(renderer, Face::Medium, kUiText, back.x + 18.0f, back.y + 22.0f, "Voltar", kText);
    const std::vector<SDL_FRect> versions = peekVersionRects(renderer, width, view);
    for (std::size_t i = 0; i < versions.size(); ++i) {
        const bool on = static_cast<int>(i) == view.version;
        fillRound(renderer, versions[i], 8, on ? kPanelActive : inside(versions[i], mouseX, mouseY) ? kPanelHover : kPanelButton);
        write(renderer, Face::Medium, kUiText, versions[i].x + 16.0f, versions[i].y + 22.0f, view.versions[i], on ? kText : kTextDim);
    }
    // Where it is: the parts opened, one inside the other.
    std::string where = i18n::tr("Por dentro");
    for (const std::string& name : view.trail) where += "  ›  " + name;
    const float left = back.x + back.w + 24.0f;
    const float right = versions.empty() ? static_cast<float>(width) - 24.0f : versions.front().x - 24.0f;
    write(renderer, Face::Medium, 15.0f, left, 37.0f, fitted(renderer, Face::Medium, 15.0f, where, right - left), kText);
}

PeekHit peekAt(SDL_Renderer* renderer, float x, float y, int width, int height, const PeekView& view) {
    if (inside(peekBackRect(), x, y)) return {PeekChoice::Back};
    const std::vector<SDL_FRect> versions = peekVersionRects(renderer, width, view);
    for (std::size_t i = 0; i < versions.size(); ++i) {
        if (inside(versions[i], x, y)) return {PeekChoice::Version, static_cast<int>(i)};
    }
    if (inside(peekBoardArea(width, height), x, y)) return {PeekChoice::Board};
    return {};
}

ReadingHit readingAt(SDL_Renderer* renderer, float x, float y, int width, int, const ReadingView& view) {
    if (inside(readingBackRect(), x, y)) return {ReadingButton::Back};
    if (inside(readingExercisesRect(width), x, y)) return {ReadingButton::Exercises};
    if (y <= kReadingBarHeight) return {};
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    const ReadingLayout layout = readingLayout(renderer, width, view);
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    const float top = kReadingBarHeight - view.scroll;
    for (std::size_t b = layout.first; b < layout.last; ++b) {
        const TheoryBlock& block = topic.blocks[b];
        const float blockY = top + layout.tops[b - layout.first] + 8.0f;
        if (block.kind == TheoryBlockKind::Question) {
            const QuestionShape q = questionShape(renderer, block, chosenAt(view, b), column);
            if (q.right) continue;  // answered: it stays as it is
            for (std::size_t o = 0; o < q.optionTops.size(); ++o) {
                if (inside(questionOption(q, o, left, blockY, column), x, y)) {
                    return {ReadingButton::Option, static_cast<int>(b), static_cast<int>(o)};
                }
            }
        }
        if (block.kind == TheoryBlockKind::Figure && inside({left, blockY, column, static_cast<float>(block.height)}, x, y)) {
            return {ReadingButton::Figure, static_cast<int>(b)};
        }
    }
    if (layout.page > 0 && inside(readingPreviousButton(width, layout, view.scroll), x, y)) return {ReadingButton::Previous};
    if (inside(readingBottomButton(width, layout, view.scroll), x, y)) {
        if (layout.page + 1 >= layout.pages) return {ReadingButton::Exercises};
        if (pageAnswered(renderer, view, layout)) return {ReadingButton::Next};
    }
    return {};
}

int readingPageCount(int topic) { return static_cast<int>(readingPages(topics()[static_cast<std::size_t>(topic)]).size()); }

bool readingPageDone(SDL_Renderer* renderer, int width, const ReadingView& view) {
    return pageAnswered(renderer, view, readingLayout(renderer, width, view));
}

bool pressReadingFigure(SDL_Renderer* renderer, float x, float y, int width, const ReadingView& view, int block) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    if (block < 0 || static_cast<std::size_t>(block) >= topic.blocks.size()) return false;
    const TheoryBlock& figure = topic.blocks[static_cast<std::size_t>(block)];
    if (figure.kind != TheoryBlockKind::Figure || figure.figure == nullptr) return false;
    const ReadingLayout layout = readingLayout(renderer, width, view);
    if (static_cast<std::size_t>(block) < layout.first || static_cast<std::size_t>(block) >= layout.last) return false;
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    const float top = kReadingBarHeight - view.scroll + layout.tops[static_cast<std::size_t>(block) - layout.first] + 8.0f;
    FigureCache& entry = figureEntry(figure.figure);
    const Camera camera = figureCamera(*entry.session, static_cast<int>(column), figure.height);
    const Vec2 at = camera.toBoard(x - left, y - top);
    // Only the switch flips: a tap near one of its pins must not start a wire on the figure.
    if (!entry.session->flipToggleAt(at.x, at.y)) return false;
    return true;
}

float readingMaxScroll(SDL_Renderer* renderer, int width, int height, const ReadingView& view) {
    return std::max(0.0f, readingLayout(renderer, width, view).height - (static_cast<float>(height) - kReadingBarHeight));
}

void drawReading(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const ReadingView& view) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    const ReadingLayout layout = readingLayout(renderer, width, view);
    const float top = kReadingBarHeight - view.scroll;
    const SDL_Rect clip{0, static_cast<int>(kReadingBarHeight), width, height - static_cast<int>(kReadingBarHeight)};
    SDL_SetRenderClipRect(renderer, &clip);

    const std::string actTitle(i18n::tr(lessonActs()[static_cast<std::size_t>(topic.act)].title));
    write(renderer, Face::Medium, 13.0f, left, top + 30.0f,
          i18n::format("{}  ·  Aula {}", {actTitle.substr(0, actTitle.find(':')), std::to_string(view.topic + 1)}), kWirePreview);
    write(renderer, Face::Medium, 30.0f, left, top + 66.0f, topic.title, kText);
    write(renderer, Face::Regular, 15.0f, left, top + 90.0f, topic.line, kTextDim);
    if (layout.pages > 1) {
        // Where we are: a dot for each screen, the ones seen filled.
        const std::string where = i18n::format("Tela {} de {}", {std::to_string(layout.page + 1), std::to_string(layout.pages)});
        write(renderer, Face::Medium, 12.5f, left, top + 118.0f, where, kTextDim);
        const float dotsX = left + textWidth(renderer, Face::Medium, 12.5f, where) + 16.0f;
        for (int p = 0; p < layout.pages; ++p) {
            const SDL_FRect dot{dotsX + static_cast<float>(p) * 18.0f, top + 108.0f, 10.0f, 10.0f};
            fillRound(renderer, dot, 5.0f, p == layout.page ? kWirePreview : p < layout.page ? mixed(kPanel, kWirePreview, 0.5f) : kPanelButton);
        }
    }

    for (std::size_t b = layout.first; b < layout.last; ++b) {
        const TheoryBlock& block = topic.blocks[b];
        const float y = top + layout.tops[b - layout.first] + 8.0f;
        switch (block.kind) {
            case TheoryBlockKind::Page: {
                const std::vector<std::string> lines = wrapped(renderer, Face::Medium, 25.0f, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) write(renderer, Face::Medium, 25.0f, left, y + 26.0f + 32.0f * static_cast<float>(l), lines[l], kText);
                break;
            }
            case TheoryBlockKind::Heading:
                write(renderer, Face::Medium, 21.0f, left, y + 22.0f, block.text, kText);
                break;
            case TheoryBlockKind::Text: {
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, kReadingText, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, kReadingText, left, y + 18.0f + kReadingLine * static_cast<float>(l), lines[l], kReadingBody);
                }
                break;
            }
            case TheoryBlockKind::Tip: {
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 15.5f, block.text, column - kMarkIndent);
                drawBlockMark(renderer, left, y, y + tipHeight(renderer, block, column) - 4.0f, "Para lembrar", kWirePreview);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, 15.5f, left + kMarkIndent, y + 53.0f + 24.0f * static_cast<float>(l), lines[l], kText);
                }
                break;
            }
            case TheoryBlockKind::Table: {
                const TableShape table = tableShape(block.text);
                const std::vector<std::vector<std::string>>& rows = table.rows;
                const float slot = table.pictured ? kPictureSlot : 0.0f;  // the pictures' room, before the first column's text
                std::vector<float> widths;
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    const std::vector<std::string>& row = rows[r];
                    for (std::size_t c = 0; c < row.size(); ++c) {
                        if (widths.size() <= c) widths.push_back(0.0f);
                        widths[c] = std::max(widths[c], textWidth(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, row[c]) + 28.0f + (c == 0 ? slot : 0.0f));
                    }
                }
                float total = 0.0f;
                for (const float w : widths) total += w;
                const float stretch = total < column ? 1.0f : column / total;
                float tableWidth = 0.0f;
                for (float& w : widths) tableWidth += (w *= stretch);
                float rowY = y;
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    const float rowH = table.heights[r];
                    fillRect(renderer, {left, rowY, tableWidth, rowH}, r == 0 ? kPanelActive : r % 2 == 0 ? kPanelRaised : kPanel);
                    if (!table.pictures[r].name.empty()) drawCellPicture(renderer, table.pictures[r], {left + 8.0f + slot / 2.0f, rowY + rowH / 2.0f});
                    float x = left;
                    for (std::size_t c = 0; c < rows[r].size() && c < widths.size(); ++c) {
                        const float pad = c == 0 ? slot : 0.0f;
                        write(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, x + 14.0f + pad, rowY + rowH / 2.0f + 5.0f,
                              fitted(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, rows[r][c], widths[c] - 20.0f - pad), r == 0 ? kText : kReadingBody);
                        x += widths[c];
                    }
                    rowY += rowH;
                }
                break;
            }
            case TheoryBlockKind::Code: {
                const std::string code(i18n::tr(block.text));
                const auto count = static_cast<float>(std::count(code.begin(), code.end(), '\n') + 1);
                const SDL_FRect box{left, y, column, count * 21.0f + 30.0f};
                fillRound(renderer, box, 10, kPanel);
                frameRound(renderer, box, 10, 1.0f, kBodyEdge);
                std::size_t start = 0;
                for (int line = 0; start <= code.size(); ++line) {
                    std::size_t end = code.find('\n', start);
                    if (end == std::string::npos) end = code.size();
                    drawCodeLine(renderer, code.substr(start, end - start), left + 18.0f, y + 26.0f + 21.0f * static_cast<float>(line));
                    start = end + 1;
                }
                break;
            }
            case TheoryBlockKind::Word: {
                ComputerState state;
                state.instruction = block.word;
                const SDL_FRect strip{left + 14.0f, y + 4.0f, column - 28.0f, kWordStripHeight};
                fillRound(renderer, {left, y - 4.0f, column, kWordStripHeight + 14.0f}, 10, kPanel);
                drawWordStrip(renderer, traceInstruction(state), strip, true);
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 13.5f, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, 13.5f, left, y + kWordStripHeight + 30.0f + 20.0f * static_cast<float>(l), lines[l], kTextDim);
                }
                break;
            }
            case TheoryBlockKind::Figure: {
                const SDL_FRect frame{left, y, column, static_cast<float>(block.height)};
                if (frame.y + frame.h > kReadingBarHeight && frame.y < static_cast<float>(height) && block.figure != nullptr) {
                    drawFigure(renderer, block.figure, frame);
                    SDL_SetRenderClipRect(renderer, &clip);
                    if (figureEntry(block.figure).switches) {  // it can be played with: say so
                        const char* tag = "Clique nas chaves";
                        const float w = textWidth(renderer, Face::Medium, 12.0f, tag) + 20.0f;
                        const SDL_FRect pill{frame.x + frame.w - w - 10.0f, frame.y + 10.0f, w, 24.0f};
                        fillRound(renderer, pill, 12, mixed(kPanel, kWirePreview, 0.25f));
                        write(renderer, Face::Medium, 12.0f, pill.x + 10.0f, pill.y + 16.5f, tag, kWirePreview);
                    }
                }
                frameRound(renderer, frame, 10, 1.2f, inside(frame, mouseX, mouseY) && figureEntry(block.figure).switches ? kWirePreview : kBodyEdge);
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 13.5f, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, 13.5f, left, frame.y + frame.h + 22.0f + 20.0f * static_cast<float>(l), lines[l], kTextDim);
                }
                break;
            }
            case TheoryBlockKind::Question: {
                const QuestionShape q = questionShape(renderer, block, chosenAt(view, b), column);
                const Color tone = q.right ? kReadingRight : kWarning;
                drawBlockMark(renderer, left, y, y + q.height - 4.0f, q.right ? "Pergunta: certo!" : "Pergunta", tone);
                for (std::size_t l = 0; l < q.prompt.size(); ++l) write(renderer, Face::Medium, 16.0f, left + kMarkIndent, y + 56.0f + 24.0f * static_cast<float>(l), q.prompt[l], kText);
                for (std::size_t o = 0; o < q.options.size(); ++o) {
                    const SDL_FRect r = questionOption(q, o, left, y, column);
                    const bool picked = static_cast<int>(o) == q.chosen;
                    const bool good = picked && q.right;
                    const bool bad = picked && !q.right;
                    const bool hover = !q.right && inside(r, mouseX, mouseY);
                    Color fill = hover ? kPanelHover : kPanelButton;
                    if (good) fill = mixed(kPanel, kReadingRight, 0.3f);
                    if (bad) fill = mixed(kPanel, kReadingWrong, 0.25f);
                    fillRound(renderer, r, 8, fill);
                    if (good || bad) frameRound(renderer, r, 8, 1.4f, good ? kReadingRight : kReadingWrong);
                    const std::string letter(1, static_cast<char>('A' + o));
                    write(renderer, Face::Medium, 14.0f, r.x + 16.0f, r.y + 25.0f, letter, good ? kReadingRight : bad ? kReadingWrong : kTextDim);
                    for (std::size_t l = 0; l < q.options[o].size(); ++l) {
                        write(renderer, Face::Regular, 15.0f, r.x + 44.0f, r.y + 25.0f + 22.0f * static_cast<float>(l), q.options[o][l], q.right && !picked ? kTextDim : kText);
                    }
                }
                if (!q.feedback.empty()) {
                    const float fy = y + q.optionTops.back() + q.optionHeights.back() + 8.0f;
                    for (std::size_t l = 0; l < q.feedback.size(); ++l) {
                        write(renderer, Face::Regular, 14.5f, left + kMarkIndent, fy + 20.0f + 22.0f * static_cast<float>(l), q.feedback[l], q.right ? kReadingRight : kReadingWrong);
                    }
                }
                break;
            }
            case TheoryBlockKind::Example:
            case TheoryBlockKind::Mistake:
            case TheoryBlockKind::Summary:
                boxedBlock(renderer, block, left, y, column, true);
                break;
        }
    }

    // The way on: the next screen, or on the last one the exercises of this reading.
    const float endY = top + layout.buttonTop;
    fillRect(renderer, {left, endY, column, 1.0f}, kRule);
    const bool lastPage = layout.page + 1 >= layout.pages;
    const bool answered = pageAnswered(renderer, view, layout);
    if (lastPage) {
        std::string names;
        for (const char* id : topic.exercises) {
            const int lesson = lessonIndex(id);
            if (lesson < 0) continue;
            names += (names.empty() ? "" : ", ") + std::string(i18n::tr(lessons()[static_cast<std::size_t>(lesson)].title));
        }
        write(renderer, Face::Regular, 14.0f, left, endY + 28.0f,
              fitted(renderer, Face::Regular, 14.0f, i18n::format("Agora, os exercícios desta aula: {}.", {names}), column), kTextDim);
    } else if (!answered) {
        write(renderer, Face::Regular, 14.0f, left, endY + 28.0f, "Responda a pergunta desta tela para seguir.", kWarning);
    }
    if (layout.page > 0) {
        const SDL_FRect previous = readingPreviousButton(width, layout, view.scroll);
        fillRound(renderer, previous, 10, inside(previous, mouseX, mouseY) ? kPanelHover : kPanelButton);
        write(renderer, Face::Medium, 15.0f, previous.x + 20.0f, previous.y + 27.0f, "Tela anterior", kText);
    }
    const SDL_FRect bottom = readingBottomButton(width, layout, view.scroll);
    const bool open = lastPage || answered;
    const bool lit = open && inside(bottom, mouseX, mouseY);
    fillRound(renderer, bottom, 10, !open ? kPanelButton : lit ? kWirePreview : kPanelActive);
    write(renderer, Face::Medium, 15.0f, bottom.x + 20.0f, bottom.y + 27.0f, lastPage ? "Começar os exercícios" : "Próxima tela", !open ? kTextOff : lit ? kBackground : kText);
    SDL_SetRenderClipRect(renderer, nullptr);

    // The bar on top, over the scrolled text.
    fillRect(renderer, {0.0f, 0.0f, static_cast<float>(width), kReadingBarHeight}, kPanel);
    fillRect(renderer, {0.0f, kReadingBarHeight - 1.0f, static_cast<float>(width), 1.0f}, kRule);
    const SDL_FRect back = readingBackRect();
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    write(renderer, Face::Medium, kUiText, back.x + 18.0f, back.y + 22.0f, "Voltar às aulas", kText);
    const std::string where = i18n::format("{}: {}", {std::to_string(view.topic + 1), i18n::tr(topic.title)});
    const std::string heading = i18n::format("Aula {}", {where});
    const float headingWidth = textWidth(renderer, Face::Medium, 15.0f, heading);
    write(renderer, Face::Medium, 15.0f, (static_cast<float>(width) - headingWidth) / 2.0f, 37.0f, heading, kTextDim);
    const SDL_FRect next = readingExercisesRect(width);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelActive);
    write(renderer, Face::Medium, kUiText, next.x + 18.0f, next.y + 22.0f, view.read ? "Ir para os exercícios" : "Pular para os exercícios", kText);
    // How far down the screen is.
    const float maxScroll = std::max(1.0f, layout.height - (static_cast<float>(height) - kReadingBarHeight));
    if (layout.height > static_cast<float>(height) - kReadingBarHeight) {
        fillRect(renderer, {0.0f, kReadingBarHeight - 3.0f, static_cast<float>(width) * std::clamp(view.scroll / maxScroll, 0.0f, 1.0f), 3.0f}, kWirePreview);
    }
}

namespace {

SDL_FRect lessonButtonRect(const SDL_FRect& area, int row) {
    return {area.x + area.w - 196.0f, area.y + 16.0f + static_cast<float>(row) * 42.0f, 180.0f, 34.0f};
}

// A lesson's buttons: the main ones a row each, the rest two to a row.
constexpr float kStripButtons = 212.0f;

struct StripButton {
    LessonButton button;
    SDL_FRect rect;
};

std::vector<StripButton> lessonStripButtons(const SDL_FRect& area, const LessonStrip& strip) {
    const float x = area.x + area.w - kStripButtons - 16.0f;
    const float half = (kStripButtons - 8.0f) / 2.0f;
    float y = area.y + 16.0f;
    std::vector<StripButton> buttons;
    if (strip.program) {
        buttons.push_back({LessonButton::Program, {x, y, kStripButtons, 34.0f}});
        y += 42.0f;
    }
    buttons.push_back({strip.passed && strip.hasNext ? LessonButton::Next : LessonButton::Check, {x, y, kStripButtons, 34.0f}});
    y += 42.0f;
    buttons.push_back({LessonButton::Hint, {x, y, half, 34.0f}});
    buttons.push_back({LessonButton::Restart, {x + half + 8.0f, y, half, 34.0f}});
    y += 42.0f;
    if (strip.hasReading) buttons.push_back({LessonButton::Reading, {x, y, half, 34.0f}});
    buttons.push_back({LessonButton::List, {x + half + 8.0f, y, half, 34.0f}});
    return buttons;
}

}  // namespace

LessonButton lessonStripAt(const SDL_FRect& area, const LessonStrip& strip, float x, float y) {
    for (const StripButton& b : lessonStripButtons(area, strip)) {
        if (inside(b.rect, x, y)) return b.button;
    }
    return LessonButton::None;
}

float programAboutHeight(const ExampleStrip* example, int view) {
    if (example == nullptr || view == kViewBoard) return 0.0f;
    return galleryItems()[static_cast<std::size_t>(example->item)].program >= 0 ? 70.0f : 0.0f;
}

int exampleStripAt(const SDL_FRect& area, const ExampleStrip& strip, float x, float y) {
    const GalleryItem& item = galleryItems()[static_cast<std::size_t>(strip.item)];
    for (int row = 0; row < static_cast<int>(item.versions.size()) && row < 3; ++row) {
        if (item.versions.size() > 1 && inside(lessonButtonRect(area, row), x, y)) return row;
    }
    if (inside(lessonButtonRect(area, 4), x, y)) return kExampleStripEdit;
    return inside(lessonButtonRect(area, 3), x, y) ? kExampleStripAll : -1;
}

void drawExampleStrip(SDL_Renderer* renderer, const SDL_FRect& area, const ExampleStrip& strip, float mouseX, float mouseY) {
    const GalleryItem& item = galleryItems()[static_cast<std::size_t>(strip.item)];
    fillRect(renderer, area, kPanel);
    fillRect(renderer, {area.x, area.y + area.h - 1.0f, area.w, 1.0f}, kRule);

    // The versions, the one on the board lit; and the way back to the gallery, at the bottom.
    if (item.versions.size() > 1) {
        for (int row = 0; row < static_cast<int>(item.versions.size()) && row < 3; ++row) {
            const SDL_FRect r = lessonButtonRect(area, row);
            const bool chosen = row == strip.version;
            fillRound(renderer, r, 8, chosen ? kPanelActive : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
            if (chosen) frameRound(renderer, r, 8, 1.4f, kWirePreview);
            const char* name = item.versions[static_cast<std::size_t>(row)].name;
            const float w = textWidth(renderer, Face::Medium, kUiText, name);
            write(renderer, Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 22.0f, name, chosen ? kText : kTextDim);
        }
    }
    const SDL_FRect all = lessonButtonRect(area, 3);
    fillRound(renderer, all, 8, inside(all, mouseX, mouseY) ? kPanelHover : kPanelButton);
    const float allWidth = textWidth(renderer, Face::Medium, kUiText, "Todos os exemplos");
    write(renderer, Face::Medium, kUiText, all.x + (all.w - allWidth) / 2.0f, all.y + 22.0f, "Todos os exemplos", kText);
    // The example is only to look at: changing it takes a copy of one's own.
    const SDL_FRect edit = lessonButtonRect(area, 4);
    fillRound(renderer, edit, 8, inside(edit, mouseX, mouseY) ? mixed(kPanelActive, kWirePreview, 0.3f) : kPanelActive);
    frameRound(renderer, edit, 8, 1.2f, kWirePreview);
    const float editWidth = textWidth(renderer, Face::Medium, kUiText, "Editar uma cópia");
    write(renderer, Face::Medium, kUiText, edit.x + (edit.w - editWidth) / 2.0f, edit.y + 22.0f, "Editar uma cópia", kText);

    const float x = area.x + 18.0f;
    const float width = area.w - 236.0f;
    write(renderer, Face::Regular, 12.5f, x, area.y + 22.0f, i18n::format("Exemplo  ·  {}", {i18n::tr(galleryGroups()[static_cast<std::size_t>(item.group)])}),
          kTextDim);
    write(renderer, Face::Medium, 19, x, area.y + 46.0f, item.title, kText);
    float y = area.y + 70.0f;
    const std::vector<std::string> about = wrapped(renderer, Face::Regular, 13.5f, item.about, width);
    for (std::size_t i = 0; i < about.size() && i < 3; ++i) {
        write(renderer, Face::Regular, 13.5f, x, y, i == 2 && about.size() > 3 ? fitted(renderer, Face::Regular, 13.5f, about[i] + " …", width) : about[i], kText);
        y += 18.0f;
    }
    // How the version on the board does it.
    if (!item.versions.empty()) {
        const GalleryVersion& version = item.versions[static_cast<std::size_t>(std::clamp(strip.version, 0, static_cast<int>(item.versions.size()) - 1))];
        y += 6.0f;
        const std::string how = i18n::format("{}: {}", {i18n::tr(version.name), i18n::tr(version.how)});
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 13, how, width);
        for (std::size_t i = 0; i < lines.size() && i < 4; ++i) {
            write(renderer, Face::Regular, 13, x, y, i == 3 && lines.size() > 4 ? fitted(renderer, Face::Regular, 13, lines[i] + " …", width) : lines[i],
                  kTextDim);
            y += 17.0f;
        }
    }
    // What to try, at the bottom.
    const std::vector<std::string> tryIt = wrapped(renderer, Face::Medium, 13, i18n::format("Experimente: {}", {i18n::tr(item.tryIt)}), width);
    float tryY = area.y + area.h - 14.0f - 17.0f * static_cast<float>(std::min<std::size_t>(tryIt.size(), 2) - 1);
    for (std::size_t i = 0; i < tryIt.size() && i < 2; ++i) {
        write(renderer, Face::Medium, 13, x, tryY, tryIt[i], kWirePreview);
        tryY += 17.0f;
    }
}

void drawLessonStrip(SDL_Renderer* renderer, const SDL_FRect& area, const LessonStrip& strip, float mouseX, float mouseY) {
    const Lesson& lesson = lessons()[static_cast<std::size_t>(strip.index)];
    fillRect(renderer, area, kPanel);
    fillRect(renderer, {area.x, area.y + area.h - 1.0f, area.w, 1.0f}, kRule);

    // The buttons on the right; the one to press next stands out.
    for (const StripButton& b : lessonStripButtons(area, strip)) {
        const SDL_FRect r = b.rect;
        const bool primary = b.button == LessonButton::Next || b.button == LessonButton::Check;
        const bool hovered = inside(r, mouseX, mouseY);
        if (b.button == LessonButton::Program) {
            fillRound(renderer, r, 8, hovered ? mixed(kBodyProgram, kToneProgram, 0.35f) : kBodyProgram);
            frameRound(renderer, r, 8, 1.4f, kToneProgram);
        } else if (primary) {
            fillRound(renderer, r, 8, hovered ? mixed(kPanelActive, kWirePreview, 0.3f) : kPanelActive);
            if (b.button == LessonButton::Next || hovered) frameRound(renderer, r, 8, 1.4f, kWirePreview);
        } else {
            fillRound(renderer, r, 8, hovered ? kPanelHover : kPanelButton);
        }
        const char* label = b.button == LessonButton::Program   ? "Programar a ROM"
                            : b.button == LessonButton::Next    ? "Próxima aula"
                            : b.button == LessonButton::Check   ? "Verificar"
                            : b.button == LessonButton::Hint    ? "Dica"
                            : b.button == LessonButton::Restart ? "Recomeçar"
                            : b.button == LessonButton::Reading ? "Ler a aula"
                                                                : "Todas as aulas";
        const std::string shown = fitted(renderer, Face::Medium, kUiText, label, r.w - 12.0f);
        const float w = textWidth(renderer, Face::Medium, kUiText, shown);
        write(renderer, Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 22.0f, shown, primary || b.button == LessonButton::Program ? kText : kTextDim);
    }

    const float x = area.x + 20.0f;
    const float width = area.w - kStripButtons - 56.0f;

    // Where it is: its kind, its place in the act, and a dot for each exercise of the act.
    const char* kind = lessonKindName(lesson);
    const Color kindColor = lessonKindColor(lesson);
    const float kindWidth = textWidth(renderer, Face::Medium, 11.5f, kind) + 18.0f;
    const SDL_FRect chip{x, area.y + 11.0f, kindWidth, 20.0f};
    fillRound(renderer, chip, 10, mixed(kPanel, kindColor, 0.18f));
    write(renderer, Face::Medium, 11.5f, chip.x + 9.0f, chip.y + 14.5f, kind, kindColor);
    const std::string actTitle(i18n::tr(lessonActs()[static_cast<std::size_t>(lesson.act)].title));
    const std::string where = i18n::format("Exercício {} de {}  ·  {}", {std::to_string(strip.position), std::to_string(strip.actDone.size()), actTitle});
    const float dotsWidth = static_cast<float>(strip.actDone.size()) * 13.0f;
    write(renderer, Face::Regular, 12.5f, chip.x + chip.w + 10.0f, area.y + 25.5f,
          fitted(renderer, Face::Regular, 12.5f, where, width - chip.w - 10.0f - dotsWidth - 16.0f), kTextDim);
    Mesh dots;
    for (std::size_t i = 0; i < strip.actDone.size(); ++i) {
        const Vec2 centre{x + width - dotsWidth + 13.0f * static_cast<float>(i) + 6.0f, area.y + 21.0f};
        const bool current = static_cast<int>(i) + 1 == strip.position;
        const bool done = strip.actDone[i] || (current && strip.passed);
        if (current) dots.addDisc(centre, 6.0f, kWirePreview);
        dots.addDisc(centre, 4.0f, done ? kKnobOn : current ? kPanel : kRule);
    }
    dots.flush(renderer);

    write(renderer, Face::Medium, 20, x, area.y + 58.0f, fitted(renderer, Face::Medium, 20, lesson.title, width - 30.0f), kText);
    if (strip.passed) drawCheckMark(renderer, {x + textWidth(renderer, Face::Medium, 20, lesson.title) + 18.0f, area.y + 51.0f}, 16.0f, kKnobOn);

    // The verdict, a problem or a hint: a box at the bottom, coloured by what it is.
    float stepsEnd = area.y + area.h - 12.0f;
    if (!strip.message.empty()) {
        const Color color = strip.passed ? kKnobOn : strip.problem ? kWarning : kWirePreview;
        const std::vector<std::string> message = wrapped(renderer, Face::Medium, 13.5f, strip.message, width - 28.0f);
        const std::size_t lines = std::min<std::size_t>(message.size(), 3);
        const float boxHeight = 14.0f + 18.0f * static_cast<float>(lines);
        const SDL_FRect box{x, area.y + area.h - 12.0f - boxHeight, width, boxHeight};
        fillRound(renderer, box, 8, mixed(kPanel, color, 0.14f));
        fillRound(renderer, {box.x, box.y, 4.0f, box.h}, 2, color);
        float messageY = box.y + 20.0f;
        for (std::size_t i = 0; i < lines; ++i) {
            write(renderer, Face::Medium, 13.5f, box.x + 16.0f, messageY, message[i], i == 0 ? color : mixed(color, kText, 0.4f));
            messageY += 18.0f;
        }
        stepsEnd = box.y - 6.0f;
    }

    // What the lesson is about, and what to do, as many lines as there is room for.
    float y = area.y + 80.0f;
    const std::vector<std::string> story = wrapped(renderer, Face::Regular, 14.0f, lesson.story, width);
    for (std::size_t i = 0; i < story.size() && i < 3 && y < stepsEnd; ++i) {
        write(renderer, Face::Regular, 14.0f, x, y, i == 2 && story.size() > 3 ? fitted(renderer, Face::Regular, 14.0f, story[i] + " …", width) : story[i], kText);
        y += 18.0f;
    }
    y += 6.0f;
    const Color stepColor = mixed(kTextDim, kText, 0.45f);
    for (std::size_t s = 0; s < lesson.steps.size() && y < stepsEnd; ++s) {
        const std::string number = std::to_string(s + 1);
        const Vec2 badge{x + 8.0f, y - 4.5f};
        Mesh disc;
        disc.addDisc(badge, 8.0f, kPanelButton);
        disc.flush(renderer);
        write(renderer, Face::Medium, 11.0f, badge.x - textWidth(renderer, Face::Medium, 11.0f, number) / 2.0f, badge.y + 4.0f, number, kWirePreview);
        for (const std::string& line : wrapped(renderer, Face::Regular, 13.5f, lesson.steps[s], width - 26.0f)) {
            if (y >= stepsEnd) break;
            write(renderer, Face::Regular, 13.5f, x + 26.0f, y, line, stepColor);
            y += 18.0f;
        }
        y += 2.0f;
    }
}

// --- The 32-bit computer: its panel, its debugger, and its instructions in the editor -------------------

namespace {

std::string hex32(std::uint32_t value) {
    char text[12];
    std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(value));
    return text;
}

Color instructionTone32(std::uint32_t word) {
    const std::uint32_t op = word >> 26;
    const std::uint32_t funct = word & 63u;
    if (op == mips::kOpSpecial) {
        if (funct == mips::kFnJr) return kToneJump;
        if (funct == mips::kFnSyscall) return kToneWorld;
        return kToneAlu;
    }
    if (op == mips::kOpLw || op == mips::kOpSw) return kToneMemory;
    if (op == mips::kOpBeq || op == mips::kOpBne) return kToneBranch;
    if (op == mips::kOpJ || op == mips::kOpJal) return kToneJump;
    if (op == mips::kOpLui) return kToneNumber;
    return kToneAlu;
}

// The 32 bits of a word, by the fields of its format: R, I or J.
void drawWordStrip32(SDL_Renderer* renderer, std::uint32_t word, const SDL_FRect& area, bool decoded = true) {
    const std::uint32_t op = word >> 26;
    // Fetched but not decoded yet: thirty-two bits and nothing more.
    if (!decoded) {
        const float cell = area.w / 32.0f;
        const SDL_FRect box{area.x, area.y, area.w - 2.0f, area.h};
        fillRound(renderer, box, 3, mixed(kBackground, kTextDim, 0.18f));
        frameRound(renderer, box, 3, 1.0f, kTextDim);
        for (int bit = 31; bit >= 0; --bit) {
            const float cx = area.x + cell * static_cast<float>(31 - bit) + cell / 2.0f - 1.0f;
            writeBit(renderer, ((word >> bit) & 1u) != 0, cx, area.y + area.h / 2.0f + 4.5f, std::min(11.5f, cell * 1.25f));
        }
        write(renderer, Face::Regular, 10.0f, area.x, area.y + area.h + 12.0f, "ainda sem campos: quem os separa é o decodificador", kTextDim);
        return;
    }
    struct Piece {
        const char* name;
        int high, low;
        Color color;
    };
    std::vector<Piece> pieces;
    if (op == mips::kOpSpecial) {
        pieces = {{"op", 31, 26, kFieldOp}, {"rs", 25, 21, kFieldRs}, {"rt", 20, 16, kFieldRt}, {"rd", 15, 11, kFieldRd},
                  {"shamt", 10, 6, kFieldUnused}, {"funct", 5, 0, kFieldFunc}};
    } else if (op == mips::kOpJ || op == mips::kOpJal) {
        pieces = {{"op", 31, 26, kFieldOp}, {"alvo", 25, 0, kFieldImm}};
    } else {
        pieces = {{"op", 31, 26, kFieldOp}, {"rs", 25, 21, kFieldRs}, {"rt", 20, 16, kFieldRt}, {"imm", 15, 0, kFieldImm}};
    }
    const float cell = area.w / 32.0f;
    float x = area.x;
    for (const Piece& piece : pieces) {
        const int bits = piece.high - piece.low + 1;
        const SDL_FRect box{x, area.y, cell * static_cast<float>(bits) - 2.0f, area.h};
        fillRound(renderer, box, 3, mixed(kBackground, piece.color, 0.2f));
        frameRound(renderer, box, 3, 1.0f, piece.color);
        for (int bit = piece.high; bit >= piece.low; --bit) {
            const float cx = x + cell * static_cast<float>(piece.high - bit) + cell / 2.0f - 1.0f;
            writeBit(renderer, ((word >> bit) & 1u) != 0, cx, box.y + area.h / 2.0f + 4.5f, std::min(11.5f, cell * 1.25f));
        }
        const float labelWidth = textWidth(renderer, Face::Regular, 10.0f, piece.name);
        if (labelWidth < box.w) write(renderer, Face::Regular, 10.0f, box.x + (box.w - labelWidth) / 2.0f, box.y + area.h + 12.0f, piece.name, piece.color);
        x += cell * static_cast<float>(bits);
    }
}

// The program as blocks, one per word: its address, the instruction and its 32 bits.
void drawCodeBlocks32(SDL_Renderer* renderer, const CodeView& view, const SDL_FRect& area) {
    if (view.program32.empty()) {
        for (const std::string& line : wrapped(renderer, Face::Regular, 13.0f, "Monte o programa com Ctrl+Enter: cada instrução aparece aqui como um bloco.", area.w)) {
            write(renderer, Face::Regular, 13.0f, area.x, area.y + 20.0f, line, kTextDim);
        }
        return;
    }
    constexpr float kBlock = kBlock32;
    const int count = static_cast<int>(view.program32.size());
    const int fit = std::min(count, blocksThatFit(area, true));
    const int first = std::clamp(view.blocksFirst, 0, std::max(0, count - fit));
    drawBlocksBar(renderer, area, first, fit, count);
    const float blockW = count > fit ? area.w - 10.0f : area.w;
    float y = area.y;
    for (int i = first; i < count && i < first + fit; ++i) {
        const bool running = i == view.runningAddress;
        const SDL_FRect r{area.x, y, blockW, kBlock};
        fillRound(renderer, r, 8, running ? mixed(kPanelRaised, kToneProgram, 0.28f) : kPanelRaised);
        frameRound(renderer, r, 8, running ? 1.8f : 1.0f, running ? kToneProgram : kBodyEdge);
        const std::uint32_t word = view.program32[static_cast<std::size_t>(i)];
        const auto address = mips::kTextBase + static_cast<std::uint32_t>(4 * i);
        write(renderer, Face::Mono, 11.5f, r.x + 10.0f, r.y + 18.0f, hex32(address).substr(4), running ? kToneProgram : kTextOff);
        write(renderer, Face::Mono, 13.0f, r.x + 50.0f, r.y + 18.0f, fitted(renderer, Face::Mono, 13.0f, mips::disassemble(word, address), r.w - 110.0f),
              instructionTone32(word));
        if (i < static_cast<int>(view.lines.size())) {
            const std::string line = i18n::format("linha {}", {std::to_string(view.lines[static_cast<std::size_t>(i)])});
            const float w = textWidth(renderer, Face::Regular, 11.0f, line);
            write(renderer, Face::Regular, 11.0f, r.x + r.w - 10.0f - w, r.y + 17.0f, line, kTextOff);
        }
        drawWordStrip32(renderer, word, {r.x + 10.0f, r.y + 26.0f, r.w - 20.0f, 16.0f});
        y += kBlock + 6.0f;
    }
}

// A box with a title, for the panel's parts.
SDL_FRect titledBox(SDL_Renderer* renderer, const SDL_FRect& r, const char* title) {
    fillRound(renderer, r, 8, kPanel);
    frameRound(renderer, r, 8, 1.0f, kRule);
    write(renderer, Face::Medium, 13.0f, r.x + 12.0f, r.y + 20.0f, title, kTextDim);
    return {r.x + 12.0f, r.y + 30.0f, r.w - 24.0f, r.h - 36.0f};
}

// The console: what the program printed with syscall, the last lines that fit.
void drawConsole(SDL_Renderer* renderer, const std::string& console, const SDL_FRect& inner) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= console.size()) {
        const std::size_t end = console.find('\n', start);
        const std::string line = console.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (const std::string& piece : wrapped(renderer, Face::Mono, 13.0f, line.empty() ? " " : line, inner.w)) lines.push_back(piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const auto fit = static_cast<std::size_t>(std::max(1.0f, inner.h / 17.0f));
    const std::size_t from = lines.size() > fit ? lines.size() - fit : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        write(renderer, Face::Mono, 13.0f, inner.x, inner.y + 12.0f + 17.0f * static_cast<float>(i - from), lines[i], kKnobOn);
    }
    if (console.empty()) {
        const std::vector<std::string> empty = wrapped(renderer, Face::Regular, 12.5f, "Vazio: o programa mostra aqui com syscall 1 e 11.", inner.w);
        for (std::size_t i = 0; i < empty.size(); ++i) write(renderer, Face::Regular, 12.5f, inner.x, inner.y + 12.0f + 16.0f * static_cast<float>(i), empty[i], kTextOff);
    }
}

}  // namespace

namespace {

constexpr float kPanelMemoryRow = 17.0f;
constexpr float kStepRow = 40.0f;

// Where the 32-bit panel's boxes go: the instruction, the registers and the steps on the left; the console,
// the memory and the stack on the right.
struct Panel32Layout {
    SDL_FRect now, regs, steps, console, memory, stack;
};

Panel32Layout panel32Layout(const SDL_FRect& area) {
    Panel32Layout l;
    const float top = area.y + 60.0f;
    const float left = area.x + 12.0f;
    const float width = area.w - 24.0f;
    const float rightWidth = std::min(400.0f, width * 0.38f);
    const float mainWidth = width - rightWidth - 12.0f;
    const float bottom = area.y + area.h - 12.0f;
    l.now = {left, top, mainWidth, 98.0f};
    l.regs = {left, top + 110.0f, mainWidth, 8.0f * 22.0f + 36.0f};
    l.steps = {left, l.regs.y + l.regs.h + 12.0f, mainWidth, std::max(60.0f, bottom - (l.regs.y + l.regs.h + 12.0f))};
    const float rx = left + mainWidth + 12.0f;
    l.console = {rx, top, rightWidth, 120.0f};
    const float rest = bottom - (top + 132.0f);
    l.memory = {rx, top + 132.0f, rightWidth, rest * 0.64f};
    l.stack = {rx, l.memory.y + l.memory.h + 12.0f, rightWidth, rest * 0.36f - 12.0f};
    return l;
}

// The words a program is using, from the start of .data: named, holding something, read or written now, or
// just changed. The list grows to two words past the last of them; the stack, at the RAM's far end, is left
// to its own box.
std::uint32_t usedRows32(const Computer32State& state, const DebugView* debug) {
    const auto words = static_cast<std::uint32_t>(state.memory.size());
    const std::uint32_t stackWord = (state.registers[mips::kSp] >> 2) % std::max(1u, words);
    const std::uint32_t end = stackWord > words / 2 ? stackWord : words;  // where the stack starts, when $sp is high
    std::uint32_t last = 0;
    for (std::uint32_t w = 0; w < end; ++w) {
        bool used = state.memory[w] != 0 || static_cast<int>(w) == state.accessed;
        if (debug != nullptr && debug->hasBefore && debug->memoryBefore32.size() == words && debug->memoryBefore32[w] != state.memory[w]) used = true;
        for (const auto& [name, at] : state.dataLabels) used = used || at == w;
        if (used) last = w;
    }
    return std::min(end, std::max(8u, last + 3u));
}

int memoryPanelVisible(const SDL_FRect& inner) { return std::max(1, static_cast<int>((inner.h - 34.0f) / kPanelMemoryRow)); }

// The RAM's words from the start of .data, as far as the program uses them: each with its address, its value,
// the .data label naming it and the registers pointing at it. The word the next lw or sw uses is framed; the one
// the last instruction changed is lit, with what it was before. A line above and one below say more memory is
// there, outside the list. The RAM keeps 1024 words, picked by an address's bits 11 to 2, so $gp (0x10008000)
// falls on the first word, the same as .data.
void drawMemory32(SDL_Renderer* renderer, const Computer32State& state, const DebugView* debug, const SDL_FRect& box) {
    const auto words = static_cast<std::uint32_t>(mips::kMemoryWords);
    if (state.memory.size() < words) {
        write(renderer, Face::Regular, 12.5f, box.x, box.y + 12.0f, "Este computador não tem RAM ligada.", kTextOff);
        return;
    }
    const std::uint32_t total = usedRows32(state, debug);
    const int visible = memoryPanelVisible(box);
    const int maxFirst = std::max(0, static_cast<int>(total) - visible);
    const auto first = static_cast<std::uint32_t>(std::clamp(debug != nullptr ? debug->memoryRow32 : 0, 0, maxFirst));
    write(renderer, Face::Regular, 11.5f, box.x, box.y + 8.0f,
          first == 0 ? "… antes de .data: o fim da RAM, onde fica a pilha" : i18n::format("… {} palavras acima: role para ver", {std::to_string(first)}),
          kTextOff);
    for (std::uint32_t row = 0; row < static_cast<std::uint32_t>(visible); ++row) {
        const std::uint32_t word = first + row;
        if (word >= total) break;
        const std::uint32_t value = state.memory[word];
        const float y = box.y + 26.0f + kPanelMemoryRow * static_cast<float>(row);
        const bool changed = debug != nullptr && debug->hasBefore && debug->memoryBefore32.size() == words && debug->memoryBefore32[word] != value;
        if (changed) fillRound(renderer, {box.x - 4.0f, y - 12.0f, box.w + 8.0f, 16.0f}, 4, Color{92, 84, 30});
        if (static_cast<int>(word) == state.accessed) frameRound(renderer, {box.x - 4.0f, y - 12.0f, box.w + 8.0f, 16.0f}, 4, 1.2f, kWirePreview);
        // Who names this word: its .data labels, then the registers holding an address on it.
        std::string names;
        for (const auto& [name, at] : state.dataLabels) {
            if (at == word) names += (names.empty() ? "" : " ") + name;
        }
        std::string pointers;
        static constexpr unsigned kOrder[] = {28, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31};
        for (const unsigned r : kOrder) {  // $gp first; not $at, the assembler's own, nor $sp, which has its box
            const std::uint32_t v = state.registers[r];
            if ((v >> 28) != 1u || (v >> 2) % words != word) continue;  // addresses near .data and $gp only
            pointers += (pointers.empty() ? "" : " ") + std::string(mips::registerName(r));
        }
        float x = box.x;
        if (!names.empty()) {
            const std::string shown = fitted(renderer, Face::Medium, 12.0f, names, 74.0f);
            write(renderer, Face::Medium, 12.0f, x, y, shown, kText);
            x += textWidth(renderer, Face::Medium, 12.0f, shown) + 6.0f;
        }
        if (!pointers.empty() && x < box.x + 80.0f) write(renderer, Face::Medium, 11.5f, x, y, fitted(renderer, Face::Medium, 11.5f, pointers, box.x + 84.0f - x), kWirePreview);
        write(renderer, Face::Mono, 11.5f, box.x + 88.0f, y, hex32(mips::kDataBase + 4u * word), kTextDim);
        write(renderer, Face::Mono, 12.0f, box.x + 158.0f, y, hex32(value), value != 0 ? kKnobOn : kTextOff);
        std::string decimal = std::to_string(static_cast<std::int32_t>(value));
        if (changed) decimal = i18n::format("era {}, agora {}", {std::to_string(static_cast<std::int32_t>(debug->memoryBefore32[word])), decimal});
        const float w = textWidth(renderer, Face::Regular, 12.0f, decimal);
        if (box.x + 228.0f + w <= box.x + box.w) write(renderer, Face::Regular, 12.0f, box.x + box.w - w, y, decimal, changed ? kText : kTextDim);
    }
    const std::uint32_t shownEnd = std::min(total, first + static_cast<std::uint32_t>(visible));
    const std::string below = shownEnd < total ? i18n::format("… {} palavras abaixo: role para ver", {std::to_string(total - shownEnd)})
                                               : i18n::format("… e mais {} palavras livres, até a pilha", {std::to_string(words - total)});
    write(renderer, Face::Regular, 11.5f, box.x, box.y + 26.0f + kPanelMemoryRow * static_cast<float>(visible) + 2.0f, below, kTextOff);
}

// What each instruction run did, the last at the top: its address and text, then what changed.
void drawSteps32(SDL_Renderer* renderer, const DebugView* debug, const SDL_FRect& box) {
    const std::vector<Step32>* steps = debug != nullptr ? debug->steps32 : nullptr;
    if (steps == nullptr || steps->empty()) {
        const std::vector<std::string> lines =
            wrapped(renderer, Face::Regular, 13.0f,
                    "Aperte Próxima instrução, ou Rodar: cada instrução que o computador executa aparece aqui, com o que ela fez nos registradores, "
                    "na memória e no console. A mais nova fica em cima.",
                    box.w);
        for (std::size_t i = 0; i < lines.size(); ++i) write(renderer, Face::Regular, 13.0f, box.x, box.y + 14.0f + 19.0f * static_cast<float>(i), lines[i], kTextDim);
        return;
    }
    const auto rows = static_cast<std::size_t>(std::max(1.0f, box.h / kStepRow));
    for (std::size_t i = 0; i < rows && i < steps->size(); ++i) {
        const Step32& step = (*steps)[steps->size() - 1 - i];
        const float y = box.y + kStepRow * static_cast<float>(i);
        if (i == 0) fillRound(renderer, {box.x - 4.0f, y, box.w + 8.0f, kStepRow - 4.0f}, 5, mixed(kPanel, kWirePreview, 0.12f));
        write(renderer, Face::Mono, 12.5f, box.x + 4.0f, y + 15.0f, fitted(renderer, Face::Mono, 12.5f, step.instruction, box.w - 8.0f), i == 0 ? kText : kTextDim);
        write(renderer, Face::Regular, 12.5f, box.x + 4.0f, y + 31.0f, fitted(renderer, Face::Regular, 12.5f, step.effect, box.w - 8.0f), i == 0 ? kKnobOn : kTextDim);
    }
}

}  // namespace

int panel32MemoryMaxRow(const Computer32State& state, const DebugView& debug, const SDL_FRect& area) {
    const SDL_FRect box = panel32Layout(area).memory;
    const SDL_FRect inner{box.x + 12.0f, box.y + 30.0f, box.w - 24.0f, box.h - 36.0f};  // as titledBox leaves it
    return std::max(0, static_cast<int>(usedRows32(state, &debug)) - memoryPanelVisible(inner));
}

SDL_FRect panel32MemoryArea(const SDL_FRect& area) { return panel32Layout(area).memory; }

namespace {

constexpr float kMemoryRowHeight = 26.0f;

struct MemoryLayout {
    SDL_FRect base;
    std::vector<SDL_FRect> jumps;
    std::vector<std::string> note;
    float listTop = 0.0f;
    int rows = 1;
};

MemoryLayout memoryLayout(SDL_Renderer* renderer, const MemoryView& view, const SDL_FRect& area) {
    MemoryLayout l;
    float right = area.x + area.w - 16.0f;
    const char* base = view.hex ? "Mostrar em decimal" : "Mostrar em hexadecimal";
    const float baseWidth = textWidth(renderer, Face::Medium, kUiText, base) + 28.0f;
    l.base = {right - baseWidth, area.y + 14.0f, baseWidth, 30.0f};
    right -= baseWidth + 14.0f;
    l.jumps.resize(view.jumps.size());
    for (std::size_t i = view.jumps.size(); i-- > 0;) {
        const float w = textWidth(renderer, Face::Medium, kUiText, view.jumps[i].first) + 24.0f;
        l.jumps[i] = {right - w, area.y + 14.0f, w, 30.0f};
        right -= w + 8.0f;
    }
    l.note = wrapped(renderer, Face::Regular, 12.5f, view.note, area.w - 32.0f);
    if (l.note.size() > 2) l.note.resize(2);
    l.listTop = area.y + 64.0f + 18.0f * static_cast<float>(l.note.size()) + 26.0f;
    l.rows = std::max(1, static_cast<int>((area.y + area.h - 30.0f - l.listTop) / kMemoryRowHeight));
    return l;
}

std::string memoryHexText(const MemoryView& view, std::uint32_t value) {
    const std::string all = hex32(value);
    return all.substr(all.size() - view.bits / 4);
}

std::string memoryDecimalText(const MemoryView& view, std::uint32_t value) {
    if (view.bits == 16) return std::to_string(static_cast<std::int16_t>(value & 0xFFFFu));
    return std::to_string(static_cast<std::int32_t>(value));
}

std::string memoryAddressText(const MemoryView& view, std::uint32_t address) {
    const std::string all = hex32(address);
    return "0x" + (view.bits == 16 ? all.substr(4) : all);
}

}  // namespace

int memoryVisibleRows(SDL_Renderer* renderer, const MemoryView& view, const SDL_FRect& area) { return memoryLayout(renderer, view, area).rows; }

MemoryHit memoryViewAt(SDL_Renderer* renderer, const MemoryView& view, const SDL_FRect& area, float x, float y) {
    const MemoryLayout l = memoryLayout(renderer, view, area);
    if (inside(l.base, x, y)) return {MemoryChoice::Base};
    for (std::size_t i = 0; i < l.jumps.size(); ++i) {
        if (inside(l.jumps[i], x, y)) return {MemoryChoice::Jump, static_cast<int>(i)};
    }
    return {};
}

// The memory tab: one word to a row, its address, its value in hexadecimal and in decimal, who names it and,
// when the last instruction changed it, what it was.
void drawMemoryView(SDL_Renderer* renderer, const MemoryView& view, const SDL_FRect& area, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const MemoryLayout l = memoryLayout(renderer, view, area);
    write(renderer, Face::Medium, 18.0f, area.x + 16.0f, area.y + 34.0f, "Memória", kText);
    const std::string size = i18n::format("{} palavras de {} bits", {std::to_string(view.words.size()), std::to_string(view.bits)});
    write(renderer, Face::Regular, 13.0f, area.x + 16.0f + textWidth(renderer, Face::Medium, 18.0f, "Memória") + 14.0f, area.y + 33.0f, size, kTextDim);
    for (std::size_t i = 0; i < l.note.size(); ++i) write(renderer, Face::Regular, 12.5f, area.x + 16.0f, area.y + 64.0f + 18.0f * static_cast<float>(i), l.note[i], kTextDim);
    fillRound(renderer, l.base, 8, inside(l.base, mouseX, mouseY) ? kPanelHover : kPanelActive);
    write(renderer, Face::Medium, kUiText, l.base.x + 14.0f, l.base.y + 20.0f, view.hex ? "Mostrar em decimal" : "Mostrar em hexadecimal", kText);
    for (std::size_t i = 0; i < l.jumps.size(); ++i) {
        const bool usable = view.jumps[i].second >= 0;
        fillRound(renderer, l.jumps[i], 8, usable && inside(l.jumps[i], mouseX, mouseY) ? kPanelHover : kPanelButton);
        write(renderer, Face::Medium, kUiText, l.jumps[i].x + 12.0f, l.jumps[i].y + 20.0f, view.jumps[i].first, usable ? kText : kTextOff);
    }
    // The columns: where each one starts.
    const float x0 = area.x + 16.0f;
    const float xValue = x0 + 140.0f, xOther = xValue + 130.0f, xNames = xOther + 130.0f, xChange = xNames + 240.0f;
    const char* valueHead = view.hex ? "Valor (hexadecimal)" : "Valor (decimal)";
    const char* otherHead = view.hex ? "Em decimal" : "Em hexadecimal";
    write(renderer, Face::Medium, 12.0f, x0, l.listTop - 8.0f, "Endereço", kTextDim);
    write(renderer, Face::Medium, 12.0f, xValue, l.listTop - 8.0f, valueHead, kTextDim);
    write(renderer, Face::Medium, 12.0f, xOther, l.listTop - 8.0f, otherHead, kTextDim);
    write(renderer, Face::Medium, 12.0f, xNames, l.listTop - 8.0f, "Quem aponta", kTextDim);
    write(renderer, Face::Medium, 12.0f, xChange, l.listTop - 8.0f, "Última mudança", kTextDim);
    const int total = static_cast<int>(view.words.size());
    const bool known = view.before.size() == view.words.size();
    for (int r = 0; r < l.rows; ++r) {
        const int index = view.firstRow + r;
        if (index >= total) break;
        const auto word = static_cast<std::uint32_t>(index);
        const std::uint32_t value = view.words[word];
        const SDL_FRect row{x0 - 6.0f, l.listTop + kMemoryRowHeight * static_cast<float>(r), area.w - 20.0f, kMemoryRowHeight - 2.0f};
        const bool changed = known && view.before[word] != value;
        if (changed) {
            fillRound(renderer, row, 5, Color{92, 84, 30});
        } else if (r % 2 == 1) {
            fillRect(renderer, row, kPanel);
        }
        if (index == view.accessed) frameRound(renderer, row, 5, 1.4f, kWirePreview);
        const float y = row.y + 17.0f;
        write(renderer, Face::Mono, 13.0f, x0, y, memoryAddressText(view, view.base + view.step * word), kWirePreview);
        const std::string main = view.hex ? memoryHexText(view, value) : memoryDecimalText(view, value);
        const std::string other = view.hex ? memoryDecimalText(view, value) : "0x" + memoryHexText(view, value);
        write(renderer, Face::Mono, 13.0f, xValue, y, main, value != 0 ? kKnobOn : kTextOff);
        write(renderer, Face::Regular, 13.0f, xOther, y, other, kTextDim);
        float nx = xNames;
        for (const MemoryMark& mark : view.marks) {
            if (mark.word != word || nx > xChange - 20.0f) continue;
            const std::string shown = fitted(renderer, Face::Medium, 12.5f, mark.name, xChange - 10.0f - nx);
            write(renderer, Face::Medium, 12.5f, nx, y, shown, mark.pointer ? kWirePreview : kText);
            nx += textWidth(renderer, Face::Medium, 12.5f, shown) + 8.0f;
        }
        if (changed) {
            const std::string was = i18n::format("era {}, agora {}", {view.hex ? memoryHexText(view, view.before[word]) : memoryDecimalText(view, view.before[word]),
                                                                       view.hex ? memoryHexText(view, value) : memoryDecimalText(view, value)});
            write(renderer, Face::Regular, 13.0f, xChange, y, fitted(renderer, Face::Regular, 13.0f, was, area.x + area.w - 16.0f - xChange), kText);
        }
    }
    const std::string where = i18n::format("palavras {} a {} de {} · a roda do mouse rola",
                                           {std::to_string(view.firstRow + 1), std::to_string(std::min(total, view.firstRow + l.rows)), std::to_string(total)});
    write(renderer, Face::Regular, 12.0f, area.x + area.w - 16.0f - textWidth(renderer, Face::Regular, 12.0f, where), area.y + area.h - 10.0f, where, kTextDim);
}

void drawComputer32Panel(SDL_Renderer* renderer, const Computer32State& state, const std::string& console, const DebugView* debug,
                         const SDL_FRect& area, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const SDL_FRect next = panelNextButton(area);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Step, {next.x + 20.0f, next.y + next.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, next.x + 36.0f, next.y + 21.0f, "Próxima instrução", kText);
    const float sentenceX = next.x + next.w + 18.0f;
    write(renderer, Face::Regular, 15, sentenceX, next.y + 22.0f,
          fitted(renderer, Face::Regular, 15, explainInstruction32(state), area.x + area.w - sentenceX - 16.0f), kText);
    const Panel32Layout l = panel32Layout(area);

    // The instruction about to run, and its bits.
    const SDL_FRect now = titledBox(renderer, l.now, "Instrução no PC");
    const float pcX = now.x + std::max(120.0f, textWidth(renderer, Face::Medium, 13.0f, "Instrução no PC") + 16.0f);
    write(renderer, Face::Mono, 14.0f, pcX, now.y - 10.0f, "PC = 0x" + hex32(state.pc), kWirePreview);
    write(renderer, Face::Mono, 16.0f, now.x, now.y + 14.0f, state.text, instructionTone32(state.instruction));
    const std::string word = "0x" + hex32(state.instruction);
    write(renderer, Face::Mono, 13.0f, now.x + now.w - textWidth(renderer, Face::Mono, 13.0f, word), now.y + 14.0f, word, kTextDim);
    drawWordStrip32(renderer, state.instruction, {now.x, now.y + 24.0f, now.w, 18.0f});

    // The 32 registers, in four columns; what the last instruction changed stands out.
    const SDL_FRect regs = titledBox(renderer, l.regs, "Registradores");
    const float column = regs.w / 4.0f;
    const float row = std::min(22.0f, regs.h / 8.0f);
    for (unsigned r = 0; r < 32; ++r) {
        const float x = regs.x + static_cast<float>(r / 8) * column;
        const float y = regs.y + static_cast<float>(r % 8) * row;
        const bool changed = debug != nullptr && debug->hasBefore && debug->before32[r] != state.registers[r];
        if (changed) fillRound(renderer, {x - 4.0f, y, column - 6.0f, row - 2.0f}, 4, Color{92, 84, 30});
        write(renderer, Face::Medium, 12.5f, x, y + row / 2.0f + 4.0f, std::string(mips::registerName(r)), kText);
        write(renderer, Face::Mono, 12.0f, x + 46.0f, y + row / 2.0f + 4.0f, hex32(state.registers[r]), state.registers[r] != 0 ? kKnobOn : kTextOff);
        const std::string decimal = std::to_string(static_cast<std::int32_t>(state.registers[r]));
        const float w = textWidth(renderer, Face::Regular, 11.5f, decimal);
        if (x + 46.0f + 64.0f + w < x + column - 8.0f) {
            write(renderer, Face::Regular, 11.5f, x + column - 10.0f - w, y + row / 2.0f + 4.0f, decimal, changed ? kText : kTextDim);
        }
    }
    drawSteps32(renderer, debug, titledBox(renderer, l.steps, "Passo a passo: o que cada instrução fez"));

    // Beside them: the console, the memory and the stack.
    drawConsole(renderer, console, titledBox(renderer, l.console, "Console"));
    drawMemory32(renderer, state, debug, titledBox(renderer, l.memory, "Memória"));
    const SDL_FRect stack = titledBox(renderer, l.stack, "Pilha (de $sp para cima)");
    const auto stackRows = static_cast<std::size_t>(std::max(1.0f, stack.h / 17.0f));
    for (std::size_t i = 0; i < state.stack.size() && i < stackRows; ++i) {
        const float y = stack.y + 12.0f + 17.0f * static_cast<float>(i);
        write(renderer, Face::Mono, 12.0f, stack.x, y, hex32(state.registers[mips::kSp] + static_cast<std::uint32_t>(4 * i)), kWirePreview);
        write(renderer, Face::Mono, 12.0f, stack.x + 80.0f, y, hex32(state.stack[i]), state.stack[i] != 0 ? kKnobOn : kTextOff);
    }
}

namespace {

// --- The 32-bit X-ray: the textbook datapath, on a 1000 by 470 plan -------------------------------------

enum class X32Shape : std::uint8_t { Box, Mux, MuxDown, Adder, Alu };

enum X32Block : int {
    kB32Next, kB32Pc, kB32Add4, kB32Rom, kB32Decoder, kB32Control, kB32Dst, kB32Registers, kB32Extender, kB32Jump, kB32MuxB, kB32Alu,
    kB32Branch, kB32Ram, kB32System, kB32Switches, kB32Back, kB32Console,
};

struct X32BlockShape {
    X32Block block;
    int stage;
    SDL_FRect rect;
    X32Shape shape;
    const char* name;
    const char* detail;
};

constexpr X32BlockShape kX32Blocks[] = {
    {kB32Next, 5, {20, 104, 108, 40}, X32Shape::MuxDown, "Próximo PC", ""},
    {kB32Pc, 0, {44, 196, 60, 48}, X32Shape::Box, "PC", ""},
    {kB32Add4, 0, {144, 92, 46, 42}, X32Shape::Adder, "PC + 4", "+4"},
    {kB32Rom, 0, {144, 180, 98, 92}, X32Shape::Box, "ROM", "instruções"},
    {kB32Decoder, 1, {272, 150, 100, 188}, X32Shape::Box, "Decodificador", ""},
    {kB32Control, 1, {272, 384, 140, 52}, X32Shape::Box, "Controle", ""},
    {kB32Dst, 1, {402, 236, 22, 56}, X32Shape::Mux, "", ""},
    {kB32Registers, 1, {440, 150, 140, 150}, X32Shape::Box, "Registradores", "32 × 32 bits"},
    {kB32Extender, 1, {440, 320, 140, 34}, X32Shape::Box, "Extensor 16 para 32", ""},
    {kB32Jump, 2, {440, 28, 140, 30}, X32Shape::Box, "Alvo do salto", ""},
    {kB32MuxB, 2, {612, 220, 24, 80}, X32Shape::Mux, "", ""},
    {kB32Alu, 2, {664, 160, 80, 140}, X32Shape::Alu, "ULA", ""},
    {kB32Branch, 2, {664, 80, 80, 44}, X32Shape::Adder, "Alvo do desvio", "PC+4+imm×4"},
    {kB32Ram, 3, {786, 190, 96, 100}, X32Shape::Box, "RAM", "dados"},
    {kB32System, 2, {786, 330, 96, 46}, X32Shape::Box, "Sistema", "syscall"},
    {kB32Switches, 4, {786, 400, 96, 30}, X32Shape::Box, "Chaves", ""},
    {kB32Back, 4, {916, 170, 24, 170}, X32Shape::Mux, "", ""},
    {kB32Console, 2, {916, 360, 76, 40}, X32Shape::Box, "Console", ""},
};

enum X32Wire : int {
    kW32PcRom, kW32PcAdd, kW32RomDecoder, kW32DecoderControl, kW32Rs, kW32Rt, kW32RtDst, kW32RdDst, kW32DstWrite, kW32Imm, kW32Target,
    kW32NextJump, kW32A, kW32B, kW32ExtMux, kW32MuxAlu, kW32ExtBranch, kW32NextBranch, kW32Zero, kW32Address, kW32Store, kW32Loaded,
    kW32AluBack, kW32Link, kW32SwitchesBack, kW32BackRegisters, kW32NextPlain, kW32BranchNext, kW32JumpNext, kW32RegisterNext, kW32NextPc,
    kW32Select, kW32Print,
    kW32SystemV0, kW32SystemA0, kW32SystemCall,  // what a syscall reads, and the control's line that says it is one
    // The control's lines: the ALU's operation, the immediate for B, writing a register and which, the sign of the
    // immediate, writing the RAM, and what goes back to the registers.
    kW32AluOp, kW32UseImm, kW32RegWrite, kW32RegDst, kW32SignExt, kW32MemWrite, kW32Back,
};

bool x32ControlLine(X32Wire wire) {
    switch (wire) {
        case kW32Select:
        case kW32SystemCall:
        case kW32AluOp:
        case kW32UseImm:
        case kW32RegWrite:
        case kW32RegDst:
        case kW32SignExt:
        case kW32MemWrite:
        case kW32Back: return true;
        default: return false;
    }
}

struct X32WireShape {
    X32Wire wire;
    int stage;
    std::vector<Vec2> points;
};

const std::vector<X32WireShape>& x32Wires() {
    static const std::vector<X32WireShape> wires{
        {kW32PcRom, 0, {{104, 220}, {144, 220}}},
        {kW32PcAdd, 0, {{114, 220}, {114, 104}, {144, 104}}},
        {kW32RomDecoder, 0, {{242, 226}, {272, 226}}},
        {kW32DecoderControl, 1, {{322, 338}, {322, 384}}},
        {kW32Rs, 1, {{372, 176}, {440, 176}}},
        {kW32Rt, 1, {{372, 204}, {440, 204}}},
        {kW32RtDst, 1, {{372, 204}, {390, 204}, {390, 250}, {402, 250}}},
        {kW32RdDst, 1, {{372, 278}, {402, 278}}},
        {kW32DstWrite, 1, {{424, 264}, {440, 264}}},
        {kW32Imm, 1, {{372, 316}, {386, 316}, {386, 337}, {440, 337}}},
        {kW32Target, 1, {{340, 150}, {340, 50}, {440, 50}}},
        {kW32NextJump, 2, {{190, 113}, {204, 113}, {204, 72}, {420, 72}, {420, 38}, {440, 38}}},
        {kW32A, 2, {{580, 180}, {664, 180}}},
        {kW32B, 2, {{580, 240}, {612, 240}}},
        {kW32ExtMux, 2, {{580, 337}, {602, 337}, {602, 282}, {612, 282}}},
        {kW32MuxAlu, 2, {{636, 260}, {664, 260}}},
        {kW32ExtBranch, 2, {{580, 346}, {648, 346}, {648, 114}, {664, 114}}},
        {kW32NextBranch, 2, {{190, 113}, {204, 113}, {204, 72}, {654, 72}, {654, 92}, {664, 92}}},
        {kW32Zero, 2, {{704, 288}, {704, 452}, {342, 452}, {342, 436}}},
        {kW32Address, 3, {{744, 230}, {786, 230}}},
        {kW32Store, 3, {{580, 240}, {592, 240}, {592, 372}, {772, 372}, {772, 272}, {786, 272}}},
        {kW32Loaded, 3, {{882, 240}, {916, 240}}},
        {kW32AluBack, 4, {{744, 230}, {764, 230}, {764, 180}, {904, 180}, {904, 200}, {916, 200}}},
        {kW32Link, 4, {{190, 113}, {204, 113}, {204, 72}, {898, 72}, {898, 280}, {916, 280}}},
        {kW32SwitchesBack, 4, {{882, 415}, {908, 415}, {908, 320}, {916, 320}}},
        {kW32BackRegisters, 4, {{940, 255}, {958, 255}, {958, 140}, {510, 140}, {510, 150}}},
        {kW32NextPlain, 5, {{190, 113}, {204, 113}, {204, 90}, {40, 90}, {40, 104}}},
        {kW32BranchNext, 5, {{744, 102}, {770, 102}, {770, 8}, {64, 8}, {64, 104}}},
        {kW32JumpNext, 5, {{580, 43}, {598, 43}, {598, 22}, {86, 22}, {86, 104}}},
        {kW32RegisterNext, 5, {{580, 180}, {590, 180}, {590, 15}, {108, 15}, {108, 104}}},
        {kW32NextPc, 5, {{74, 144}, {74, 196}}},
        {kW32Select, 5, {{272, 410}, {10, 410}, {10, 124}, {24, 124}}},
        {kW32Print, 2, {{882, 353}, {900, 353}, {900, 380}, {916, 380}}},
        // $v0 from read port A and $a0 from read port B, into the System unit; and the control's Sistema line.
        {kW32SystemV0, 2, {{580, 180}, {586, 180}, {586, 388}, {776, 388}, {776, 342}, {786, 342}}},
        {kW32SystemA0, 2, {{580, 240}, {592, 240}, {592, 380}, {768, 380}, {768, 364}, {786, 364}}},
        {kW32SystemCall, 1, {{412, 422}, {760, 422}, {760, 394}, {834, 394}, {834, 376}}},
        {kW32AluOp, 1, {{412, 396}, {684, 396}, {684, 290}}},
        {kW32UseImm, 1, {{412, 404}, {624, 404}, {624, 296}}},
        {kW32RegWrite, 1, {{412, 388}, {432, 388}, {432, 310}, {460, 310}, {460, 300}}},
        {kW32RegDst, 1, {{395, 384}, {395, 300}, {413, 300}, {413, 292}}},
        {kW32SignExt, 1, {{370, 384}, {370, 362}, {510, 362}, {510, 354}}},
        {kW32MemWrite, 1, {{380, 436}, {380, 458}, {782, 458}, {782, 300}, {800, 300}, {800, 290}}},
        {kW32Back, 1, {{390, 436}, {390, 464}, {996, 464}, {996, 150}, {928, 150}, {928, 170}}},
    };
    return wires;
}

// The values on the 32-bit wires, in the base the X-ray is showing: an address is never negative, a value is signed.
std::string shortHex(std::uint32_t value) { return gZoom.hex ? xrayHex(value) : std::to_string(value); }
std::string decimal32(std::uint32_t value) { return gZoom.hex ? xrayHex(value) : std::to_string(static_cast<std::int32_t>(value)); }
// The 16-bit immediate as the instruction has it, before the extender.
std::string immediate32(std::uint32_t value) { return gZoom.hex ? xrayHex(value) : std::to_string(value); }

bool x32BlockUsed(const Trace32& t, X32Block block) {
    using Back = Trace32::Back;
    switch (block) {
        case kB32Registers: return t.readsA || t.readsB || t.back != Back::None;
        case kB32Dst: return t.back != Back::None;
        case kB32Extender: return t.usesImmediate;
        case kB32Jump: return t.next == Trace32::Next::Jump;
        case kB32MuxB:
        case kB32Alu: return t.usesAlu;
        case kB32Branch: return t.branches;
        case kB32Ram: return t.readsMemory || t.writesMemory;
        case kB32System: return t.syscall;
        case kB32Switches: return t.back == Back::Switches;
        case kB32Back: return t.back != Back::None;
        case kB32Console: return t.printsNumber || t.printsChar;
        default: return true;
    }
}

bool x32WireUsed(const Trace32& t, X32Wire wire) {
    using Back = Trace32::Back;
    using Next = Trace32::Next;
    switch (wire) {
        case kW32Rs: return t.readsA;
        case kW32Rt: return t.readsB;
        case kW32RtDst: return t.back != Back::None && t.destination == 0;
        case kW32RdDst: return t.back != Back::None && t.destination == 1;
        case kW32DstWrite:
        case kW32BackRegisters: return t.back != Back::None;
        case kW32Imm: return t.usesImmediate;
        case kW32Target:
        case kW32NextJump: return t.next == Next::Jump;
        case kW32A: return t.usesAlu && t.readsA && !t.syscall;
        case kW32B: return t.usesAlu && t.readsB && !t.aluTakesImmediate;
        case kW32ExtMux: return t.aluTakesImmediate;
        case kW32MuxAlu: return t.usesAlu;
        case kW32ExtBranch:
        case kW32NextBranch:
        case kW32Zero: return t.branches;
        case kW32Address: return t.readsMemory || t.writesMemory;
        case kW32Store: return t.writesMemory;
        case kW32Loaded: return t.readsMemory;
        case kW32AluBack: return t.back == Back::Alu;
        case kW32Link: return t.back == Back::Link;
        case kW32SwitchesBack: return t.back == Back::Switches;
        case kW32NextPlain: return t.next == Next::Plain;
        case kW32BranchNext: return t.next == Next::Branch;
        case kW32JumpNext: return t.next == Next::Jump;
        case kW32RegisterNext: return t.next == Next::Register;
        case kW32NextPc: return !t.halts;
        case kW32Select: return t.branches || t.next == Next::Jump || t.next == Next::Register;
        case kW32Print: return t.printsNumber || t.printsChar;
        case kW32AluOp:
        case kW32UseImm: return t.usesAlu;
        case kW32RegWrite:
        case kW32RegDst:
        case kW32Back: return t.back != Trace32::Back::None;
        case kW32SignExt: return t.usesImmediate;
        case kW32MemWrite: return t.writesMemory;
        case kW32SystemV0:
        case kW32SystemA0:
        case kW32SystemCall: return t.syscall;
        default: return true;
    }
}

std::string x32WireValue(const Trace32& t, X32Wire wire) {
    const auto name = [](unsigned index) { return std::string(mips::registerName(index)); };
    switch (wire) {
        case kW32PcRom: return shortHex(t.pc);
        case kW32RomDecoder: return "0x" + hex32(t.word);  // the word stays in hexadecimal: its bits are in the strip above
        case kW32Rs: return name(t.registerA);
        case kW32Rt: return name(t.registerB);
        case kW32RtDst: return name(t.rt);
        case kW32RdDst: return name(t.rd);
        case kW32DstWrite: return name(t.written);
        case kW32Imm: return immediate32(t.imm);
        case kW32Target: return shortHex(t.word & 0x03FFFFFFu);
        case kW32A: return decimal32(t.valueA);
        case kW32B: return decimal32(t.valueB);
        case kW32ExtMux: return decimal32(t.extended);
        case kW32ExtBranch: return decimal32(t.extended << 2);
        case kW32NextBranch:
        case kW32NextPlain:
        case kW32Link: return shortHex(t.pc4);
        case kW32Zero: return t.zero ? "Z=1" : "Z=0";
        case kW32Address: return shortHex(t.address);
        case kW32Store: return decimal32(t.valueB);
        case kW32Loaded: return decimal32(t.memoryValue);
        case kW32AluBack: return decimal32(t.aluResult);
        case kW32SwitchesBack:
        case kW32BackRegisters: return decimal32(t.writtenValue);
        case kW32BranchNext: return shortHex(t.branchTarget);
        case kW32JumpNext: return shortHex(t.jumpTarget);
        case kW32RegisterNext: return shortHex(t.valueA);
        case kW32NextPc: return shortHex(t.nextPc);
        case kW32Select:
            if (t.branches) return i18n::tr(t.taken ? "desvia" : "não desvia");
            return i18n::tr(t.next == Trace32::Next::Jump ? "salta" : "salta p/ reg");
        case kW32Print: return t.printsNumber ? decimal32(t.printed) : std::string(1, static_cast<char>(t.printed & 0x7Fu));
        case kW32AluOp: {
            static const std::pair<const char*, const char*> kNames[] = {{"+", "ADD"}, {"-", "SUB"}, {"E", "AND"}, {"OU", "OR"}, {"OUX", "XOR"},
                                                                         {"NOU", "NOR"}, {"<", "SLT"}, {"<u", "SLTU"}, {"<<", "SLL"}, {">>", "SRL"},
                                                                         {">>\u00b1", "SRA"}, {"<<16", "LUI"}};
            for (const auto& [symbol, mnemonic] : kNames) {
                if (t.aluOperation == symbol) return std::string("OpULA ") + mnemonic;
            }
            return "OpULA";
        }
        case kW32UseImm: return t.aluTakesImmediate ? "UsaImm 1" : "UsaImm 0";
        case kW32RegWrite: return "RegEsc 1";
        case kW32RegDst: {
            static constexpr const char* kWhich[] = {"rt", "rd", "$ra", "$v0"};
            return std::string("RegDest ") + kWhich[t.destination & 3u];
        }
        case kW32SignExt: return t.signExtends ? "SinalImm 1" : "SinalImm 0";
        case kW32MemWrite: return "MemEsc 1";
        case kW32Back: {
            switch (t.back) {
                case Trace32::Back::Alu: return i18n::tr("escolhe ULA");
                case Trace32::Back::Memory: return i18n::tr("escolhe RAM");
                case Trace32::Back::Link: return i18n::tr("escolhe PC+4");
                case Trace32::Back::Switches: return i18n::tr("escolhe chaves");
                case Trace32::Back::None: break;
            }
            return "";
        }
        case kW32SystemV0: return "$v0=" + decimal32(t.valueA);
        case kW32SystemA0: return "$a0=" + decimal32(t.valueB);
        default: return "";
    }
}

const char* instructionKind32(const Trace32& t) {
    if (t.syscall) return "syscall";
    if (t.readsMemory || t.writesMemory) return "memória";
    if (t.branches) return "desvio";
    if (t.next == Trace32::Next::Jump || t.next == Trace32::Next::Register) return "salto";
    if (t.format == 'R') return "tipo R";
    return "tipo I";
}

void drawXray32Plan(SDL_Renderer* renderer, const Trace32& t, bool running, const SDL_FRect& plan, int stage, float progress) {
    const Color tone = instructionTone32(t.word);
    fillRound(renderer, plan, 10, kPanel);
    const float scale = zoomedScale(plan, 1000.0f, 470.0f);
    const Vec2 origin = zoomedOrigin(plan, 1000.0f, 470.0f, scale);
    const SDL_Rect clip{static_cast<int>(plan.x), static_cast<int>(plan.y), static_cast<int>(plan.w), static_cast<int>(plan.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const auto at = [&](Vec2 p) { return Vec2{origin.x + p.x * scale, origin.y + p.y * scale}; };
    const float text = std::clamp(12.5f * scale, 9.5f, 15.0f * gZoom.zoom);
    const float tagSize = std::clamp(11.0f * scale, 8.5f, 13.0f * gZoom.zoom);
    const float travel = std::clamp(progress / 1.1f, 0.0f, 1.0f);

    Mesh unused;
    Mesh waiting;
    Mesh lit;
    struct Packet {
        Vec2 where;
        std::string text;
        bool moving;
        bool now;
        Color color;
    };
    std::vector<Packet> packets;
    for (const X32WireShape& wire : x32Wires()) {
        const bool used = running && x32WireUsed(t, wire.wire);
        const bool on = used && wire.stage <= stage;
        Mesh& mesh = !used ? unused : on ? lit : waiting;
        std::vector<Vec2> points;
        float length = 0.0f;
        for (const Vec2 p : wire.points) points.push_back(at(p));
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            length += std::hypot(points[i + 1].x - points[i].x, points[i + 1].y - points[i].y);
            mesh.addLine(points[i], points[i + 1], on ? 2.6f : 1.3f, on ? (x32ControlLine(wire.wire) ? kToneControl : tone) : kBodyEdge, !used ? 0.5f : on ? 1.0f : 0.9f);
        }
        if (!on) continue;
        const std::string value = x32WireValue(t, wire.wire);
        if (value.empty()) continue;
        const float half = (textWidth(renderer, Face::Mono, tagSize, value) + 10.0f) / 2.0f;
        if (half * 2.0f > length && wire.wire == kW32RomDecoder) {
            const Vec2 middle = alongPath(points, length / 2.0f);
            const bool now = wire.stage == stage;
            packets.push_back({{middle.x, middle.y - tagSize * 1.7f}, value, now && travel < 1.0f, now, tone});
            continue;
        }
        if (half * 2.0f > length) continue;
        const float parked = std::max(0.0f, length - half - 5.0f);
        const bool now = wire.stage == stage;
        const bool moving = now && travel < 1.0f;
        packets.push_back({alongPath(points, moving ? parked * travel : parked), value, moving, now, x32ControlLine(wire.wire) ? kToneControl : tone});
    }
    unused.flush(renderer);
    waiting.flush(renderer);
    lit.flush(renderer);

    for (const X32BlockShape& shape : kX32Blocks) {
        const bool used = running && x32BlockUsed(t, shape.block);
        const bool on = used && shape.stage <= stage;
        const bool now = on && shape.stage == stage;
        const Vec2 corner = at({shape.rect.x, shape.rect.y});
        const SDL_FRect r{corner.x, corner.y, shape.rect.w * scale, shape.rect.h * scale};
        const Color fill = on ? mixed(kPanelRaised, tone, now ? 0.34f : 0.2f) : used ? kPanelRaised : kBackground;
        const Color edge = on ? tone : used ? kBodyEdge : kRule;
        const float frame = now ? 2.4f : on ? 1.4f : 1.0f;
        const Color ink = on ? kText : used ? kTextDim : kTextOff;
        Mesh body;
        std::vector<Vec2> outline;
        switch (shape.shape) {
            case X32Shape::Mux: {
                const float dip = std::min(10.0f * scale, r.h * 0.2f);
                outline = {{r.x, r.y}, {r.x + r.w, r.y + dip}, {r.x + r.w, r.y + r.h - dip}, {r.x, r.y + r.h}};
                break;
            }
            case X32Shape::MuxDown: {
                const float in = 14.0f * scale;
                outline = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w - in, r.y + r.h}, {r.x + in, r.y + r.h}};
                break;
            }
            case X32Shape::Adder:
            case X32Shape::Alu:
                outline = {{r.x, r.y}, {r.x + r.w, r.y + r.h * 0.28f}, {r.x + r.w, r.y + r.h * 0.72f}, {r.x, r.y + r.h},
                           {r.x, r.y + r.h * 0.6f}, {r.x + r.w * 0.22f, r.y + r.h * 0.5f}, {r.x, r.y + r.h * 0.4f}};
                break;
            case X32Shape::Box: break;
        }
        if (outline.empty()) {
            fillRound(renderer, r, 7, fill);
            frameRound(renderer, r, 7, frame, edge);
        } else {
            body.addFan(outline, {r.x + r.w * 0.55f, r.y + r.h * 0.5f}, fill);
            addLoop(body, outline, frame, edge);
            body.flush(renderer);
        }
        const float small = std::clamp(10.0f * scale, 8.5f, 11.5f);
        const auto centred = [&](Face face, float size, float y, const std::string& line, Color color, float cx) {
            write(renderer, face, size, cx - textWidth(renderer, face, size, line) / 2.0f, y, line, color);
        };
        switch (shape.shape) {
            case X32Shape::Mux: {
                const float tiny = std::clamp(8.5f * scale, 7.5f, 10.0f);
                centred(Face::Medium, tiny, r.y + r.h / 2.0f + tiny * 0.4f, "MUX", ink, r.x + r.w / 2.0f);
                const char* what = shape.block == kB32Dst ? "destino" : shape.block == kB32MuxB ? "B" : "volta";
                centred(Face::Regular, small, r.y - 5.0f * scale, i18n::tr(what), kTextDim, r.x + r.w / 2.0f);
                break;
            }
            case X32Shape::MuxDown:
                centred(Face::Medium, small, r.y + r.h * 0.42f, "MUX", ink, r.x + r.w / 2.0f);
                centred(Face::Medium, small, r.y + r.h * 0.42f + small * 1.3f, i18n::tr(shape.name), ink, r.x + r.w / 2.0f);
                break;
            case X32Shape::Adder: {
                const float size = std::clamp(14.0f * scale, 10.0f, 18.0f);
                centred(Face::Medium, size, r.y + r.h * 0.5f + size * 0.36f, shape.block == kB32Add4 ? "+4" : "+", on ? tone : ink, r.x + r.w * 0.6f);
                centred(Face::Medium, small, r.y + r.h + small * 1.35f, i18n::tr(shape.name), ink, r.x + r.w * 0.5f);
                if (shape.block == kB32Branch) centred(Face::Mono, small * 0.95f, r.y + r.h + small * 2.6f, shape.detail, kTextDim, r.x + r.w * 0.5f);
                break;
            }
            case X32Shape::Alu: {
                const float size = std::clamp(18.0f * scale, 12.0f, 22.0f);
                centred(Face::Medium, text, r.y + r.h * 0.5f - size * 0.9f, i18n::tr(shape.name), ink, r.x + r.w * 0.6f);
                if (on) centred(Face::Medium, size, r.y + r.h * 0.5f + size * 0.36f, i18n::tr(t.aluOperation.c_str()), tone, r.x + r.w * 0.6f);
                if (on) centred(Face::Mono, small, r.y + r.h * 0.5f + size * 1.5f, decimal32(t.aluResult), kText, r.x + r.w * 0.6f);
                break;
            }
            case X32Shape::Box: {
                if (shape.block == kB32Decoder && on) {  // the word is only cut into fields in Decodifica
                    const float x = r.x + 8.0f * scale;
                    write(renderer, Face::Medium, text, x, r.y + 18.0f * scale, i18n::tr(shape.name), ink);
                    std::vector<std::string> fields{"op " + std::to_string(t.op)};
                    if (t.format == 'J') {
                        fields.push_back(i18n::tr("alvo") + std::string(" ") + shortHex(t.word & 0x03FFFFFFu));
                    } else {
                        fields.push_back("rs " + std::to_string(t.rs));
                        fields.push_back("rt " + std::to_string(t.rt));
                        if (t.format == 'R') {
                            fields.push_back("rd " + std::to_string(t.rd));
                            fields.push_back("shamt " + std::to_string(t.shamt));
                            fields.push_back("funct " + std::to_string(t.funct));
                        } else {
                            fields.push_back("imm " + immediate32(t.imm));
                        }
                    }
                    float y = r.y + 72.0f * scale;
                    for (const std::string& field : fields) {
                        write(renderer, Face::Mono, std::clamp(11.0f * scale, 8.5f, 12.5f), x, y, field, ink);
                        y += 17.0f * scale;
                    }
                    break;
                }
                const bool tall = r.h > 60.0f * scale;
                const float nameY = tall ? r.y + r.h / 2.0f - 4.0f * scale : r.y + r.h / 2.0f + text * 0.36f;
                centred(Face::Medium, text, nameY, i18n::tr(shape.name), ink, r.x + r.w / 2.0f);
                if (shape.detail[0] != '\0' && tall) centred(Face::Regular, text * 0.9f, nameY + text * 1.3f, i18n::tr(shape.detail), kTextDim, r.x + r.w / 2.0f);
                if (shape.block == kB32Registers) {
                    write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 180}).y, i18n::tr("lê A"), kTextDim);
                    write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 208}).y, i18n::tr("lê B"), kTextDim);
                    write(renderer, Face::Regular, small, r.x + 5.0f * scale, at({0, 268}).y, i18n::tr("escreve"), kTextDim);
                    const float a = textWidth(renderer, Face::Regular, small, "A");
                    write(renderer, Face::Regular, small, r.x + r.w - a - 5.0f * scale, at({0, 184}).y, "A", kTextDim);
                    write(renderer, Face::Regular, small, r.x + r.w - a - 5.0f * scale, at({0, 244}).y, "B", kTextDim);
                }
                if (shape.block == kB32Control && on) {
                    std::string signals;
                    const auto add = [&](const char* signal) { signals += (signals.empty() ? "" : " · ") + std::string(i18n::tr(signal)); };
                    if (t.back != Trace32::Back::None) add("escreve reg");
                    if (t.aluTakesImmediate) add("usa imm");
                    if (t.usesImmediate && t.signExtends) add("com sinal");
                    if (t.readsMemory) add("lê RAM");
                    if (t.writesMemory) add("grava RAM");
                    if (t.branches) add(t.op == mips::kOpBeq ? "se igual" : "se diferente");
                    if (t.next == Trace32::Next::Jump) add("salta");
                    if (t.next == Trace32::Next::Register) add("salta p/ reg");
                    if (t.syscall) add("sistema");
                    if (t.invalid) add("inválida");
                    write(renderer, Face::Regular, small, r.x + 6.0f * scale, r.y + r.h - 8.0f * scale,
                          fitted(renderer, Face::Regular, small, signals.empty() ? i18n::tr("nada") : signals, r.w - 12.0f * scale), on ? tone : kTextDim);
                    write(renderer, Face::Medium, small, r.x + 6.0f * scale, r.y + 14.0f * scale, t.name + ":", tone);  // the row the op picked
                }
                break;
            }
        }
    }

    for (const Packet& packet : packets) {
        const float w = textWidth(renderer, Face::Mono, tagSize, packet.text) + 10.0f;
        const SDL_FRect box{packet.where.x - w / 2.0f, packet.where.y - tagSize * 0.8f, w, tagSize * 1.6f};
        fillRound(renderer, box, 5, packet.moving ? packet.color : packet.now ? mixed(kPanelRaised, packet.color, 0.35f) : kPanelRaised);
        frameRound(renderer, box, 5, packet.now ? 1.6f : 1.0f, packet.color);
        write(renderer, Face::Mono, tagSize, box.x + 5.0f, packet.where.y + tagSize * 0.36f, packet.text, packet.moving ? kBackground : kText);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

}  // namespace

void drawXray32(SDL_Renderer* renderer, const Computer32State& state, const SDL_FRect& area, int stage, float progress, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const Trace32 trace = traceInstruction32(state);
    const bool running = !state.halted || trace.halts;  // the syscall 10 itself still shows its way
    const Color tone = instructionTone32(trace.word);

    const SDL_FRect next = panelNextButton(area);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Step, {next.x + 20.0f, next.y + next.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, next.x + 36.0f, next.y + 21.0f, "Próxima instrução", kText);
    const float titleX = next.x + next.w + 18.0f;
    std::string source = trace.text;
    if (state.index < state.lineOfWord.size() && !state.source.empty()) {
        int line = state.lineOfWord[state.index];
        std::size_t start = 0;
        while (--line > 0 && start != std::string::npos) {
            start = state.source.find('\n', start);
            if (start != std::string::npos) ++start;
        }
        if (start != std::string::npos) {
            std::string text = state.source.substr(start, state.source.find('\n', start) - start);
            const std::size_t colon = text.find(':');
            const std::size_t comment = text.find('#');
            if (colon != std::string::npos && (comment == std::string::npos || colon < comment)) text = text.substr(colon + 1);
            const std::size_t first = text.find_first_not_of(" \t");
            if (first != std::string::npos) source = text.substr(first, text.find_last_not_of(" \t") - first + 1);
        }
    }
    const std::string title = running ? i18n::format("Endereço {}:  {}", {"0x" + hex32(state.pc), source}) : std::string(i18n::tr("O computador parou. Zerar começa de novo."));
    const char* kind = i18n::tr(instructionKind32(trace));
    const float kindWidth = running ? textWidth(renderer, Face::Medium, 12.5f, kind) + 22.0f : 0.0f;
    const std::string shown = fitted(renderer, Face::Medium, 16, title, area.x + area.w - titleX - 30.0f - kindWidth);
    write(renderer, Face::Medium, 16, titleX, next.y + 23.0f, shown, kText);
    if (running) {
        const float x = titleX + textWidth(renderer, Face::Medium, 16, shown) + 14.0f;
        const SDL_FRect chip{x, next.y + 6.0f, kindWidth, 22.0f};
        fillRound(renderer, chip, 11, mixed(kPanelRaised, tone, 0.3f));
        frameRound(renderer, chip, 11, 1.2f, tone);
        write(renderer, Face::Medium, 12.5f, chip.x + 11.0f, chip.y + 15.5f, kind, kText);
    }
    for (int s = 0; s < kTraceStages; ++s) {
        const SDL_FRect r = xrayStageRect(area, s);
        const bool now = running && s == stage;
        const bool done = running && s < stage;
        fillRound(renderer, r, 7, now ? mixed(kPanelRaised, tone, 0.45f) : inside(r, mouseX, mouseY) ? kPanelHover : done ? mixed(kPanelRaised, tone, 0.18f) : kPanelButton);
        if (now) frameRound(renderer, r, 7, 1.4f, tone);
        char label[32];
        std::snprintf(label, sizeof label, "%d  %s", s + 1, i18n::tr(kTraceStageNames[s]));
        const float w = textWidth(renderer, Face::Medium, 12.5f, label);
        write(renderer, Face::Medium, 12.5f, r.x + (r.w - w) / 2.0f, r.y + 19.0f, label, now || done ? kText : kTextDim);
    }
    const XrayParts parts = xrayParts(area);
    if (running) {
        drawStepColumn(renderer, parts.side, stage, explainStage32(trace, stage, gZoom.hex), tone);
        drawWordStrip32(renderer, trace.word, {parts.strip.x, parts.strip.y + 6.0f, std::min(parts.strip.w, 760.0f), 18.0f}, stage >= 1);
    }
    const std::vector<std::string> legend =
        wrapped(renderer, Face::Regular, 12.5f,
                "Siglas: op = o código do tipo · rs, rt = os registradores lidos · rd = o que recebe o resultado (tipo R) · shamt = quantas "
                "casas deslocar · funct = qual conta, no tipo R · imm = o imediato, 16 bits dentro da instrução · alvo = 26 bits do salto",
                parts.legend.w);
    float y = parts.legend.y + 18.0f;
    for (std::size_t i = 0; i < legend.size() && i < 3; ++i, y += 17.0f) write(renderer, Face::Regular, 12.5f, parts.legend.x, y, legend[i], kTextDim);
    drawXray32Plan(renderer, trace, running, parts.plan, stage, progress);
    drawZoomButtons(renderer, parts.plan, mouseX, mouseY);
}

namespace {

DebugLayout debugLayout32(const Computer32State& state, const SDL_FRect& area) {
    DebugLayout layout;
    const float gap = 6.0f;
    const float w = (area.w - 24.0f - static_cast<float>(kDebugButtonCount - 1) * gap) / static_cast<float>(kDebugButtonCount);
    for (int i = 0; i < kDebugButtonCount; ++i) layout.buttons[i] = {area.x + 12.0f + static_cast<float>(i) * (w + gap), area.y + 40.0f, w, 46.0f};
    layout.memory = {area.x + 8.0f, area.y + area.h - 8.0f - 96.0f, area.w - 16.0f, 96.0f};
    layout.registers = {area.x + 8.0f, layout.memory.y - 8.0f - 150.0f, area.w - 16.0f, 150.0f};
    layout.program = {area.x + 8.0f, area.y + 96.0f, area.w - 16.0f, layout.registers.y - 8.0f - (area.y + 96.0f)};
    layout.rows = std::max(1, static_cast<int>((layout.program.h - 30.0f) / kDebugRow));
    if (!state.source.empty()) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= state.source.size()) {
            const std::size_t end = state.source.find('\n', start);
            lines.push_back(state.source.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        std::vector<int> addressOfLine(lines.size(), -1);
        for (std::size_t address = 0; address < state.lineOfWord.size(); ++address) {
            const int line = state.lineOfWord[address] - 1;
            if (line >= 0 && line < static_cast<int>(lines.size()) && addressOfLine[static_cast<std::size_t>(line)] < 0) {
                addressOfLine[static_cast<std::size_t>(line)] = static_cast<int>(address);
            }
        }
        int current = state.index < state.lineOfWord.size() ? state.lineOfWord[state.index] - 1 : -1;
        const auto width = static_cast<std::size_t>(std::max(0.0f, (layout.program.w - 60.0f - 18.0f) / 6.4f));
        for (std::size_t line = 0; line < lines.size(); ++line) {
            if (static_cast<int>(line) == current) layout.current = static_cast<int>(layout.lines.size());
            const std::vector<std::string> pieces = wrapCodeLine(lines[line], width);
            for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                layout.lines.push_back(pieces[piece]);
                layout.addressOfLine.push_back(piece == 0 ? addressOfLine[line] : -1);
                layout.sourceOfLine.push_back(static_cast<int>(line));
            }
        }
        layout.currentSource = current;
    } else {
        const int from = std::max(0, static_cast<int>(state.index) - layout.rows / 3);
        for (int address = from; address < from + layout.rows; ++address) {
            layout.lines.push_back(address == static_cast<int>(state.index) ? state.text : "");
            layout.addressOfLine.push_back(address);
            layout.sourceOfLine.push_back(address - from);
        }
        layout.current = static_cast<int>(state.index) - from;
        layout.currentSource = layout.current;
    }
    layout.first = std::clamp(layout.current - layout.rows / 3, 0, std::max(0, static_cast<int>(layout.lines.size()) - layout.rows));
    return layout;
}

}  // namespace

DebugHit debugger32At(const Computer32State& state, const SDL_FRect& area, float x, float y) {
    const DebugLayout layout = debugLayout32(state, area);
    for (int i = 0; i < kDebugButtonCount; ++i) {
        if (inside(layout.buttons[i], x, y)) return {kDebugActions[i], 0};
    }
    if (inside(layout.program, x, y)) {
        const int row = static_cast<int>((y - layout.program.y - 28.0f) / kDebugRow);
        const int line = layout.first + row;
        if (row >= 0 && line < static_cast<int>(layout.lines.size()) && layout.addressOfLine[static_cast<std::size_t>(line)] >= 0) {
            return {DebugAction::Breakpoint, static_cast<std::uint16_t>(layout.addressOfLine[static_cast<std::size_t>(line)])};
        }
    }
    return {};
}

void drawDebugger32(SDL_Renderer* renderer, const Computer32State& state, const DebugView& view, const std::string& console, const SDL_FRect& area,
                    float mouseX, float mouseY) {
    const DebugLayout layout = debugLayout32(state, area);
    const Color tone = instructionTone32(state.instruction);
    fillRect(renderer, area, kPanel);
    fillRect(renderer, {area.x + area.w - 1.0f, area.y, 1.0f, area.h}, kRule);
    write(renderer, Face::Medium, 15, area.x + 14.0f, area.y + 26.0f, "Depurador", kText);
    const char* status = state.invalid ? "Parado: instrução inválida" : state.halted ? "Terminou" : view.running ? "Rodando" : "Pausado";
    const float statusWidth = textWidth(renderer, Face::Medium, 12.5f, status);
    write(renderer, Face::Medium, 12.5f, area.x + area.w - 14.0f - statusWidth, area.y + 26.0f, status,
          state.halted ? kWarning : view.running ? kKnobOn : kTextDim);

    constexpr Icon kIcons[] = {Icon::Play, Icon::Step, Icon::ChevronRight, Icon::Reset};
    for (int i = 0; i < kDebugButtonCount; ++i) {
        const SDL_FRect r = layout.buttons[i];
        const bool pauses = i == 0 && view.running;
        const Icon icon = pauses ? Icon::Pause : i == 0 && state.halted ? Icon::Reset : kIcons[i];
        const char* label = pauses ? "Pausar" : i == 0 && state.halted ? "De novo" : kDebugButtons[i];
        fillRound(renderer, r, 8, i == 0 ? (inside(r, mouseX, mouseY) ? kWirePreview : kPanelActive) : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
        const Color ink = i == 0 && inside(r, mouseX, mouseY) ? kBackground : kText;
        drawIcon(renderer, icon, {r.x + r.w / 2.0f, r.y + 16.0f}, 16, ink);
        const float w = textWidth(renderer, Face::Regular, 11, label);
        write(renderer, Face::Regular, 11, r.x + (r.w - w) / 2.0f, r.y + 38.0f, label, i == 0 ? ink : kTextDim);
    }

    const SDL_FRect program = layout.program;
    fillRound(renderer, program, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, program.x + 10.0f, program.y + 18.0f, "Programa", kTextDim);
    write(renderer, Face::Regular, 11, program.x + 76.0f, program.y + 18.0f, "número = endereço (0x0040....)", kTextOff);
    const SDL_Rect clip{static_cast<int>(program.x), static_cast<int>(program.y), static_cast<int>(program.w), static_cast<int>(program.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const std::vector<std::uint16_t> none;
    const std::vector<std::uint16_t>& breakpoints = view.breakpoints != nullptr ? *view.breakpoints : none;
    for (int row = 0; row < layout.rows; ++row) {
        const int line = layout.first + row;
        if (line >= static_cast<int>(layout.lines.size())) break;
        const float y = program.y + 28.0f + static_cast<float>(row) * kDebugRow;
        const int address = layout.addressOfLine[static_cast<std::size_t>(line)];
        const bool here = layout.sourceOfLine[static_cast<std::size_t>(line)] == layout.currentSource && layout.currentSource >= 0 && !state.halted;
        if (here) fillRound(renderer, {program.x + 4.0f, y, program.w - 8.0f, kDebugRow}, 4, mixed(kBackground, tone, 0.35f));
        if (address >= 0 && inside({program.x, y, program.w, kDebugRow}, mouseX, mouseY) && !here) {
            fillRound(renderer, {program.x + 4.0f, y, program.w - 8.0f, kDebugRow}, 4, kPanelRaised);
        }
        if (address >= 0 && std::find(breakpoints.begin(), breakpoints.end(), static_cast<std::uint16_t>(address)) != breakpoints.end()) {
            Mesh dot;
            dot.addDisc({program.x + 10.0f, y + kDebugRow / 2.0f}, 5.0f, kWireConflict);
            dot.flush(renderer);
        }
        if (line == layout.current && !state.halted) {
            Mesh arrow;
            const Vec2 tip{program.x + 22.0f, y + kDebugRow / 2.0f};
            arrow.addFan({{tip.x - 7.0f, tip.y - 5.0f}, tip, {tip.x - 7.0f, tip.y + 5.0f}}, {tip.x - 4.0f, tip.y}, tone);
            arrow.flush(renderer);
        }
        if (address >= 0) {
            const std::string number = hex32(mips::kTextBase + static_cast<std::uint32_t>(4 * address)).substr(4);
            const float numberWidth = textWidth(renderer, Face::Mono, 11.0f, number);
            write(renderer, Face::Mono, 11.0f, program.x + 56.0f - numberWidth, y + 13.0f, number, here ? kText : kWirePreview);
        }
        drawCodeLine(renderer, layout.lines[static_cast<std::size_t>(line)], program.x + 62.0f, y + 13.0f, 12.5f, 6.4f);
    }
    SDL_SetRenderClipRect(renderer, nullptr);

    // The registers a program uses most, and the PC; the Panel shows all 32.
    const SDL_FRect registers = layout.registers;
    fillRound(renderer, registers, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, registers.x + 10.0f, registers.y + 18.0f, "Registradores", kTextDim);
    static constexpr unsigned kShown[] = {2, 4, 8, 9, 10, 11, 16, 17, 29, 31};
    const float column = (registers.w - 20.0f) / 2.0f;
    const auto cell = [&](float x, float y, const std::string& name, std::uint32_t value, bool changed) {
        if (changed) fillRound(renderer, {x - 4.0f, y, column - 6.0f, kDebugRow}, 4, Color{92, 84, 30});
        write(renderer, Face::Medium, 12, x, y + 13.0f, name, kText);
        write(renderer, Face::Mono, 11.5f, x + 40.0f, y + 13.0f, hex32(value), value != 0 ? kKnobOn : kTextOff);
    };
    for (std::size_t i = 0; i < std::size(kShown); ++i) {
        const unsigned r = kShown[i];
        const float x = registers.x + 14.0f + static_cast<float>(i % 2) * column;
        const float y = registers.y + 26.0f + static_cast<float>(i / 2) * kDebugRow;
        cell(x, y, std::string(mips::registerName(r)), state.registers[r], view.hasBefore && view.before32[r] != state.registers[r]);
    }
    cell(registers.x + 14.0f, registers.y + 26.0f + 5.0f * kDebugRow + 4.0f, "PC", state.pc, false);

    const SDL_FRect bottom = layout.memory;
    fillRound(renderer, bottom, 8, kBackground);
    write(renderer, Face::Medium, 12.5f, bottom.x + 10.0f, bottom.y + 18.0f, "Console", kTextDim);
    drawConsole(renderer, console, {bottom.x + 10.0f, bottom.y + 26.0f, bottom.w - 20.0f, bottom.h - 30.0f});
}

// --- A lesson of questions ---------------------------------------------------------------------------

SDL_FRect quizArea(float rightEdge, int height) {
    const float top = kToolbarHeight + kLessonStripHeight;
    return {kPanelWidth, top, rightEdge - kPanelWidth, static_cast<float>(height) - top - kStatusHeight};
}

namespace {

// How far down the card the answer goes: under the lights when the question shows bits, else right under it.
float quizAnswerTop(const editor::Question* question) {
    return question != nullptr && std::string_view(question->bits).find_first_of("01") != std::string_view::npos ? 228.0f : 124.0f;
}

SDL_FRect quizCard(const SDL_FRect& area, const editor::Question* question) {
    const float w = std::min(760.0f, area.w - 40.0f);
    const float h = std::min(quizAnswerTop(question) + 172.0f, area.h - 30.0f);
    return {area.x + (area.w - w) / 2.0f, area.y + std::max(14.0f, (area.h - h) / 2.0f), w, h};
}

}  // namespace

SDL_FRect quizButtonRect(const SDL_FRect& area, const editor::Question* question) {
    const SDL_FRect card = quizCard(area, question);
    return {card.x + card.w - 190.0f, card.y + quizAnswerTop(question) + 10.0f, 166.0f, 40.0f};
}

void drawQuiz(SDL_Renderer* renderer, const SDL_FRect& area, const QuizView& quiz, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const SDL_FRect card = quizCard(area, quiz.question);
    const float answerTop = quizAnswerTop(quiz.question);
    fillRound(renderer, card, 14, kPanel);
    frameRound(renderer, card, 14, 1.0f, kRule);
    const float x = card.x + 24.0f;
    if (quiz.question == nullptr) {
        write(renderer, Face::Medium, 22, x, card.y + 60.0f, "Todas as perguntas certas!", kKnobOn);
        write(renderer, Face::Regular, 15, x, card.y + 92.0f, "O exercício está feito. Próximo segue para o que vem depois.", kTextDim);
        return;
    }
    const editor::Question& q = *quiz.question;

    // How far along: one dot per question.
    write(renderer, Face::Medium, 13, x, card.y + 30.0f, i18n::format("Pergunta {} de {}", {std::to_string(quiz.number), std::to_string(quiz.count)}), kTextDim);
    Mesh dots;
    for (int i = 0; i < quiz.count; ++i) {
        const Color color = i + 1 < quiz.number || (i + 1 == quiz.number && quiz.right) ? kKnobOn : i + 1 == quiz.number ? kWirePreview : kRule;
        dots.addDisc({card.x + card.w - 24.0f - static_cast<float>(quiz.count - 1 - i) * 16.0f, card.y + 25.0f}, 5.0f, color);
    }
    dots.flush(renderer);
    float y = card.y + 64.0f;
    for (const std::string& line : wrapped(renderer, Face::Medium, 19, i18n::tr(q.prompt), card.w - 48.0f)) {
        write(renderer, Face::Medium, 19, x, y, line, kText);
        y += 26.0f;
    }

    // The bits: a light each, its digit above and what it's worth below, in groups of four.
    std::string bits;
    for (const char c : std::string(q.bits)) {
        if (c == '0' || c == '1' || c == ' ') bits += c;
    }
    int count = 0;
    for (const char c : bits) count += c == ' ' ? 0 : 1;
    if (count > 0) {
        const float step = 54.0f;
        const float gap = 22.0f;
        int groups = 0;
        for (const char c : bits) groups += c == ' ' ? 1 : 0;
        float bx = x + 10.0f;
        const float lightY = y + 44.0f;
        int position = count - 1;
        Mesh lights;
        for (const char c : bits) {
            if (c == ' ') {
                bx += gap;
                continue;
            }
            const bool on = c == '1';
            const Vec2 centre{bx + step / 2.0f, lightY};
            if (on) lights.addDisc(centre, 21.0f, mixed(kBackground, kWireConflict, 0.35f));
            lights.addDisc(centre, 15.0f, on ? kWireConflict : mixed(kBackground, kBodyEdge, 0.6f));
            lights.flush(renderer);
            const char digit[2] = {c, '\0'};
            write(renderer, Face::Mono, 15, centre.x - textWidth(renderer, Face::Mono, 15, digit) / 2.0f, lightY - 26.0f, digit, on ? kText : kTextDim);
            const std::string weight = std::to_string(std::uint64_t{1} << position);
            write(renderer, Face::Regular, 13, centre.x - textWidth(renderer, Face::Regular, 13, weight) / 2.0f, lightY + 38.0f, weight, on ? kWarning : kTextOff);
            bx += step;
            --position;
        }
        (void)groups;
        y = lightY + 52.0f;
    } else {
        y += 20.0f;
    }

    // The answer: what's been typed, in a box, and the button.
    const char* how = q.base == 2 ? "em binário" : q.base == 16 ? "em hexadecimal" : "em decimal";
    write(renderer, Face::Regular, 13, x, card.y + answerTop, i18n::format("Resposta, {}:", {i18n::tr(how)}), kTextDim);
    const SDL_FRect box{x, card.y + answerTop + 10.0f, card.w - 48.0f - 190.0f, 40.0f};
    fillRound(renderer, box, 8, kBackground);
    frameRound(renderer, box, 8, 1.4f, quiz.right ? kKnobOn : quiz.wrong ? kWireConflict : kWirePreview);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    const std::string shown = quiz.typed + (quiz.right || !blink ? "" : "_");
    write(renderer, Face::Mono, 20, box.x + 14.0f, box.y + 28.0f, shown, kText);
    const SDL_FRect button = quizButtonRect(area, quiz.question);
    const bool hovered = inside(button, mouseX, mouseY);
    fillRound(renderer, button, 8, hovered ? kWirePreview : kPanelActive);
    const char* label = quiz.right ? "Próxima" : "Responder";
    const float w = textWidth(renderer, Face::Medium, kUiText, label);
    write(renderer, Face::Medium, kUiText, button.x + (button.w - w) / 2.0f, button.y + 25.0f, label, hovered ? kBackground : kText);

    // Right, with why; or not yet.
    float fy = card.y + answerTop + 84.0f;
    if (quiz.right) {
        const std::string line = std::string(i18n::tr("Certo!")) + " " + i18n::tr(q.explain);
        for (const std::string& piece : wrapped(renderer, Face::Regular, 15, line, card.w - 48.0f)) {
            write(renderer, Face::Regular, 15, x, fy, piece, kKnobOn);
            fy += 21.0f;
        }
        write(renderer, Face::Regular, 12.5f, x, card.y + card.h - 16.0f, "Enter: próxima pergunta", kTextOff);
    } else if (quiz.badDigit) {
        const char* only = q.base == 2 ? "Em binário só existem os dígitos 0 e 1." : q.base == 10 ? "Em decimal, os dígitos vão de 0 a 9." : "Em hexadecimal, os dígitos vão de 0 a 9 e de A a F.";
        write(renderer, Face::Regular, 15, x, fy, only, kWarning);
    } else if (quiz.wrong) {
        write(renderer, Face::Regular, 15, x, fy, "Ainda não. Tente de novo: a Dica, lá em cima, mostra o caminho.", kWireConflict);
    } else {
        write(renderer, Face::Regular, 12.5f, x, card.y + card.h - 16.0f, "Digite a resposta e aperte Enter. Backspace apaga.", kTextOff);
    }
}

// In a lesson of questions the library has nothing to offer: its place lists the questions, the ones
// answered ticked and the one being asked marked.
void drawQuizList(SDL_Renderer* renderer, const Lesson& lesson, const QuizView& quiz, int height) {
    write(renderer, Face::Medium, 12.0f, 14.0f, 80.0f, "Perguntas", kTextDim);
    float y = 96.0f;
    const float room = kPanelWidth - 56.0f;
    for (std::size_t i = 0; i < lesson.questions.size() && y + 40.0f < static_cast<float>(height) - 60.0f; ++i) {
        const int number = static_cast<int>(i) + 1;
        const bool done = quiz.question == nullptr || number < quiz.number || (number == quiz.number && quiz.right);
        const bool current = quiz.question != nullptr && number == quiz.number;
        const SDL_FRect row{8.0f, y, kPanelWidth - 16.0f, 50.0f};
        if (current) fillRound(renderer, row, 8, kPanelRaised);
        const Vec2 badge{24.0f, y + 18.0f};
        Mesh disc;
        if (current) disc.addDisc(badge, 11.0f, kWirePreview);
        disc.addDisc(badge, current ? 9.5f : 10.0f, done ? kPanelActive : kPanelButton);
        disc.flush(renderer);
        if (done) {
            drawCheckMark(renderer, badge, 11.0f, kKnobOn);
        } else {
            const std::string digits = std::to_string(number);
            write(renderer, Face::Medium, 11.5f, badge.x - textWidth(renderer, Face::Medium, 11.5f, digits) / 2.0f, badge.y + 4.0f, digits,
                  current ? kText : kTextDim);
        }
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 12.0f, i18n::tr(lesson.questions[i].prompt), room);
        for (std::size_t l = 0; l < lines.size() && l < 2; ++l) {
            const std::string shown = l == 1 && lines.size() > 2 ? fitted(renderer, Face::Regular, 12.0f, lines[l] + " …", room) : lines[l];
            write(renderer, Face::Regular, 12.0f, 44.0f, y + 18.0f + 16.0f * static_cast<float>(l), shown, current ? kText : done ? kTextDim : kTextOff);
        }
        y += 56.0f;
    }
}

void drawInterface(SDL_Renderer* renderer, const Session& session, const PaletteView& palette, int width, int height,
                   const InterfaceText& text) {
    gZoom = text.zoom;
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    const bool table = text.showTable && text.view == kViewBoard;
    const float rightEdge = table ? w - rightPanelWidth(session) : w;
    // The panel and the X-ray start after the debugger, which takes the library's place on the left.
    const float about = programAboutHeight(text.example, text.view);
    if (about > 0.0f) {
        const GalleryItem& item = galleryItems()[static_cast<std::size_t>(text.example->item)];
        const SDL_FRect bar{kDebugWidth, kToolbarHeight, rightEdge - kDebugWidth, about};
        fillRect(renderer, bar, kPanel);
        fillRect(renderer, {bar.x, bar.y + bar.h - 1.0f, bar.w, 1.0f}, kRule);
        const std::string line = i18n::format("{}: {}", {i18n::tr(item.title), i18n::tr(item.about)});
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 13, line, bar.w - 28.0f);
        for (std::size_t i = 0; i < lines.size() && i < 3; ++i) {
            const std::string shown = i == 2 && lines.size() > 3 ? fitted(renderer, Face::Regular, 13, lines[i] + " …", bar.w - 28.0f) : lines[i];
            write(renderer, Face::Regular, 13, bar.x + 14.0f, bar.y + 20.0f + 18.0f * static_cast<float>(i), shown, i == 0 ? kText : kTextDim);
        }
    }
    const SDL_FRect computerArea{kDebugWidth, kToolbarHeight + about, rightEdge - kDebugWidth, h - kToolbarHeight - kStatusHeight - about};
    if (text.view == kViewPanel && text.computer != nullptr) drawComputerPanel(renderer, *text.computer, computerArea, text.mouseX, text.mouseY);
    if (text.computer32 != nullptr && text.view == kViewPanel) {
        drawComputer32Panel(renderer, *text.computer32, text.console != nullptr ? *text.console : std::string(), text.debug, computerArea, text.mouseX, text.mouseY);
    }
    if (text.computer32 != nullptr && text.view == kViewXray) {
        drawXray32(renderer, *text.computer32, computerArea, text.xrayStage, text.xrayProgress, text.mouseX, text.mouseY);
    }
    if (text.view == kViewXray && text.computer != nullptr) {
        drawXray(renderer, *text.computer, computerArea, text.xrayStage, text.xrayProgress, text.mouseX, text.mouseY);
    }
    if (text.view == kViewMemory && text.memory != nullptr) drawMemoryView(renderer, *text.memory, computerArea, text.mouseX, text.mouseY);

    fillRect(renderer, {0, 0, kPanelWidth, h}, kPanel);
    fillRect(renderer, {kPanelWidth - 1.0f, 0, 1.0f, h}, kRule);
    // Beside the debugger, the status line starts after it.
    const bool anyComputer = text.computer != nullptr || text.computer32 != nullptr;
    const float statusLeft = text.view != kViewBoard && anyComputer ? kDebugWidth : kPanelWidth;
    fillRect(renderer, {statusLeft, h - kStatusHeight, rightEdge - statusLeft, kStatusHeight}, kPanel);
    fillRect(renderer, {statusLeft, h - kStatusHeight, rightEdge - statusLeft, 1.0f}, kRule);

    write(renderer, Face::Medium, 17, 14, 25, "Bancada", kText);
    std::string projectLine = text.project;
    if (session.currentCircuit() != 0) projectLine = session.currentName();
    if (text.dirty) projectLine += " *";
    write(renderer, Face::Regular, 12.5f, 14, 42, fitted(renderer, Face::Regular, 12.5f, projectLine, kPanelWidth - 26.0f), kTextDim);

    if (text.quiz != nullptr && text.lesson != nullptr && text.view == kViewBoard) {
        drawQuizList(renderer, lessons()[static_cast<std::size_t>(text.lesson->index)], *text.quiz, height);
    } else {
        fillRound(renderer, kSearchBox, 7, kPanelButton);
        if (text.searching) frameRound(renderer, kSearchBox, 7, 1, kWirePreview);
        drawIcon(renderer, Icon::Search, {kSearchBox.x + 16.0f, kSearchBox.y + kSearchBox.h / 2.0f}, 15, kTextDim);
        const std::string searchText = text.searching ? palette.filter + "_" : palette.filter;
        write(renderer, Face::Regular, kUiText, kSearchBox.x + 30.0f, kSearchBox.y + 20.0f, searchText.empty() ? "Buscar peça" : searchText,
              searchText.empty() ? kTextOff : kText);
        drawPalette(renderer, session, paletteGroups(session, palette.filter, text.limit), palette, height, text.mouseX, text.mouseY);
    }
    // Inside a chip: the way back, so that going in is never a place with no way out.
    if (session.currentCircuit() != 0 && text.view == kViewBoard) {
        const std::vector<TrailPiece> pieces = trailPieces(renderer, session, kPanelWidth, boardTopFor(text.lesson != nullptr || text.example != nullptr));
        const std::vector<CircuitEntry> trail = session.trail();
        const SDL_FRect last = pieces.back().rect;
        const std::string here = session.currentName();
        const float hereWidth = textWidth(renderer, Face::Medium, kUiText, here);
        const SDL_FRect bar{pieces.front().rect.x - 6.0f, last.y - 5.0f, last.x + last.w + 18.0f + hereWidth + 16.0f - pieces.front().rect.x + 6.0f, last.h + 10.0f};
        fillRound(renderer, bar, 10, kPanel, 0.95f);
        frameRound(renderer, bar, 10, 1.0f, kBodyEdge);
        for (const TrailPiece& piece : pieces) {
            const bool hovered = inside(piece.rect, text.mouseX, text.mouseY);
            if (piece.index < 0) {
                fillRound(renderer, piece.rect, 7, hovered ? mixed(kPanelActive, kWirePreview, 0.3f) : kPanelActive);
                frameRound(renderer, piece.rect, 7, 1.2f, kWirePreview);
                const Vec2 a{piece.rect.x + 15.0f, piece.rect.y + piece.rect.h / 2.0f};
                Mesh arrow;
                arrow.addLine({a.x, a.y}, {a.x + 10.0f, a.y}, 1.8f, kText);
                arrow.addLine({a.x, a.y}, {a.x + 5.0f, a.y - 5.0f}, 1.8f, kText);
                arrow.addLine({a.x, a.y}, {a.x + 5.0f, a.y + 5.0f}, 1.8f, kText);
                arrow.flush(renderer);
                write(renderer, Face::Medium, kUiText, piece.rect.x + 32.0f, piece.rect.y + 20.0f, "Voltar", kText);
                continue;
            }
            if (hovered) fillRound(renderer, piece.rect, 6, kPanelHover);
            write(renderer, Face::Medium, kUiText, piece.rect.x + 8.0f, piece.rect.y + 20.0f, trail[static_cast<std::size_t>(piece.index)].name,
                  hovered ? kText : kWirePreview);
            write(renderer, Face::Regular, kUiText, piece.rect.x + piece.rect.w + 4.0f, piece.rect.y + 20.0f, "›", kTextDim);
        }
        write(renderer, Face::Medium, kUiText, last.x + last.w + 18.0f, last.y + 20.0f, here, kText);
    }
    drawToolbar(renderer, session, width, rightEdge, text.mouseX, text.mouseY);
    if (text.lesson != nullptr && text.view == kViewBoard) {
        drawLessonStrip(renderer, {kPanelWidth, kToolbarHeight, rightEdge - kPanelWidth, kLessonStripHeight}, *text.lesson, text.mouseX, text.mouseY);
        if (text.quiz != nullptr) drawQuiz(renderer, quizArea(rightEdge, height), *text.quiz, text.mouseX, text.mouseY);
    }
    if (text.example != nullptr && text.view == kViewBoard) {
        drawExampleStrip(renderer, {kPanelWidth, kToolbarHeight, rightEdge - kPanelWidth, kLessonStripHeight}, *text.example, text.mouseX, text.mouseY);
    }
    if (anyComputer) {
        // The program first, in its colour, then the three ways of looking at the computer.
        const SDL_FRect program = programSwitchRect(rightEdge);
        fillRound(renderer, program, 7, text.programShown ? mixed(kBodyProgram, kToneProgram, 0.45f)
                                        : inside(program, text.mouseX, text.mouseY) ? mixed(kBodyProgram, kToneProgram, 0.25f) : kBodyProgram);
        frameRound(renderer, program, 7, text.programShown ? 1.6f : 1.0f, kToneProgram);
        const char* programLabel = program.w >= 120.0f ? "Programar a ROM" : "ROM";
        const float programWidth = textWidth(renderer, Face::Medium, kUiText, programLabel);
        write(renderer, Face::Medium, kUiText, program.x + (program.w - programWidth) / 2.0f, program.y + 19.0f, programLabel,
              text.programShown ? kText : kToneProgram);
        constexpr const char* kViews[kViewCount] = {"Placa", "Painel", "Raio-X", "Memória"};
        for (int which = 0; which < kViewCount; ++which) {
            const SDL_FRect r = viewSwitchRect(rightEdge, which);
            const bool chosen = which == text.view && !text.programShown;
            fillRound(renderer, r, 7, chosen ? kPanelActive : inside(r, text.mouseX, text.mouseY) ? kPanelHover : kPanelButton);
            const float labelWidth = textWidth(renderer, Face::Medium, kUiText, kViews[which]);
            write(renderer, Face::Medium, kUiText, r.x + (r.w - labelWidth) / 2.0f, r.y + 19.0f, kViews[which], chosen ? kText : kTextDim);
        }
    }

    fillRect(renderer, {8, h - 44.0f, kPanelWidth - 16.0f, 1.0f}, kRule);
    write(renderer, Face::Regular, 12, 14, h - 25, fitted(renderer, Face::Regular, 12, text.profile, kPanelWidth - 26.0f), kTextDim);
    write(renderer, Face::Regular, 11.5f, 14, h - 9, text.dataNote, kTextOff);

    const char* hint = text.quiz != nullptr ? "Digite a resposta e aperte Enter. Dica, no alto, mostra o caminho para a pergunta."
                                            : "Arraste peças da biblioteca. Clique em um pino para ligar. F1: atalhos.";
    switch (session.mode()) {
        case Mode::Placing: hint = "Clique para colocar. R gira. Esc termina."; break;
        case Mode::Wiring: hint = "Clique no vazio fixa um ponto. Clique em pino ou fio liga. Esc cancela."; break;
        case Mode::MovingParts: hint = "Solte para colocar."; break;
        case Mode::BoxSelect: hint = "Solte para selecionar."; break;
        case Mode::Pasting: hint = session.pasteFits() ? "Clique para colar. Esc cancela." : "Não cabe aqui. Mova para um espaço livre."; break;
        default: break;
    }

    std::string labelText;
    if (const Part* focus = focusedLabel(session); focus != nullptr && session.mode() == Mode::Idle) {
        const auto count = std::count_if(session.board().parts().begin(), session.board().parts().end(),
                                         [&](const Part& part) { return part.kind == PartKind::Tunnel && part.label == focus->label; });
        labelText = count > 1 ? i18n::format("O rótulo {} aparece em {} lugares, em laranja: todos são o mesmo fio.", {focus->label, std::to_string(count)})
                              : i18n::format("O rótulo {} só aparece aqui. Outro rótulo com o mesmo nome seria o mesmo fio.", {focus->label});
        hint = labelText.c_str();
    }

    char hoverText[96];
    if (session.mode() == Mode::Idle) {
        if (const std::optional<HoverInfo> info = session.hoverInfo()) {
            std::snprintf(hoverText, sizeof hoverText, "%u bit%s: 0x%llX = %llu", info->bits, info->bits == 1 ? "" : "s",
                          static_cast<unsigned long long>(info->value), static_cast<unsigned long long>(info->value));
            hint = hoverText;
        }
    }

    const BoardCircuit& circuit = session.circuit();
    Color hintColor = kTextDim;
    if (text.note != nullptr) {
        hintColor = text.noteIsError ? kWarning : kKnobOn;
        hint = text.note;
    } else if (circuit.hasConflict()) {
        hintColor = kWireConflict;
        hint = "Duas saídas no mesmo fio: o fio vermelho tem saídas em conflito.";
    } else if (circuit.hasWidthConflict()) {
        hintColor = kWireConflict;
        hint = "Larguras diferentes no mesmo fio: o fio vermelho liga pinos de bits diferentes.";
    } else if (circuit.oscillating()) {
        hintColor = kWarning;
        hint = "Oscilando: um sinal corre atrás do próprio rabo.";
    } else if (circuit.electricTooBig()) {
        hintColor = kWarning;
        hint = "Este circuito elétrico recebe mais de 8 sinais: a lógica em volta não consegue ler as saídas dele.";
    } else if (circuit.electricUnsettled()) {
        hintColor = kWarning;
        hint = "Uma tensão ficou entre 0 e 1: a lógica não sabe ler esse fio.";
    } else if (text.reading[0] != '\0') {
        hintColor = kText;
        hint = text.reading;
    }
    write(renderer, Face::Regular, kUiText, statusLeft + 14, h - kStatusHeight + 20, fitted(renderer, Face::Regular, kUiText, hint, rightEdge - statusLeft - 28.0f), hintColor);

    if (table) {
        const Lesson* goal = text.lesson != nullptr ? &lessons()[static_cast<std::size_t>(text.lesson->index)] : nullptr;
        drawRightPanel(renderer, session, width, height, text.tableScroll, text.mouseX, text.mouseY,
                       goal != nullptr && goal->check == LessonCheck::Table ? goal : nullptr);
    }
    if (text.view != kViewBoard && text.computer != nullptr && text.debug != nullptr) {
        drawDebugger(renderer, *text.computer, *text.debug, {0.0f, kToolbarHeight, kDebugWidth, h - kToolbarHeight}, text.mouseX, text.mouseY);
    }
    if (text.view != kViewBoard && text.computer32 != nullptr && text.debug != nullptr) {
        drawDebugger32(renderer, *text.computer32, *text.debug, text.console != nullptr ? *text.console : std::string(),
                       {0.0f, kToolbarHeight, kDebugWidth, h - kToolbarHeight}, text.mouseX, text.mouseY);
    }
    drawTooltip(renderer, text.mouseX, text.mouseY);
    if (text.showHelp) drawHelp(renderer, width, height);
}


// --- Por dentro: a computer drawn whole -------------------------------------------------------------------
// One plan, three pictures: the logic gates, the NANDs inside each gate, and the four transistors inside each
// NAND. Frames say what each part is. Everything is culled to the screen, and drawn simpler when it's small.

namespace {

constexpr float kInsideTop = 104.0f;    // the bar with the title, the buttons, the counts and what the picture is
constexpr float kInsideBottom = 30.0f;  // the line with the hints
constexpr Color kInsidePmos{255, 128, 176};
constexpr Color kInsideNmos{92, 168, 255};
constexpr Color kInsideNand{255, 190, 92};
constexpr Color kInsideRom{255, 186, 84};
constexpr Color kInsideRam{196, 140, 255};

Color insideGateColor(editor::ArchGate gate) {
    using editor::ArchGate;
    switch (gate) {
        case ArchGate::And:
        case ArchGate::Nand: return {92, 168, 255};
        case ArchGate::Or:
        case ArchGate::Nor: return {110, 226, 140};
        case ArchGate::Xor:
        case ArchGate::Xnor: return {255, 168, 96};
        case ArchGate::Not: return {200, 206, 196};
    }
    return kText;
}

// A number with its thousands apart, the way the language writes them: 7.353.716 or 7,353,716.
std::string grouped(std::size_t number) {
    const std::string digits = std::to_string(number);
    const char mark = i18n::language() == i18n::Language::English ? ',' : '.';
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += mark;
        out += digits[i];
    }
    return out;
}

bool bubbled(editor::ArchGate gate) {
    using editor::ArchGate;
    return gate == ArchGate::Not || gate == ArchGate::Nand || gate == ArchGate::Nor || gate == ArchGate::Xnor;
}

bool curved(editor::ArchGate gate) {
    using editor::ArchGate;
    return gate == ArchGate::Or || gate == ArchGate::Nor || gate == ArchGate::Xor || gate == ArchGate::Xnor;
}

// A gate's body, in a box from (x, top) w wide and h tall, its output at the middle of the right side:
// a D for AND and NAND, a shield for OR, NOR, XOR and XNOR, a triangle for NOT.
std::vector<Vec2> gateOutline(editor::ArchGate gate, float x, float top, float w, float h) {
    std::vector<Vec2> ring;
    const auto at = [&](float u, float v) { return Vec2{x + u * w, top + v * h}; };
    const auto curve = [&](Vec2 a, Vec2 control, Vec2 b, int steps) {
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float u = 1.0f - t;
            ring.push_back(at(u * u * a.x + 2.0f * u * t * control.x + t * t * b.x, u * u * a.y + 2.0f * u * t * control.y + t * t * b.y));
        }
    };
    if (gate == editor::ArchGate::Not) return {at(0.0f, 0.0f), at(1.0f, 0.5f), at(0.0f, 1.0f)};
    if (curved(gate)) {
        curve({0.0f, 0.0f}, {0.62f, 0.0f}, {1.0f, 0.5f}, 10);
        curve({1.0f, 0.5f}, {0.62f, 1.0f}, {0.0f, 1.0f}, 10);
        curve({0.0f, 1.0f}, {0.28f, 0.5f}, {0.0f, 0.0f}, 8);
        return ring;
    }
    ring.push_back(at(0.0f, 0.0f));
    for (int i = 0; i <= 14; ++i) {
        const float angle = -1.5707963f + 3.14159265f * static_cast<float>(i) / 14.0f;
        ring.push_back(at(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle)));
    }
    ring.push_back(at(0.0f, 1.0f));
    return ring;
}

// Where an input lead meets the back of a gate drawn at (x, top, w, h), at height y.
float gateBack(editor::ArchGate gate, float x, float top, float w, float h, float y) {
    const float v = std::clamp((y - top) / std::max(h, 0.001f), 0.0f, 1.0f);
    if (curved(gate)) return x + 2.0f * v * (1.0f - v) * 0.28f * w - (gate == editor::ArchGate::Xor || gate == editor::ArchGate::Xnor ? w * 0.12f : 0.0f);
    return x;
}

// A gate in the space of its NANDs, its leads ending exactly on the pins the wires come to: the inputs on the
// left edge at their heights, the output on the right edge.
void addCellGate(Mesh& mesh, editor::ArchGate gate, const SDL_FRect& r, const std::vector<float>& inputs, float output, Color color, float line) {
    float span = 0.0f;
    for (const float y : inputs) span = std::max(span, std::abs(y - output));
    const float half = std::max(span + std::max(r.h * 0.08f, line * 2.0f), r.h * 0.3f);
    const float h = std::min(half * 2.0f, r.h + line * 4.0f);
    const float top = output - h / 2.0f;
    const bool bubble = bubbled(gate);
    const float bubbleR = bubble ? std::min(h * 0.11f, r.w * 0.06f) : 0.0f;
    const float right = r.x + r.w - r.w * 0.06f - 2.0f * bubbleR;
    const float w = std::min(h * 1.15f, (right - r.x) * 0.72f);
    const float x = right - w;
    std::vector<Vec2> ring = gateOutline(gate, x, top, w, h);
    Vec2 centre{0.0f, 0.0f};
    for (const Vec2& p : ring) centre = {centre.x + p.x, centre.y + p.y};
    centre = {centre.x / static_cast<float>(ring.size()), centre.y / static_cast<float>(ring.size())};
    mesh.addFan(ring, centre, mixed(kBackground, color, 0.22f));
    for (std::size_t i = 0; i < ring.size(); ++i) mesh.addLine(ring[i], ring[(i + 1) % ring.size()], line, color);
    if (gate == editor::ArchGate::Xor || gate == editor::ArchGate::Xnor) {  // the second curve at the back
        Vec2 last{x - w * 0.12f, top};
        for (int i = 1; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            const Vec2 next{x - w * 0.12f + 2.0f * t * (1.0f - t) * 0.28f * w, top + t * h};
            mesh.addLine(last, next, line, color);
            last = next;
        }
    }
    if (bubble) mesh.addDisc({right + bubbleR, output}, bubbleR, color);
    mesh.addLine({right + 2.0f * bubbleR, output}, {r.x + r.w, output}, line, color);
    for (const float y : inputs) mesh.addLine({r.x, y}, {gateBack(gate, x, top, w, h, y) + line * 0.3f, y}, line, color);
}

// A NAND in its square: inputs at a quarter and three quarters on the left, the output halfway on the right.
void addNand(Mesh& mesh, const SDL_FRect& s, Color color, float line) {
    const float x = s.x + s.w * 0.2f, w = s.w * 0.58f, top = s.y + s.h * 0.1f, h = s.h * 0.8f;
    std::vector<Vec2> ring = gateOutline(editor::ArchGate::Nand, x, top, w, h);
    mesh.addFan(ring, {x + w * 0.4f, top + h / 2.0f}, mixed(kBackground, color, 0.22f));
    for (std::size_t i = 0; i < ring.size(); ++i) mesh.addLine(ring[i], ring[(i + 1) % ring.size()], line, color);
    const float bubble = s.w * 0.045f;
    mesh.addDisc({x + w + bubble, s.y + s.h * 0.5f}, bubble, color);
    mesh.addLine({x + w + 2.0f * bubble, s.y + s.h * 0.5f}, {s.x + s.w, s.y + s.h * 0.5f}, line, color);
    mesh.addLine({s.x, s.y + s.h * 0.25f}, {x, s.y + s.h * 0.25f}, line, color);
    mesh.addLine({s.x, s.y + s.h * 0.75f}, {x, s.y + s.h * 0.75f}, line, color);
}

// A NAND as its four transistors, with the same pins: two P (pink) from + to the output, side by side, and two
// N (blue) from the output to ground, one over the other. A goes to the left P and the upper N; B to the right
// P and the lower N, its wire over the top. Where B's wire crosses A's there's no dot: they don't meet.
void addNandTransistors(Mesh& mesh, const SDL_FRect& s, float line) {
    const auto P = [&](float u, float v) { return Vec2{s.x + u * s.w, s.y + v * s.h}; };
    const auto wire = [&](float u0, float v0, float u1, float v1, Color c) { mesh.addLine(P(u0, v0), P(u1, v1), line, c); };
    if (s.w < 14.0f) {  // too small for the drawing: four little squares, two P over two N
        for (int i = 0; i < 4; ++i) {
            const Vec2 a = P(0.12f + 0.42f * static_cast<float>(i % 2), 0.12f + 0.42f * static_cast<float>(i / 2));
            const float side = s.w * 0.34f;
            mesh.addFan({a, {a.x + side, a.y}, {a.x + side, a.y + side}, {a.x, a.y + side}}, {a.x + side / 2.0f, a.y + side / 2.0f}, i < 2 ? kInsidePmos : kInsideNmos);
        }
        return;
    }
    const Color lead = kWireOff;
    // A transistor: its channel down x from v0 to v1, its gate on the side the input comes from.
    const auto transistor = [&](float u, float v0, float v1, bool p, bool gateRight) {
        const Color c = p ? kInsidePmos : kInsideNmos;
        const float a = v0 + (v1 - v0) * 0.24f, b = v1 - (v1 - v0) * 0.24f;
        wire(u, v0, u, a, c);
        wire(u, b, u, v1, c);
        mesh.addLine(P(u, a), P(u, b), line * 2.4f, c);
        const float g = gateRight ? u + 0.05f : u - 0.05f;
        wire(g, a, g, b, c);
        if (p) mesh.addDisc(P(gateRight ? g + 0.025f : g - 0.025f, (v0 + v1) / 2.0f), s.w * 0.022f, c);
    };
    wire(0.36f, 0.05f, 0.84f, 0.05f, kLedOn);  // +
    mesh.addLine(P(0.48f, 0.95f), P(0.72f, 0.95f), line * 1.4f, kTextDim);  // ground
    wire(0.53f, 0.985f, 0.67f, 0.985f, kTextDim);
    transistor(0.42f, 0.05f, 0.42f, true, false);   // P for A
    transistor(0.78f, 0.05f, 0.42f, true, true);    // P for B
    transistor(0.60f, 0.42f, 0.68f, false, false);  // N for A
    transistor(0.60f, 0.68f, 0.95f, false, false);  // N for B
    wire(0.42f, 0.42f, 0.78f, 0.42f, kText);  // the output node
    wire(0.78f, 0.42f, 0.96f, 0.42f, kText);
    wire(0.96f, 0.42f, 0.96f, 0.5f, kText);
    wire(0.96f, 0.5f, 1.0f, 0.5f, kText);
    // A: to the left P's gate, and down to the upper N's.
    wire(0.0f, 0.25f, 0.345f, 0.25f, lead);
    wire(0.18f, 0.25f, 0.18f, 0.55f, lead);
    wire(0.18f, 0.55f, 0.55f, 0.55f, lead);
    // B: to the lower N's gate, and up, over the top, to the right P's.
    wire(0.0f, 0.75f, 0.55f, 0.75f, lead);
    wire(0.10f, 0.75f, 0.10f, 0.015f, lead);
    wire(0.10f, 0.015f, 0.93f, 0.015f, lead);
    wire(0.93f, 0.015f, 0.93f, 0.235f, lead);
    wire(0.93f, 0.235f, 0.855f, 0.235f, lead);
    if (s.w > 40.0f) {
        const float dot = s.w * 0.016f;
        mesh.addDisc(P(0.18f, 0.25f), dot, lead);
        mesh.addDisc(P(0.10f, 0.75f), dot, lead);
        mesh.addDisc(P(0.60f, 0.42f), dot, kText);
    }
}

// The bar's buttons, right to left: Voltar (0), the zoom: Inteiro (3), + (2), - (1), then the three pictures:
// transistors (6), NANDs (5), gates (4).
SDL_FRect insideButton(int which, int width) {
    static constexpr float kWidths[] = {96.0f, 38.0f, 38.0f, 72.0f, 132.0f, 132.0f, 132.0f};
    static constexpr int kOrder[] = {0, 3, 2, 1, 6, 5, 4};
    static constexpr float kGapAfter[] = {18.0f, 6.0f, 6.0f, 18.0f, 4.0f, 4.0f, 0.0f};
    float right = static_cast<float>(width) - 16.0f;
    for (int i = 0; i < 7; ++i) {
        const int b = kOrder[i];
        const SDL_FRect r{right - kWidths[b], 14.0f, kWidths[b], 34.0f};
        if (b == which) return r;
        right = r.x - kGapAfter[i];
    }
    return {};
}

void drawInsidePlan(SDL_Renderer* renderer, const InsideView& view, const SDL_FRect& area) {
    using editor::ArchFrameKind;
    const editor::Architecture& plan = *view.plan;
    const float k = view.scale;
    const auto sx = [&](float x) { return view.originX + x * k; };
    const auto sy = [&](float y) { return view.originY + y * k; };
    const auto screen = [&](const editor::ArchRect& r) { return SDL_FRect{sx(r.x), sy(r.y), r.w * k, r.h * k}; };
    const auto visible = [&](const SDL_FRect& r) { return r.x + r.w >= area.x && r.y + r.h >= area.y && r.x <= area.x + area.w && r.y <= area.y + area.h; };
    const SDL_Rect clip{static_cast<int>(area.x), static_cast<int>(area.y), static_cast<int>(area.w), static_cast<int>(area.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    Mesh mesh;
    const auto flushSometimes = [&]() {
        if (mesh.size() > 120000) mesh.flush(renderer);
    };
    const float line = std::clamp(k * 0.045f, 0.8f, 2.6f);

    // The frames, outside in: a tint for what it is, and its edge.
    for (const editor::ArchFrame& frame : plan.frames) {
        const SDL_FRect r = screen(frame.rect);
        if (!visible(r) || r.w < 2.0f) continue;
        Color fill = kBackground, edge = kRule;
        switch (frame.kind) {
            case ArchFrameKind::Computer: fill = mixed(kBackground, kPanel, 0.6f); edge = kBodyEdge; break;
            case ArchFrameKind::Chip: fill = mixed(kBackground, kBodyChip, 0.5f); edge = mixed(kBodyChip, kText, 0.35f); break;
            case ArchFrameKind::Box: fill = mixed(kBackground, kBodyBox, 0.55f); edge = mixed(kBodyBox, kSelected, 0.45f); break;
            case ArchFrameKind::Detail: fill = mixed(kBackground, kBodyBox, 0.8f); edge = mixed(kBodyBox, kTextDim, 0.45f); break;
        }
        const float radius = std::min(8.0f, k * 0.4f);
        mesh.addRoundRect(r, radius, fill);
        mesh.addRoundFrame(r, radius, frame.kind == ArchFrameKind::Detail ? 1.0f : 1.4f, edge);
        flushSometimes();
    }
    mesh.flush(renderer);

    // The memories: their part, and the grid of bits, its lines once they're far enough apart.
    for (const editor::ArchMemory& memory : plan.memories) {
        const SDL_FRect r = screen(memory.rect);
        if (!visible(r)) continue;
        const Color tone = memory.rom ? kInsideRom : kInsideRam;
        mesh.addRoundRect(r, std::min(6.0f, k * 0.3f), mixed(kBackground, tone, 0.12f));
        mesh.addRoundFrame(r, std::min(6.0f, k * 0.3f), 1.4f, tone);
        const SDL_FRect g = screen(memory.grid);
        const float cell = editor::kArchMemoryCell * k;
        const unsigned rowsPerBank = memory.words / std::max(memory.banks, 1u);
        const float bankW = static_cast<float>(memory.bits) * cell;
        for (unsigned bank = 0; bank < memory.banks; ++bank) {
            const SDL_FRect b{g.x + static_cast<float>(bank) * (bankW + 2.0f * cell), g.y, bankW, static_cast<float>(rowsPerBank) * cell};
            if (!visible(b)) continue;
            mesh.addFan({{b.x, b.y}, {b.x + b.w, b.y}, {b.x + b.w, b.y + b.h}, {b.x, b.y + b.h}}, {b.x + b.w / 2.0f, b.y + b.h / 2.0f}, mixed(kBackground, tone, 0.3f));
            if (cell < 3.0f) continue;
            const float top = std::max(b.y, area.y), low = std::min(b.y + b.h, area.y + area.h);
            for (unsigned bit = 0; bit <= memory.bits; ++bit) {
                const float x = b.x + static_cast<float>(bit) * cell;
                if (x >= area.x && x <= area.x + area.w) mesh.addLine({x, top}, {x, low}, 1.0f, tone, 0.45f);
            }
            const auto first = static_cast<unsigned>(std::max(0.0f, (area.y - b.y) / cell));
            const auto last = static_cast<unsigned>(std::max(0.0f, std::min(static_cast<float>(rowsPerBank), (area.y + area.h - b.y) / cell + 1.0f)));
            for (unsigned word = first; word <= last && word <= rowsPerBank; ++word) {
                const float y = b.y + static_cast<float>(word) * cell;
                mesh.addLine({std::max(b.x, area.x), y}, {std::min(b.x + b.w, area.x + area.w), y}, 1.0f, tone, 0.45f);
            }
            flushSometimes();
        }
        mesh.flush(renderer);
        if (r.w > 140.0f && r.h > 50.0f) {
            write(renderer, Face::Medium, 13, std::max(r.x, area.x) + 8.0f, std::max(r.y, area.y) + 20.0f,
                  i18n::format("{} palavras de {} bits", {grouped(memory.words), std::to_string(memory.bits)}), kText);
        }
    }

    // The wires between the parts of each frame, once the frame is big enough to tell them apart: every one
    // from the pin that drives it to every pin that reads it. A dot where a wire branches.
    // Live, a wire at 1 lights up like the board's, and one at 0 stays dark.
    const auto wireColor = [&](std::uint32_t signal) {
        if (view.values == nullptr || signal >= view.values->size()) return kWireOff;
        return (*view.values)[signal] != 0 ? kWireOn : mixed(kWireOff, kBackground, 0.35f);
    };
    const auto drawWires = [&](const editor::ArchWires& wires, float thick, float alpha) {
        for (std::uint32_t i = 0; i < wires.segments; ++i) {
            const editor::ArchSegment& s = plan.segments[wires.firstSegment + i];
            const float x0 = sx(s.x0), y0 = sy(s.y0), x1 = sx(s.x1), y1 = sy(s.y1);
            if (!visible({std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1.0f, std::abs(y1 - y0) + 1.0f})) continue;
            const bool lit = view.values != nullptr && s.signal < view.values->size() && (*view.values)[s.signal] != 0;
            mesh.addLine({x0, y0}, {x1, y1}, lit ? thick * 1.3f : thick, wireColor(s.signal), lit ? std::max(alpha, 0.85f) : alpha);
        }
        if (k >= 4.0f) {
            for (std::uint32_t i = 0; i < wires.dots; ++i) {
                const editor::ArchPoint& p = plan.dots[wires.firstDot + i];
                const float x = sx(p.x), y = sy(p.y);
                if (x >= area.x && y >= area.y && x <= area.x + area.w && y <= area.y + area.h) mesh.addDisc({x, y}, std::max(1.6f, thick * 1.4f), kWireOff, alpha);
            }
        }
        flushSometimes();
    };
    for (const editor::ArchFrame& frame : plan.frames) {
        const SDL_FRect r = screen(frame.rect);
        if (!visible(r) || r.w < 50.0f) continue;
        // Tracks are a few tenths of a unit apart: until they're a few pixels apart, they're drawn faint, so a
        // channel full of them reads as a band and the parts stay visible.
        drawWires(frame.wires, std::clamp(k * 0.03f, 0.8f, 2.0f), std::clamp(k * 0.35f / 3.0f, 0.12f, 0.9f));
    }
    mesh.flush(renderer);

    // The gates. A dot of colour while they're tiny; then the chosen picture, on the very pins of the wires.
    const bool nandsShown = view.level != InsideLevel::Gates && k >= 6.0f;
    for (const editor::ArchCell& cell : plan.cells) {
        const SDL_FRect r = screen(cell.rect);
        if (!visible(r)) continue;
        const Color color = insideGateColor(cell.gate);
        if (r.w < 5.0f) {
            const Color tiny = view.level == InsideLevel::Gates ? color : view.level == InsideLevel::Nands ? kInsideNand : mixed(kInsidePmos, kInsideNmos, 0.5f);
            mesh.addFan({{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}}, {r.x + r.w / 2.0f, r.y + r.h / 2.0f}, mixed(kBackground, tiny, 0.75f));
            flushSometimes();
            continue;
        }
        if (!nandsShown) {
            std::vector<float> inputs;
            for (const float y : cell.inputs) inputs.push_back(sy(y));
            addCellGate(mesh, cell.gate, r, inputs, sy(cell.output), color, line);
        } else {
            drawWires(cell.wires, std::clamp(k * 0.03f, 0.8f, 1.6f), 0.9f);
            for (std::uint32_t i = 0; i < cell.nands; ++i) {
                const SDL_FRect s = screen(plan.nands[cell.firstNand + i].rect);
                if (view.level == InsideLevel::Nands) addNand(mesh, s, kInsideNand, line);
                else addNandTransistors(mesh, s, std::clamp(k * 0.02f, 0.8f, 2.0f));
            }
        }
        flushSometimes();
    }
    mesh.flush(renderer);

    // Pins held at 0 or 1, and where the computer's inputs come in.
    if (k >= 10.0f) {
        for (const editor::ArchTie& tie : plan.ties) {
            const float x = sx(tie.x), y = sy(tie.y);
            if (x < area.x - 20.0f || x > area.x + area.w || y < area.y || y > area.y + area.h) continue;
            const char* text = tie.value != 0 ? "1" : "0";
            const float size = std::clamp(k * 0.22f, 9.0f, 14.0f);
            write(renderer, Face::Mono, size, x - textWidth(renderer, Face::Mono, size, text) - 2.0f, y + size * 0.36f, text, kTextDim);
        }
    }
    for (const editor::ArchPort& port : plan.ports) {
        const float x = sx(port.x), y = sy(port.y);
        if (y < area.y || y > area.y + area.h || x < area.x || x > area.x + area.w + 200.0f) continue;
        const char* name = port.label == "clock" ? "Clock" : port.label == "keyboard" ? "Teclado" : port.label == "button" ? "Botão" : "Chave";
        write(renderer, Face::Regular, 11, x + 4.0f, y - 3.0f, name, kTextDim);
    }

    // The frames' names, over everything, on a dark tab. Outer frames first; a name that would cover one
    // already written tries the other corners, and waits for a closer view if none is free.
    std::vector<SDL_FRect> taken;
    for (const editor::ArchFrame& frame : plan.frames) {
        const SDL_FRect r = screen(frame.rect);
        if (!visible(r) || frame.kind == ArchFrameKind::Computer) continue;
        const float size = frame.kind == ArchFrameKind::Chip ? 15.0f : frame.kind == ArchFrameKind::Box ? 12.5f : 11.0f;
        const Face face = frame.kind == ArchFrameKind::Detail ? Face::Regular : Face::Medium;
        const float textW = textWidth(renderer, face, size, frame.label);
        if (r.w < textW + 16.0f || r.h < size * 2.2f) continue;
        const Vec2 places[] = {{r.x + 6.0f, r.y + 4.0f}, {r.x + r.w - textW - 10.0f, r.y + 4.0f}, {r.x + 6.0f, r.y + r.h - size - 10.0f}};
        bool placed = false;
        SDL_FRect tab{};
        for (const Vec2& place : places) {
            tab = {place.x - 4.0f, place.y, textW + 8.0f, size + 6.0f};
            if (tab.y < area.y || tab.x + tab.w > area.x + area.w) continue;
            if (std::any_of(taken.begin(), taken.end(), [&](const SDL_FRect& t) { return SDL_HasRectIntersectionFloat(&t, &tab); })) continue;
            placed = true;
            break;
        }
        if (!placed) continue;
        taken.push_back(tab);
        fillRound(renderer, tab, 4.0f, kBackground, 0.82f);
        const Color tone = frame.kind == ArchFrameKind::Chip ? mixed(kBodyChip, kText, 0.7f) : frame.kind == ArchFrameKind::Box ? mixed(kSelected, kText, 0.4f) : kTextDim;
        write(renderer, face, size, tab.x + 4.0f, tab.y + size, frame.label, tone);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

}  // namespace

SDL_FRect insidePlanArea(int width, int height) {
    return {0.0f, kInsideTop, static_cast<float>(width), static_cast<float>(height) - kInsideTop - kInsideBottom};
}

InsideView insideWhole(const editor::Architecture& plan, int width, int height) {
    const SDL_FRect area = insidePlanArea(width, height);
    InsideView view;
    view.plan = &plan;
    view.scale = std::min((area.w - 40.0f) / std::max(plan.width, 1.0f), (area.h - 40.0f) / std::max(plan.height, 1.0f));
    view.originX = area.x + (area.w - plan.width * view.scale) / 2.0f;
    view.originY = area.y + (area.h - plan.height * view.scale) / 2.0f;
    return view;
}

// Live, under the zoom: the clock run or paused, and one step of it.
SDL_FRect insideClockButton(int which, int width) {
    const float right = static_cast<float>(width) - 16.0f;
    return which == 0 ? SDL_FRect{right - 150.0f - 8.0f - 118.0f, 56.0f, 118.0f, 28.0f} : SDL_FRect{right - 150.0f, 56.0f, 150.0f, 28.0f};
}

InsideView insideOn(const editor::Architecture& plan, const editor::ArchRect& rect, int width, int height) {
    InsideView view = insideWhole(plan, width, height);
    const SDL_FRect area = insidePlanArea(width, height);
    const float scale = std::clamp(std::min(area.w * 0.6f / std::max(rect.w, 1.0f), area.h * 0.6f / std::max(rect.h, 1.0f)), view.scale, 90.0f);
    view.scale = scale;
    view.originX = area.x + area.w / 2.0f - (rect.x + rect.w / 2.0f) * scale;
    view.originY = area.y + area.h / 2.0f - (rect.y + rect.h / 2.0f) * scale;
    return view;
}

InsideChoice insideAt(float x, float y, int width, int height, bool live) {
    (void)height;
    if (live && inside(insideClockButton(0, width), x, y)) return InsideChoice::Run;
    if (live && inside(insideClockButton(1, width), x, y)) return InsideChoice::Step;
    static constexpr InsideChoice kChoices[] = {InsideChoice::Back,  InsideChoice::Farther, InsideChoice::Closer,     InsideChoice::Whole,
                                                InsideChoice::Gates, InsideChoice::Nands,   InsideChoice::Transistors};
    for (int i = 0; i < 7; ++i) {
        if (inside(insideButton(i, width), x, y)) return kChoices[i];
    }
    return InsideChoice::None;
}

void drawInside(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const InsideView& view) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    if (view.plan == nullptr) return;
    const editor::Architecture& plan = *view.plan;
    drawInsidePlan(renderer, view, insidePlanArea(width, height));

    // The bar: the title, what's in it, and the buttons.
    fillRect(renderer, {0.0f, 0.0f, static_cast<float>(width), kInsideTop}, kPanel);
    fillRect(renderer, {0.0f, kInsideTop - 1.0f, static_cast<float>(width), 1.0f}, kRule);
    const std::string title = !view.focus.empty() ? i18n::format("{} por dentro, ao vivo", {i18n::tr(view.focus)})
                              : plan.frames.empty()   ? std::string()
                                                      : i18n::format("{} por dentro", {i18n::tr(plan.frames.front().label)});
    write(renderer, Face::Medium, 20, 18.0f, 38.0f, fitted(renderer, Face::Medium, 20, title, insideButton(4, width).x - 36.0f), kText);
    // The counts, the one of the picture shown lit.
    const editor::ArchCounts& c = plan.counts;
    struct Piece {
        std::string text;
        bool lit;
    };
    const Piece pieces[] = {
        {i18n::format("{} portas lógicas e {} flip-flops", {grouped(c.gates), grouped(c.flipFlops)}), view.level == InsideLevel::Gates},
        {i18n::format("{} NANDs", {grouped(c.nands)}), view.level == InsideLevel::Nands},
        {i18n::format("{} transistores", {grouped(c.transistors)}), view.level == InsideLevel::Transistors},
        {i18n::format("{} bits de memória", {grouped(c.memoryBits)}), false},
    };
    float x = 18.0f;
    for (std::size_t i = 0; i < std::size(pieces); ++i) {
        if (i > 0) {
            write(renderer, Face::Regular, 13.5f, x, 72.0f, "·", kTextOff);
            x += 14.0f;
        }
        write(renderer, pieces[i].lit ? Face::Medium : Face::Regular, 13.5f, x, 72.0f, pieces[i].text, pieces[i].lit ? kToneProgram : kTextDim);
        x += textWidth(renderer, pieces[i].lit ? Face::Medium : Face::Regular, 13.5f, pieces[i].text) + 10.0f;
    }
    static constexpr const char* kWhat[] = {
        "Cada símbolo é uma porta lógica. Os quadros dizem que parte do computador é aquela.",
        "Cada porta é feita de NANDs: um NOT é 1, um AND 2, um OR 3, um XOR 4. É assim que o simulador monta tudo.",
        "Cada NAND são 4 transistores: 2 do tipo P em cima, ligados ao +, e 2 do tipo N embaixo, ligados ao terra.",
    };
    write(renderer, Face::Regular, 13, 18.0f, 93.0f, fitted(renderer, Face::Regular, 13, kWhat[static_cast<int>(view.level)], static_cast<float>(width) - 36.0f), kTextDim);

    static constexpr const char* kLabels[] = {"Voltar", "-", "+", "Inteiro", "Portas lógicas", "NANDs", "Transistores"};
    for (int i = 0; i < 7; ++i) {
        const SDL_FRect r = insideButton(i, width);
        const bool lit = i >= 4 && static_cast<int>(view.level) == i - 4;
        const bool hovered = inside(r, mouseX, mouseY);
        fillRound(renderer, r, 8, lit ? mixed(kPanelButton, kToneProgram, 0.4f) : hovered ? kPanelHover : kPanelButton);
        if (lit) frameRound(renderer, r, 8, 1.4f, kToneProgram);
        const float size = i == 1 || i == 2 ? 18.0f : kUiText;
        const float w = textWidth(renderer, Face::Medium, size, kLabels[i]);
        write(renderer, Face::Medium, size, r.x + (r.w - w) / 2.0f, r.y + (i == 1 || i == 2 ? 24.0f : 22.0f), kLabels[i], lit || i < 4 ? kText : kTextDim);
    }
    if (view.values != nullptr) {
        // The clock: run, pause, a step; and where it is now.
        for (int i = 0; i < 2; ++i) {
            const SDL_FRect r = insideClockButton(i, width);
            const bool hovered = inside(r, mouseX, mouseY);
            const bool lit = i == 0 && view.running;
            fillRound(renderer, r, 7, lit ? mixed(kPanelButton, kWireOn, 0.3f) : hovered ? kPanelHover : kPanelButton);
            if (lit) frameRound(renderer, r, 7, 1.2f, kWireOn);
            const char* label = i == 0 ? (view.running ? "Pausar clock" : "Rodar clock") : "Passo do clock (N)";
            const float w = textWidth(renderer, Face::Medium, 12.5f, label);
            write(renderer, Face::Medium, 12.5f, r.x + (r.w - w) / 2.0f, r.y + 19.0f, label, kText);
        }
        const SDL_FRect run = insideClockButton(0, width);
        const char* clock = view.clockHigh ? "clock em 1" : "clock em 0";
        write(renderer, Face::Medium, 12.5f, run.x - 14.0f - textWidth(renderer, Face::Medium, 12.5f, clock), run.y + 19.0f, clock, view.clockHigh ? kWireOn : kTextDim);
    }
    write(renderer, Face::Regular, 12, 18.0f, static_cast<float>(height) - 10.0f,
          view.values != nullptr ? "Ao vivo: fio claro é 1, escuro é 0 · Espaço roda, N dá um passo · arraste para mover · roda do mouse: aproximar · Esc: voltar à placa"
                                 : "Arraste para mover · roda do mouse: aproximar · 1, 2, 3: portas, NANDs, transistores · Esc: voltar",
          kTextOff);
}

// --- A splitter or a joiner from the inside ---------------------------------------------------------------

SDL_FRect bundleBackRect(int width, int height) {
    (void)height;
    return {static_cast<float>(width) - 16.0f - 140.0f, 14.0f, 140.0f, 34.0f};
}

void drawBundle(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const BundleView& view) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    if (view.part == nullptr) return;
    const Part& part = *view.part;
    const bool splitter = part.kind == PartKind::Splitter;
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    fillRect(renderer, {0.0f, 0.0f, w, 64.0f}, kPanel);
    fillRect(renderer, {0.0f, 63.0f, w, 1.0f}, kRule);
    write(renderer, Face::Medium, 20, 18.0f, 38.0f, splitter ? "Separador por dentro" : "Juntador por dentro", kText);
    const SDL_FRect back = bundleBackRect(width, height);
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    const float backWidth = textWidth(renderer, Face::Medium, kUiText, "Voltar à placa");
    write(renderer, Face::Medium, kUiText, back.x + (back.w - backWidth) / 2.0f, back.y + 22.0f, "Voltar à placa", kText);
    write(renderer, Face::Regular, 14.0f, 18.0f, 92.0f,
          splitter ? "Não há portas aqui dentro, só fios: o barramento que entra se abre em pedaços, e cada bit segue reto até o seu lugar."
                   : "Não há portas aqui dentro, só fios: os pedaços que entram se juntam num barramento, e cada bit segue reto até o seu lugar.",
          kTextDim);

    unsigned total = 0;
    std::vector<unsigned> widths;
    for (unsigned b = 0; b < part.inputs; ++b) {
        widths.push_back(branchBits(part, b));
        total += widths.back();
    }
    if (total == 0) return;
    const std::uint64_t bus = splitter ? (view.values.empty() ? 0 : view.values[0]) : (view.values.size() > part.inputs ? view.values[part.inputs] : 0);

    // Every bit has a row of its own, the same on both sides, with a little gap between the pieces: each wire is
    // a straight line, and the bits of a piece sit together.
    const float top = 150.0f;
    const float bottom = h - 62.0f;
    constexpr float kGap = 0.8f;
    const float units = static_cast<float>(total) + kGap * static_cast<float>(part.inputs - 1);
    const float pitch = std::clamp((bottom - top) / units, 9.0f, 30.0f);
    const float label = pitch >= 16.0f ? 12.0f : 10.0f;
    const float busX = splitter ? w * 0.28f : w * 0.66f;     // the bus bar's middle
    const float piecesX = splitter ? w * 0.60f : w * 0.22f;  // where the pieces' boxes start
    const float pieceWidth = 120.0f;
    const auto rowY = [&](unsigned bit, unsigned piece) { return top + pitch * (static_cast<float>(bit) + kGap * static_cast<float>(piece)) + pitch / 2.0f; };
    const float lastY = rowY(total - 1, part.inputs - 1);

    // The bus: a bar with a pin per bit, its size above and its value under it.
    const SDL_FRect bar{busX - 20.0f, top - 6.0f, 40.0f, lastY - top + pitch / 2.0f + 12.0f};
    fillRound(renderer, bar, 10, mixed(kPanel, kBusOn, bus != 0 ? 0.35f : 0.15f));
    frameRound(renderer, bar, 10, 1.4f, kBusOn);
    write(renderer, Face::Medium, 14.0f, bar.x - 20.0f, top - 22.0f, i18n::format(splitter ? "Entra: {} bits" : "Sai: {} bits", {std::to_string(total)}), kBusOn);
    char value[64];
    std::snprintf(value, sizeof value, "= %llu  (0x%llX)", static_cast<unsigned long long>(bus), static_cast<unsigned long long>(bus));
    write(renderer, Face::Medium, 14.0f, bar.x - 20.0f, bar.y + bar.h + 22.0f, value, kText);
    write(renderer, Face::Medium, 14.0f, piecesX, top - 22.0f, splitter ? "Saídas" : "Entradas", kWirePreview);

    Mesh mesh;
    unsigned bit = 0;
    for (unsigned b = 0; b < part.inputs; ++b) {
        const std::size_t pin = splitter ? 1 + b : b;
        const std::uint64_t piece = pin < view.values.size() ? view.values[pin] : 0;
        const float firstY = rowY(bit, b);
        const float endY = rowY(bit + widths[b] - 1, b);
        const SDL_FRect box{piecesX, firstY - pitch / 2.0f + 1.0f, pieceWidth, endY - firstY + pitch - 2.0f};
        fillRound(renderer, box, 6, kPanel);
        frameRound(renderer, box, 6, 1.0f, piece != 0 ? kWireOn : kRule);
        for (unsigned k = 0; k < widths[b]; ++k, ++bit) {
            const float y = rowY(bit, b);
            const bool on = ((bus >> bit) & 1u) != 0;
            const Color color = on ? kWireOn : mixed(kWireOff, kBackground, 0.25f);
            // From the bus's pin to the piece's, straight across.
            const float busPin = splitter ? bar.x + bar.w : bar.x;
            const float piecePin = splitter ? box.x : box.x + box.w;
            mesh.addLine({std::min(busPin, piecePin), y}, {std::max(busPin, piecePin), y}, on ? 2.4f : 1.4f, color);
            mesh.addDisc({busPin, y}, std::min(5.0f, pitch * 0.3f), on ? kWireOn : kPanelButton);
            mesh.addDisc({piecePin, y}, std::min(5.0f, pitch * 0.3f), on ? kWireOn : kPanelButton);
            mesh.flush(renderer);
            // The bit's place in the bus, beside the bar; its place in the piece, inside the box.
            const std::string busLabel = std::to_string(bit);
            const float busLabelX = splitter ? bar.x - 8.0f - textWidth(renderer, Face::Mono, label, busLabel) : bar.x + bar.w + 8.0f;
            write(renderer, Face::Mono, label, busLabelX, y + label * 0.35f, busLabel, on ? kText : kTextDim);
            const std::string pieceLabel = i18n::format("bit {}", {std::to_string(k)});
            write(renderer, Face::Mono, label, box.x + 10.0f, y + label * 0.35f, pieceLabel, on ? kText : kTextDim);
            write(renderer, Face::Mono, label, box.x + box.w - 18.0f, y + label * 0.35f, on ? "1" : "0", on ? kWireOn : kTextOff);
        }
        // The piece's name, and its value in decimal and in binary.
        std::string binary;
        for (unsigned k = widths[b]; k-- > 0;) binary += ((piece >> k) & 1u) != 0 ? '1' : '0';
        const std::string name = i18n::format(splitter ? "saída {}" : "entrada {}", {std::to_string(b + 1)});
        const std::string number = i18n::format("= {} ({} em binário)", {std::to_string(piece), binary});
        const float tx = splitter ? box.x + box.w + 14.0f : box.x - 14.0f - std::max(textWidth(renderer, Face::Medium, 13.0f, name), textWidth(renderer, Face::Regular, 12.5f, number));
        if (box.h >= 30.0f) {
            write(renderer, Face::Medium, 13.0f, tx, box.y + box.h / 2.0f - 2.0f, name, kText);
            write(renderer, Face::Regular, 12.5f, tx, box.y + box.h / 2.0f + 14.0f, number, piece != 0 ? kWireOn : kTextDim);
        } else {
            const std::string line = name + "  " + number;
            const float lx = splitter ? tx : box.x - 14.0f - textWidth(renderer, Face::Regular, 12.5f, line);
            write(renderer, Face::Regular, 12.5f, lx, box.y + box.h / 2.0f + 5.0f, line, piece != 0 ? kText : kTextDim);
        }
    }
    write(renderer, Face::Regular, 12, 18.0f, h - 10.0f,
          "O bit 0 é o de menor valor. Fio claro é 1, escuro é 0, e tudo muda junto com a placa. Esc: voltar à placa", kTextOff);
}

}  // namespace bancada::app
