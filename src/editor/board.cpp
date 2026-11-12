#include "editor/board.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bancada::editor {

namespace {

// In PartKind order.
// The logic output is "Luz": "LED" is the electric part that looks like one. In the same way "Bateria"
// is the one that looks like a battery, and "Fonte" the schematic symbol.
constexpr const char* kNames[] = {"NAND", "Entrada", "Luz", "NOT", "AND", "OR", "NOR", "XOR", "XNOR", "Saída 0/1", "Caixa",
                                  "Separador", "Juntador", "Constante", "Clock", "Chip", "Rótulo",
                                  "Fonte", "Terra", "Resistor", "Diodo", "Diodo LED", "Chave", "NPN", "PNP", "NMOS", "PMOS", "Capacitor",
                                  "LED", "Bateria", "Botão", "Display 7 segmentos", "Teclado"};
static_assert(std::size(kNames) == kPartKindCount);

constexpr int kGateWidth = 4;
constexpr int kBoxWidth = 10;
constexpr int kBundleWidth = 3;

// Evenly spaced pins, centred on a side of this height.
int pinY(int height, int count, int index) { return (height - 2 * count) / 2 + 1 + 2 * index; }

bool hasInterface(const Part& part) { return part.kind == PartKind::Box || part.kind == PartKind::Chip; }

// Two pins, one each side, apart from the ground's one and the transistors' three.
unsigned electricPins(PartKind kind) {
    if (kind == PartKind::Ground) return 1;
    return isTransistor(kind) ? 3 : 2;
}

// A pin before scale and rotation.
Point basePin(const Part& part, unsigned pin) {
    const int width = baseWidth(part);
    const int height = baseHeight(part);
    const auto index = static_cast<int>(pin);
    const auto inputs = static_cast<int>(inputCount(part));
    const auto outputs = static_cast<int>(outputCount(part));
    if (part.kind == PartKind::Display7) return {0, 1 + 2 * index};
    if (part.kind == PartKind::Keyboard) return {8, 1 + 2 * index};
    if (isElectric(part.kind)) {
        if (part.kind == PartKind::Ground) return {1, 0};
        if (isTransistor(part.kind)) return index == 0 ? Point{0, 2} : index == 1 ? Point{3, 0} : Point{3, 4};
        if (part.kind == PartKind::LedBulb) return index == 0 ? Point{0, 3} : Point{4, 3};  // the legs, under the dome
        return index == 0 ? Point{0, 1} : Point{4, 1};
    }
    if (hasInterface(part) || isBundle(part.kind)) {
        if (index < inputs) return {0, pinY(height, inputs, index)};
        return {width, pinY(height, outputs, index - inputs)};
    }
    if (isGate(part.kind)) {
        if (pin == part.inputs) return {kGateWidth, height / 2};
        return {0, height / 2 - (part.inputs - 1) + 2 * index};
    }
    return inputs > 0 ? Point{0, height / 2} : Point{width, height / 2};
}

bool shapeIsValid(const Part& part) {
    if ((part.kind == PartKind::Box) != (part.box != kNoBox)) return false;
    if (part.box > boxCount()) return false;
    if (part.width < 1 || part.width > kMaxBits) return false;
    if (part.inputs < minInputs(part.kind) || part.inputs > maxInputs(part.kind)) return false;
    if (part.scale < 1 || part.scale > kMaxScale) return false;
    if (isBundle(part.kind) && part.inputs > part.width) return false;
    if (part.width > 1 && !hasWidth(part)) return false;
    if (part.kind == PartKind::Tunnel && (part.label.empty() || part.label.size() > 32)) return false;
    if (const std::optional<electric::Quantity> quantity = valueQuantity(part.kind)) {
        const electric::ValueRange range = electric::rangeOf(*quantity);
        if (part.value < range.least || part.value > range.most) return false;
    } else if (isElectric(part.kind) && part.value != 0) {
        return false;
    }
    if (part.kind == PartKind::Chip && (part.chip == 0 || part.interface == nullptr)) return false;
    if (part.kind != PartKind::Chip && part.chip != 0) return false;
    if (hasAddress(part)) {
        if (part.address < 1 || part.address > kMaxPartAddress) return false;
    } else if (part.address != 0) {
        return false;
    }
    if (!part.source.empty() && part.rom == nullptr) return false;
    if (part.rom != nullptr) {
        // A ROM holds its program; a RAM or a register file may start with words in it: a program's .data,
        // the stack pointer.
        if (part.kind != PartKind::Box) return false;
        if (!boxSpec(part.box).hasContents && part.box != boxFromKey("ram") && part.box != boxFromKey("regfile")) return false;
        if (part.rom->size() > (std::size_t{1} << part.address)) return false;
    }
    if (part.kind == PartKind::Box) return true;
    return true;
}

// Fills in what follows from the other fields, once they're known to be valid.
void settleInterface(Part& part) {
    if (part.kind == PartKind::Box) part.interface = boxInterface(part.box, part.width, part.address);
}

template <typename Item>
auto findById(Item* begin, Item* end, std::uint32_t id) {
    Item* found = std::lower_bound(begin, end, id, [](const Item& item, std::uint32_t wanted) { return item.id < wanted; });
    return found != end && found->id == id ? found : nullptr;
}

template <typename Item>
Item* findIn(std::vector<Item>& items, std::uint32_t id) {
    return findById(items.data(), items.data() + items.size(), id);
}

template <typename Item>
const Item* findIn(const std::vector<Item>& items, std::uint32_t id) {
    return findById(items.data(), items.data() + items.size(), id);
}

bool isStraight(Point a, Point b) { return a.x == b.x || a.y == b.y; }

bool onSegment(Point a, Point b, Point p) {
    if (a.x == b.x) return p.x == a.x && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
    if (a.y == b.y) return p.y == a.y && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x);
    return false;
}

