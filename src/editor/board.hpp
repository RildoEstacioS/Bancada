// What the player edits: parts on a grid, joints, and wires between them. This is the source of truth,
// the thing that gets saved. Netlists and simulators are derived from it and thrown away.
//
// The rule that shapes everything here: a wire's drawing never decides what it connects to. A wire is
// a path with two explicit ends, each one a pin or a joint. Paths may cross or run over anything and
// nothing happens; only a joint or a pin joins things. So boards can be as dense as the player likes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/boxes.hpp"
#include "electric/units.hpp"

namespace bancada::editor {

struct Point {
    int x = 0;
    int y = 0;
    friend bool operator==(const Point&, const Point&) = default;
};

inline Point operator+(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
inline Point operator-(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool containsCell(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
    bool touches(Point p) const { return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom; }
    bool overlaps(const Rect& o) const { return left < o.right && o.left < right && top < o.bottom && o.top < bottom; }
    bool holds(const Rect& o) const { return o.left >= left && o.right <= right && o.top >= top && o.bottom <= bottom; }
};

// Only ever add to the end: files name kinds as text, but the tables indexed by this follow this order.
enum class PartKind : std::uint8_t {
    Nand, Toggle, Led, Not, And, Or, Nor, Xor, Xnor, Probe, Box, Splitter, Joiner, Constant, Clock, Chip,
    Tunnel,  // connects to every other one on the board with the same name
    Battery, Ground, Resistor, Diode, Light, Switch, Npn, Pnp, Nmos, Pmos, Capacitor,  // electric: real voltages and currents
    LedBulb,      // an LED drawn the way it looks, a dome on two legs, for those who don't read schematics yet
    BatteryCell,  // a battery drawn the way it looks, a cell with its + cap; Battery is the schematic symbol
    Button,       // an input that's 1 only while it's held down, and 0 again when let go
    Display7,     // seven segments and a point that draw a digit: an input for each
    Keyboard,     // the real keyboard, while it's selected: the code of the last key, and whether one is down
};
inline constexpr std::size_t kPartKindCount = 33;

constexpr bool isGate(PartKind kind) {
    return kind == PartKind::Nand || kind == PartKind::Not || kind == PartKind::And || kind == PartKind::Or ||
           kind == PartKind::Nor || kind == PartKind::Xor || kind == PartKind::Xnor;
}
constexpr bool isOutputPart(PartKind kind) { return kind == PartKind::Led || kind == PartKind::Probe; }
constexpr bool isBundle(PartKind kind) { return kind == PartKind::Splitter || kind == PartKind::Joiner; }
constexpr bool isElectric(PartKind kind) { return kind >= PartKind::Battery && kind <= PartKind::BatteryCell; }
// The two ways to power a circuit, the schematic symbol and the battery drawn as it looks: the same part.
constexpr bool isBattery(PartKind kind) { return kind == PartKind::Battery || kind == PartKind::BatteryCell; }
// The two LEDs, the schematic one and the one drawn as it looks: the same part, electrically.
constexpr bool isLed(PartKind kind) { return kind == PartKind::Light || kind == PartKind::LedBulb; }
constexpr bool isTransistor(PartKind kind) {
    return kind == PartKind::Npn || kind == PartKind::Pnp || kind == PartKind::Nmos || kind == PartKind::Pmos;
}

// What an electric part's value means, for the parts that have one.
std::optional<electric::Quantity> valueQuantity(PartKind kind);

inline constexpr std::uint8_t kMaxInputs = 8;
inline constexpr std::uint8_t kMaxFan = 32;
inline constexpr std::uint8_t kMaxScale = 3;
inline constexpr std::uint8_t kMaxBits = 64;
inline constexpr std::uint8_t kMaxPartAddress = 20;

constexpr std::uint8_t minInputs(PartKind kind) {
    if (kind == PartKind::Not) return 1;
    return isGate(kind) || isBundle(kind) ? 2 : 0;
}
constexpr std::uint8_t maxInputs(PartKind kind) {
    if (kind == PartKind::Not) return 1;
    if (isBundle(kind)) return kMaxFan;
    return isGate(kind) ? kMaxInputs : 0;
}

enum class PinRole : std::uint8_t { Input, Output };

using PartId = std::uint32_t;
using JointId = std::uint32_t;
using WireId = std::uint32_t;
using ChipId = std::uint32_t;  // 0 is the project's main board

using RomWords = std::shared_ptr<const std::vector<std::uint64_t>>;

struct Part {
    PartId id = 0;
    PartKind kind = PartKind::Nand;
    Point position;             // the top-left corner after rotation
    std::uint8_t rotation = 0;  // quarter turns clockwise
    std::uint8_t inputs = 2;    // gates: how many. Splitters and joiners: how many branches. Else 0.
    std::uint8_t scale = 1;     // size, 1 to kMaxScale: everything about the part grows by this
    BoxId box = kNoBox;         // boxes only: which one
    std::uint8_t width = 1;     // bits on its data pins, on parts that have a width
    std::uint8_t address = 0;   // boxes with memory: address bits
    std::uint64_t value = 0;    // constants: the value. Electric parts: millivolts, milliohms or picofarads
    ChipId chip = 0;            // chips only: which circuit of the project
    std::string label;          // the player's own name for it, if any
    std::string source;         // ROMs: the assembly its words came from, if any
    RomWords rom;               // ROMs: the words in it
    InterfacePtr interface;     // boxes and chips: their pins. Follows from the fields above.

    friend bool operator==(const Part& a, const Part& b);
};

// A part of this kind, with 0 for inputs meaning the fewest it can have. Boxes start at their default size.
Part makePart(PartKind kind, Point position = {}, std::uint8_t rotation = 0, std::uint8_t inputs = 0, std::uint8_t scale = 1,
              BoxId box = kNoBox);

const char* partName(const Part& part);

// True for kinds where the player can choose the number of bits.
bool hasWidth(const Part& part);
bool hasAddress(const Part& part);
// The boxes that are screens (the LED matrix, the terminal): bigger than a box, with a picture inside.
bool isScreenBox(const Part& part, std::string_view key);

struct Joint {
    JointId id = 0;
    Point position;
    friend bool operator==(const Joint&, const Joint&) = default;
};

// One end of a wire.
struct Terminal {
    enum class Kind : std::uint8_t { Pin, Joint };
    Kind kind = Kind::Joint;
    std::uint32_t id = 0;   // PartId or JointId
    std::uint8_t pin = 0;
    friend bool operator==(const Terminal&, const Terminal&) = default;
};

struct Wire {
    WireId id = 0;
    Terminal from;
    Terminal to;
    std::vector<Point> path;  // from end to end, with a point at each corner; every segment is straight
    friend bool operator==(const Wire&, const Wire&) = default;
};

// What's at a point, for starting or ending a wire.
struct Drop {
    enum class Kind : std::uint8_t { Empty, Pin, Joint, Wire };
    Kind kind = Kind::Empty;
    Point at;
    std::uint32_t id = 0;   // PartId, JointId or WireId
    std::uint8_t pin = 0;
};

// The cells a part takes before scale and rotation. Gates get taller with more inputs.
int baseWidth(const Part& part);
int baseHeight(const Part& part);

Rect boundsOf(const Part& part);
inline Rect boundsOf(PartKind kind, Point position, std::uint8_t rotation = 0, std::uint8_t inputs = 0, std::uint8_t scale = 1) {
    return boundsOf(makePart(kind, position, rotation, inputs, scale));
}

// Pins are a part's inputs, top to bottom, then its outputs. On a gate, the single output is always last.
unsigned inputCount(const Part& part);
unsigned outputCount(const Part& part);
inline unsigned pinCount(const Part& part) { return inputCount(part) + outputCount(part); }
PinRole pinRole(const Part& part, unsigned pin);
Point pinPosition(const Part& part, unsigned pin);
unsigned pinBits(const Part& part, unsigned pin);
const char* pinName(const Part& part, unsigned pin);  // "" if the pin has no name

// The bits a splitter or joiner gives to one of its branches. Low bits come first, and bits that don't
// divide evenly go to the last branches.
unsigned branchBits(const Part& part, unsigned branch);

// Which bit of a many-bit switch is at a point in its own cells, or -1. Bit 0 is at the far end.
int toggleBitAt(const Part& part, float x, float y);

unsigned hexDigits(unsigned bits);

// Removes repeated points and corners that aren't corners. A path always keeps at least two points.
void simplifyPath(std::vector<Point>& path);
bool pathContains(const std::vector<Point>& path, Point point);

// How far from the origin anything may be in a saved file. Far beyond any real board, and small enough
// that adding sizes and offsets to a coordinate can't overflow.
inline constexpr int kMaxCoordinate = 100'000'000;

// What Board::paste made, in the same order as the fragment it came from.
struct Pasted {
    std::vector<std::pair<PartId, PartId>> parts;  // old id, new id
    std::vector<WireId> wires;
};

class Board {
public:
    // Builds a board from saved data, or returns nothing if the data breaks any rule a board keeps: ids
    // in order, parts apart, wires straight and starting and ending exactly at their terminals, no
    // joints left unused. Input may come from anywhere, so this assumes nothing about it. Chip parts
    // must come with their interface set.
    static std::optional<Board> restore(std::vector<Part> parts, std::vector<Joint> joints, std::vector<Wire> wires);

    // Same parts, joints and wires. Which ids come next isn't content.
    bool sameContent(const Board& other) const {
        return parts_ == other.parts_ && joints_ == other.joints_ && wires_ == other.wires_;
    }

    // Returns nothing if it doesn't fit, or if inputs or scale are out of range for this kind.
    std::optional<PartId> addPart(PartKind kind, Point position, std::uint8_t rotation = 0, std::uint8_t inputs = 0,
                                  std::uint8_t scale = 1, BoxId box = kNoBox);
    std::optional<PartId> addPart(Part part);
    bool canPlace(const Part& candidate, std::span<const PartId> ignoring = {}) const;

    // Makes a wire along path, whose ends must be at the two drops. Ending on a wire splits that wire
    // at a new joint; ending on empty ground makes a joint there. Returns nothing if the path is
    // invalid, or joins a terminal to itself.
    std::optional<WireId> connect(const Drop& from, const Drop& to, std::vector<Point> path);

    // Removes the parts with every wire on their pins, and the wires. Joints left with nothing are
    // removed, and two wires left meeting at a joint become one.
    void remove(std::span<const PartId> parts, std::span<const WireId> wires);

    // Moves parts, dragging their wires along. False, with no change, if they wouldn't fit.
    bool moveParts(std::span<const PartId> parts, Point delta);
    bool rotatePart(PartId part);

    // Changes a part's inputs and size. Wires on inputs that no longer exist are removed, and every
    // other wire stays on its pin. False, with no change, if it wouldn't fit or the values are out of range.
    bool reshapePart(PartId part, std::uint8_t inputs, std::uint8_t scale);

    // The same for any change of shape: the part becomes this one, keeping its id and position.
    bool reconfigurePart(PartId part, Part shape);

    // Gives every part made from this chip its new pins. Wires stay on pins that still exist. Unlike a
    // player's edit this can't be refused, so a chip that grew may end up overlapping its neighbours.
    void updateChip(ChipId chip, const InterfacePtr& interface);

    // A copy of just these parts and the wires between them, keeping their ids and places. Wires that
    // went on to anything else stay behind, except any named in wires, which come along as they are.
    Board fragment(std::span<const PartId> parts, std::span<const WireId> wires) const;

    // Adds a copy of a fragment, moved by delta, with new ids. Returns nothing, with no change, if any
    // part wouldn't fit.
    std::optional<Pasted> paste(const Board& fragment, Point delta);

    // The smallest rectangle around everything on the board.
    Rect extent() const;
    bool empty() const { return parts_.empty() && wires_.empty(); }

    Drop dropAt(Point at) const;
    const Part* partAtCell(Point cell) const;
    const Wire* wireAt(Point at) const;

    const Part* part(PartId id) const;
    const Joint* joint(JointId id) const;
    const Wire* wire(WireId id) const;
    Point terminalPosition(const Terminal& terminal) const;

    // Sorted by id, which is what makes everything derived from a board deterministic.
    const std::vector<Part>& parts() const { return parts_; }
    const std::vector<Joint>& joints() const { return joints_; }
    const std::vector<Wire>& wires() const { return wires_; }

private:
    Terminal resolve(const Drop& drop);
    Terminal splitWire(WireId id, Point at, WireId* secondPiece);
    void tidyJoint(JointId id);
    void dragWireEnds(std::span<const PartId> moved, Point delta);
    void reattachWires(PartId part);
    void applyShape(PartId id, const Part& shape);

    std::vector<Part> parts_;
    std::vector<Joint> joints_;
    std::vector<Wire> wires_;
    std::uint32_t nextPart_ = 1;
    std::uint32_t nextJoint_ = 1;
    std::uint32_t nextWire_ = 1;
};

// A board's switches and outputs in reading order, top to bottom and then left to right: the columns of
// its truth table, and the pins it gets as a chip.
std::vector<PartId> inputsInOrder(const Board& board);
std::vector<PartId> outputsInOrder(const Board& board);
// The real LEDs, the electric ones. They show in the truth table, but a chip doesn't get pins for them.
std::vector<PartId> lightsInOrder(const Board& board);
std::string defaultInputName(std::size_t index);
std::string defaultOutputName(std::size_t index, std::size_t count);

// What a board looks like from outside, when used as a chip.
InterfacePtr makeChipInterface(const Board& board, const std::string& name);

}  // namespace bancada::editor
