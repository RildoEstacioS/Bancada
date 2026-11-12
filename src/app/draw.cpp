#include "app/draw.hpp"


#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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
    }
    return controls;
}

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
        {"jk_flip_flop", "FF JK"}, {"buffer", "BUF"},        {"parity_odd", "ÍMPAR"},     {"parity_even", "PAR"},  {"bit_select", "SEL BIT"},
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
                      button.sim == SimButton::SpeedLabel || button.sim == SimButton::Reset || button.sim == SimButton::Home;
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

void drawRightPanel(SDL_Renderer* renderer, const Session& session, int width, int height, int scroll, float mouseX, float mouseY) {
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
                default: break;
            }
        }
        const float ruleY = tableTop(session) - 6.0f;
        fillRect(renderer, {left + 12, ruleY, panelWidth - 24, 1.0f}, kRule);
    }

    const float top = tableTop(session);
    write(renderer, Face::Medium, 14, left + 14, top + 24, "Tabela-verdade", kText);
    const TruthTable& table = session.truthTable();

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
                std::size_t at = 0;
                for (std::size_t i = 0; i < table.outputs.size(); ++i) {
                    const std::size_t digits = hexDigits(table.outputBits[i]);
                    const std::string cell = values.substr(at, digits);
                    at += digits;
                    const bool zero = cell.find_first_not_of('0') == std::string::npos;
                    const Color color = cell[0] == '~' ? kWarning : zero ? kTextDim : kKnobOn;
                    write(renderer, Face::Mono, 14, outputX[i], baseline, cell, color);
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

// The panel: the whole instruction at once on the same datapath as the X-ray, every value where it
// arrived. The program, the registers and the memory are in the debugger beside it.
void drawComputerPanel(SDL_Renderer* renderer, const ComputerState& state, const SDL_FRect& area, float mouseX, float mouseY) {
    fillRect(renderer, area, kBackground);
    const SDL_FRect next = panelNextButton(area);
    fillRound(renderer, next, 8, inside(next, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Step, {next.x + 20.0f, next.y + next.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, next.x + 36.0f, next.y + 21.0f, "Próxima instrução", kText);
    const float sentenceX = next.x + next.w + 18.0f;
    write(renderer, Face::Regular, 15, sentenceX, next.y + 22.0f, fitted(renderer, Face::Regular, 15, explainInstruction(state), area.x + area.w - sentenceX - 16.0f), kText);
    drawXrayPlan(renderer, state, {area.x + 10.0f, area.y + 58.0f, area.w - 20.0f, area.h - 68.0f}, kTraceStages - 1, 100.0f);
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
};

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
        {kWSelectNext, 2, {{262, 416}, {8, 416}, {8, 132}, {22, 132}}, {150, 416}},
    };
    return wires;
}

std::string xrayNumber(std::uint16_t value) { return std::to_string(value); }
std::string xraySigned(std::uint16_t value) { return std::to_string(static_cast<std::int16_t>(value)); }

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
            char text[8];
            std::snprintf(text, sizeof text, "%04X", static_cast<unsigned>(t.word));
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
        case kWDecoderA: return t.branches ? kFieldRd : kFieldRs;
        case kWDecoderB: return t.branches ? kFieldRs : t.op == assembly::kOpSt ? kFieldRd : kFieldRt;
        case kWDecoderWrite: return kFieldRd;
        case kWDecoderExtender:
        case kWExtenderMux:
        case kWImmBranch:
        case kWJump: return kFieldImm;
        case kWMuxAlu: return t.aluTakesImmediate ? kFieldImm : tone;
        default: return tone;
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
    const float scale = std::min((plan.w - 20.0f) / 980.0f, (plan.h - 20.0f) / 450.0f);
    const Vec2 origin{plan.x + (plan.w - 980.0f * scale) / 2.0f, plan.y + (plan.h - 450.0f * scale) / 2.0f};
    const auto at = [&](Vec2 p) { return Vec2{origin.x + p.x * scale, origin.y + p.y * scale}; };
    const float text = std::clamp(12.5f * scale, 9.5f, 15.0f);
    const float tagSize = std::clamp(11.5f * scale, 9.0f, 13.5f);
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
            mesh.addLine(points[i], points[i + 1], on ? 2.8f : 1.4f, on ? tone : kBodyEdge, !used ? 0.2f : on ? 1.0f : 0.9f);
        }
        if (!on) continue;
        const std::string value = xrayWireValue(trace, wire.wire);
        if (value.empty()) continue;
        const float half = (textWidth(renderer, Face::Mono, tagSize, value) + 10.0f) / 2.0f;
        // A wire shorter than its tag, between a multiplexer and the ALU, keeps the value its input already shows.
        if (half * 2.0f > length) continue;
        float parked = std::max(0.0f, length - half - 5.0f);
        if (wire.wire == kWZeroA || wire.wire == kWImmBranch || wire.wire == kWZero || wire.wire == kWSelectB || wire.wire == kWSelectNext) {
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
            const char* operation = used ? trace.aluOperation : "";
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

        if (shape.block == kXDecoder && used) {
            const float x = r.x + 8.0f * scale;
            write(renderer, Face::Medium, text, x, r.y + 20.0f * scale, shape.name, ink);
            write(renderer, Face::Medium, std::clamp(20.0f * scale, 12.0f, 22.0f), x, r.y + 52.0f * scale, trace.name, on ? tone : ink);
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
        if (shape.block == kXControl && used) {
            std::string signals;
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

void drawWordStrip(SDL_Renderer* renderer, const InstructionTrace& t, const SDL_FRect& area) {
    const float cell = std::clamp((area.w - 340.0f) / 19.0f, 14.0f, 22.0f);
    const float gap = 4.0f;
    float x = area.x;
    const float y = area.y;
    write(renderer, Face::Medium, 11.5f, x, y + 10.0f, "A instrução em binário, campo por campo", kTextDim);
    const float bitsTop = y + 18.0f;
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
    if (running) {
        const SDL_FRect first = xrayStageRect(area, 0);
        // Up to three lines: what happens, where each value comes from, and which control lines are on.
        const float width = area.x + area.w - first.x - 16.0f;
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 14.5f, explainStage(trace, stage), width);
        for (std::size_t i = 0; i < lines.size() && i < 3; ++i) {
            const std::string line = i == 2 && lines.size() > 3 ? fitted(renderer, Face::Regular, 14.5f, lines[i] + " …", width) : lines[i];
            write(renderer, Face::Regular, 14.5f, first.x, first.y + first.h + 24.0f + 20.0f * static_cast<float>(i), line, tone);
        }
    }

    const float legend = drawXrayLegend(renderer, {xrayStageRect(area, 0).x, area.y, area.w - (xrayStageRect(area, 0).x - area.x) - 16.0f, area.h});
    const float strip = running ? kWordStripHeight + 6.0f : 0.0f;
    if (running) drawWordStrip(renderer, trace, {xrayStageRect(area, 0).x, area.y + 178.0f, area.w - 40.0f, kWordStripHeight});
    drawXrayPlan(renderer, state, {area.x + 10.0f, area.y + 176.0f + strip, area.w - 20.0f, area.h - 186.0f - legend - strip}, stage, progress);
}

// What the short names on the X-ray mean, under it: the fields of an instruction.
float drawXrayLegend(SDL_Renderer* renderer, const SDL_FRect& area) {
    const float size = 12.5f;
    const std::vector<std::string> lines =
        wrapped(renderer, Face::Regular, size,
                "Siglas: op = o código que diz qual é a instrução · rd = o registrador que recebe o resultado · rs, rt = os registradores "
                "lidos · imm = o imediato, um número escrito dentro da própria instrução, como o 10 de LI R1, 10 (imm6, imm9 e imm12 dizem "
                "quantos bits ele ocupa)",
                area.w);
    float y = area.y + area.h - 6.0f - 17.0f * static_cast<float>(std::min<std::size_t>(lines.size(), 2) - 1);
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i, y += 17.0f) write(renderer, Face::Regular, size, area.x, y, lines[i], kTextDim);
    return 40.0f;
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
};

constexpr InstructionHelp kInstructionHelp[] = {
    {"Contas", "", "", Format::Text, kToneAlu},
    {"ADD Rd, Rs, Rt", "Soma Rs com Rt e guarda o resultado em Rd.", "ADD R3, R1, R2", Format::R3, kToneAlu},
    {"SUB Rd, Rs, Rt", "Subtrai Rt de Rs e guarda em Rd. Abaixo de zero, o número dá a volta: 0 - 1 vira 65535.", "SUB R3, R1, R2", Format::R3, kToneAlu},
    {"AND Rd, Rs, Rt", "E bit a bit: cada bit de Rd é 1 só onde Rs e Rt têm 1. Serve para isolar bits.", "AND R1, R1, R2", Format::R3, kToneAlu},
    {"OR Rd, Rs, Rt", "OU bit a bit: cada bit de Rd é 1 onde Rs ou Rt têm 1. Serve para ligar bits.", "OR R1, R1, R2", Format::R3, kToneAlu},
    {"XOR Rd, Rs, Rt", "OU exclusivo bit a bit: 1 onde os bits de Rs e Rt são diferentes.", "XOR R1, R1, R2", Format::R3, kToneAlu},
    {"NOT Rd, Rs", "Inverte cada bit de Rs: os 0 viram 1 e os 1 viram 0.", "NOT R2, R1", Format::R2, kToneAlu},
    {"SHL Rd, Rs", "Desloca os bits de Rs uma casa para a esquerda, entrando um 0: multiplica por 2.", "SHL R1, R1", Format::R2, kToneAlu},
    {"SHR Rd, Rs", "Desloca os bits de Rs uma casa para a direita: divide por 2, sem resto.", "SHR R1, R1", Format::R2, kToneAlu},
    {"ADDI Rd, Rs, n", "Soma o número n, de -32 a 31, a Rs e guarda em Rd. É o jeito de contar e descontar.", "ADDI R1, R1, -1", Format::I6, kToneAlu},
    {"MOV Rd, Rs", "Copia Rs para Rd. Não é uma instrução própria: o montador escreve ADD Rd, Rs, R0.", "MOV R2, R1", Format::Pseudo, kToneAlu},
    {"Números", "", "", Format::Text, kToneNumber},
    {"LI Rd, n", "Põe o número n, de 0 a 511, em Rd. O número fica escrito na própria instrução.", "LI R1, 10", Format::I9, kToneNumber},
    {"LUI Rd, n", "Põe n × 128 em Rd: a parte de cima de um número grande, de 128 em 128.", "LUI R1, 4", Format::I9, kToneNumber},
    {"LIW Rd, n", "Põe qualquer número de 0 a 65535 em Rd. O montador troca por um LUI e alguns ADDI, então ocupa mais de uma palavra.",
     "LIW R1, 1000", Format::Pseudo, kToneNumber},
    {"Memória", "", "", Format::Text, kToneMemory},
    {"LD Rd, n(Rs)", "Lê a palavra da RAM no endereço Rs + n e guarda em Rd. Sem n, lê do endereço que está em Rs.", "LD R2, 0(R1)", Format::I6, kToneMemory},
    {"ST Rd, n(Rs)", "Grava o valor de Rd na RAM, no endereço Rs + n.", "ST R2, 1(R1)", Format::I6, kToneMemory},
    {"Caminho", "", "", Format::Text, kToneBranch},
    {"BEQ Ra, Rb, lugar", "Se Ra for igual a Rb, pula para o lugar; se não, segue para a instrução seguinte. O lugar fica a no máximo 32 instruções "
     "para trás ou 31 para a frente: o número guardado é a distância.", "BEQ R1, R0, fim", Format::I6, kToneBranch},
    {"BNE Ra, Rb, lugar", "Se Ra for diferente de Rb, pula para o lugar. É o que faz um laço repetir.", "BNE R1, R0, laço", Format::I6, kToneBranch},
    {"JMP lugar", "Pula para o lugar, sempre. Alcança qualquer endereço de 0 a 4095.", "JMP início", Format::J, kToneJump},
    {"CALL lugar", "Chama uma sub-rotina: pula para o lugar e guarda em R7 o endereço da instrução seguinte, para poder voltar.", "CALL triplica",
     Format::J, kToneJump},
    {"RET", "Volta de uma sub-rotina, para o endereço guardado em R7. O montador escreve JR R7.", "RET", Format::Pseudo, kToneJump},
    {"JR Rs", "Pula para o endereço que está em Rs.", "JR R7", Format::Rs, kToneJump},
    {"Mundo", "", "", Format::Text, kToneWorld},
    {"IN Rd", "Lê o número que as chaves formam e guarda em Rd.", "IN R1", Format::Rd, kToneWorld},
    {"OUT Rs", "Mostra o valor de Rs na saída, onde ele fica até o próximo OUT.", "OUT R2", Format::Rs, kToneWorld},
    {"NOP", "Não faz nada: ocupa uma palavra e passa adiante. O montador escreve ADD R0, R0, R0.", "NOP", Format::Pseudo, kTextDim},
    {"HLT", "Para o computador. O PC fica parado nesta instrução.", "HLT", Format::None, kToneHalt},
    {"Escrita", "", "", Format::Text, kWirePreview},
    {"R0 a R7", "Os oito registradores. R0 vale sempre 0, e o que se grava nele se perde; R7 recebe o endereço de volta do CALL.", "ADD R1, R0, R0",
     Format::Text, kWirePreview},
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
    {"add $d, $s, $t", "Soma $s com $t e guarda em $d. addu faz o mesmo: aqui não há aviso de estouro.", "add $t2, $t0, $t1", Format::Text, kToneAlu},
    {"sub $d, $s, $t", "Subtrai $t de $s e guarda em $d.", "sub $t2, $t0, $t1", Format::Text, kToneAlu},
    {"and, or, xor, nor", "Bit a bit: E, OU, OU exclusivo e NÃO-OU de $s com $t, em $d.", "and $t2, $t0, $t1", Format::Text, kToneAlu},
    {"slt $d, $s, $t", "Põe 1 em $d se $s for menor que $t, com sinal; senão, 0. sltu compara sem sinal.", "slt $t2, $t0, $t1", Format::Text, kToneAlu},
    {"sll, srl, sra $d, $t, n", "Desloca $t n casas: sll para a esquerda (vezes 2^n), srl para a direita entrando zeros, sra copiando o sinal.",
     "sll $t1, $t0, 2", Format::Text, kToneAlu},
    {"addi $t, $s, n", "Soma o número n, de -32768 a 32767, a $s e guarda em $t.", "addi $t0, $t0, 1", Format::Text, kToneAlu},
    {"andi, ori, xori $t, $s, n", "Bit a bit com o número n, de 0 a 65535.", "andi $t1, $t0, 0xFF", Format::Text, kToneAlu},
    {"slti $t, $s, n", "Põe 1 em $t se $s for menor que n.", "slti $t0, $a0, 2", Format::Text, kToneAlu},
    {"move $d, $s", "Copia $s para $d. O montador escreve addu $d, $zero, $s.", "move $a0, $t2", Format::Pseudo, kToneAlu},
    {"Números", "", "", Format::Text, kToneNumber},
    {"li $t, n", "Põe qualquer número de 32 bits em $t. Um número pequeno vira uma instrução; um grande, lui e ori.", "li $t0, 100000", Format::Pseudo, kToneNumber},
    {"lui $t, n", "Põe n nos 16 bits de cima de $t, com zeros embaixo.", "lui $t0, 0x1001", Format::Text, kToneNumber},
    {"la $t, rótulo", "Põe em $t o endereço do rótulo, com lui e ori: para percorrer um vetor do .data.", "la $t0, vetor", Format::Pseudo, kToneNumber},
    {"Memória", "", "", Format::Text, kToneMemory},
    {"lw $t, n($s)", "Lê a palavra da RAM no endereço $s + n e guarda em $t. Os endereços contam bytes: as palavras vão de 4 em 4.",
     "lw $t1, 4($t0)", Format::Text, kToneMemory},
    {"sw $t, n($s)", "Grava $t na RAM, no endereço $s + n.", "sw $ra, 0($sp)", Format::Text, kToneMemory},
    {"lw $t, rótulo", "Lê a palavra que está no rótulo do .data.", "lw $t1, n", Format::Pseudo, kToneMemory},
    {"Caminho", "", "", Format::Text, kToneBranch},
    {"beq $s, $t, rótulo", "Se $s for igual a $t, desvia para o rótulo; senão, segue.", "beq $t0, $zero, fim", Format::Text, kToneBranch},
    {"bne $s, $t, rótulo", "Se $s for diferente de $t, desvia. É o que faz um laço repetir.", "bne $t0, $t1, laco", Format::Text, kToneBranch},
    {"blt, bgt, ble, bge", "Compara e desvia: menor, maior, menor ou igual, maior ou igual. O montador escreve slt e um beq ou bne, usando $at.",
     "bge $t3, $t1, fim", Format::Pseudo, kToneBranch},
    {"j rótulo", "Salta para o rótulo, sempre.", "j laco", Format::Text, kToneJump},
    {"jal rótulo", "Chama uma função: salta e guarda em $ra o endereço da instrução seguinte.", "jal fib", Format::Text, kToneJump},
    {"jr $s", "Salta para o endereço em $s. jr $ra volta de uma função.", "jr $ra", Format::Text, kToneJump},
    {"Mundo: syscall", "", "", Format::Text, kToneWorld},
    {"syscall", "Pede algo de fora da CPU, pelo código em $v0: 1 mostra o número em $a0, 11 mostra o caractere em $a0, 5 lê as chaves para $v0, "
     "10 termina o programa.", "li $v0, 1", Format::Text, kToneWorld},
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

constexpr float kHelpFormatWidth = 250.0f;

float helpRowHeight(SDL_Renderer* renderer, const InstructionHelp& row, float meaningWidth) {
    if (row.meaning[0] == '\0') return 40.0f;
    return std::max(54.0f, static_cast<float>(wrapped(renderer, Face::Regular, 14.0f, row.meaning, meaningWidth).size()) * 20.0f + 18.0f);
}

float helpMeaningWidth(int width) { return std::max(200.0f, static_cast<float>(width) - 48.0f - 250.0f - kHelpFormatWidth - 40.0f); }

std::span<const InstructionHelp> helpRows(bool mips) {
    return mips ? std::span<const InstructionHelp>(kMipsHelp) : std::span<const InstructionHelp>(kInstructionHelp);
}

float helpContentHeight(SDL_Renderer* renderer, int width, bool mips) {
    float total = 110.0f;
    for (const InstructionHelp& row : helpRows(mips)) total += helpRowHeight(renderer, row, helpMeaningWidth(width));
    return total + 30.0f;
}

void drawCodeHelp(SDL_Renderer* renderer, int width, int height, float scroll, bool mips) {
    const SDL_FRect area{0.0f, kCodeBarHeight, static_cast<float>(width), static_cast<float>(height) - kCodeBarHeight};
    fillRect(renderer, area, kBackground);
    const SDL_Rect clip{0, static_cast<int>(area.y), width, static_cast<int>(area.h)};
    SDL_SetRenderClipRect(renderer, &clip);
    const float left = 24.0f;
    const float meaningX = left + 250.0f;
    const float meaningWidth = helpMeaningWidth(width);
    const float formatX = meaningX + meaningWidth + 40.0f;
    float y = area.y + 20.0f - scroll;
    write(renderer, Face::Medium, 22.0f, left, y + 22.0f, "Todas as instruções", kText);
    const std::vector<std::string> intro = wrapped(renderer, Face::Regular, 14.0f,
                                                   mips ? "Cada instrução vira uma palavra de 32 bits na ROM, a partir de 0x00400000. Os 6 bits de cima (op) "
                                                          "dizem o tipo; nas instruções R (op 0) o funct, nos 6 de baixo, diz qual conta. Abra Blocos para "
                                                          "ver os bits de cada uma."
                                                        : "Cada linha do programa vira uma palavra de 16 bits na ROM. Os 4 bits de cima (op) dizem qual é a "
                                                          "instrução; o resto traz registradores e números. À direita, como os 16 bits de cada uma se "
                                                          "dividem, nas mesmas cores do Raio-X.",
                                                   static_cast<float>(width) - 48.0f);
    for (std::size_t l = 0; l < intro.size() && l < 2; ++l) write(renderer, Face::Regular, 14.0f, left, y + 48.0f + 20.0f * static_cast<float>(l), intro[l], kTextDim);
    y += 110.0f;
    for (const InstructionHelp& row : helpRows(mips)) {
        const float rowHeight = helpRowHeight(renderer, row, meaningWidth);
        if (y + rowHeight < area.y || y > area.y + area.h) {
            y += rowHeight;
            continue;
        }
        if (row.meaning[0] == '\0') {
            fillRound(renderer, {left, y + 14.0f, 4.0f, 18.0f}, 2, row.tone);
            write(renderer, Face::Medium, 16.0f, left + 14.0f, y + 29.0f, row.syntax, row.tone);
            fillRect(renderer, {left, y + rowHeight - 2.0f, static_cast<float>(width) - 48.0f, 1.0f}, kRule);
            y += rowHeight;
            continue;
        }
        write(renderer, Face::Mono, 14.5f, left, y + 22.0f, row.syntax, row.tone);
        write(renderer, Face::Mono, 12.5f, left, y + 42.0f, row.example, kTextDim);
        const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 14.0f, row.meaning, meaningWidth);
        for (std::size_t l = 0; l < lines.size(); ++l) write(renderer, Face::Regular, 14.0f, meaningX, y + 22.0f + 20.0f * static_cast<float>(l), lines[l], kText);
        const std::vector<FormatPiece> pieces = formatPieces(row.format);
        if (!pieces.empty()) {
            float x = formatX;
            const float unit = kHelpFormatWidth / 16.0f;
            for (const FormatPiece& piece : pieces) {
                const float w = unit * static_cast<float>(piece.bits);
                const SDL_FRect box{x, y + 9.0f, w - 2.0f, 20.0f};
                fillRound(renderer, box, 3, mixed(kBackground, piece.color, piece.name[0] == '\0' ? 0.22f : 0.4f));
                frameRound(renderer, box, 3, 1.0f, piece.color);
                const std::string bits = std::to_string(piece.bits);
                write(renderer, Face::Mono, 10.5f, box.x + (box.w - textWidth(renderer, Face::Mono, 10.5f, bits)) / 2.0f, box.y + 14.0f, bits, kText);
                const char* label = piece.name[0] == '\0' ? "-" : piece.name;
                const float labelWidth = textWidth(renderer, Face::Mono, 10.5f, label);
                if (labelWidth <= w) write(renderer, Face::Mono, 10.5f, box.x + (box.w - labelWidth) / 2.0f, box.y + 34.0f, label, piece.color);
                x += w;
            }
        } else if (row.format == Format::Pseudo) {
            write(renderer, Face::Regular, 12.5f, formatX, y + 24.0f, "pseudo-instrução", kTextDim);
        }
        fillRect(renderer, {left, y + rowHeight - 1.0f, static_cast<float>(width) - 48.0f, 1.0f}, Color{30, 40, 44});
        y += rowHeight;
    }
    SDL_SetRenderClipRect(renderer, nullptr);
}

}  // namespace