bool isPinOf(const Terminal& terminal, std::span<const PartId> parts) {
    return terminal.kind == Terminal::Kind::Pin && std::find(parts.begin(), parts.end(), terminal.id) != parts.end();
}

// Moves the first point of a path, keeping every segment straight: the first corner slides along the
// segment after it, or, on a straight wire, a new corner appears.
void moveFront(std::vector<Point>& path, Point to) {
    path[0] = to;
    if (path.size() >= 3) {
        Point& corner = path[1];
        if (corner.y == path[2].y) {
            corner.x = to.x;
        } else {
            corner.y = to.y;
        }
    } else if (!isStraight(to, path[1])) {
        path.insert(path.begin() + 1, Point{path[1].x, to.y});
    }
    simplifyPath(path);
}

void moveBack(std::vector<Point>& path, Point to) {
    std::reverse(path.begin(), path.end());
    moveFront(path, to);
    std::reverse(path.begin(), path.end());
}

}  // namespace

bool operator==(const Part& a, const Part& b) {
    const bool sameFields = a.id == b.id && a.kind == b.kind && a.position == b.position && a.rotation == b.rotation &&
                            a.inputs == b.inputs && a.scale == b.scale && a.box == b.box && a.width == b.width &&
                            a.address == b.address && a.value == b.value && a.chip == b.chip && a.label == b.label &&
                            a.source == b.source;
    if (!sameFields) return false;
    if (a.rom == nullptr || b.rom == nullptr) return a.rom == b.rom;
    return *a.rom == *b.rom;
}

const char* partName(const Part& part) {
    if (hasInterface(part) && part.interface != nullptr) return part.interface->name.c_str();
    return kNames[static_cast<std::size_t>(part.kind)];
}

bool hasWidth(const Part& part) {
    if (part.kind == PartKind::Box) return boxSpec(part.box).defaultWidth > 1;
    return isGate(part.kind) || isBundle(part.kind) || part.kind == PartKind::Toggle || part.kind == PartKind::Probe ||
           part.kind == PartKind::Constant || part.kind == PartKind::Tunnel;
}

bool hasAddress(const Part& part) { return part.kind == PartKind::Box && part.box != kNoBox && boxSpec(part.box).defaultAddress > 0; }

Part makePart(PartKind kind, Point position, std::uint8_t rotation, std::uint8_t inputs, std::uint8_t scale, BoxId box) {
    Part part;
    part.kind = kind;
    part.position = position;
    part.rotation = rotation;
    part.inputs = inputs == 0 ? minInputs(kind) : inputs;
    part.scale = scale;
    part.box = box;
    if (kind == PartKind::Box && box != kNoBox && box <= boxCount()) {
        const BoxSpec& spec = boxSpec(box);
        part.width = static_cast<std::uint8_t>(spec.defaultWidth);
        part.address = static_cast<std::uint8_t>(spec.defaultAddress);
        settleInterface(part);
    }
    if (isBundle(kind)) part.width = std::max(part.width, static_cast<std::uint8_t>(8));
    if (kind == PartKind::Tunnel) part.label = "sinal";
    if (isBattery(kind)) part.value = 5'000;                        // 5 V
    if (kind == PartKind::Resistor) part.value = 1'000'000;         // 1 kΩ
    if (kind == PartKind::Capacitor) part.value = 10'000'000;       // 10 µF
    return part;
}

unsigned hexDigits(unsigned bits) { return (bits + 3) / 4; }

// The boxes that are screens, drawn bigger than a box so their picture fits.
bool isScreenBox(const Part& part, std::string_view key) { return part.kind == PartKind::Box && part.box != kNoBox && boxSpec(part.box).key == key; }

int baseWidth(const Part& part) {
    if (isScreenBox(part, "terminal")) return 26;
    if (isScreenBox(part, "led_matrix")) return 18;
    if (part.kind == PartKind::Keyboard) return 8;
    if (hasInterface(part)) return kBoxWidth;
    if (part.kind == PartKind::Display7) return 8;
    if (isElectric(part.kind)) return part.kind == PartKind::Ground ? 2 : 4;
    if (part.kind == PartKind::Tunnel) return std::max(4, static_cast<int>(part.label.size()) + 2);
    if (isGate(part.kind)) return kGateWidth;
    if (isBundle(part.kind)) return kBundleWidth;
    if (part.width == 1) return 3;
    if (part.kind == PartKind::Toggle) return 2 * std::min<int>(part.width, 8) + 1;
    if (part.kind == PartKind::Probe || part.kind == PartKind::Constant) return std::max(3, static_cast<int>(hexDigits(part.width)) + 2);
    return 3;
}

int baseHeight(const Part& part) {
    if (isScreenBox(part, "terminal")) return 12;
    if (part.kind == PartKind::Keyboard) return 4;
    if (hasInterface(part)) {
        if (part.interface == nullptr) return 4;
        return std::max<int>(4, 2 * static_cast<int>(std::max(part.interface->inputs.size(), part.interface->outputs.size())));
    }
    if (isBundle(part.kind)) return std::max(4, 2 * static_cast<int>(part.inputs));
    if (isGate(part.kind)) return std::max(4, 2 * static_cast<int>(part.inputs));
    if (isTransistor(part.kind) || part.kind == PartKind::LedBulb) return 4;
    if (part.kind == PartKind::Display7) return 16;
    if (part.kind == PartKind::Toggle) return 2 * ((part.width + 7) / 8);
    return 2;
}

Rect boundsOf(const Part& part) {
    const int width = baseWidth(part) * part.scale;
    const int height = baseHeight(part) * part.scale;
    const bool sideways = (part.rotation & 1) != 0;
    return {part.position.x, part.position.y, part.position.x + (sideways ? height : width),
            part.position.y + (sideways ? width : height)};
}

unsigned inputCount(const Part& part) {
    if (part.kind == PartKind::Display7) return 8;
    if (isElectric(part.kind)) return electricPins(part.kind);  // no direction: current goes either way
    if (hasInterface(part)) return part.interface == nullptr ? 0 : static_cast<unsigned>(part.interface->inputs.size());
    if (isGate(part.kind)) return part.inputs;
    if (part.kind == PartKind::Splitter) return 1;
    if (part.kind == PartKind::Joiner) return part.inputs;
    return isOutputPart(part.kind) ? 1 : 0;
}

unsigned outputCount(const Part& part) {
    if (isElectric(part.kind) || part.kind == PartKind::Display7) return 0;
    if (part.kind == PartKind::Keyboard) return 2;
    if (hasInterface(part)) return part.interface == nullptr ? 0 : static_cast<unsigned>(part.interface->outputs.size());
    if (part.kind == PartKind::Splitter) return part.inputs;
    return isOutputPart(part.kind) ? 0 : 1;
}

PinRole pinRole(const Part& part, unsigned pin) { return pin < inputCount(part) ? PinRole::Input : PinRole::Output; }

Point pinPosition(const Part& part, unsigned pin) {
    const Point base = basePin(part, pin);
    const Point p{base.x * part.scale, base.y * part.scale};
    const int width = baseWidth(part) * part.scale;
    const int height = baseHeight(part) * part.scale;
    switch (part.rotation & 3) {
        case 0: return part.position + p;
        case 1: return part.position + Point{height - p.y, p.x};
        case 2: return part.position + Point{width - p.x, height - p.y};
        default: return part.position + Point{p.y, width - p.x};
    }
}

unsigned branchBits(const Part& part, unsigned branch) {
    const unsigned even = part.width / part.inputs;
    const unsigned extra = part.width % part.inputs;
    return even + (branch >= part.inputs - extra ? 1u : 0u);
}

unsigned pinBits(const Part& part, unsigned pin) {
    if (hasInterface(part)) {
        const unsigned inputs = inputCount(part);
        return pin < inputs ? part.interface->inputs[pin].bits : part.interface->outputs[pin - inputs].bits;
    }
    if (part.kind == PartKind::Splitter) return pin == 0 ? part.width : branchBits(part, pin - 1);
    if (part.kind == PartKind::Keyboard) return pin == 0 ? 7 : 1;
    if (part.kind == PartKind::Joiner) return pin == part.inputs ? part.width : branchBits(part, pin);
    return part.width;
}

std::optional<electric::Quantity> valueQuantity(PartKind kind) {
    if (isBattery(kind)) return electric::Quantity::Volts;
    if (kind == PartKind::Resistor) return electric::Quantity::Ohms;
    if (kind == PartKind::Capacitor) return electric::Quantity::Farads;
    return std::nullopt;
}