float codeHelpMaxScroll(SDL_Renderer* renderer, int width, int height, bool mips) {
    return std::max(0.0f, helpContentHeight(renderer, width, mips) - (static_cast<float>(height) - kCodeBarHeight));
}

namespace {

void drawCodeBlocks32(SDL_Renderer* renderer, const CodeView& view, const SDL_FRect& area);  // with the 32-bit computer

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
    constexpr float kBlock = 56.0f;
    const int fit = std::max(1, static_cast<int>((area.h - 40.0f) / (kBlock + 6.0f)));
    const int count = static_cast<int>(view.program.size());
    int first = 0;
    if (view.runningAddress >= 0) first = std::clamp(view.runningAddress - fit / 3, 0, std::max(0, count - fit));
    float y = area.y;
    for (int i = first; i < count && i < first + fit; ++i) {
        const bool running = i == view.runningAddress;
        const SDL_FRect r{area.x, y, area.w, kBlock};
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
            fillRound(renderer, box, 3, mixed(kBackground, field.color, field.name[0] == '\0' ? 0.22f : 0.36f));
            frameRound(renderer, box, 3, 1.0f, field.color);
            for (int bit = field.high; bit >= field.low; --bit) {
                const char digit[2] = {((trace.word >> bit) & 1u) != 0 ? '1' : '0', '\0'};
                const float cx = bx + cell * static_cast<float>(field.high - bit) + cell / 2.0f - 1.0f;
                write(renderer, Face::Mono, 10.5f, cx - textWidth(renderer, Face::Mono, 10.5f, digit) / 2.0f, box.y + 13.0f, digit, kText);
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
    layout.help = {w - 392.0f, 5.0f, 100.0f, 28.0f};
    layout.close = {w - 96.0f, 5.0f, 84.0f, 28.0f};
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
    drawButton(renderer, layout.help, view.help ? "Voltar" : "Ajuda", view.help ? kPanelActive : kPanelButton);
    if (view.help) drawCodeHelp(renderer, width, height, view.helpScroll, view.mips);
}

namespace {

constexpr float kHomeCardWidth = 178.0f;
constexpr float kHomeCardHeight = 118.0f;

struct HomeCard {
    HomeChoice choice;
    Icon icon;
    const char* title;
    const char* line;
};

constexpr HomeCard kHomeCards[] = {
    {HomeChoice::Lessons, Icon::Home, "Aulas", ""},
    {HomeChoice::Continue, Icon::Play, "Continuar", "Volta ao último circuito"},
    {HomeChoice::New, Icon::Plus, "Novo circuito", "Uma bancada vazia"},
    {HomeChoice::Example, Icon::Step, "Exemplos", "Circuitos e programas"},
    {HomeChoice::Computers, Icon::ChevronRight, "Computadores", "16 e 32 bits"},
    {HomeChoice::Open, Icon::ChevronRight, "Abrir arquivo", "Um projeto salvo"},
};
constexpr int kHomeCardCount = static_cast<int>(std::size(kHomeCards));

constexpr const char* kComingModes[][2] = {
    {"Puzzles", "Desafios com estrelas"},
    {"Laboratório", "Medir e experimentar"},
};

// The computers' page: one card for each computer, wide enough to say what it has, and a way back.
struct ComputerCard {
    HomeChoice choice;
    const char* title;
    const char* lines[3];
    bool ready;
};

constexpr ComputerCard kComputerCards[] = {
    {HomeChoice::Computer16, "16 bits",
     {"O computador do curso: 8 registradores,", "instruções de 16 bits, ROM e RAM.", "Programe a ROM e veja rodar no Raio-X."},
     true},
    {HomeChoice::Computer32, "32 bits · MIPS",
     {"32 registradores ($t0, $s0, $sp...), lw, sw,", "la, li, jal e syscall, como nas aulas.", "Programe em MIPS. Raio-X em breve."},
     true},
};
constexpr int kComputerCardCount = static_cast<int>(std::size(kComputerCards));
constexpr float kComputerCardWidth = 340.0f;

SDL_FRect computerCard(int index, int width, int height) {
    const float total = static_cast<float>(kComputerCardCount) * kComputerCardWidth + static_cast<float>(kComputerCardCount - 1) * 20.0f;
    const float left = (static_cast<float>(width) - total) / 2.0f;
    const float top = static_cast<float>(height) / 2.0f - 44.0f;
    return {left + static_cast<float>(index) * (kComputerCardWidth + 20.0f), top, kComputerCardWidth, kHomeCardHeight + 30.0f};
}

SDL_FRect computersBackRect(int width, int height) {
    const SDL_FRect first = computerCard(0, width, height);
    return {first.x, first.y + first.h + 22.0f, 120.0f, 34.0f};
}

SDL_FRect homeCard(int index, int width, int height) {
    const float total = static_cast<float>(kHomeCardCount) * kHomeCardWidth + static_cast<float>(kHomeCardCount - 1) * 16.0f;
    const float left = (static_cast<float>(width) - total) / 2.0f;
    const float top = static_cast<float>(height) / 2.0f - 70.0f;
    return {left + static_cast<float>(index) * (kHomeCardWidth + 16.0f), top, kHomeCardWidth, kHomeCardHeight};
}

// --- The lesson list: the acts on the left, the chosen act's readings and exercises on the right -----

constexpr float kLessonCardHeight = 54.0f;  // the gallery shares these
constexpr float kLessonCardGap = 8.0f;
constexpr float kActListWidth = 300.0f;
constexpr float kActRowHeight = 50.0f;
constexpr float kListTop = 124.0f;
constexpr float kExerciseHeight = 54.0f;
constexpr float kReadingCardHeight = 70.0f;

struct ListItem {
    LessonListKind kind;
    int index;
    SDL_FRect rect;
    const char* text = nullptr;  // for a heading: kind None
};

SDL_FRect actRowRect(int act) {
    return {24.0f, kListTop + static_cast<float>(act) * (kActRowHeight + 5.0f), kActListWidth, kActRowHeight};
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
    constexpr const char* kLines[] = {
        "Clique ou arraste uma peça da biblioteca",  "Clique em um pino para ligar um fio",
        "Clique no vazio fixa um ponto do fio",      "Arraste do meio de um fio para derivar",
        "Shift: fio em L      Enter: ponta solta",   "Arraste no vazio: seleciona",
        "Botão direito ou do meio: move a vista",    "Roda: zoom      F: enquadrar",
        "R: girar      Delete ou Backspace: apagar", "Ctrl+C, X e V: copiar, recortar, colar",
        "Ctrl+Z e Y: desfazer, refazer",             "+ e -: entradas      [ e ]: tamanho",
        ", e .: bits",                               "Espaço: rodar ou pausar      N: passo do clock",
        "Clique duplo em um chip: abre",             "Ctrl+S salvar   Ctrl+O abrir   Ctrl+N novo",
        "T: tabela      P: perfis      F1: esta ajuda",  "ROM: Editar programa abre o assembler",
    };
    dimScreen(renderer, width, height);
    const float boxWidth = 440;
    const float boxHeight = static_cast<float>(std::size(kLines)) * 21.0f + 96.0f;
    const float x = (static_cast<float>(width) - boxWidth) / 2.0f;
    const float y = (static_cast<float>(height) - boxHeight) / 2.0f;
    drawDialog(renderer, {x, y, boxWidth, boxHeight});
    write(renderer, Face::Medium, 16, x + 20, y + 32, "Atalhos", kText);
    float line = y + 58;
    for (const char* text : kLines) {
        write(renderer, Face::Regular, kUiText, x + 20, line, text, kTextDim);
        line += 21;
    }
    write(renderer, Face::Regular, 11.5f, x + 20, line + 8, "Fontes Ubuntu, © Canonical Ltd. Ubuntu Font Licence 1.0: design.ubuntu.com/font", kTextOff);
}

}  // namespace

SDL_FRect profileRow(std::size_t row, std::size_t rows, int width, int height) {
    const float boxHeight = static_cast<float>(rows) * kProfileRowHeight + 120.0f;
    const float left = (static_cast<float>(width) - kProfileBoxWidth) / 2.0f;
    const float top = (static_cast<float>(height) - boxHeight) / 2.0f;
    return {left + 12.0f, top + 44.0f + static_cast<float>(row) * kProfileRowHeight, kProfileBoxWidth - 24.0f,
            kProfileRowHeight - 4.0f};
}

void drawProfiles(SDL_Renderer* renderer, const ProfileList& list, int width, int height) {
    const std::size_t rows = list.names.size();
    const SDL_FRect first = profileRow(0, rows, width, height);
    const float boxHeight = static_cast<float>(rows) * kProfileRowHeight + 120.0f;
    const SDL_FRect box{first.x - 12.0f, first.y - 44.0f, kProfileBoxWidth, boxHeight};
    dimScreen(renderer, width, height);
    drawDialog(renderer, box);
    write(renderer, Face::Medium, 16, box.x + 16, box.y + 30, "Perfis nesta máquina", kText);

    for (std::size_t i = 0; i < rows; ++i) {
        const SDL_FRect row = profileRow(i, rows, width, height);
        fillRound(renderer, row, 6, i == list.current ? kPanelActive : kPanelButton);
        write(renderer, Face::Regular, kUiText, row.x + 12, row.y + 18, list.names[i], kText);
    }

    const float y = box.y + boxHeight - 66.0f;
    write(renderer, Face::Regular, 12.5f, box.x + 16, y + 10, "Novo perfil: digite o nome e tecle Enter", kTextDim);
    const std::string line = list.typing + "_";
    write(renderer, Face::Regular, kUiText, box.x + 16, y + 30, line, kText);
    write(renderer, Face::Regular, 12.5f, box.x + 16, y + 50, "Esc fecha", kTextOff);
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

HomeChoice homeAt(float x, float y, int width, int height, bool computers) {
    if (inside(homeLanguageRect(width, i18n::Language::Portuguese), x, y)) return HomeChoice::Portuguese;
    if (inside(homeLanguageRect(width, i18n::Language::English), x, y)) return HomeChoice::English;
    if (computers) {
        for (int i = 0; i < kComputerCardCount; ++i) {
            if (inside(computerCard(i, width, height), x, y)) return kComputerCards[i].choice;
        }
        return inside(computersBackRect(width, height), x, y) ? HomeChoice::Back : HomeChoice::None;
    }
    for (int i = 0; i < kHomeCardCount; ++i) {
        if (inside(homeCard(i, width, height), x, y)) return kHomeCards[i].choice;
    }
    return HomeChoice::None;
}

namespace {
SDL_Texture* figureTexture(SDL_Renderer* renderer, TheoryFigure (*make)(), int w, int h, bool redraw);  // below
void drawComputerCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY);
void drawHomeCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const std::string& lessonsLine);
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

void drawHome(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const std::string& lessonsLine, bool computers) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    drawHomeBackdrop(renderer, width, height);
    const SDL_FRect first = homeCard(0, width, height);
    const SDL_FRect last = homeCard(kHomeCardCount - 1, width, height);
    // A sheet of dark glass under the menu, so the circuits behind never get in the way of reading.
    const SDL_FRect glass{first.x - 36.0f, first.y - 150.0f, last.x + last.w - first.x + 72.0f, kHomeCardHeight + 150.0f + 176.0f};
    fillRound(renderer, glass, 22, kBackground, 0.86f);
    frameRound(renderer, glass, 22, 1.0f, mixed(kBackground, kWirePreview, 0.3f));
    drawChipMark(renderer, {first.x + 26.0f, first.y - 86.0f}, 46.0f);
    write(renderer, Face::Medium, 46, first.x + 66.0f, first.y - 70.0f, "Bancada", kText);
    write(renderer, Face::Regular, 16, first.x + 68.0f, first.y - 44.0f, "Do LED ao computador, montando cada peça", kWirePreview);
    if (computers) {
        drawComputerCards(renderer, width, height, mouseX, mouseY);
    } else {
        drawHomeCards(renderer, width, height, mouseX, mouseY, lessonsLine);
    }
    write(renderer, Face::Regular, 12, first.x, static_cast<float>(height) - 24.0f,
          computers ? "Esc: voltar ao menu" : "Esc ou Enter: continuar  ·  F1 na bancada: atalhos", kTextOff);
    drawHomeLanguages(renderer, width, mouseX, mouseY);
}

namespace {

void drawComputerCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY) {
    const SDL_FRect first = computerCard(0, width, height);
    write(renderer, Face::Medium, 18, first.x, first.y - 18.0f, "Escolha um computador para programar", kText);
    for (int i = 0; i < kComputerCardCount; ++i) {
        const ComputerCard& card = kComputerCards[i];
        const SDL_FRect r = computerCard(i, width, height);
        const bool hovered = inside(r, mouseX, mouseY) && card.ready;
        // The 32-bit one is still being built: shown, so its place is known, but not open yet.
        fillRound(renderer, r, 12, hovered ? kPanelHover : kPanelRaised, card.ready ? 1.0f : 0.6f);
        frameRound(renderer, r, 12, hovered ? 1.6f : 1.0f, hovered ? kToneProgram : card.ready ? kBodyEdge : kRule);
        drawChipMark(renderer, {r.x + 34.0f, r.y + 36.0f}, 34.0f);
        write(renderer, Face::Medium, 20, r.x + 64.0f, r.y + 44.0f, card.title, card.ready ? kText : kTextDim);
        for (int line = 0; line < 3; ++line) {
            const bool last = line == 2;
            const Color color = !card.ready && last ? kToneProgram : card.ready ? kTextDim : kTextOff;
            write(renderer, Face::Regular, 13, r.x + 20.0f, r.y + 84.0f + 20.0f * static_cast<float>(line),
                  fitted(renderer, Face::Regular, 13, card.lines[line], r.w - 34.0f), color);
        }
    }
    const SDL_FRect back = computersBackRect(width, height);
    const bool hovered = inside(back, mouseX, mouseY);
    fillRound(renderer, back, 8, hovered ? kPanelHover : kPanelButton);
    const float w = textWidth(renderer, Face::Medium, kUiText, "Voltar");
    write(renderer, Face::Medium, kUiText, back.x + (back.w - w) / 2.0f, back.y + 22.0f, "Voltar", kText);
}

void drawHomeCards(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const std::string& lessonsLine) {
    const SDL_FRect first = homeCard(0, width, height);
    for (int i = 0; i < kHomeCardCount; ++i) {
        const HomeCard& card = kHomeCards[i];
        const SDL_FRect r = homeCard(i, width, height);
        const bool hovered = inside(r, mouseX, mouseY);
        const bool lessonsCard = card.choice == HomeChoice::Lessons;
        // The lessons are the way in for someone new, so their card stands out.
        fillRound(renderer, r, 12, lessonsCard ? kPanelActive : hovered ? kPanelHover : kPanelRaised);
        frameRound(renderer, r, 12, hovered || lessonsCard ? 1.6f : 1.0f, hovered || lessonsCard ? kWirePreview : kBodyEdge);
        drawIcon(renderer, card.icon, {r.x + 28.0f, r.y + 30.0f}, 22, lessonsCard ? kText : kWirePreview);
        write(renderer, Face::Medium, 15, r.x + 18.0f, r.y + 72.0f, card.title, kText);
        const std::string line = lessonsCard ? lessonsLine : card.line;
        write(renderer, Face::Regular, 12.5f, r.x + 18.0f, r.y + 94.0f, fitted(renderer, Face::Regular, 12.5f, line, r.w - 30.0f), lessonsCard ? kText : kTextDim);
    }
    const float total = static_cast<float>(kHomeCardCount) * kHomeCardWidth + static_cast<float>(kHomeCardCount - 1) * 16.0f;
    const float y = first.y + kHomeCardHeight + 46.0f;
    write(renderer, Face::Medium, 13, first.x, y, "Em breve", kTextOff);
    constexpr int kComing = static_cast<int>(std::size(kComingModes));
    for (int i = 0; i < kComing; ++i) {
        const float w = (total - 12.0f * static_cast<float>(kComing - 1)) / static_cast<float>(kComing);
        const SDL_FRect r{first.x + static_cast<float>(i) * (w + 12.0f), y + 12.0f, w, 58.0f};
        frameRound(renderer, r, 10, 1, kRule);
        write(renderer, Face::Medium, 14, r.x + 16.0f, r.y + 25.0f, kComingModes[i][0], kTextDim);
        write(renderer, Face::Regular, 12.5f, r.x + 16.0f, r.y + 44.0f, kComingModes[i][1], kTextOff);
    }
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

float galleryGap() { return galleryGroups().size() > 6 ? 10.0f : 16.0f; }

float galleryColumnWidth(int width) {
    const auto groups = static_cast<float>(galleryGroups().size());
    return std::min(280.0f, (static_cast<float>(width) - 48.0f - galleryGap() * (groups - 1.0f)) / groups);
}

float galleryLeft(int width) {
    const auto groups = static_cast<float>(galleryGroups().size());
    return (static_cast<float>(width) - groups * galleryColumnWidth(width) - (groups - 1.0f) * galleryGap()) / 2.0f;
}

// Cards get shorter when the fullest group would run past the bottom of the window.
float galleryCardHeight(int height) {
    std::vector<int> rows(galleryGroups().size(), 0);
    for (const GalleryItem& item : galleryItems()) ++rows[static_cast<std::size_t>(item.group)];
    const int most = std::max(1, *std::max_element(rows.begin(), rows.end()));
    const float room = static_cast<float>(height) - 150.0f - 40.0f;
    return std::clamp(room / static_cast<float>(most) - kLessonCardGap, 34.0f, kLessonCardHeight);
}

SDL_FRect galleryCardRect(int index, int width, int height) {
    const GalleryItem& item = galleryItems()[static_cast<std::size_t>(index)];
    int row = 0;
    for (int i = 0; i < index; ++i) row += galleryItems()[static_cast<std::size_t>(i)].group == item.group ? 1 : 0;
    const float column = galleryColumnWidth(width);
    const float card = galleryCardHeight(height);
    return {galleryLeft(width) + static_cast<float>(item.group) * (column + galleryGap()),
            150.0f + static_cast<float>(row) * (card + kLessonCardGap), column, card};
}

}  // namespace

int galleryAt(float x, float y, int width, int height) {
    if (inside(lessonBackRect(), x, y)) return kGalleryBack;
    for (int i = 0; i < static_cast<int>(galleryItems().size()); ++i) {
        if (inside(galleryCardRect(i, width, height), x, y)) return i;
    }
    return -1;
}

void drawGallery(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY) {
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const SDL_FRect back = lessonBackRect();
    fillRound(renderer, back, 8, inside(back, mouseX, mouseY) ? kPanelHover : kPanelButton);
    drawIcon(renderer, Icon::Home, {back.x + 20.0f, back.y + back.h / 2.0f}, 16, kText);
    write(renderer, Face::Medium, kUiText, back.x + 38.0f, back.y + 22.0f, "Início", kText);

    const float left = galleryLeft(width);
    write(renderer, Face::Medium, 30, left, 92.0f, "Exemplos", kText);
    write(renderer, Face::Regular, 15, left + textWidth(renderer, Face::Medium, 30, "Exemplos") + 22.0f, 90.0f,
          "Circuitos e programas prontos para abrir, mexer e entender", kTextDim);
    const float column = galleryColumnWidth(width);
    for (std::size_t g = 0; g < galleryGroups().size(); ++g) {
        write(renderer, Face::Medium, 14, left + static_cast<float>(g) * (column + galleryGap()), 136.0f,
              fitted(renderer, Face::Medium, 14, galleryGroups()[g], column), kWirePreview);
    }
    for (int i = 0; i < static_cast<int>(galleryItems().size()); ++i) {
        const GalleryItem& item = galleryItems()[static_cast<std::size_t>(i)];
        const SDL_FRect r = galleryCardRect(i, width, height);
        const bool hovered = inside(r, mouseX, mouseY);
        fillRound(renderer, r, 10, hovered ? kPanelHover : kPanelRaised);
        frameRound(renderer, r, 10, 1, hovered ? kWirePreview : kBodyEdge);
        const SDL_FRect badge{r.x + 9.0f, r.y + (r.h - 24.0f) / 2.0f, 24.0f, 24.0f};
        // On a short card, the two lines move up to fit.
        const float lift = (kLessonCardHeight - r.h) * 0.5f;
        fillRound(renderer, badge, 6, item.program >= 0 ? kBodyChip : kBodyBox);
        drawIcon(renderer, item.program >= 0 ? Icon::Step : Icon::Play, {badge.x + 12.0f, badge.y + 12.0f}, 13, kWirePreview);
        // A title too long for one line takes the second, in place of the description.
        const float textX = r.x + 41.0f;
        const float room = r.w - 49.0f;
        const std::vector<std::string> title = wrapped(renderer, Face::Medium, 13.0f, item.title, room);
        if (title.size() <= 1) {
            write(renderer, Face::Medium, 13.0f, textX, r.y + 24.0f - lift, fitted(renderer, Face::Medium, 13.0f, item.title, room), kText);
            write(renderer, Face::Regular, 11.5f, textX, r.y + 42.0f - lift * 1.4f, fitted(renderer, Face::Regular, 11.5f, item.line, room), kTextDim);
        } else {
            write(renderer, Face::Medium, 13.0f, textX, r.y + 23.0f - lift, title[0], kText);
            std::string rest = title[1];
            for (std::size_t l = 2; l < title.size(); ++l) rest += " " + title[l];
            write(renderer, Face::Medium, 13.0f, textX, r.y + 40.0f - lift * 1.4f, fitted(renderer, Face::Medium, 13.0f, rest, room), kText);
        }
    }
    write(renderer, Face::Regular, 12, left, static_cast<float>(height) - 24.0f,
          "Abrir um exemplo guarda o que estava na bancada: Continuar, na tela inicial, traz de volta. Esc volta ao início.", kTextOff);
}

LessonListHit lessonListAt(float x, float y, int width, int height, const LessonListView& view) {
    if (inside(lessonBackRect(), x, y)) return {LessonListKind::Back, -1};
    if (inside(lessonResetRect(width), x, y)) return {LessonListKind::Reset, -1};
    if (inside(lessonUnlockRect(width), x, y)) return {LessonListKind::Unlock, -1};
    for (int a = 0; a < static_cast<int>(lessonActs().size()); ++a) {
        if (inside(actRowRect(a), x, y)) return {LessonListKind::Act, a};
    }
    const SDL_FRect area = lessonContentArea(width, height);
    if (!inside(area, x, y) || view.finished == nullptr || !(view.unlocked || actOpen(view.act, *view.finished))) return {};
    for (const ListItem& item : actContent(width, height, view)) {
        if (item.kind != LessonListKind::None && inside(item.rect, x, y)) return {item.kind, item.index};
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
        const SDL_FRect r = actRowRect(a);
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
        write(renderer, Face::Medium, 14.0f, textX, r.y + 21.0f, fitted(renderer, Face::Medium, 14.0f, name, room), open ? kText : kTextOff);
        write(renderer, Face::Regular, 11.5f, textX, r.y + 38.0f, fitted(renderer, Face::Regular, 11.5f, act.line, room), open ? kTextDim : kTextOff);
        const std::string count = std::to_string(done) + "/" + std::to_string(total);
        const float countWidth = textWidth(renderer, Face::Medium, 12.0f, count);
        write(renderer, Face::Medium, 12.0f, r.x + r.w - 14.0f - countWidth, r.y + 21.0f, count, open ? kTextDim : kTextOff);
        // How far along, as a thin bar under the count.
        const SDL_FRect bar{r.x + r.w - 46.0f, r.y + 30.0f, 32.0f, 4.0f};
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
        ++exerciseNumber;
        fillRound(renderer, r, 10, hovered ? kPanelHover : kPanelRaised);
        frameRound(renderer, r, 10, 1, hovered ? kWirePreview : done ? kPanelActive : open ? kBodyEdge : kRule);
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
        write(renderer, Face::Regular, 11.0f, textX, r.y + 20.0f, "Exercício", open ? kTextOff : kTextOff);
        write(renderer, Face::Medium, 13.5f, textX, r.y + 39.0f, fitted(renderer, Face::Medium, 13.5f, lesson.title, room), open ? kText : kTextOff);
    }
    SDL_SetRenderClipRect(renderer, nullptr);
    write(renderer, Face::Regular, 12, 24.0f, static_cast<float>(height) - 14.0f,
          "Leia a aula e faça os exercícios dela. Cada ato abre quando o anterior termina. Esc volta ao início.", kTextOff);
}

namespace {

// --- A reading -----------------------------------------------------------------------------------------

constexpr float kReadingBarHeight = 64.0f;
constexpr float kReadingText = 16.0f;
constexpr float kReadingLine = 26.0f;

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

// Each block's top, from the top of the content, and the whole height; drawing and clicking share it.
struct ReadingLayout {
    std::vector<float> tops;
    float height = 0.0f;
    float buttonTop = 0.0f;
};

ReadingLayout readingLayout(SDL_Renderer* renderer, int width, const Topic& topic) {
    const float column = readingColumnWidth(width);
    ReadingLayout layout;
    float y = 92.0f;  // under the title and the line
    for (const TheoryBlock& block : topic.blocks) {
        switch (block.kind) {
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
                y += static_cast<float>(wrapped(renderer, Face::Regular, 15.5f, block.text, column - 44.0f).size()) * 24.0f + 30.0f + 18.0f;
                break;
            case TheoryBlockKind::Table:
                y += 4.0f;
                layout.tops.push_back(y);
                y += static_cast<float>(tableCells(block.text).size()) * 32.0f + 22.0f;
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
        }
    }
    layout.buttonTop = y + 20.0f;
    layout.height = layout.buttonTop + 120.0f;
    return layout;
}

SDL_FRect readingBottomButton(int width, const ReadingLayout& layout, float scroll) {
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    return {left, kReadingBarHeight + layout.buttonTop + 44.0f - scroll, 280.0f, 42.0f};
}

// The figures: a live board each, drawn to its own texture, built once.
struct FigureCache {
    std::unique_ptr<Session> session;
    SDL_Texture* texture = nullptr;
    int w = 0;
    int h = 0;
};

SDL_Texture* figureTexture(SDL_Renderer* renderer, TheoryFigure (*make)(), int w, int h, bool redraw) {
    static std::map<TheoryFigure (*)(), FigureCache> cache;
    FigureCache& entry = cache[make];
    if (!entry.session) {
        TheoryFigure figure = make();
        entry.session = std::make_unique<Session>();
        entry.session->adoptProjectChips(std::move(figure.chips));
        entry.session->adopt(std::move(figure.board), figure.closed);
    }
    const bool fresh = entry.texture == nullptr || entry.w != w || entry.h != h;
    if (fresh) {
        if (entry.texture != nullptr) SDL_DestroyTexture(entry.texture);
        entry.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
        entry.w = w;
        entry.h = h;
    }
    if (entry.texture == nullptr) return nullptr;
    if (!fresh && !redraw) return entry.texture;
    // The board fitted in, with a margin, never blown up past a comfortable size.
    const Rect extent = entry.session->board().extent();
    // Names stand to the right of the last parts and above the first ones: room for them too.
    const float boardW = static_cast<float>(extent.right - extent.left) + 7.0f;
    const float boardH = static_cast<float>(extent.bottom - extent.top) + 4.0f;
    Camera camera;
    camera.cellPixels = std::clamp(std::min(static_cast<float>(w) / boardW, static_cast<float>(h) / boardH), 3.0f, 16.0f);
    camera.originX = static_cast<float>(extent.left) - 1.5f - (static_cast<float>(w) / camera.cellPixels - boardW) / 2.0f;
    camera.originY = static_cast<float>(extent.top) - 2.5f - (static_cast<float>(h) / camera.cellPixels - boardH) / 2.0f;
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

ReadingButton readingAt(SDL_Renderer* renderer, float x, float y, int width, int, const ReadingView& view) {
    if (inside(readingBackRect(), x, y)) return ReadingButton::Back;
    if (inside(readingExercisesRect(width), x, y)) return ReadingButton::Exercises;
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    const SDL_FRect bottom = readingBottomButton(width, readingLayout(renderer, width, topic), view.scroll);
    if (y > kReadingBarHeight && inside(bottom, x, y)) return ReadingButton::Exercises;
    return ReadingButton::None;
}

float readingMaxScroll(SDL_Renderer* renderer, int width, int height, const ReadingView& view) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    return std::max(0.0f, readingLayout(renderer, width, topic).height - (static_cast<float>(height) - kReadingBarHeight));
}

void drawReading(SDL_Renderer* renderer, int width, int height, float mouseX, float mouseY, const ReadingView& view) {
    const Topic& topic = topics()[static_cast<std::size_t>(view.topic)];
    setColor(renderer, kBackground);
    SDL_RenderClear(renderer);
    const float column = readingColumnWidth(width);
    const float left = (static_cast<float>(width) - column) / 2.0f;
    const ReadingLayout layout = readingLayout(renderer, width, topic);
    const float top = kReadingBarHeight - view.scroll;
    const SDL_Rect clip{0, static_cast<int>(kReadingBarHeight), width, height - static_cast<int>(kReadingBarHeight)};
    SDL_SetRenderClipRect(renderer, &clip);

    const std::string actTitle(i18n::tr(lessonActs()[static_cast<std::size_t>(topic.act)].title));
    write(renderer, Face::Medium, 13.0f, left, top + 30.0f,
          i18n::format("{}  ·  Aula {}", {actTitle.substr(0, actTitle.find(':')), std::to_string(view.topic + 1)}), kWirePreview);
    write(renderer, Face::Medium, 30.0f, left, top + 66.0f, topic.title, kText);
    write(renderer, Face::Regular, 15.0f, left, top + 90.0f, topic.line, kTextDim);

    for (std::size_t b = 0; b < topic.blocks.size(); ++b) {
        const TheoryBlock& block = topic.blocks[b];
        const float y = top + layout.tops[b] + 8.0f;
        switch (block.kind) {
            case TheoryBlockKind::Heading:
                write(renderer, Face::Medium, 21.0f, left, y + 22.0f, block.text, kText);
                break;
            case TheoryBlockKind::Text: {
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, kReadingText, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, kReadingText, left, y + 18.0f + kReadingLine * static_cast<float>(l), lines[l], Color{214, 224, 232});
                }
                break;
            }
            case TheoryBlockKind::Tip: {
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 15.5f, block.text, column - 44.0f);
                const SDL_FRect box{left, y, column, static_cast<float>(lines.size()) * 24.0f + 30.0f};
                fillRound(renderer, box, 10, kPanelRaised);
                fillRound(renderer, {box.x, box.y, 5.0f, box.h}, 2, kWirePreview);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, 15.5f, left + 24.0f, y + 26.0f + 24.0f * static_cast<float>(l), lines[l], kText);
                }
                break;
            }
            case TheoryBlockKind::Table: {
                const std::vector<std::vector<std::string>> rows = tableCells(block.text);
                std::vector<float> widths;
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    const std::vector<std::string>& row = rows[r];
                    for (std::size_t c = 0; c < row.size(); ++c) {
                        if (widths.size() <= c) widths.push_back(0.0f);
                        widths[c] = std::max(widths[c], textWidth(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, row[c]) + 28.0f);
                    }
                }
                float total = 0.0f;
                for (const float w : widths) total += w;
                const float stretch = total < column ? 1.0f : column / total;
                float tableWidth = 0.0f;
                for (float& w : widths) tableWidth += (w *= stretch);
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    const float rowY = y + 32.0f * static_cast<float>(r);
                    fillRect(renderer, {left, rowY, tableWidth, 32.0f}, r == 0 ? kPanelActive : r % 2 == 0 ? kPanelRaised : kPanel);
                    float x = left;
                    for (std::size_t c = 0; c < rows[r].size() && c < widths.size(); ++c) {
                        write(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, x + 14.0f, rowY + 21.0f,
                              fitted(renderer, r == 0 ? Face::Medium : Face::Regular, 14.5f, rows[r][c], widths[c] - 20.0f), r == 0 ? kText : Color{214, 224, 232});
                        x += widths[c];
                    }
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
                drawWordStrip(renderer, traceInstruction(state), strip);
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
                }
                frameRound(renderer, frame, 10, 1.2f, kBodyEdge);
                const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 13.5f, block.text, column);
                for (std::size_t l = 0; l < lines.size(); ++l) {
                    write(renderer, Face::Regular, 13.5f, left, frame.y + frame.h + 22.0f + 20.0f * static_cast<float>(l), lines[l], kTextDim);
                }
                break;
            }
        }
    }

    // The way on: the exercises of this reading.
    const float endY = top + layout.buttonTop;
    fillRect(renderer, {left, endY, column, 1.0f}, kRule);
    std::string names;
    for (const char* id : topic.exercises) {
        const int lesson = lessonIndex(id);
        if (lesson < 0) continue;
        names += (names.empty() ? "" : ", ") + std::string(i18n::tr(lessons()[static_cast<std::size_t>(lesson)].title));
    }
    write(renderer, Face::Regular, 14.0f, left, endY + 28.0f, i18n::format("Agora, os exercícios desta aula: {}.", {names}), kTextDim);
    const SDL_FRect bottom = readingBottomButton(width, layout, view.scroll);
    fillRound(renderer, bottom, 10, inside(bottom, mouseX, mouseY) ? kWirePreview : kPanelActive);
    write(renderer, Face::Medium, 15.0f, bottom.x + 20.0f, bottom.y + 27.0f, "Começar os exercícios", inside(bottom, mouseX, mouseY) ? kBackground : kText);
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
    // How far down the reading is.
    const float maxScroll = std::max(1.0f, layout.height - (static_cast<float>(height) - kReadingBarHeight));
    if (layout.height > static_cast<float>(height) - kReadingBarHeight) {
        fillRect(renderer, {0.0f, kReadingBarHeight - 3.0f, static_cast<float>(width) * std::clamp(view.scroll / maxScroll, 0.0f, 1.0f), 3.0f}, kWirePreview);
    }
}