const char* pinName(const Part& part, unsigned pin) {
    if (isElectric(part.kind)) {
        static constexpr const char* kBattery[] = {"+", "-"};
        static constexpr const char* kDiode[] = {"A", "K"};
        static constexpr const char* kBipolar[] = {"B", "C", "E"};
        static constexpr const char* kMos[] = {"G", "D", "S"};
        if (pin >= electricPins(part.kind)) return "";
        if (isBattery(part.kind)) return kBattery[pin];
        if (part.kind == PartKind::Diode || isLed(part.kind)) return kDiode[pin];
        if (part.kind == PartKind::Npn || part.kind == PartKind::Pnp) return kBipolar[pin];
        if (part.kind == PartKind::Nmos || part.kind == PartKind::Pmos) return kMos[pin];
        return "";
    }
    if (part.kind == PartKind::Display7) {
        static constexpr const char* kSegments[] = {"a", "b", "c", "d", "e", "f", "g", "ponto"};
        return pin < 8 ? kSegments[pin] : "";
    }
    if (part.kind == PartKind::Keyboard) return pin == 0 ? "Código" : pin == 1 ? "Tecla" : "";
    if (!hasInterface(part)) return "";
    const unsigned inputs = inputCount(part);
    return pin < inputs ? part.interface->inputs[pin].name.c_str() : part.interface->outputs[pin - inputs].name.c_str();
}

int toggleBitAt(const Part& part, float x, float y) {
    if (part.kind != PartKind::Toggle || part.width < 2) return -1;
    const auto scale = static_cast<float>(part.scale);
    const float width = static_cast<float>(baseWidth(part)) * scale;
    const float height = static_cast<float>(baseHeight(part)) * scale;
    float localX = x - static_cast<float>(part.position.x);
    float localY = y - static_cast<float>(part.position.y);
    switch (part.rotation & 3) {
        case 1: { const float t = localX; localX = localY; localY = height - t; break; }
        case 2: localX = width - localX; localY = height - localY; break;
        case 3: { const float t = localX; localX = width - localY; localY = t; break; }
        default: break;
    }
    const int column = static_cast<int>(std::floor((localX / scale - 0.5f) / 2.0f));
    const int row = static_cast<int>(std::floor(localY / scale / 2.0f));
    if (column < 0 || column >= 8 || row < 0) return -1;
    const int fromTop = row * 8 + column;
    if (fromTop >= part.width) return -1;
    return part.width - 1 - fromTop;
}

void simplifyPath(std::vector<Point>& path) {
    if (path.size() < 2) return;
    std::vector<Point> out;
    out.reserve(path.size());
    for (const Point& point : path) {
        if (!out.empty() && out.back() == point) continue;
        while (out.size() >= 2) {
            const Point a = out[out.size() - 2];
            const Point b = out.back();
            const bool inLine = (a.x == b.x && b.x == point.x) || (a.y == b.y && b.y == point.y);
            if (!inLine) break;
            out.pop_back();
        }
        if (out.empty() || !(out.back() == point)) out.push_back(point);
    }
    if (out.size() < 2) out.push_back(out.back());
    path = std::move(out);
}

bool pathContains(const std::vector<Point>& path, Point point) {
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (onSegment(path[i], path[i + 1], point)) return true;
    }
    return false;
}

std::optional<Board> Board::restore(std::vector<Part> parts, std::vector<Joint> joints, std::vector<Wire> wires) {
    auto inRange = [](Point p) { return std::abs(p.x) <= kMaxCoordinate && std::abs(p.y) <= kMaxCoordinate; };
    auto idsAscend = [](const auto& items) {
        std::uint32_t last = 0;
        for (const auto& item : items) {
            if (item.id <= last) return false;
            last = item.id;
        }
        return true;
    };
    if (!idsAscend(parts) || !idsAscend(joints) || !idsAscend(wires)) return std::nullopt;

    // Every part claims its cells. Parts are small, so this stays linear on huge boards.
    std::unordered_set<std::uint64_t> taken;
    taken.reserve(parts.size() * 16);
    for (Part& part : parts) {
        if (part.rotation > 3 || static_cast<std::size_t>(part.kind) >= kPartKindCount) return std::nullopt;
        if (!shapeIsValid(part) || !inRange(part.position)) return std::nullopt;
        settleInterface(part);
        if (part.kind == PartKind::Chip) continue;  // a chip that grew may overlap: see updateChip
        const Rect bounds = boundsOf(part);
        for (int y = bounds.top; y < bounds.bottom; ++y) {
            for (int x = bounds.left; x < bounds.right; ++x) {
                const std::uint64_t cell = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                                           static_cast<std::uint32_t>(y);
                if (!taken.insert(cell).second) return std::nullopt;
            }
        }
    }
    for (const Joint& joint : joints) {
        if (!inRange(joint.position)) return std::nullopt;
    }

    Board board;
    board.parts_ = std::move(parts);
    board.joints_ = std::move(joints);

    std::vector<std::uint8_t> jointUsed(board.joints_.size(), 0);
    auto endAt = [&](const Terminal& terminal) -> std::optional<Point> {
        if (terminal.kind == Terminal::Kind::Joint) {
            const Joint* found = board.joint(terminal.id);
            if (found == nullptr) return std::nullopt;
            jointUsed[static_cast<std::size_t>(found - board.joints_.data())] = 1;
            return found->position;
        }
        const Part* found = board.part(terminal.id);
        if (found == nullptr || terminal.pin >= pinCount(*found)) return std::nullopt;
        return pinPosition(*found, terminal.pin);
    };
    for (const Wire& wire : wires) {
        if (wire.path.size() < 2) return std::nullopt;
        for (std::size_t i = 0; i < wire.path.size(); ++i) {
            if (!inRange(wire.path[i])) return std::nullopt;
            if (i > 0 && !isStraight(wire.path[i - 1], wire.path[i])) return std::nullopt;
        }
        const std::optional<Point> from = endAt(wire.from);
        const std::optional<Point> to = endAt(wire.to);
        if (!from || !to || !(*from == wire.path.front()) || !(*to == wire.path.back())) return std::nullopt;
    }
    if (std::find(jointUsed.begin(), jointUsed.end(), std::uint8_t{0}) != jointUsed.end()) return std::nullopt;

    board.wires_ = std::move(wires);
    board.nextPart_ = board.parts_.empty() ? 1 : board.parts_.back().id + 1;
    board.nextJoint_ = board.joints_.empty() ? 1 : board.joints_.back().id + 1;
    board.nextWire_ = board.wires_.empty() ? 1 : board.wires_.back().id + 1;
    return board;
}

const Part* Board::part(PartId id) const { return findIn(parts_, id); }
const Joint* Board::joint(JointId id) const { return findIn(joints_, id); }
const Wire* Board::wire(WireId id) const { return findIn(wires_, id); }

Point Board::terminalPosition(const Terminal& terminal) const {
    if (terminal.kind == Terminal::Kind::Pin) return pinPosition(*part(terminal.id), terminal.pin);
    return joint(terminal.id)->position;
}

bool Board::canPlace(const Part& candidate, std::span<const PartId> ignoring) const {
    const Rect bounds = boundsOf(candidate);
    for (const Part& other : parts_) {
        if (!boundsOf(other).overlaps(bounds)) continue;
        if (std::find(ignoring.begin(), ignoring.end(), other.id) == ignoring.end()) return false;
    }
    return true;
}

std::optional<PartId> Board::addPart(PartKind kind, Point position, std::uint8_t rotation, std::uint8_t inputs, std::uint8_t scale,
                                     BoxId box) {
    Part part = makePart(kind, position, static_cast<std::uint8_t>(rotation & 3), inputs, scale, box);
    if ((kind == PartKind::Box) != (box != kNoBox) || box > boxCount()) return std::nullopt;
    return addPart(std::move(part));
}

std::optional<PartId> Board::addPart(Part part) {
    if (!shapeIsValid(part)) return std::nullopt;
    settleInterface(part);
    if (!canPlace(part)) return std::nullopt;
    part.id = nextPart_++;
    const PartId id = part.id;
    parts_.push_back(std::move(part));
    return id;
}

Drop Board::dropAt(Point at) const {
    for (const Part& candidate : parts_) {
        if (!boundsOf(candidate).touches(at)) continue;
        const unsigned pins = pinCount(candidate);
        for (unsigned pin = 0; pin < pins; ++pin) {
            if (pinPosition(candidate, pin) == at) return {Drop::Kind::Pin, at, candidate.id, static_cast<std::uint8_t>(pin)};
        }
    }
    for (const Joint& candidate : joints_) {
        if (candidate.position == at) return {Drop::Kind::Joint, at, candidate.id, 0};
    }
    if (const Wire* found = wireAt(at)) return {Drop::Kind::Wire, at, found->id, 0};
    return {Drop::Kind::Empty, at, 0, 0};
}

const Part* Board::partAtCell(Point cell) const {
    for (const Part& candidate : parts_) {
        if (boundsOf(candidate).containsCell(cell)) return &candidate;
    }
    return nullptr;
}

const Wire* Board::wireAt(Point at) const {
    for (const Wire& candidate : wires_) {
        if (pathContains(candidate.path, at)) return &candidate;
    }
    return nullptr;
}