namespace {

SDL_FRect lessonButtonRect(const SDL_FRect& area, int row) {
    return {area.x + area.w - 196.0f, area.y + 16.0f + static_cast<float>(row) * 42.0f, 180.0f, 34.0f};
}

}  // namespace

// The strip's buttons, top to bottom: a programming lesson starts with the one that opens the code.
std::vector<LessonButton> lessonStripButtons(const LessonStrip& strip) {
    std::vector<LessonButton> buttons;
    if (strip.program) buttons.push_back(LessonButton::Program);
    buttons.push_back(strip.passed && strip.hasNext ? LessonButton::Next : LessonButton::Check);
    buttons.insert(buttons.end(), {LessonButton::Hint, LessonButton::Restart, LessonButton::List});
    return buttons;
}

LessonButton lessonStripAt(const SDL_FRect& area, const LessonStrip& strip, float x, float y) {
    const std::vector<LessonButton> buttons = lessonStripButtons(strip);
    for (std::size_t row = 0; row < buttons.size(); ++row) {
        if (inside(lessonButtonRect(area, static_cast<int>(row)), x, y)) return buttons[row];
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
    const std::vector<LessonButton> buttons = lessonStripButtons(strip);
    for (std::size_t row = 0; row < buttons.size(); ++row) {
        const SDL_FRect r = lessonButtonRect(area, static_cast<int>(row));
        const LessonButton button = buttons[row];
        const bool primary = button == LessonButton::Next || (row == 0 && !(strip.passed && strip.hasNext));
        if (button == LessonButton::Program) {
            fillRound(renderer, r, 8, inside(r, mouseX, mouseY) ? mixed(kBodyProgram, kToneProgram, 0.35f) : kBodyProgram);
            frameRound(renderer, r, 8, 1.4f, kToneProgram);
        } else {
            fillRound(renderer, r, 8, primary ? kPanelActive : inside(r, mouseX, mouseY) ? kPanelHover : kPanelButton);
            if (primary && inside(r, mouseX, mouseY)) frameRound(renderer, r, 8, 1.4f, kWirePreview);
        }
        const char* label = button == LessonButton::Program ? "Programar a ROM"
                            : button == LessonButton::Next  ? "Próxima aula"
                            : button == LessonButton::Check ? "Verificar"
                            : button == LessonButton::Hint  ? "Dica"
                            : button == LessonButton::Restart ? "Recomeçar"
                                                              : "Todas as aulas";
        const float w = textWidth(renderer, Face::Medium, kUiText, label);
        write(renderer, Face::Medium, kUiText, r.x + (r.w - w) / 2.0f, r.y + 22.0f, label, kText);
    }

    // What the lesson is about, and what to do.
    const float x = area.x + 18.0f;
    const float width = area.w - 236.0f;
    char where[96];
    std::snprintf(where, sizeof where, i18n::tr("Aula %d de %zu  ·  %s"), strip.index + 1, lessons().size(),
                  i18n::tr(lessonActs()[static_cast<std::size_t>(lesson.act)].title));
    write(renderer, Face::Regular, 12.5f, x, area.y + 22.0f, where, kTextDim);
    write(renderer, Face::Medium, 19, x, area.y + 46.0f, lesson.title, kText);
    if (strip.passed) drawCheckMark(renderer, {x + textWidth(renderer, Face::Medium, 19, lesson.title) + 18.0f, area.y + 40.0f}, 16.0f, kKnobOn);

    float y = area.y + 70.0f;
    const std::vector<std::string> story = wrapped(renderer, Face::Regular, 13.5f, lesson.story, width);
    for (std::size_t i = 0; i < story.size() && i < 3; ++i) {
        write(renderer, Face::Regular, 13.5f, x, y, i == 2 && story.size() > 3 ? fitted(renderer, Face::Regular, 13.5f, story[i] + " …", width) : story[i], kText);
        y += 18.0f;
    }
    y += 6.0f;
    int lines = 0;
    for (std::size_t s = 0; s < lesson.steps.size() && lines < 4; ++s) {
        char label[8];
        std::snprintf(label, sizeof label, "%zu.", s + 1);
        write(renderer, Face::Medium, 13, x, y, label, kWirePreview);
        for (const std::string& line : wrapped(renderer, Face::Regular, 13, lesson.steps[s], width - 22.0f)) {
            if (lines >= 4) break;
            write(renderer, Face::Regular, 13, x + 22.0f, y, line, kTextDim);
            y += 17.0f;
            ++lines;
        }
    }

    // The verdict, a problem or a hint, at the bottom.
    if (!strip.message.empty()) {
        const Color color = strip.passed ? kKnobOn : strip.problem ? kWarning : kWirePreview;
        const std::vector<std::string> message = wrapped(renderer, Face::Medium, 13.5f, strip.message, width);
        float messageY = area.y + area.h - 14.0f - 18.0f * static_cast<float>(std::min<std::size_t>(message.size(), 2) - 1);
        for (std::size_t i = 0; i < message.size() && i < 2; ++i) {
            write(renderer, Face::Medium, 13.5f, x, messageY, message[i], color);
            messageY += 18.0f;
        }
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
void drawWordStrip32(SDL_Renderer* renderer, std::uint32_t word, const SDL_FRect& area) {
    const std::uint32_t op = word >> 26;
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
        fillRound(renderer, box, 3, mixed(kBackground, piece.color, 0.36f));
        frameRound(renderer, box, 3, 1.0f, piece.color);
        for (int bit = piece.high; bit >= piece.low; --bit) {
            const char digit[2] = {((word >> bit) & 1u) != 0 ? '1' : '0', '\0'};
            const float cx = x + cell * static_cast<float>(piece.high - bit) + cell / 2.0f - 1.0f;
            write(renderer, Face::Mono, 10.0f, cx - textWidth(renderer, Face::Mono, 10.0f, digit) / 2.0f, box.y + area.h / 2.0f + 4.0f, digit, kText);
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
    constexpr float kBlock = 60.0f;
    const int fit = std::max(1, static_cast<int>((area.h - 40.0f) / (kBlock + 6.0f)));
    const int count = static_cast<int>(view.program32.size());
    int first = 0;
    if (view.runningAddress >= 0) first = std::clamp(view.runningAddress - fit / 3, 0, std::max(0, count - fit));
    float y = area.y;
    for (int i = first; i < count && i < first + fit; ++i) {
        const bool running = i == view.runningAddress;
        const SDL_FRect r{area.x, y, area.w, kBlock};
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

    const float top = area.y + 60.0f;
    const float left = area.x + 12.0f;
    const float width = area.w - 24.0f;
    const float rightWidth = std::min(360.0f, width * 0.36f);
    const float mainWidth = width - rightWidth - 12.0f;

    // The instruction about to run, and its bits.
    const SDL_FRect now = titledBox(renderer, {left, top, mainWidth, 98.0f}, "Instrução no PC");
    const float pcX = now.x + std::max(120.0f, textWidth(renderer, Face::Medium, 13.0f, "Instrução no PC") + 16.0f);
    write(renderer, Face::Mono, 14.0f, pcX, now.y - 10.0f, "PC = 0x" + hex32(state.pc), kWirePreview);
    write(renderer, Face::Mono, 16.0f, now.x, now.y + 14.0f, state.text, instructionTone32(state.instruction));
    const std::string word = "0x" + hex32(state.instruction);
    write(renderer, Face::Mono, 13.0f, now.x + now.w - textWidth(renderer, Face::Mono, 13.0f, word), now.y + 14.0f, word, kTextDim);
    drawWordStrip32(renderer, state.instruction, {now.x, now.y + 24.0f, now.w, 18.0f});

    // The 32 registers, in four columns; what the last instruction changed stands out.
    const SDL_FRect regs = titledBox(renderer, {left, top + 110.0f, mainWidth, area.y + area.h - top - 122.0f}, "Registradores");
    const float column = regs.w / 4.0f;
    const float row = std::min(24.0f, regs.h / 8.0f);
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

    // Beside them: the console, the data and the stack.
    const float rx = left + mainWidth + 12.0f;
    const float consoleHeight = 150.0f;
    drawConsole(renderer, console, titledBox(renderer, {rx, top, rightWidth, consoleHeight}, "Console"));
    const float rest = area.y + area.h - 12.0f - (top + consoleHeight + 12.0f);
    const SDL_FRect data = titledBox(renderer, {rx, top + consoleHeight + 12.0f, rightWidth, rest * 0.55f}, "Dados (.data)");
    const auto dataRows = static_cast<std::size_t>(std::max(1.0f, data.h / 17.0f));
    for (std::size_t i = 0; i < state.data.size() && i < dataRows; ++i) {
        const float y = data.y + 12.0f + 17.0f * static_cast<float>(i);
        write(renderer, Face::Mono, 12.0f, data.x, y, hex32(mips::kDataBase + static_cast<std::uint32_t>(4 * i)), kWirePreview);
        write(renderer, Face::Mono, 12.0f, data.x + 80.0f, y, hex32(state.data[i]), state.data[i] != 0 ? kKnobOn : kTextOff);
        const std::string decimal = std::to_string(static_cast<std::int32_t>(state.data[i]));
        write(renderer, Face::Regular, 12.0f, data.x + data.w - textWidth(renderer, Face::Regular, 12.0f, decimal), y, decimal, kTextDim);
    }
    const SDL_FRect stack = titledBox(renderer, {rx, data.y + data.h + 18.0f, rightWidth, rest * 0.45f - 12.0f}, "Pilha (de $sp para cima)");
    const auto stackRows = static_cast<std::size_t>(std::max(1.0f, stack.h / 17.0f));
    for (std::size_t i = 0; i < state.stack.size() && i < stackRows; ++i) {
        const float y = stack.y + 12.0f + 17.0f * static_cast<float>(i);
        write(renderer, Face::Mono, 12.0f, stack.x, y, hex32(state.registers[mips::kSp] + static_cast<std::uint32_t>(4 * i)), kWirePreview);
        write(renderer, Face::Mono, 12.0f, stack.x + 80.0f, y, hex32(state.stack[i]), state.stack[i] != 0 ? kKnobOn : kTextOff);
    }
}

void drawXray32(SDL_Renderer* renderer, const SDL_FRect& area) {
    fillRect(renderer, area, kBackground);
    const std::vector<std::string> lines = wrapped(renderer, Face::Regular, 16.0f,
                                                   "O Raio-X do computador de 32 bits ainda está sendo montado. Por enquanto, siga o programa pelo "
                                                   "Painel e pelo depurador, ou abra a CPU na Placa: o circuito inteiro já está lá.",
                                                   std::min(640.0f, area.w - 40.0f));
    float y = area.y + area.h / 2.0f - 20.0f * static_cast<float>(lines.size()) / 2.0f;
    for (const std::string& line : lines) {
        write(renderer, Face::Regular, 16.0f, area.x + (area.w - textWidth(renderer, Face::Regular, 16.0f, line)) / 2.0f, y, line, kTextDim);
        y += 22.0f;
    }
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

void drawInterface(SDL_Renderer* renderer, const Session& session, const PaletteView& palette, int width, int height,
                   const InterfaceText& text) {
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
    if (text.computer32 != nullptr && text.view == kViewXray) drawXray32(renderer, computerArea);
    if (text.view == kViewXray && text.computer != nullptr) {
        drawXray(renderer, *text.computer, computerArea, text.xrayStage, text.xrayProgress, text.mouseX, text.mouseY);
    }

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

    fillRound(renderer, kSearchBox, 7, kPanelButton);
    if (text.searching) frameRound(renderer, kSearchBox, 7, 1, kWirePreview);
    drawIcon(renderer, Icon::Search, {kSearchBox.x + 16.0f, kSearchBox.y + kSearchBox.h / 2.0f}, 15, kTextDim);
    const std::string searchText = text.searching ? palette.filter + "_" : palette.filter;
    write(renderer, Face::Regular, kUiText, kSearchBox.x + 30.0f, kSearchBox.y + 20.0f, searchText.empty() ? "Buscar peça" : searchText, searchText.empty() ? kTextOff : kText);

    drawPalette(renderer, session, paletteGroups(session, palette.filter, text.limit), palette, height, text.mouseX, text.mouseY);
    drawToolbar(renderer, session, width, rightEdge, text.mouseX, text.mouseY);
    if (text.lesson != nullptr && text.view == kViewBoard) {
        drawLessonStrip(renderer, {kPanelWidth, kToolbarHeight, rightEdge - kPanelWidth, kLessonStripHeight}, *text.lesson, text.mouseX, text.mouseY);
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
        constexpr const char* kViews[kViewCount] = {"Placa", "Painel", "Raio-X"};
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

    const char* hint = "Arraste peças da biblioteca. Clique em um pino para ligar. F1: atalhos.";
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

    if (table) drawRightPanel(renderer, session, width, height, text.tableScroll, text.mouseX, text.mouseY);
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

}  // namespace bancada::app