Terminal Board::splitWire(WireId id, Point at, WireId* secondPiece) {
    Wire& first = *findIn(wires_, id);
    if (first.path.front() == at) return first.from;
    if (first.path.back() == at) return first.to;

    std::size_t segment = 0;
    while (!onSegment(first.path[segment], first.path[segment + 1], at)) ++segment;

    const JointId jointId = nextJoint_++;
    joints_.push_back({jointId, at});
    const Terminal middle{Terminal::Kind::Joint, jointId, 0};

    Wire second;
    second.id = nextWire_++;
    second.from = middle;
    second.to = first.to;
    second.path.push_back(at);
    second.path.insert(second.path.end(), first.path.begin() + static_cast<std::ptrdiff_t>(segment + 1), first.path.end());
    simplifyPath(second.path);

    first.path.resize(segment + 1);
    first.path.push_back(at);
    simplifyPath(first.path);
    first.to = middle;

    if (secondPiece != nullptr) *secondPiece = second.id;
    wires_.push_back(std::move(second));
    return middle;
}

Terminal Board::resolve(const Drop& drop) {
    switch (drop.kind) {
        case Drop::Kind::Pin:
            return {Terminal::Kind::Pin, drop.id, drop.pin};
        case Drop::Kind::Joint:
            return {Terminal::Kind::Joint, drop.id, 0};
        case Drop::Kind::Wire:
            return splitWire(drop.id, drop.at, nullptr);
        default: {
            const JointId id = nextJoint_++;
            joints_.push_back({id, drop.at});
            return {Terminal::Kind::Joint, id, 0};
        }
    }
}

std::optional<WireId> Board::connect(const Drop& from, const Drop& to, std::vector<Point> path) {
    if (path.size() < 2 || !(path.front() == from.at) || !(path.back() == to.at)) return std::nullopt;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (!isStraight(path[i], path[i + 1])) return std::nullopt;
    }
    if (from.at == to.at) return std::nullopt;
    simplifyPath(path);

    const Terminal start = resolve(from);

    // If both ends are on one wire, the first split may have left the second point on the new piece.
    Drop end = to;
    if (end.kind == Drop::Kind::Wire && end.id == from.id && from.kind == Drop::Kind::Wire) {
        const Wire& original = *findIn(wires_, end.id);
        if (!pathContains(original.path, end.at)) {
            for (const Wire& candidate : wires_) {
                if (candidate.from == start && pathContains(candidate.path, end.at)) {
                    end.id = candidate.id;
                    break;
                }
            }
        }
    }
    const Terminal finish = resolve(end);

    Wire wire;
    wire.id = nextWire_++;
    wire.from = start;
    wire.to = finish;
    wire.path = std::move(path);
    const WireId id = wire.id;
    wires_.push_back(std::move(wire));
    return id;
}

void Board::tidyJoint(JointId id) {
    if (findIn(joints_, id) == nullptr) return;
    const Terminal self{Terminal::Kind::Joint, id, 0};

    std::vector<WireId> touching;
    for (const Wire& wire : wires_) {
        if (wire.from == self) touching.push_back(wire.id);
        if (wire.to == self) touching.push_back(wire.id);
    }

    if (touching.empty()) {
        joints_.erase(std::find_if(joints_.begin(), joints_.end(), [&](const Joint& j) { return j.id == id; }));
        return;
    }
    if (touching.size() != 2 || touching[0] == touching[1]) return;

    // Two wires meet here and nothing else does: they're one wire now.
    Wire& keep = *findIn(wires_, touching[0]);
    Wire other = *findIn(wires_, touching[1]);
    if (keep.to != self) {
        std::reverse(keep.path.begin(), keep.path.end());
        std::swap(keep.from, keep.to);
    }
    if (other.from != self) {
        std::reverse(other.path.begin(), other.path.end());
        std::swap(other.from, other.to);
    }
    keep.path.insert(keep.path.end(), other.path.begin() + 1, other.path.end());
    keep.to = other.to;
    simplifyPath(keep.path);

    const WireId gone = other.id;
    wires_.erase(std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == gone; }));
    joints_.erase(std::find_if(joints_.begin(), joints_.end(), [&](const Joint& j) { return j.id == id; }));
}

void Board::remove(std::span<const PartId> parts, std::span<const WireId> wires) {
    std::vector<JointId> touched;
    auto goes = [&](const Wire& wire) {
        return std::find(wires.begin(), wires.end(), wire.id) != wires.end() || isPinOf(wire.from, parts) ||
               isPinOf(wire.to, parts);
    };
    for (const Wire& wire : wires_) {
        if (!goes(wire)) continue;
        if (wire.from.kind == Terminal::Kind::Joint) touched.push_back(wire.from.id);
        if (wire.to.kind == Terminal::Kind::Joint) touched.push_back(wire.to.id);
    }
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(), goes), wires_.end());
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(),
                                [&](const Part& part) { return std::find(parts.begin(), parts.end(), part.id) != parts.end(); }),
                 parts_.end());

    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (JointId id : touched) tidyJoint(id);
}

void Board::dragWireEnds(std::span<const PartId> moved, Point delta) {
    for (Wire& wire : wires_) {
        const bool fromMoved = isPinOf(wire.from, moved);
        const bool toMoved = isPinOf(wire.to, moved);
        if (fromMoved && toMoved) {
            for (Point& point : wire.path) point = point + delta;
        } else if (fromMoved) {
            moveFront(wire.path, terminalPosition(wire.from));
        } else if (toMoved) {
            moveBack(wire.path, terminalPosition(wire.to));
        }
    }
}

bool Board::moveParts(std::span<const PartId> parts, Point delta) {
    for (PartId id : parts) {
        const Part* moving = part(id);
        if (moving == nullptr) return false;
        Part moved = *moving;
        moved.position = moved.position + delta;
        if (!canPlace(moved, parts)) return false;
    }
    for (PartId id : parts) findIn(parts_, id)->position = findIn(parts_, id)->position + delta;
    dragWireEnds(parts, delta);
    return true;
}

void Board::reattachWires(PartId id) {
    const std::array<PartId, 1> self{id};
    for (Wire& wire : wires_) {
        if (isPinOf(wire.from, self)) moveFront(wire.path, terminalPosition(wire.from));
        if (isPinOf(wire.to, self)) moveBack(wire.path, terminalPosition(wire.to));
    }
}

bool Board::rotatePart(PartId id) {
    Part* turning = findIn(parts_, id);
    if (turning == nullptr) return false;
    Part turned = *turning;
    turned.rotation = static_cast<std::uint8_t>((turned.rotation + 1) & 3);
    const std::array<PartId, 1> self{id};
    if (!canPlace(turned, self)) return false;
    *turning = turned;
    reattachWires(id);
    return true;
}

bool Board::reshapePart(PartId id, std::uint8_t inputs, std::uint8_t scale) {
    const Part* found = part(id);
    if (found == nullptr) return false;
    Part shape = *found;
    shape.inputs = inputs;
    shape.scale = scale;
    return reconfigurePart(id, std::move(shape));
}

bool Board::reconfigurePart(PartId id, Part shape) {
    const Part* found = part(id);
    if (found == nullptr || !shapeIsValid(shape)) return false;
    shape.id = id;
    shape.position = found->position;
    settleInterface(shape);
    const std::array<PartId, 1> self{id};
    if (!canPlace(shape, self)) return false;
    applyShape(id, shape);
    return true;
}

// Pins are inputs then outputs, so the same input keeps its number, and outputs shift with the number of
// inputs. Wires on pins that no longer exist go.
void Board::applyShape(PartId id, const Part& shape) {
    const Part old = *part(id);
    const unsigned oldInputs = inputCount(old);
    const unsigned newInputs = inputCount(shape);
    const unsigned newOutputs = outputCount(shape);
    const std::array<PartId, 1> self{id};

    const auto newPin = [&](unsigned pin) -> std::optional<unsigned> {
        if (pin < oldInputs) return pin < newInputs ? std::optional<unsigned>(pin) : std::nullopt;
        const unsigned output = pin - oldInputs;
        return output < newOutputs ? std::optional<unsigned>(newInputs + output) : std::nullopt;
    };

    std::vector<WireId> lost;
    for (const Wire& wire : wires_) {
        const auto gone = [&](const Terminal& t) { return isPinOf(t, self) && !newPin(t.pin).has_value(); };
        if (gone(wire.from) || gone(wire.to)) lost.push_back(wire.id);
    }
    remove({}, lost);

    for (Wire& wire : wires_) {
        if (isPinOf(wire.from, self)) wire.from.pin = static_cast<std::uint8_t>(*newPin(wire.from.pin));
        if (isPinOf(wire.to, self)) wire.to.pin = static_cast<std::uint8_t>(*newPin(wire.to.pin));
    }
    *findIn(parts_, id) = shape;
    reattachWires(id);
}

void Board::updateChip(ChipId chip, const InterfacePtr& interface) {
    std::vector<PartId> users;
    for (const Part& candidate : parts_) {
        if (candidate.kind == PartKind::Chip && candidate.chip == chip) users.push_back(candidate.id);
    }
    for (PartId id : users) {
        Part shape = *part(id);
        shape.interface = interface;
        applyShape(id, shape);
    }
}

Board Board::fragment(std::span<const PartId> parts, std::span<const WireId> wires) const {
    Board out = *this;
    std::vector<PartId> others;
    for (const Part& candidate : parts_) {
        if (std::find(parts.begin(), parts.end(), candidate.id) == parts.end()) others.push_back(candidate.id);
    }
    out.remove(others, {});

    // What's left may still have stubs that used to lead to something else. Cut them back until every
    // wire goes somewhere, which also clears away wires that had nothing to do with these parts.
    for (;;) {
        std::unordered_map<JointId, int> degree;
        for (const Wire& wire : out.wires_) {
            if (wire.from.kind == Terminal::Kind::Joint) ++degree[wire.from.id];
            if (wire.to.kind == Terminal::Kind::Joint) ++degree[wire.to.id];
        }
        const auto loose = [&](const Terminal& t) { return t.kind == Terminal::Kind::Joint && degree[t.id] < 2; };
        std::vector<WireId> cut;
        for (const Wire& wire : out.wires_) {
            if (std::find(wires.begin(), wires.end(), wire.id) != wires.end()) continue;
            if (loose(wire.from) || loose(wire.to)) cut.push_back(wire.id);
        }
        if (cut.empty()) return out;
        out.remove({}, cut);
    }
}

std::optional<Pasted> Board::paste(const Board& fragment, Point delta) {
    for (const Part& original : fragment.parts_) {
        Part moved = original;
        moved.position = moved.position + delta;
        if (!canPlace(moved)) return std::nullopt;
    }

    Pasted pasted;
    std::unordered_map<std::uint32_t, std::uint32_t> newPart;
    std::unordered_map<std::uint32_t, std::uint32_t> newJoint;
    for (const Part& original : fragment.parts_) {
        Part copy = original;
        copy.id = nextPart_++;
        copy.position = copy.position + delta;
        parts_.push_back(copy);
        newPart[original.id] = copy.id;
        pasted.parts.emplace_back(original.id, copy.id);
    }
    for (const Joint& original : fragment.joints_) {
        const JointId id = nextJoint_++;
        joints_.push_back({id, original.position + delta});
        newJoint[original.id] = id;
    }
    const auto renamed = [&](Terminal terminal) {
        terminal.id = terminal.kind == Terminal::Kind::Pin ? newPart.at(terminal.id) : newJoint.at(terminal.id);
        return terminal;
    };
    for (const Wire& original : fragment.wires_) {
        Wire copy = original;
        copy.id = nextWire_++;
        copy.from = renamed(original.from);
        copy.to = renamed(original.to);
        for (Point& point : copy.path) point = point + delta;
        pasted.wires.push_back(copy.id);
        wires_.push_back(std::move(copy));
    }
    return pasted;
}

Rect Board::extent() const {
    Rect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    const auto include = [&](const Rect& r) {
        box.left = std::min(box.left, r.left);
        box.top = std::min(box.top, r.top);
        box.right = std::max(box.right, r.right);
        box.bottom = std::max(box.bottom, r.bottom);
    };
    for (const Part& part : parts_) include(boundsOf(part));
    for (const Joint& joint : joints_) include({joint.position.x, joint.position.y, joint.position.x, joint.position.y});
    for (const Wire& wire : wires_) {
        for (const Point& p : wire.path) include({p.x, p.y, p.x, p.y});
    }
    return box;
}

namespace {

std::vector<PartId> inReadingOrder(const Board& board, bool (*wanted)(PartKind)) {
    std::vector<const Part*> found;
    for (const Part& part : board.parts()) {
        if (wanted(part.kind)) found.push_back(&part);
    }
    std::stable_sort(found.begin(), found.end(), [](const Part* a, const Part* b) {
        return a->position.y != b->position.y ? a->position.y < b->position.y : a->position.x < b->position.x;
    });
    std::vector<PartId> ids;
    ids.reserve(found.size());
    for (const Part* part : found) ids.push_back(part->id);
    return ids;
}

bool isToggle(PartKind kind) { return kind == PartKind::Toggle; }

}  // namespace

std::vector<PartId> inputsInOrder(const Board& board) { return inReadingOrder(board, isToggle); }

std::vector<PartId> outputsInOrder(const Board& board) { return inReadingOrder(board, isOutputPart); }

std::vector<PartId> lightsInOrder(const Board& board) {
    return inReadingOrder(board, [](PartKind kind) { return isLed(kind); });
}

// A to Z, then AA, AB... so that a board with many switches still gets a name for each.
std::string defaultInputName(std::size_t index) {
    std::string name;
    if (index >= 26) name += static_cast<char>('A' + index / 26 - 1);
    name += static_cast<char>('A' + index % 26);
    return name;
}

std::string defaultOutputName(std::size_t index, std::size_t count) {
    return count == 1 ? "S" : "S" + std::to_string(index + 1);
}

InterfacePtr makeChipInterface(const Board& board, const std::string& name) {
    auto made = std::make_shared<PartInterface>();
    made->name = name;
    const std::vector<PartId> inputs = inputsInOrder(board);
    const std::vector<PartId> outputs = outputsInOrder(board);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Part& part = *board.part(inputs[i]);
        made->inputs.push_back({part.label.empty() ? defaultInputName(i) : part.label, part.width});
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const Part& part = *board.part(outputs[i]);
        made->outputs.push_back({part.label.empty() ? defaultOutputName(i, outputs.size()) : part.label, part.width});
    }
    return made;
}

}  // namespace bancada::editor
