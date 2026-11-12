#include "editor/gallery_circuits.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "editor/boxes.hpp"
#include "editor/electric_examples.hpp"
#include "editor/layout.hpp"

namespace bancada::editor {

namespace {

constexpr std::uint64_t kOhm = 1000;  // values are kept in milliohms
constexpr std::uint8_t kUpright = 1;   // a quarter turn: first pin at the top

GalleryProject finished(Board board) {
    GalleryProject out;
    out.board = std::move(board);
    return out;
}

using Signal = std::pair<PartId, unsigned>;

// One gate of resistor-transistor logic, the family of the first logic chips: NPN transistors under a
// resistor that pulls the output up to 5 V. With every transistor off, the output is 5 V, a 1; a
// transistor that turns on pulls it down to almost 0 V, a 0. In series they make a NAND, side by side a
// NOR, and one alone a NOT. Each input reaches its transistor's base through a 10 kΩ resistor.
enum class Cell : std::uint8_t { Not, Nand, Nor };

// A circuit written as its parts, each taking signals by name and giving one. A signal travels by a wire,
// or, for one used all over the board, by labels with its name.
class Net {
public:
    explicit Net(Board& board) : board_(board), l(board) {}

    void source(const std::string& name, PartId part, unsigned pin) { sources_[name] = {part, pin}; }

    Signal at(const std::string& name) const {
        const auto found = sources_.find(name);
        if (found == sources_.end()) throw std::logic_error("no signal " + name);
        return found->second;
    }

    unsigned bits(const std::string& name) const {
        const Signal s = at(name);
        return pinBits(*board_.part(s.first), s.second);
    }

    void feed(const std::string& signal, PartId part, unsigned pin) {
        if (labelled_.count(signal) != 0) {
            l.name(part, pin, signal.c_str(), true, static_cast<std::uint8_t>(bits(signal)));
            return;
        }
        const Signal from = at(signal);
        l.wire(from.first, from.second, part, pin);
    }

    // From here on the signal goes by labels: one at its source, one at each pin it feeds.
    void label(const std::string& signal) {
        const Signal s = at(signal);
        l.name(s.first, s.second, signal.c_str(), false, static_cast<std::uint8_t>(bits(signal)));
        labelled_.insert(signal);
    }

    PartId input(const char* name, Point p, std::uint8_t width = 1) {
        const PartId id = l.input(name, p, width);
        source(name, id, 0);
        return id;
    }

    PartId labelledInput(const char* name, Point p, std::uint8_t width = 1) {
        const PartId id = input(name, p, width);
        label(name);
        return id;
    }

    PartId gate(PartKind kind, Point p, std::initializer_list<const char*> ins, const char* out, std::uint8_t width = 1) {
        Part part = makePart(kind, p, 0, static_cast<std::uint8_t>(ins.size()));
        part.width = width;
        const PartId id = l.add(std::move(part));
        unsigned pin = 0;
        for (const char* in : ins) feed(in, id, pin++);
        source(out, id, pin);
        return id;
    }

    PartId light(const char* name, Point p, const char* signal) {
        const PartId id = l.output(name, p);
        feed(signal, id, 0);
        return id;
    }

    PartId display(const char* name, Point p, const char* signal, std::uint8_t width) {
        const PartId id = l.output(name, p, width);
        feed(signal, id, 0);
        return id;
    }

    PartId box(const char* key, Point p, std::initializer_list<std::pair<const char*, const char*>> ins,
               std::initializer_list<std::pair<const char*, const char*>> outs, std::uint8_t width = 0) {
        const PartId id = l.box(key, p, width);
        for (const auto& [pin, signal] : ins) feed(signal, id, l.pin(id, pin));
        for (const auto& [pin, signal] : outs) source(signal, id, l.pin(id, pin));
        return id;
    }

    // A bus opened into branches, the first branch the lowest bits; and branches joined into a bus.
    PartId split(const char* bus, Point p, std::uint8_t width, const std::vector<std::string>& branches) {
        const PartId id = l.bundle(PartKind::Splitter, p, width, static_cast<std::uint8_t>(branches.size()));
        feed(bus, id, 0u);
        for (std::size_t b = 0; b < branches.size(); ++b) {
            if (!branches[b].empty()) source(branches[b], id, static_cast<unsigned>(1 + b));
        }
        return id;
    }

    PartId join(Point p, std::uint8_t width, const std::vector<std::string>& branches, const char* bus) {
        const PartId id = l.bundle(PartKind::Joiner, p, width, static_cast<std::uint8_t>(branches.size()));
        for (std::size_t b = 0; b < branches.size(); ++b) {
            if (!branches[b].empty()) feed(branches[b], id, static_cast<unsigned>(b));  // empty: left loose, a 0
        }
        source(bus, id, static_cast<unsigned>(branches.size()));
        return id;
    }

    void cell(Cell kind, Point p, std::initializer_list<const char*> ins, const char* out) {
        const std::vector<const char*> in(ins);
        const int x = p.x;
        const int y = p.y;
        const PartId pull = l.part(PartKind::Resistor, {x + 2, y}, kUpright, 1'000 * kOhm);
        l.name(pull, 0, "5V", true);
        const auto stage = [&](Point at, Point resistorAt, const char* signal) {
            const PartId transistor = l.part(PartKind::Npn, at);
            const PartId resistor = l.part(PartKind::Resistor, resistorAt, 0, 10'000 * kOhm);
            feed(signal, resistor, 0u);
            l.wire(resistor, 1u, transistor, 0u);
            return transistor;
        };
        const auto grounded = [&](PartId transistor, Point at) {
            const PartId ground = l.part(PartKind::Ground, at);
            l.wire(transistor, 2u, ground, 0u);
        };
        const PartId first = stage({x, y + 6}, {x - 7, y + 7}, in[0]);
        l.wire(pull, 1u, first, 1u);
        if (kind == Cell::Not) {
            grounded(first, {x + 2, y + 12});
            source(out, pull, 1);
        } else if (kind == Cell::Nand) {
            const PartId second = stage({x, y + 12}, {x - 7, y + 13}, in[1]);
            l.wire(first, 2u, second, 1u);
            grounded(second, {x + 2, y + 18});
            source(out, pull, 1);
        } else {
            grounded(first, {x + 2, y + 12});
            const PartId second = stage({x + 16, y + 6}, {x + 11, y + 7}, in[1]);
            l.wire(second, 1u, pull, 1u);
            grounded(second, {x + 18, y + 12});
            source(out, second, 1);
        }
    }

    // The 5 V every transistor gate hangs from, and its ground.
    void supply(Point p) {
        const PartId battery = l.part(PartKind::Battery, p);
        l.name(battery, 0, "5V", true);
        const PartId ground = l.part(PartKind::Ground, {p.x + 6, p.y + 3});
        l.wire(battery, 1u, ground, 0u);
    }

private:
    Board& board_;

public:
    Layout l;

private:
    std::map<std::string, Signal> sources_;
    std::set<std::string> labelled_;
};

std::vector<std::string> bitNames(const char* prefix, int count) {
    std::vector<std::string> names;
    for (int bit = 0; bit < count; ++bit) names.push_back(prefix + std::to_string(bit));
    return names;
}

// The ready part from the library, with a switch on each input and a light on each output, named
// after its pins.
GalleryProject readyBox(const char* key, std::uint8_t width = 0) {
    Board board;
    Layout l(board);
    Board scratch;  // the box tried on a board of its own first, only to read its pins' sizes
    const Part shape = *scratch.part(Layout(scratch).box(key, {0, 0}, width));
    const unsigned inputs = inputCount(shape);
    int widest = 3;
    for (unsigned pin = 0; pin < inputs; ++pin) widest = std::max(widest, 2 * static_cast<int>(std::min(pinBits(shape, pin), 8u)) + 1);
    // Pins of many bits get room: every input on a row of its own, four apart.
    const PartId box = l.box(key, {widest + 9, 0}, width);
    const Part part = *board.part(box);
    for (unsigned pin = 0; pin < pinCount(part); ++pin) {
        const Point at = pinPosition(part, pin);
        const auto bits = static_cast<std::uint8_t>(pinBits(part, pin));
        if (pin < inputs) {
            const PartId toggle = l.input(pinName(part, pin), {widest + 9 - 6 - (bits == 1 ? 3 : 2 * std::min<int>(bits, 8) + 1), at.y - 1}, bits);
            l.wire(toggle, 0u, box, pin);
        } else {
            const PartId shown = l.output(pinName(part, pin), {at.x + 8, at.y - 1}, bits);
            l.wire(box, pin, shown, 0u);
        }
    }
    return finished(std::move(board));
}

// --- Logic gates --------------------------------------------------------------------------------------

GalleryProject gateAlone(PartKind kind) {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    if (kind == PartKind::Not) {
        n.gate(kind, {12, 1}, {"A"}, "S");
        n.light("S", {22, 2}, "S");
    } else {
        n.input("B", {0, 8});
        n.gate(kind, {12, 4}, {"A", "B"}, "S");
        n.light("S", {22, 5}, "S");
    }
    return finished(std::move(board));
}

// The two inputs, by label, and the supply under them: every transistor version starts the same way.
void transistorInputs(Net& n, bool twoInputs, int supplyY) {
    n.labelledInput("A", {0, 2});
    if (twoInputs) n.labelledInput("B", {0, 6});
    n.supply({0, supplyY});
}

GalleryProject notTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, false, 14);
    n.cell(Cell::Not, {24, 0}, {"A"}, "S");
    n.light("S", {34, 3}, "S");
    return finished(std::move(board));
}

GalleryProject nandTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 16);
    n.cell(Cell::Nand, {24, 0}, {"A", "B"}, "S");
    n.light("S", {34, 3}, "S");
    return finished(std::move(board));
}

GalleryProject norTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 16);
    n.cell(Cell::Nor, {24, 0}, {"A", "B"}, "S");
    n.light("S", {48, 5}, "S");
    return finished(std::move(board));
}

GalleryProject andTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 16);
    n.cell(Cell::Nand, {24, 0}, {"A", "B"}, "n");
    n.cell(Cell::Not, {48, 0}, {"n"}, "S");
    n.light("S", {58, 3}, "S");
    return finished(std::move(board));
}

GalleryProject orTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 16);
    n.cell(Cell::Nor, {24, 0}, {"A", "B"}, "n");
    n.cell(Cell::Not, {64, 0}, {"n"}, "S");
    n.light("S", {74, 3}, "S");
    return finished(std::move(board));
}

GalleryProject xorTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 40);
    n.cell(Cell::Nand, {24, 12}, {"A", "B"}, "n1");
    n.cell(Cell::Nand, {50, 0}, {"A", "n1"}, "n2");
    n.cell(Cell::Nand, {50, 24}, {"n1", "B"}, "n3");
    n.cell(Cell::Nand, {76, 12}, {"n2", "n3"}, "S");
    n.light("S", {88, 15}, "S");
    return finished(std::move(board));
}

GalleryProject xnorTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 40);
    n.cell(Cell::Nor, {24, 12}, {"A", "B"}, "n1");
    n.cell(Cell::Nor, {64, 0}, {"n1", "A"}, "n2");
    n.cell(Cell::Nor, {64, 24}, {"n1", "B"}, "n3");
    n.cell(Cell::Nor, {104, 12}, {"n2", "n3"}, "S");
    n.light("S", {128, 17}, "S");
    return finished(std::move(board));
}

// --- Sums of one bit ------------------------------------------------------------------------------------

GalleryProject halfAdderGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 8});
    n.gate(PartKind::Xor, {12, 1}, {"A", "B"}, "s");
    n.gate(PartKind::And, {12, 9}, {"A", "B"}, "c");
    n.light("S", {24, 2}, "s");
    n.light("C", {24, 10}, "c");
    return finished(std::move(board));
}

GalleryProject halfAdderTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 56);
    n.cell(Cell::Nand, {24, 12}, {"A", "B"}, "n1");
    n.cell(Cell::Nand, {50, 0}, {"A", "n1"}, "n2");
    n.cell(Cell::Nand, {50, 24}, {"n1", "B"}, "n3");
    n.cell(Cell::Nand, {76, 12}, {"n2", "n3"}, "s");
    n.cell(Cell::Not, {76, 40}, {"n1"}, "c");
    n.light("S", {88, 15}, "s");
    n.light("C", {88, 43}, "c");
    return finished(std::move(board));
}

GalleryProject fullAdderGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 6});
    n.input("Ce", {0, 12});
    n.gate(PartKind::Xor, {10, 2}, {"A", "B"}, "x");
    n.gate(PartKind::And, {10, 20}, {"A", "B"}, "a1");
    n.gate(PartKind::Xor, {22, 8}, {"x", "Ce"}, "s");
    n.gate(PartKind::And, {22, 14}, {"x", "Ce"}, "a2");
    n.gate(PartKind::Or, {34, 17}, {"a2", "a1"}, "cs");
    n.light("S", {44, 9}, "s");
    n.light("Cs", {44, 18}, "cs");
    return finished(std::move(board));
}

// Nine NANDs: two exclusive ORs in a row make the sum, and one last NAND makes the carry. The names
// in and carry are for the subtractor, which is the same circuit with a different last gate.
void nineNands(Net& n, const char* carryIn, bool subtract) {
    n.cell(Cell::Nand, {24, 24}, {"A", "B"}, "n1");
    n.cell(Cell::Nand, {50, 12}, {"A", "n1"}, "n2");
    n.cell(Cell::Nand, {50, 36}, {"n1", "B"}, "n3");
    n.cell(Cell::Nand, {76, 24}, {"n2", "n3"}, "x");
    n.cell(Cell::Nand, {102, 24}, {"x", carryIn}, "m1");
    n.cell(Cell::Nand, {128, 12}, {"x", "m1"}, "m2");
    n.cell(Cell::Nand, {128, 36}, {"m1", carryIn}, "m3");
    n.cell(Cell::Nand, {154, 24}, {"m2", "m3"}, "result");
    if (subtract) {
        n.cell(Cell::Nand, {154, 52}, {"n3", "m3"}, "carry");
    } else {
        n.cell(Cell::Nand, {154, 52}, {"m1", "n1"}, "carry");
    }
}

GalleryProject fullAdderTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 70);
    n.labelledInput("Ce", {0, 10});
    nineNands(n, "Ce", false);
    n.light("S", {166, 27}, "result");
    n.light("Cs", {166, 55}, "carry");
    return finished(std::move(board));
}

GalleryProject subtractorGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 6});
    n.input("Ve", {0, 12});
    n.gate(PartKind::Xor, {12, 2}, {"A", "B"}, "x");
    n.gate(PartKind::Xor, {26, 8}, {"x", "Ve"}, "d");
    n.gate(PartKind::Not, {12, 16}, {"A"}, "na");
    n.gate(PartKind::And, {26, 16}, {"na", "B"}, "a1");
    n.gate(PartKind::Not, {26, 24}, {"x"}, "nx");
    n.gate(PartKind::And, {38, 24}, {"nx", "Ve"}, "a2");
    n.gate(PartKind::Or, {50, 18}, {"a1", "a2"}, "vs");
    n.light("D", {60, 9}, "d");
    n.light("Vs", {60, 19}, "vs");
    return finished(std::move(board));
}

GalleryProject subtractorTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 70);
    n.labelledInput("Ve", {0, 10});
    nineNands(n, "Ve", true);
    n.light("D", {166, 27}, "result");
    n.light("Vs", {166, 55}, "carry");
    return finished(std::move(board));
}

GalleryProject comparatorGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 4});
    n.input("B", {0, 10});
    n.gate(PartKind::Not, {12, 2}, {"B"}, "nb");
    n.gate(PartKind::Not, {12, 14}, {"A"}, "na");
    n.gate(PartKind::And, {24, 1}, {"A", "nb"}, "gt");
    n.gate(PartKind::Xnor, {24, 8}, {"A", "B"}, "eq");
    n.gate(PartKind::And, {24, 15}, {"na", "B"}, "lt");
    n.light("A>B", {34, 2}, "gt");
    n.light("A=B", {34, 9}, "eq");
    n.light("A<B", {34, 16}, "lt");
    return finished(std::move(board));
}

GalleryProject comparatorTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, true, 52);
    n.cell(Cell::Nand, {24, 12}, {"A", "B"}, "n1");
    n.cell(Cell::Nand, {50, 0}, {"A", "n1"}, "notGt");
    n.cell(Cell::Nand, {50, 24}, {"n1", "B"}, "notLt");
    n.cell(Cell::Not, {76, 0}, {"notGt"}, "gt");
    n.cell(Cell::Not, {76, 24}, {"notLt"}, "lt");
    n.cell(Cell::Nor, {102, 12}, {"gt", "lt"}, "eq");
    n.light("A>B", {128, 3}, "gt");
    n.light("A=B", {128, 17}, "eq");
    n.light("A<B", {128, 27}, "lt");
    return finished(std::move(board));
}

GalleryProject aluBitBlocks() {
    Board board;
    Net n(board);
    n.input("A", {0, 0});
    n.input("B", {0, 4});
    n.input("Ce", {0, 8});
    n.input("Op0", {0, 28});
    n.input("Op1", {0, 32});
    n.gate(PartKind::And, {14, 0}, {"A", "B"}, "and");
    n.gate(PartKind::Or, {14, 6}, {"A", "B"}, "or");
    n.gate(PartKind::Xor, {14, 12}, {"A", "B"}, "xor");
    n.box("full_adder", {12, 18}, {{"A", "A"}, {"B", "B"}, {"Ce", "Ce"}}, {{"S", "sum"}, {"Cs", "carry"}});
    n.box("mux4", {32, 4}, {{"D0", "and"}, {"D1", "or"}, {"D2", "xor"}, {"D3", "sum"}, {"S0", "Op0"}, {"S1", "Op1"}}, {{"Y", "r"}});
    n.gate(PartKind::And, {32, 26}, {"Op0", "Op1"}, "adding");
    n.gate(PartKind::And, {44, 22}, {"carry", "adding"}, "cs");
    n.light("R", {50, 9}, "r");
    n.light("Cs", {54, 23}, "cs");
    return finished(std::move(board));
}

GalleryProject aluBitGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 0});
    n.input("B", {0, 4});
    n.input("Ce", {0, 18});
    n.input("Op1", {0, 30});
    n.input("Op0", {0, 36});
    n.gate(PartKind::And, {12, 0}, {"A", "B"}, "and");
    n.gate(PartKind::Or, {12, 6}, {"A", "B"}, "or");
    n.gate(PartKind::Xor, {12, 12}, {"A", "B"}, "xor");
    n.gate(PartKind::Not, {12, 28}, {"Op1"}, "nop1");
    n.gate(PartKind::Not, {12, 34}, {"Op0"}, "nop0");
    n.gate(PartKind::Xor, {24, 16}, {"xor", "Ce"}, "sum");
    n.gate(PartKind::And, {24, 22}, {"xor", "Ce"}, "a2");
    n.gate(PartKind::And, {24, 40}, {"Op0", "Op1"}, "adding");
    n.gate(PartKind::And, {36, 0}, {"and", "nop1", "nop0"}, "s0");
    n.gate(PartKind::And, {36, 7}, {"or", "nop1", "Op0"}, "s1");
    n.gate(PartKind::And, {36, 14}, {"xor", "Op1", "nop0"}, "s2");
    n.gate(PartKind::And, {36, 21}, {"sum", "Op1", "Op0"}, "s3");
    n.gate(PartKind::Or, {36, 30}, {"and", "a2"}, "carry");
    n.gate(PartKind::Or, {48, 8}, {"s0", "s1", "s2", "s3"}, "r");
    n.gate(PartKind::And, {48, 34}, {"carry", "adding"}, "cs");
    n.light("R", {58, 11}, "r");
    n.light("Cs", {58, 35}, "cs");
    return finished(std::move(board));
}

// --- Sums of many bits ------------------------------------------------------------------------------

// A and B of four bits, opened into bits a0..a3 and b0..b3, and the carry in.
void fourBitInputs(Net& n, const char* carry) {
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 10}, 4);
    n.input(carry, {0, 20});
    n.split("A", {14, -3}, 4, bitNames("a", 4));
    n.split("B", {14, 7}, 4, bitNames("b", 4));
}

GalleryProject adder4Ready() {
    Board board;
    Net n(board);
    fourBitInputs(n, "Ce");
    n.box("adder4", {24, 0},
          {{"A0", "a0"}, {"A1", "a1"}, {"A2", "a2"}, {"A3", "a3"}, {"B0", "b0"}, {"B1", "b1"}, {"B2", "b2"}, {"B3", "b3"}, {"Ce", "Ce"}},
          {{"S0", "s0"}, {"S1", "s1"}, {"S2", "s2"}, {"S3", "s3"}, {"Cs", "cs"}});
    n.join({40, 4}, 4, bitNames("s", 4), "s");
    n.display("S", {48, 7}, "s", 4);
    n.light("Cs", {48, 12}, "cs");
    return finished(std::move(board));
}

// Four one-bit blocks in a row, the carry (or borrow) out of each going into the next.
GalleryProject rippleOfBlocks(const char* key, const char* carryIn, const char* carryOut, const char* result) {
    Board board;
    Net n(board);
    fourBitInputs(n, carryIn);
    std::string carry = carryIn;
    for (int bit = 0; bit < 4; ++bit) {
        const std::string a = "a" + std::to_string(bit);
        const std::string b = "b" + std::to_string(bit);
        const std::string r = "r" + std::to_string(bit);
        const std::string next = "c" + std::to_string(bit + 1);
        const char* resultPin = std::string_view(key) == "full_subtractor" ? "D" : "S";
        n.box(key, {26, bit * 8}, {{"A", a.c_str()}, {"B", b.c_str()}, {carryIn, carry.c_str()}}, {{resultPin, r.c_str()}, {carryOut, next.c_str()}});
        carry = next;
    }
    n.join({44, 6}, 4, bitNames("r", 4), "r");
    n.display(result, {52, 9}, "r", 4);
    n.light(carryOut, {52, 28}, carry.c_str());
    return finished(std::move(board));
}

GalleryProject adder4Blocks() { return rippleOfBlocks("full_adder", "Ce", "Cs", "S"); }

GalleryProject adder4Gates() {
    Board board;
    Net n(board);
    fourBitInputs(n, "Ce");
    std::string carry = "Ce";
    for (int bit = 0; bit < 4; ++bit) {
        const int y = bit * 16;
        const std::string i = std::to_string(bit);
        const std::string a = "a" + i, b = "b" + i, x = "x" + i, s = "s" + i, a1 = "p" + i, a2 = "q" + i, next = "c" + std::to_string(bit + 1);
        n.gate(PartKind::Xor, {24, y}, {a.c_str(), b.c_str()}, x.c_str());
        n.gate(PartKind::Xor, {36, y + 4}, {x.c_str(), carry.c_str()}, s.c_str());
        n.gate(PartKind::And, {24, y + 8}, {a.c_str(), b.c_str()}, a1.c_str());
        n.gate(PartKind::And, {36, y + 10}, {x.c_str(), carry.c_str()}, a2.c_str());
        n.gate(PartKind::Or, {48, y + 11}, {a2.c_str(), a1.c_str()}, next.c_str());
        carry = next;
    }
    n.join({60, 20}, 4, bitNames("s", 4), "s");
    n.display("S", {68, 23}, "s", 4);
    n.light("Cs", {68, 60}, carry.c_str());
    return finished(std::move(board));
}

GalleryProject adderNReady() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 8);
    n.input("B", {0, 6}, 8);
    n.input("Ce", {0, 12});
    n.box("adder_n", {24, 0}, {{"A", "A"}, {"B", "B"}, {"Ce", "Ce"}}, {{"S", "s"}, {"Cs", "cs"}}, 8);
    n.display("S", {40, 1}, "s", 8);
    n.light("Cs", {40, 5}, "cs");
    return finished(std::move(board));
}

GalleryProject adderNHalves() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 8);
    n.input("B", {0, 6}, 8);
    n.input("Ce", {0, 12});
    n.split("A", {22, -1}, 8, {"aLow", "aHigh"});
    n.split("B", {22, 5}, 8, {"bLow", "bHigh"});
    n.box("adder_n", {32, 0}, {{"A", "aLow"}, {"B", "bLow"}, {"Ce", "Ce"}}, {{"S", "sLow"}, {"Cs", "middle"}}, 4);
    n.box("adder_n", {32, 10}, {{"A", "aHigh"}, {"B", "bHigh"}, {"Ce", "middle"}}, {{"S", "sHigh"}, {"Cs", "cs"}}, 4);
    n.join({48, 5}, 8, {"sLow", "sHigh"}, "s");
    n.display("S", {56, 6}, "s", 8);
    n.light("Cs", {56, 13}, "cs");
    return finished(std::move(board));
}

void subtractorInputs(Net& n) {
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 6}, 4);
    n.input("Ve", {0, 12});
}

GalleryProject subNReady() {
    Board board;
    Net n(board);
    subtractorInputs(n);
    n.box("sub_n", {16, 0}, {{"A", "A"}, {"B", "B"}, {"Ve", "Ve"}}, {{"D", "d"}, {"Vs", "vs"}}, 4);
    n.display("D", {32, 1}, "d", 4);
    n.light("Vs", {32, 4}, "vs");
    return finished(std::move(board));
}

GalleryProject subNBlocks() { return rippleOfBlocks("full_subtractor", "Ve", "Vs", "D"); }

// A − B − Ve = A + (NOT B) + (NOT Ve): the complement of B, and the carry in that adds the 1.
GalleryProject subNWithAdder() {
    Board board;
    Net n(board);
    subtractorInputs(n);
    n.gate(PartKind::Not, {14, 5}, {"B"}, "notB", 4);
    n.gate(PartKind::Not, {14, 11}, {"Ve"}, "ce");
    n.box("adder_n", {24, 0}, {{"A", "A"}, {"B", "notB"}, {"Ce", "ce"}}, {{"S", "d"}, {"Cs", "carry"}}, 4);
    n.gate(PartKind::Not, {38, 6}, {"carry"}, "vs");
    n.display("D", {46, 1}, "d", 4);
    n.light("Vs", {46, 7}, "vs");
    return finished(std::move(board));
}

GalleryProject cmpNReady() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 6}, 4);
    n.box("cmp_n", {16, 0}, {{"A", "A"}, {"B", "B"}}, {{"A>B", "gt"}, {"A=B", "eq"}, {"A<B", "lt"}}, 4);
    n.light("A>B", {32, 0}, "gt");
    n.light("A=B", {32, 3}, "eq");
    n.light("A<B", {32, 6}, "lt");
    return finished(std::move(board));
}

GalleryProject cmpNBlocks() {
    Board board;
    Net n(board);
    n.labelledInput("A", {0, 0}, 4);
    n.labelledInput("B", {0, 6}, 4);
    n.box("sub_n", {24, 0}, {{"A", "A"}, {"B", "B"}}, {{"Vs", "lt"}}, 4);
    n.gate(PartKind::Xnor, {24, 12}, {"A", "B"}, "same", 4);
    n.split("same", {32, 10}, 4, bitNames("e", 4));
    n.gate(PartKind::And, {38, 10}, {"e0", "e1", "e2", "e3"}, "eq");
    n.gate(PartKind::Nor, {48, 4}, {"lt", "eq"}, "gt");
    n.light("A>B", {58, 5}, "gt");
    n.light("A=B", {58, 13}, "eq");
    n.light("A<B", {58, 0}, "lt");
    return finished(std::move(board));
}

void aluInputs(Net& n) {
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 10}, 4);
    n.input("Op1", {0, 22});
    n.input("Op0", {0, 26});
    n.split("A", {14, -3}, 4, bitNames("a", 4));
    n.split("B", {14, 7}, 4, bitNames("b", 4));
}

GalleryProject alu4Ready() {
    Board board;
    Net n(board);
    aluInputs(n);
    n.box("alu4", {24, 0},
          {{"A0", "a0"}, {"A1", "a1"}, {"A2", "a2"}, {"A3", "a3"}, {"B0", "b0"}, {"B1", "b1"}, {"B2", "b2"}, {"B3", "b3"}, {"Op0", "Op0"}, {"Op1", "Op1"}},
          {{"R0", "r0"}, {"R1", "r1"}, {"R2", "r2"}, {"R3", "r3"}, {"Cs", "cs"}});
    n.join({40, 5}, 4, bitNames("r", 4), "r");
    n.display("R", {48, 8}, "r", 4);
    n.light("Cs", {48, 13}, "cs");
    return finished(std::move(board));
}

GalleryProject alu4Slices() {
    Board board;
    Net n(board);
    aluInputs(n);
    n.label("Op0");
    n.label("Op1");
    std::string carry;
    for (int bit = 0; bit < 4; ++bit) {
        const std::string i = std::to_string(bit);
        const std::string a = "a" + i, b = "b" + i, r = "r" + i, next = "c" + std::to_string(bit + 1);
        if (carry.empty()) {
            n.box("alu1", {28, bit * 12}, {{"A", a.c_str()}, {"B", b.c_str()}, {"Op0", "Op0"}, {"Op1", "Op1"}}, {{"R", r.c_str()}, {"Cs", next.c_str()}});
        } else {
            n.box("alu1", {28, bit * 12}, {{"A", a.c_str()}, {"B", b.c_str()}, {"Ce", carry.c_str()}, {"Op0", "Op0"}, {"Op1", "Op1"}},
                  {{"R", r.c_str()}, {"Cs", next.c_str()}});
        }
        carry = next;
    }
    n.join({46, 14}, 4, bitNames("r", 4), "r");
    n.display("R", {52, 17}, "r", 4);
    n.light("Cs", {52, 44}, carry.c_str());
    return finished(std::move(board));
}

GalleryProject aluNReady() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 6}, 4);
    n.input("Op", {0, 12}, 3);
    n.box("alu_n", {14, 0}, {{"A", "A"}, {"B", "B"}, {"Op", "Op"}}, {{"R", "r"}, {"Cs", "cs"}, {"Z", "z"}}, 4);
    n.display("R", {30, 0}, "r", 4);
    n.light("Cs", {36, 2}, "cs");
    n.light("Z", {30, 4}, "z");
    return finished(std::move(board));
}

// Every operation at once, each by its own circuit, and multiplexers that pick one by Op.
GalleryProject aluNBlocks() {
    Board board;
    Net n(board);
    n.labelledInput("A", {0, 0}, 4);
    n.labelledInput("B", {0, 8}, 4);
    n.input("Op", {0, 70}, 3);
    n.split("Op", {12, 68}, 3, {"op0", "op1", "op2"});
    n.label("op0");
    n.label("op1");
    n.label("op2");
    n.join({28, 64}, 2, {"op0", "op1"}, "sel");
    n.label("sel");

    n.gate(PartKind::And, {26, 0}, {"A", "B"}, "and", 4);
    n.gate(PartKind::Or, {26, 6}, {"A", "B"}, "or", 4);
    n.gate(PartKind::Xor, {26, 12}, {"A", "B"}, "xor", 4);
    n.box("adder_n", {22, 18}, {{"A", "A"}, {"B", "B"}}, {{"S", "sum"}, {"Cs", "addCarry"}}, 4);
    n.box("sub_n", {22, 26}, {{"A", "A"}, {"B", "B"}}, {{"D", "difference"}, {"Vs", "borrow"}}, 4);
    n.gate(PartKind::Not, {26, 34}, {"A"}, "notA", 4);
    n.split("A", {22, 42}, 4, bitNames("a", 4));
    n.join({30, 40}, 4, {"", "a0", "a1", "a2"}, "left");   // shifted left: a 0 comes in at the bottom
    n.join({30, 50}, 4, {"a1", "a2", "a3", ""}, "right");  // shifted right: a 0 comes in at the top

    n.box("mux4_n", {44, 2}, {{"D0", "and"}, {"D1", "or"}, {"D2", "xor"}, {"D3", "sum"}, {"Sel", "sel"}}, {{"Y", "logic"}}, 4);
    n.box("mux4_n", {44, 26}, {{"D0", "difference"}, {"D1", "notA"}, {"D2", "left"}, {"D3", "right"}, {"Sel", "sel"}}, {{"Y", "other"}}, 4);
    n.box("mux2_n", {62, 14}, {{"A", "logic"}, {"B", "other"}, {"Sel", "op2"}}, {{"S", "r"}}, 4);

    // The carry: the adder's for operation 3, the subtractor's borrow for operation 4, else 0.
    n.box("mux4", {62, 36}, {{"D3", "addCarry"}, {"S0", "op0"}, {"S1", "op1"}}, {{"Y", "c1"}});
    n.box("mux4", {62, 52}, {{"D0", "borrow"}, {"S0", "op0"}, {"S1", "op1"}}, {{"Y", "c2"}});
    n.box("mux2", {80, 44}, {{"A", "c1"}, {"B", "c2"}, {"Sel", "op2"}}, {{"S", "cs"}});

    n.split("r", {78, 22}, 4, bitNames("r", 4));
    n.gate(PartKind::Nor, {86, 22}, {"r0", "r1", "r2", "r3"}, "z");
    n.display("R", {96, 16}, "r", 4);
    n.light("Z", {96, 25}, "z");
    n.light("Cs", {96, 46}, "cs");
    return finished(std::move(board));
}

// --- Memory and choosing -------------------------------------------------------------------------------

GalleryProject decoderGates() {
    Board board;
    Net n(board);
    n.input("A0", {0, 2});
    n.input("A1", {0, 8});
    n.gate(PartKind::Not, {12, 0}, {"A0"}, "n0");
    n.gate(PartKind::Not, {12, 6}, {"A1"}, "n1");
    n.gate(PartKind::And, {26, 0}, {"n0", "n1"}, "y0");
    n.gate(PartKind::And, {26, 6}, {"A0", "n1"}, "y1");
    n.gate(PartKind::And, {26, 12}, {"n0", "A1"}, "y2");
    n.gate(PartKind::And, {26, 18}, {"A0", "A1"}, "y3");
    n.light("Y0", {36, 1}, "y0");
    n.light("Y1", {36, 7}, "y1");
    n.light("Y2", {36, 13}, "y2");
    n.light("Y3", {36, 19}, "y3");
    return finished(std::move(board));
}

GalleryProject latchGates() {
    Board board;
    Net n(board);
    n.input("S", {0, 2});
    n.input("R", {0, 14});
    const PartId upper = n.l.gate(PartKind::Nor, {12, 1});
    const PartId lower = n.l.gate(PartKind::Nor, {12, 13});
    n.feed("S", upper, 0);
    n.feed("R", lower, 1);
    n.l.wire(upper, 2u, lower, 0u);
    n.l.wire(lower, 2u, upper, 1u);
    n.source("q", lower, 2);
    n.source("notQ", upper, 2);
    n.light("Q", {26, 14}, "q");
    n.light("!Q", {26, 2}, "notQ");
    return finished(std::move(board));
}

GalleryProject counterReady() {
    Board board;
    Layout l(board);
    const PartId clock = *board.addPart(PartKind::Clock, {0, 6});
    const PartId on = l.input("Conta", {0, 0});
    const PartId reset = l.input("Zera", {0, 12});
    const PartId box = l.box("counter", {10, 2}, 4);
    const PartId shown = l.output("Valor", {28, 3}, 4);
    const PartId full = l.output("Máximo", {28, 8});
    l.wire(on, 0u, box, "En");
    l.wire(reset, 0u, box, "Rst");
    l.wire(clock, 0u, box, "Clk");
    l.wire(box, "Q", shown, 0u);
    l.wire(box, "Max", full, 0u);
    GalleryProject out = finished(std::move(board));
    out.toggles.emplace_back(on, 1);
    return out;
}

// The counter is a register that keeps its own value plus one: an adder feeds it back.
GalleryProject counterBlocks() {
    Board board;
    Net n(board);
    const PartId on = n.input("Conta", {0, 0});
    const PartId clock = *board.addPart(PartKind::Clock, {0, 6});
    n.source("clock", clock, 0);
    n.input("Zera", {0, 12});
    const PartId keep = n.l.box("register", {30, 2}, 4);
    n.source("q", keep, n.l.pin(keep, "Q"));
    n.box("adder_n", {14, 18}, {{"A", "q"}, {"Ce", "Conta"}}, {{"S", "next"}}, 4);
    n.feed("next", keep, n.l.pin(keep, "D"));
    n.feed("Conta", keep, n.l.pin(keep, "En"));
    n.feed("Zera", keep, n.l.pin(keep, "Rst"));
    n.feed("clock", keep, n.l.pin(keep, "Clk"));
    n.split("q", {44, 10}, 4, bitNames("q", 4));
    n.gate(PartKind::And, {50, 10}, {"q0", "q1", "q2", "q3"}, "max");
    n.display("Valor", {50, 3}, "q", 4);
    n.light("Máximo", {60, 13}, "max");
    GalleryProject out = finished(std::move(board));
    out.toggles.emplace_back(on, 1);
    return out;
}

GalleryProject registerReady() {
    Board board;
    Layout l(board);
    const PartId data = l.input("Dado", {0, 1}, 4);
    const PartId keep = l.input("Grava", {0, 7});
    const PartId clock = *board.addPart(PartKind::Clock, {0, 12});
    const PartId box = l.box("register", {14, 2}, 4);
    const PartId shown = l.output("Guardado", {32, 4}, 4);
    l.wire(data, 0u, box, "D");
    l.wire(keep, 0u, box, "En");
    l.wire(clock, 0u, box, "Clk");
    l.wire(box, "Q", shown, 0u);
    return finished(std::move(board));
}

// Four flip-flops, each with a multiplexer in front: with Grava off it gives the flip-flop its own bit
// back, so the bit stays; with Grava on, the new one.
GalleryProject registerFlipFlops() {
    Board board;
    Net n(board);
    n.input("Dado", {0, 0}, 4);
    n.labelledInput("Grava", {0, 10});
    const PartId clock = *board.addPart(PartKind::Clock, {0, 16});
    n.source("Clk", clock, 0);
    n.label("Clk");
    n.split("Dado", {12, -3}, 4, bitNames("d", 4));
    for (int bit = 0; bit < 4; ++bit) {
        const std::string i = std::to_string(bit);
        const std::string d = "d" + i, q = "q" + i, next = "n" + i;
        const PartId choose = n.l.box("mux2", {24, bit * 10});
        const PartId flop = n.l.box("d_flip_flop", {44, bit * 10});
        n.source(q, flop, n.l.pin(flop, "Q"));
        n.source(next, choose, n.l.pin(choose, "S"));
        n.feed(q, choose, n.l.pin(choose, "A"));
        n.feed(d, choose, n.l.pin(choose, "B"));
        n.feed("Grava", choose, n.l.pin(choose, "Sel"));
        n.feed(next, flop, n.l.pin(flop, "D"));
        n.feed("Clk", flop, n.l.pin(flop, "Clk"));
    }
    n.join({60, 12}, 4, bitNames("q", 4), "q");
    n.display("Guardado", {66, 15}, "q", 4);
    return finished(std::move(board));
}

// --- Phase two: more gates, choosing, more sums ----------------------------------------------------------

GalleryProject bufferGates() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.gate(PartKind::Not, {12, 1}, {"A"}, "n");
    n.gate(PartKind::Not, {22, 1}, {"n"}, "s");
    n.light("S", {32, 2}, "s");
    return finished(std::move(board));
}

GalleryProject bufferTransistors() {
    Board board;
    Net n(board);
    transistorInputs(n, false, 16);
    n.cell(Cell::Not, {24, 0}, {"A"}, "n");
    n.cell(Cell::Not, {48, 0}, {"n"}, "s");
    n.light("S", {58, 3}, "s");
    return finished(std::move(board));
}

// The bits of D one after the other through XORs: each one that's 1 flips the answer.
GalleryProject parityGates(bool even) {
    Board board;
    Net n(board);
    n.input("D", {0, 2}, 4);
    n.split("D", {14, -1}, 4, bitNames("d", 4));
    n.gate(PartKind::Xor, {22, 0}, {"d0", "d1"}, "x1");
    n.gate(PartKind::Xor, {32, 4}, {"x1", "d2"}, "x2");
    n.gate(even ? PartKind::Xnor : PartKind::Xor, {42, 8}, {"x2", "d3"}, "s");
    n.light("S", {52, 9}, "s");
    return finished(std::move(board));
}

GalleryProject priorityGates() {
    Board board;
    Net n(board);
    n.labelledInput("D0", {0, 0});
    n.labelledInput("D1", {0, 4});
    n.labelledInput("D2", {0, 8});
    n.labelledInput("D3", {0, 12});
    n.gate(PartKind::Not, {22, 18}, {"D2"}, "notD2");
    n.gate(PartKind::And, {32, 18}, {"notD2", "D1"}, "t");
    n.gate(PartKind::Or, {44, 2}, {"D3", "t"}, "a0");
    n.gate(PartKind::Or, {44, 10}, {"D3", "D2"}, "a1");
    n.gate(PartKind::Or, {44, 24}, {"D0", "D1", "D2", "D3"}, "v");
    n.light("A0", {56, 3}, "a0");
    n.light("A1", {56, 11}, "a1");
    n.light("V", {56, 27}, "v");
    return finished(std::move(board));
}

// Eight bits narrowed down in three rounds of multiplexers: Sel bit 0 picks in each pair, bit 1 between the
// pairs, bit 2 between the halves.
GalleryProject bitSelectMuxes() {
    Board board;
    Net n(board);
    n.input("D", {0, 0}, 8);
    n.input("Sel", {0, 22}, 3);
    n.split("D", {22, -2}, 8, bitNames("d", 8));
    for (int bit = 0; bit < 8; ++bit) n.label("d" + std::to_string(bit));
    n.split("Sel", {14, 20}, 3, bitNames("s", 3));
    for (int bit = 0; bit < 3; ++bit) n.label("s" + std::to_string(bit));
    for (int k = 0; k < 4; ++k) {
        const std::string a = "d" + std::to_string(2 * k), b = "d" + std::to_string(2 * k + 1), m = "m" + std::to_string(k);
        n.box("mux2", {40, 8 * k}, {{"A", a.c_str()}, {"B", b.c_str()}, {"Sel", "s0"}}, {{"S", m.c_str()}});
    }
    n.box("mux2", {60, 4}, {{"A", "m0"}, {"B", "m1"}, {"Sel", "s1"}}, {{"S", "low"}});
    n.box("mux2", {60, 20}, {{"A", "m2"}, {"B", "m3"}, {"Sel", "s1"}}, {{"S", "high"}});
    n.box("mux2", {80, 12}, {{"A", "low"}, {"B", "high"}, {"Sel", "s2"}}, {{"S", "s"}});
    n.light("S", {96, 14}, "s");
    return finished(std::move(board));
}

// The array multiplier: one AND for each pair of bits, a_j·b_i worth 2^(i+j), each row added to the last.
GalleryProject multiplierArray() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 8}, 4);
    n.split("A", {14, -3}, 4, bitNames("a", 4));
    n.split("B", {14, 7}, 4, bitNames("b", 4));
    for (int bit = 0; bit < 4; ++bit) {
        n.label("a" + std::to_string(bit));
        n.label("b" + std::to_string(bit));
    }
    std::string total;
    for (int i = 0; i < 4; ++i) {
        const int y = 24 * i;
        std::vector<std::string> row(8);
        for (int j = 0; j < 4; ++j) {
            const std::string a = "a" + std::to_string(j), b = "b" + std::to_string(i);
            const std::string product = "p" + std::to_string(i) + std::to_string(j);
            n.gate(PartKind::And, {34, y + 5 * j}, {a.c_str(), b.c_str()}, product.c_str());
            row[static_cast<std::size_t>(i + j)] = product;
        }
        const std::string rowName = "row" + std::to_string(i);
        n.join({46, y}, 8, row, rowName.c_str());
        if (i == 0) {
            total = rowName;
        } else {
            const std::string sum = "sum" + std::to_string(i);
            n.box("adder_n", {58, y - 2}, {{"A", total.c_str()}, {"B", rowName.c_str()}}, {{"S", sum.c_str()}}, 8);
            total = sum;
        }
    }
    n.split(total.c_str(), {74, 70}, 8, {"low", "high"});
    n.display("Baixo", {82, 70}, "low", 4);
    n.display("Alto", {82, 74}, "high", 4);
    return finished(std::move(board));
}

// Long division in four steps, from the top bit of A: the remainder takes the next bit, B is tried
// against it, and where it fits it comes off and that bit of the quotient is 1.
GalleryProject dividerSteps() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 4);
    n.input("B", {0, 8}, 4);
    n.split("A", {14, -3}, 4, bitNames("a", 4));
    for (int bit = 0; bit < 4; ++bit) n.label("a" + std::to_string(bit));
    n.split("B", {12, 7}, 4, bitNames("b", 4));
    n.join({16, 7}, 5, {"b0", "b1", "b2", "b3", ""}, "divisor");  // B with a 0 on top, as wide as the remainder
    n.label("divisor");
    std::string remainder;
    for (int step = 0; step < 4; ++step) {
        const int bit = 3 - step;
        const int y = 20 * step;
        const std::string i = std::to_string(bit);
        std::vector<std::string> tried{"a" + i};
        if (remainder.empty()) {
            tried.insert(tried.end(), {"", "", "", ""});
        } else {
            n.split(remainder.c_str(), {22, y}, 5, {"r" + i + "0", "r" + i + "1", "r" + i + "2", "r" + i + "3", ""});
            for (int k = 0; k < 4; ++k) tried.push_back("r" + i + std::to_string(k));
        }
        const std::string triedName = "t" + i, difference = "diff" + i, borrow = "borrow" + i, next = "rem" + i, q = "q" + i;
        n.join({36, y}, 5, tried, triedName.c_str());
        n.box("sub_n", {54, y}, {{"A", triedName.c_str()}, {"B", "divisor"}}, {{"D", difference.c_str()}, {"Vs", borrow.c_str()}}, 5);
        n.box("mux2_n", {72, y}, {{"A", difference.c_str()}, {"B", triedName.c_str()}, {"Sel", borrow.c_str()}}, {{"S", next.c_str()}}, 5);
        n.gate(PartKind::Not, {72, y + 10}, {borrow.c_str()}, q.c_str());
        n.label(q);
        remainder = next;
    }
    n.join({92, 10}, 4, bitNames("q", 4), "quotient");
    n.split(remainder.c_str(), {92, 30}, 5, {"R0", "R1", "R2", "R3", ""});
    n.join({100, 30}, 4, bitNames("R", 4), "rest");
    n.display("Q", {106, 13}, "quotient", 4);
    n.display("R", {108, 33}, "rest", 4);
    return finished(std::move(board));
}

GalleryProject negatorAdder() {
    Board board;
    Net n(board);
    n.input("A", {0, 0}, 4);
    n.gate(PartKind::Not, {14, -1}, {"A"}, "notA", 4);
    const PartId one = n.l.constant(1, {14, 6}, 1);
    n.source("one", one, 0);
    n.box("adder_n", {24, 0}, {{"A", "notA"}, {"Ce", "one"}}, {{"S", "minus"}}, 4);
    n.display("N", {40, 1}, "minus", 4);
    return finished(std::move(board));
}

// Each bit added in turn to a running total.
GalleryProject bitCountAdders() {
    Board board;
    Net n(board);
    n.input("D", {0, 0}, 8);
    n.split("D", {22, -2}, 8, bitNames("d", 8));
    for (int bit = 0; bit < 8; ++bit) n.label("d" + std::to_string(bit));
    std::string total;
    for (int bit = 0; bit < 8; ++bit) {
        const std::string d = "d" + std::to_string(bit), widened = "w" + std::to_string(bit), sum = "c" + std::to_string(bit);
        const int y = 16 * bit;
        n.join({40, y}, 7, {d, "", "", "", "", "", ""}, widened.c_str());
        if (bit == 0) {
            total = widened;
            continue;
        }
        n.box("adder_n", {52, y - 4}, {{"A", total.c_str()}, {"B", widened.c_str()}}, {{"S", sum.c_str()}}, 7);
        total = sum;
    }
    n.display("C", {70, 116}, total.c_str(), 7);
    return finished(std::move(board));
}

// From the top bit down, each bit that's on puts its own number through: the last to do so, the lowest,
// is what comes out.
GalleryProject bitFindMuxes() {
    Board board;
    Net n(board);
    n.input("D", {0, 0}, 8);
    n.split("D", {22, -2}, 8, bitNames("d", 8));
    for (int bit = 0; bit < 8; ++bit) n.label("d" + std::to_string(bit));
    std::string index;
    for (int k = 0; k < 8; ++k) {
        const int bit = 7 - k;
        const int y = 10 * k;
        const std::string number = "n" + std::to_string(bit), next = "i" + std::to_string(bit), d = "d" + std::to_string(bit);
        const PartId constant = n.l.constant(static_cast<std::uint64_t>(bit), {34, y + 3}, 6);
        n.source(number, constant, 0);
        if (index.empty()) {
            n.box("mux2_n", {46, y}, {{"B", number.c_str()}, {"Sel", d.c_str()}}, {{"S", next.c_str()}}, 6);
        } else {
            n.box("mux2_n", {46, y}, {{"A", index.c_str()}, {"B", number.c_str()}, {"Sel", d.c_str()}}, {{"S", next.c_str()}}, 6);
        }
        index = next;
    }
    n.gate(PartKind::Or, {64, 84}, {"d0", "d1", "d2", "d3"}, "low");
    n.gate(PartKind::Or, {64, 94}, {"d4", "d5", "d6", "d7"}, "high");
    n.gate(PartKind::Or, {76, 89}, {"low", "high"}, "v");
    n.display("I", {66, 72}, index.c_str(), 6);
    n.light("V", {86, 90}, "v");
    return finished(std::move(board));
}

// --- Input and output -----------------------------------------------------------------------------------

// A number to its digit: the decoder turns the four bits into the seven segments.
GalleryProject displayWithDecoder() {
    Board board;
    Net n(board);
    n.input("N", {0, 6}, 4);
    n.box("decoder7", {14, 0}, {{"N", "N"}}, {{"a", "a"}, {"b", "b"}, {"c", "c"}, {"d", "d"}, {"e", "e"}, {"f", "f"}, {"g", "g"}});
    const PartId shown = n.l.part(PartKind::Display7, {32, -1});
    const char* segments[] = {"a", "b", "c", "d", "e", "f", "g"};
    for (unsigned pin = 0; pin < 7; ++pin) n.feed(segments[pin], shown, pin);
    return finished(std::move(board));
}

// Each segment by its own switch, to see which is which.
GalleryProject displayByHand() {
    Board board;
    Net n(board);
    const char* segments[] = {"a", "b", "c", "d", "e", "f", "g"};
    for (int i = 0; i < 7; ++i) n.input(segments[i], {0, 2 * i});
    const PartId shown = n.l.part(PartKind::Display7, {14, -1});
    for (unsigned pin = 0; pin < 7; ++pin) n.feed(segments[pin], shown, pin);
    return finished(std::move(board));
}

// Each press of the button is a clock pulse for a counter, and the count shows as a digit.
GalleryProject buttonCounter() {
    Board board;
    Net n(board);
    Part key = makePart(PartKind::Button, {0, 4});
    key.label = "Botão";
    const PartId button = n.l.add(std::move(key));
    n.source("press", button, 0);
    const PartId on = n.l.constant(1, {0, 0}, 1);
    n.source("one", on, 0);
    n.box("counter", {10, 0}, {{"En", "one"}, {"Clk", "press"}}, {{"Q", "count"}}, 4);
    n.display("Contagem", {28, 6}, "count", 4);
    n.box("decoder7", {28, 12}, {{"N", "count"}}, {{"a", "a"}, {"b", "b"}, {"c", "c"}, {"d", "d"}, {"e", "e"}, {"f", "f"}, {"g", "g"}});
    const PartId shown = n.l.part(PartKind::Display7, {46, 11});
    const char* segments[] = {"a", "b", "c", "d", "e", "f", "g"};
    for (unsigned pin = 0; pin < 7; ++pin) n.feed(segments[pin], shown, pin);
    return finished(std::move(board));
}

GalleryProject randomReady() {
    Board board;
    Net n(board);
    const PartId clock = *board.addPart(PartKind::Clock, {0, 2});
    n.source("clock", clock, 0);
    n.box("random", {10, 0}, {{"Clk", "clock"}}, {{"Q", "q"}});
    n.display("Q", {28, 1}, "q", 8);
    return finished(std::move(board));
}

// A register of eight bits that shifts left on every pulse: its bits go back into it one place up, and
// the bit that comes in at the bottom is an XNOR of four of them.
GalleryProject randomShiftRegister() {
    Board board;
    Net n(board);
    const PartId clock = *board.addPart(PartKind::Clock, {0, 2});
    n.source("Clk", clock, 0);
    const PartId on = n.l.constant(1, {0, 8}, 1);
    n.source("one", on, 0);
    const PartId keep = n.l.box("register", {50, 0}, 8);
    n.source("q", keep, n.l.pin(keep, "Q"));
    n.split("q", {66, 10}, 8, bitNames("q", 8));
    for (int bit = 0; bit < 8; ++bit) n.label("q" + std::to_string(bit));
    n.gate(PartKind::Xnor, {14, 20}, {"q7", "q5"}, "x1");
    n.gate(PartKind::Xnor, {14, 28}, {"q4", "q3"}, "x2");
    n.gate(PartKind::Xnor, {26, 24}, {"x1", "x2"}, "feedback");
    n.join({38, 10}, 8, {"feedback", "q0", "q1", "q2", "q3", "q4", "q5", "q6"}, "shifted");
    n.feed("shifted", keep, n.l.pin(keep, "D"));
    n.feed("one", keep, n.l.pin(keep, "En"));
    n.feed("Clk", keep, n.l.pin(keep, "Clk"));
    n.display("Q", {66, 1}, "q", 8);
    return finished(std::move(board));
}

// A smiling face: each row of the matrix is a number, its bits the dots.
constexpr std::uint64_t kFace[8] = {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C};

GalleryProject matrixConstants() {
    Board board;
    Layout l(board);
    const PartId matrix = l.box("led_matrix", {10, 0}, 8);
    for (unsigned row = 0; row < 8; ++row) {
        const PartId value = l.constant(kFace[row], {0, 2 * static_cast<int>(row)}, 8);
        l.wire(value, 0u, matrix, row);
    }
    return finished(std::move(board));
}

GalleryProject matrixSwitches() {
    Board board;
    Layout l(board);
    const PartId matrix = l.box("led_matrix", {26, 0}, 8);
    GalleryProject out;
    for (unsigned row = 0; row < 8; ++row) {
        const std::string name = "L" + std::to_string(row);
        const PartId toggle = l.input(name.c_str(), {0, 2 * static_cast<int>(row)}, 8);
        l.wire(toggle, 0u, matrix, row);
        out.toggles.emplace_back(toggle, kFace[row]);
    }
    out.board = std::move(board);
    return out;
}

// The real keyboard into the terminal: each key that goes down is a clock pulse that writes its code.
GalleryProject keyboardTerminal() {
    Board board;
    Layout l(board);
    Part keys = makePart(PartKind::Keyboard, {0, 2});  // Código straight into Char
    keys.label = "Teclado";
    const PartId keyboard = l.add(std::move(keys));
    const PartId screen = l.box("terminal", {20, 0});
    const PartId one = l.constant(1, {14, 4}, 1);  // right next to Write, out of Tecla's way
    l.wire(keyboard, 0u, screen, "Char");
    l.wire(keyboard, 1u, screen, "Clk");
    l.wire(one, 0u, screen, "Write");
    const PartId count = l.output("N", {50, 5}, 6);
    l.wire(screen, "N", count, 0u);
    return finished(std::move(board));
}

// --- Electronics -------------------------------------------------------------------------------------

GalleryProject ledAndResistor() { return finished(buildLedExample().board); }
GalleryProject blinker() { return finished(buildBlinker().board); }

// Two battery circuits side by side: switches in a row light the LED only together, side by side either one does.
GalleryProject switchesInSeriesAndParallel() {
    Board board;
    Layout l(board);
    const auto circuit = [&](int x, bool parallel) {
        const PartId battery = l.part(PartKind::BatteryCell, {x, 16});
        const PartId first = l.part(PartKind::Switch, {x + 4, 2});
        const PartId second = l.part(PartKind::Switch, parallel ? Point{x + 4, 7} : Point{x + 11, 2});
        const PartId resistor = l.part(PartKind::Resistor, {x + 18, 2}, 0, 330 * kOhm);
        const PartId light = l.part(PartKind::LedBulb, {x + 25, 0});
        l.wire(battery, 0u, first, 0u);
        if (parallel) {
            l.wire(battery, 0u, second, 0u);
            l.wire(first, 1u, resistor, 0u);
            l.wire(second, 1u, resistor, 0u);
        } else {
            l.wire(first, 1u, second, 0u);
            l.wire(second, 1u, resistor, 0u);
        }
        l.wire(resistor, 1u, light, 0u);
        l.wire(light, 1u, battery, 1u);
    };
    circuit(0, false);
    circuit(40, true);
    return finished(std::move(board));
}


// --- The examples for the lessons that had none --------------------------------------------------------

constexpr std::uint64_t kOhms = 1000;  // resistors keep milliohms

// The LED circuit drawn the way schematics draw it: a source, and the way back through grounds.
GalleryProject sourceAndGroundExample() {
    Board board;
    Layout l(board);
    const PartId source = l.part(PartKind::Battery, {0, 10});
    l.wire(source, 1u, l.part(PartKind::Ground, {9, 13}), 0u);
    const PartId resistor = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhms);
    const PartId light = l.part(PartKind::LedBulb, {16, 2});
    l.wire(source, 0u, resistor, 0u);
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, l.part(PartKind::Ground, {22, 8}), 0u);
    return finished(std::move(board));
}

// A switch sends current into a transistor's base, and the transistor lights the LED.
GalleryProject transistorSwitchExample() {
    Board board;
    Layout l(board);
    const PartId source = l.part(PartKind::Battery, {40, 24});
    l.name(source, 0, "5V", true);
    l.wire(source, 1u, l.part(PartKind::Ground, {46, 27}), 0u);
    Part supply = makePart(PartKind::Tunnel, {0, 14});
    supply.label = "5V";
    const PartId label = l.add(std::move(supply));
    const PartId load = l.part(PartKind::Resistor, {26, 0}, 1, 330 * kOhms);
    l.name(load, 0, "5V", true);
    const PartId light = l.part(PartKind::LedBulb, {28, 5});
    l.wire(load, 1u, light, 0u);
    const PartId transistor = l.part(PartKind::Npn, {24, 15});
    const PartId toggle = l.part(PartKind::Switch, {8, 14});
    const PartId base = l.part(PartKind::Resistor, {15, 14}, 0, 10'000 * kOhms);
    l.wire(label, 0u, toggle, 0u);
    l.wire(toggle, 1u, base, 0u);
    l.wire(base, 1u, transistor, 0u);
    l.wire(transistor, 2u, l.part(PartKind::Ground, {26, 24}), 0u);
    l.wire(light, 1u, transistor, 1u);
    return finished(std::move(board));
}

// The gates built from NAND alone.
GalleryProject notOfNand() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.gate(PartKind::Nand, {12, 1}, {"A", "A"}, "S");
    n.light("S", {22, 2}, "S");
    return finished(std::move(board));
}

GalleryProject andOfNand() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 8});
    n.gate(PartKind::Nand, {12, 4}, {"A", "B"}, "n");
    n.gate(PartKind::Nand, {24, 4}, {"n", "n"}, "S");
    n.light("S", {34, 5}, "S");
    return finished(std::move(board));
}

GalleryProject orOfNand() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 12});
    n.gate(PartKind::Nand, {12, 1}, {"A", "A"}, "na");
    n.gate(PartKind::Nand, {12, 11}, {"B", "B"}, "nb");
    n.gate(PartKind::Nand, {26, 6}, {"na", "nb"}, "S");
    n.light("S", {36, 7}, "S");
    return finished(std::move(board));
}

// XOR from four NANDs, the classic way.
GalleryProject xorOfNand() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 14});
    n.gate(PartKind::Nand, {12, 7}, {"A", "B"}, "m");
    n.gate(PartKind::Nand, {24, 2}, {"A", "m"}, "p");
    n.gate(PartKind::Nand, {24, 12}, {"m", "B"}, "q");
    n.gate(PartKind::Nand, {36, 7}, {"p", "q"}, "S");
    n.light("S", {46, 8}, "S");
    return finished(std::move(board));
}

// MUX 2:1 of gates: S = (A AND NOT Sel) OR (B AND Sel).
GalleryProject mux2Gates() {
    Board board;
    Net n(board);
    n.input("A", {0, 2});
    n.input("B", {0, 10});
    n.input("Sel", {0, 18});
    n.gate(PartKind::Not, {12, 17}, {"Sel"}, "ns");
    n.gate(PartKind::And, {24, 3}, {"A", "ns"}, "a");
    n.gate(PartKind::And, {24, 11}, {"B", "Sel"}, "b");
    n.gate(PartKind::Or, {36, 7}, {"a", "b"}, "S");
    n.light("S", {46, 8}, "S");
    return finished(std::move(board));
}

// MUX 4:1 of gates: a decoder of S1 S0, an AND per input, and an OR.
GalleryProject mux4Gates() {
    Board board;
    Net n(board);
    n.input("D0", {0, 0});
    n.input("D1", {0, 6});
    n.input("D2", {0, 12});
    n.input("D3", {0, 18});
    n.input("S0", {0, 26});
    n.input("S1", {0, 32});
    n.gate(PartKind::Not, {12, 25}, {"S0"}, "n0");
    n.gate(PartKind::Not, {12, 31}, {"S1"}, "n1");
    n.gate(PartKind::And, {28, 0}, {"D0", "n1", "n0"}, "a0");
    n.gate(PartKind::And, {28, 8}, {"D1", "n1", "S0"}, "a1");
    n.gate(PartKind::And, {28, 16}, {"D2", "S1", "n0"}, "a2");
    n.gate(PartKind::And, {28, 24}, {"D3", "S1", "S0"}, "a3");
    n.gate(PartKind::Or, {42, 11}, {"a0", "a1", "a2", "a3"}, "Y");
    n.light("Y", {54, 13}, "Y");
    return finished(std::move(board));
}

// Latch D of gates: two ANDs let D through only while En is on, into an SR latch of NORs.
GalleryProject dLatchGates() {
    Board board;
    Net n(board);
    n.input("D", {0, 2});
    n.input("En", {0, 10});
    n.gate(PartKind::Not, {12, 17}, {"D"}, "nd");
    n.gate(PartKind::And, {24, 2}, {"D", "En"}, "s");
    n.gate(PartKind::And, {24, 14}, {"nd", "En"}, "r");
    const PartId upper = n.l.gate(PartKind::Nor, {38, 1});
    const PartId lower = n.l.gate(PartKind::Nor, {38, 13});
    n.feed("s", upper, 0);
    n.feed("r", lower, 1);
    n.l.wire(upper, 2u, lower, 0u);
    n.l.wire(lower, 2u, upper, 1u);
    n.source("q", lower, 2);
    n.source("notQ", upper, 2);
    n.light("Q", {50, 14}, "q");
    n.light("!Q", {50, 2}, "notQ");
    return finished(std::move(board));
}

// One D latch of gates, for the flip-flop: two ANDs let the data through while the enable is on, into an SR
// latch of NORs that starts with Q at 0.
void gateLatch(Net& n, int x, const char* data, const char* enable, const std::string& q, const std::string& notQ) {
    const std::string inverted = q + "_nd";
    const std::string set = q + "_s";
    const std::string reset = q + "_r";
    n.gate(PartKind::Not, {x, 18}, {data}, inverted.c_str());
    n.gate(PartKind::And, {x + 12, 2}, {data, enable}, set.c_str());
    n.gate(PartKind::And, {x + 12, 14}, {inverted.c_str(), enable}, reset.c_str());
    const PartId upper = n.l.gate(PartKind::Nor, {x + 26, 1});
    const PartId lower = n.l.gate(PartKind::Nor, {x + 26, 13});
    n.feed(set, upper, 0);
    n.feed(reset, lower, 1);
    n.l.wire(upper, 2u, lower, 0u);
    n.l.wire(lower, 2u, upper, 1u);
    n.source(q, lower, 2);
    n.source(notQ, upper, 2);
}

// Flip-flop D of two latches of gates: the master open while the clock is low, the slave while it's high.
GalleryProject flipFlopLatches() {
    Board board;
    Net n(board);
    n.input("D", {0, 2});
    n.input("Clk", {0, 26});
    n.gate(PartKind::Not, {12, 25}, {"Clk"}, "nclk");
    gateLatch(n, 24, "D", "nclk", "m", "nm");
    gateLatch(n, 66, "m", "Clk", "q", "nq");
    n.light("Q", {106, 14}, "q");
    n.light("!Q", {106, 2}, "nq");
    return finished(std::move(board));
}

// The words on the version buttons.
constexpr const char* kReady = "Peça pronta";
constexpr const char* kGates = "Portas lógicas";
constexpr const char* kTransistors = "Transistores";
constexpr const char* kReadyHow = "A peça pronta da biblioteca, com uma chave em cada entrada e uma luz em cada saída. As outras versões mostram o que existe "
                                  "dentro dela.";

}  // namespace

std::vector<GalleryItem> circuitExamples() {
    const auto gate = [](const char* id, const char* title, const char* line, GalleryProject (*gates)(), GalleryProject (*transistors)(),
                         const char* gatesHow, const char* transistorsHow, const char* about) {
        return GalleryItem{id, kGroupGates, title, line, {{kGates, gates, gatesHow}, {kTransistors, transistors, transistorsHow}}, -1, about,
                           "Clique nas entradas e acompanhe a saída e a tabela-verdade ao lado. Na versão com transistores, passe o mouse nas "
                           "peças para ler tensões e correntes."};
    };
    std::vector<GalleryItem> items{
        // Electronics.
        {"led", kGroupElectronics, "LED e resistor", "O primeiro circuito, com uma chave",
         {{"Circuito", ledAndResistor,
           "O LED fica com cerca de 2 V; os 3 V que sobram ficam no resistor de 330 Ω, que deixa passar I = 3 V ÷ 330 Ω ≈ 9 mA, uma "
           "corrente segura para o LED."}},
         -1,
         "O circuito elétrico mais simples: uma bateria, uma chave, um resistor e um LED em série. A corrente sai do + da bateria, atravessa "
         "todas as peças e volta pelo -; abrindo a chave, o caminho se interrompe e ela para.",
         "Feche a chave e passe o mouse sobre cada peça para ler tensão e corrente. Troque o resistor com Editar valor e veja o brilho mudar."},
        {"chaves", kGroupElectronics, "Chaves em série e em paralelo", "A lógica antes do transistor",
         {{"Circuito", switchesInSeriesAndParallel,
           "É lógica feita com contatos, como nos primeiros computadores, de relés: a forma de ligar as chaves decide a função, sem nenhuma "
           "porta."}},
         -1,
         "Duas chaves controlam um LED de dois jeitos. Em série (à esquerda), a corrente atravessa uma e depois a outra: só passa com as duas "
         "fechadas, a função E. Em paralelo (à direita), cada chave é um caminho: basta uma fechada, a função OU.",
         "Feche só uma chave de cada lado e compare: apenas o LED da direita acende."},
        {"fonte-terra", kGroupElectronics, "Fonte e terra", "O mesmo circuito, do jeito dos esquemas",
         {{"Circuito", sourceAndGroundExample,
           "A fonte é a bateria desenhada como nos esquemas: o traço comprido é o +. O - vai num terra, e o LED volta por outro terra, que é o "
           "mesmo ponto: em vez de um fio comprido até o -, cada peça liga no terra mais perto."}},
         -1,
         "O LED e o resistor do primeiro circuito, agora com fonte e terra. O terra é o ponto de 0 V, a referência das tensões, e todos os "
         "terras de um esquema estão ligados entre si mesmo sem fio desenhado.",
         "Passe o mouse no LED e nos terras para ler as tensões: os terras ficam todos em 0 V."},
        {"transistor-chave", kGroupElectronics, "Transistor como chave", "Uma corrente pequena comanda uma grande",
         {{"Circuito", transistorSwitchExample,
           "A chave leva corrente à base do NPN por um resistor de 10 kΩ, uns 0,4 mA. Com isso o transistor conduz entre o coletor e o emissor, "
           "e a corrente do LED, uns 9 mA, vai do 5V ao terra passando por ele. Sem corrente na base, o caminho fecha."}},
         -1,
         "O transistor é uma chave comandada: a base recebe um sinal fraco, e o caminho entre o coletor e o emissor deixa passar uma corrente "
         "bem maior. Repare que a corrente do LED nem passa pela chave.",
         "Abra e feche a chave. Passe o mouse na base e no coletor para comparar as correntes."},
        {"pisca", kGroupElectronics, "Pisca-pisca", "Multivibrador astável",
         {{"Circuito", blinker,
           "Quando um transistor liga, ele leva a 0 V um lado do capacitor ligado à base do outro, que desliga. Esse capacitor então se recarrega "
           "pelo resistor de base; quando a base passa de 0,6 V, o outro transistor liga e tudo se inverte. Cada metade dura cerca de 0,7 × R × C."}},
         -1,
         "Um oscilador de dois transistores, o multivibrador astável: os LEDs acendem um de cada vez, sem nenhum sinal de comando. É um dos "
         "circuitos mais clássicos da eletrônica, parente do clock que dá o ritmo a um computador.",
         "Aperte Rodar e passe o mouse sobre os capacitores e as bases dos transistores: a tensão sobe devagar até o lado virar."},

        // Logic gates.
        gate("nao", "Porta NÃO (NOT)", "A saída é o contrário da entrada", [] { return gateAlone(PartKind::Not); }, notTransistors,
             "O símbolo é um triângulo com uma bolinha na saída. A bolinha, em qualquer porta, quer dizer: invertido.",
             "Um transistor com um resistor até 5 V. Com A em 0, o transistor fica desligado e o resistor puxa S para 5 V, o 1. Com A em 1, a "
             "corrente na base liga o transistor, que puxa S para perto de 0 V, o 0.",
             "A porta NÃO, ou inversor, tem uma entrada e entrega o contrário dela: S = NÃO A. É a porta mais simples, e não há memória nem "
             "oscilador digital sem inversores."),
        gate("e", "Porta E (AND)", "Liga só com todas as entradas", [] { return gateAlone(PartKind::And); }, andTransistors,
             "O símbolo tem a frente arredondada e o fundo reto. Com mais entradas, a regra é a mesma: todas em 1.",
             "Dois transistores em série sob um resistor formam uma NAND: a saída só cai a 0 quando os dois ligam. Um terceiro transistor inverte "
             "esse resultado e entrega o E. Por isso, em transistores, a NAND é mais simples que o E.",
             "A porta E liga a saída só quando todas as entradas estão ligadas: S = A E B. Em binário, é a multiplicação de bits: qualquer coisa "
             "vezes 0 dá 0. Ela deixa um sinal passar só quando outro autoriza."),
        gate("ou", "Porta OU (OR)", "Liga com qualquer entrada", [] { return gateAlone(PartKind::Or); }, orTransistors,
             "O símbolo tem o fundo curvo e a ponta afinada. Com mais entradas, basta uma delas em 1.",
             "Dois transistores lado a lado, em paralelo, formam uma NOR: qualquer um que ligue leva a saída a 0. Um terceiro transistor inverte "
             "esse resultado e entrega o OU.",
             "A porta OU liga a saída quando pelo menos uma entrada está ligada: S = A OU B. Só dá 0 com todas as entradas em 0. Ela junta "
             "sinais: qualquer um deles basta."),
        gate("nand", "Porta NAND", "Um E invertido, a porta universal", [] { return gateAlone(PartKind::Nand); }, nandTransistors,
             "O símbolo é o do E com a bolinha da inversão na saída.",
             "Dois transistores em série entre a saída e o terra. A corrente só encontra caminho para o terra com os dois ligados, A e B em 1, e "
             "só então S cai a 0. Em qualquer outro caso o resistor mantém S em 5 V.",
             "A NAND é um E com a saída invertida: só dá 0 quando A e B são 1. Ela é universal: com NANDs se constrói qualquer outra porta, e um "
             "computador inteiro. Por ser a mais simples em transistores, é a base dos circuitos integrados."),
        gate("nor", "Porta NOR", "Um OU invertido, também universal", [] { return gateAlone(PartKind::Nor); }, norTransistors,
             "O símbolo é o do OU com a bolinha da inversão na saída.",
             "Dois transistores em paralelo entre a saída e o terra. Basta um ligar para abrir um caminho ao terra e levar S a 0. Só com A e B em "
             "0 os dois ficam desligados e o resistor puxa S para 5 V.",
             "A NOR é um OU com a saída invertida: só dá 1 com A e B em 0. Como a NAND, é universal: o computador de bordo da Apollo 11 foi feito "
             "só de portas NOR."),
        gate("xor", "Porta OU exclusivo (XOR)", "Liga quando as entradas diferem", [] { return gateAlone(PartKind::Xor); }, xorTransistors,
             "O símbolo é o do OU com uma segunda curva atrás das entradas.",
             "Quatro NANDs, oito transistores. A primeira calcula NAND(A, B); as duas do meio combinam esse resultado com A e com B; a última "
             "junta as duas e só dá 1 quando A e B são diferentes. É a montagem clássica do XOR com portas universais.",
             "A XOR liga a saída quando as entradas são diferentes: uma ou outra, mas não as duas. Ela é a soma de dois bits sem o vai-um "
             "(1 + 1 = 0, vai 1), e também compara bits e inverte um sinal sob comando."),
        gate("xnor", "Porta XNOR", "Liga quando as entradas são iguais", [] { return gateAlone(PartKind::Xnor); }, xnorTransistors,
             "O símbolo é o do XOR com a bolinha da inversão na saída.",
             "Quatro NORs, oito transistores, na mesma montagem do XOR feito com NANDs. Trocar NAND por NOR inverte a função: em vez de "
             "diferentes, o circuito responde iguais.",
             "A XNOR é a XOR invertida: liga a saída quando as entradas são iguais, e por isso também se chama porta de igualdade. É a peça "
             "básica de um comparador: uma XNOR por bit diz se dois números são iguais."),

        // More gates and choosing.
        {"buffer", kGroupMoreGates, "Buffer", "Passa o sinal adiante, refeito",
         {{kReady, [] { return readyBox("buffer"); }, kReadyHow},
          {kGates, bufferGates, "Dois inversores em fila: o primeiro inverte, o segundo desfaz. A lógica não muda, mas o sinal sai da segunda porta com força nova."},
          {kTransistors, bufferTransistors,
           "Dois inversores de um transistor cada. A saída do segundo vem do seu próprio resistor até 5 V: é por isso que um buffer pode alimentar "
           "muitas entradas, ou um fio longo, sem enfraquecer."}},
         -1,
         "O buffer entrega na saída o mesmo valor da entrada: S = A. Parece inútil, mas cada saída só consegue alimentar algumas entradas; quando "
         "um sinal precisa ir para muitos lugares, um buffer o refaz com força total.",
         "Clique em A: S acompanha. Na versão com transistores, passe o mouse para ver as tensões em cada estágio."},
        {"paridade-impar", kGroupMoreGates, "Paridade ímpar", "1 quando há uma quantidade ímpar de 1s",
         {{kReady, [] { return readyBox("parity_odd", 4); }, kReadyHow},
          {kGates, [] { return parityGates(false); },
           "Os bits passam por XORs em fila. Cada XOR inverte o resultado quando o bit que chega é 1, então no fim ele diz se houve um número "
           "ímpar de inversões."}},
         -1,
         "Conta os 1s de D e diz se são ímpares. A paridade é o jeito mais simples de detectar erros: quem envia acrescenta um bit de paridade, "
         "e quem recebe confere se a conta bate. Um bit trocado no caminho muda a paridade.",
         "Mude os bits de D: S acende com um, três ou cinco bits em 1."},
        {"paridade-par", kGroupMoreGates, "Paridade par", "1 quando há uma quantidade par de 1s",
         {{kReady, [] { return readyBox("parity_even", 4); }, kReadyHow},
          {kGates, [] { return parityGates(true); }, "A mesma fila de XORs, com a última trocada por uma XNOR, que inverte o resultado: par em vez de ímpar."}},
         -1,
         "O contrário da paridade ímpar: S é 1 quando D tem uma quantidade par de 1s, zero inclusive. Junto com os bits de D, ele forma uma "
         "palavra que sempre tem uma quantidade ímpar de 1s.",
         "Com D em 0, S já acende: zero é par."},
        {"prioridade", kGroupMoreGates, "Codificador de prioridade", "O número da entrada mais alta ligada",
         {{kReady, [] { return readyBox("priority4"); }, kReadyHow},
          {kGates, priorityGates,
           "A1 liga quando D3 ou D2 está ligada. A0 liga com D3, ou com D1 se D2 estiver desligada: assim a entrada mais alta manda. V é um OU das "
           "quatro."}},
         -1,
         "Diz, em binário, qual é a entrada mais alta que está ligada, e V avisa que alguma está. É assim que um processador decide qual pedido "
         "atender primeiro quando vários chegam juntos, como as interrupções de vários aparelhos.",
         "Ligue D1 e depois D3: A1 A0 passa de 01 para 11, porque D3 tem prioridade."},
        {"seletor-bits", kGroupMoreGates, "Seletor de bits", "Escolhe um bit pelo número",
         {{kReady, [] { return readyBox("bit_select", 8); }, kReadyHow},
          {"Multiplexadores", bitSelectMuxes,
           "Sete multiplexadores 2:1 em três rodadas. O bit 0 de Sel escolhe dentro de cada par, o bit 1 entre os pares e o bit 2 entre as "
           "metades: a cada rodada sobra metade, até restar um."}},
         -1,
         "Entrega em S o bit de D cujo número está em Sel: com Sel em 5, S mostra o bit 5. É um multiplexador de muitas entradas, e serve para "
         "ler um bit de um registrador, como as instruções que testam bits.",
         "Ponha um número em D e passe Sel de 0 a 7: S mostra cada bit."},
        {"decodificador", kGroupMoreGates, "Decodificador 2 para 4", "Uma saída para cada número",
         {{kReady, [] { return readyBox("decoder2"); }, kReadyHow},
          {kGates, decoderGates,
           "Dois inversores dão A0 e A1 negados, e cada uma das quatro E reconhece um número: Y0 quer os dois em 0, Y3 os dois em 1."}},
         -1,
         "Acende só a saída do número que A1 e A0 formam em binário: 00 acende Y0, 11 acende Y3. Decodificadores escolhem qual registrador ou "
         "qual endereço de memória vai trabalhar.",
         "Passe A1 A0 por 00, 01, 10 e 11."},

        {"mux2", kGroupMoreGates, "Multiplexador 2:1", "Escolhe entre duas entradas",
         {{kReady, [] { return readyBox("mux2"); }, kReadyHow},
          {kGates, mux2Gates,
           "Uma NÃO inverte Sel. Uma E deixa A passar quando Sel é 0, a outra deixa B passar quando Sel é 1, e uma OU junta as duas: só uma "
           "delas pode estar ligada de cada vez."}},
         -1,
         "Uma chave eletrônica de duas posições: Sel em 0 manda A para a saída, Sel em 1 manda B. Em expressão, S = (A E NÃO Sel) OU (B E Sel). "
         "Os MUX do Raio-X são assim, um por bit.",
         "Ligue só A e mude Sel: S segue A com Sel em 0 e apaga com Sel em 1."},
        {"mux4", kGroupMoreGates, "Multiplexador 4:1", "Escolhe entre quatro entradas",
         {{kReady, [] { return readyBox("mux4"); }, kReadyHow},
          {kGates, mux4Gates,
           "Duas NÃO e as quatro E formam um decodificador da seleção: cada E só deixa passar a sua entrada quando S1 S0 forma o número dela. "
           "Uma OU de quatro entradas junta tudo."}},
         -1,
         "S1 e S0 formam um número de 0 a 3, e Y mostra a entrada D com esse número. É o que, dentro da ULA, escolhe qual operação vai para o "
         "resultado.",
         "Ligue D2 e escolha S1 = 1, S0 = 0: Y acende. Mude a seleção e veja Y seguir outra entrada."},
        {"display7", kGroupMoreGates, "Display de 7 segmentos", "Desenha um dígito",
         {{"Com decodificador", displayWithDecoder,
           "Um decodificador de 7 segmentos olha os quatro bits de N e liga os segmentos do dígito, de 0 a F. Por dentro, cada segmento é uma "
           "função dos quatro bits, com a sua própria tabela-verdade."},
          {"Segmento por segmento", displayByHand,
           "Uma chave para cada segmento, de a a g, para ver qual é qual: a em cima, b e c à direita, d embaixo, e e f à esquerda e g no meio."}},
         -1,
         "Sete barras, a até g, desenham qualquer algarismo, e um ponto marca a vírgula. É o display das calculadoras, relógios e painéis: "
         "cada segmento é um LED, aceso pelo seu pino.",
         "Na versão com decodificador, passe N de 0 a 15 e veja os dígitos de 0 a F."},

        {"matriz", kGroupMoreGates, "Matriz de LEDs", "Desenhos ponto a ponto",
         {{"Desenho", matrixConstants,
           "Oito constantes, uma por linha. Cada linha é um número de 8 bits, e cada bit 1 acende um ponto: 0x3C, por exemplo, é 00111100, os "
           "quatro pontos do meio."},
          {"Com chaves", matrixSwitches,
           "Uma entrada de 8 bits para cada linha: clique nos bits para acender e apagar pontos e desenhar o que quiser."}},
         -1,
         "Uma grade de 8 por 8 LEDs, ligada linha a linha: cada linha recebe um número, e os seus bits dizem quais pontos acendem. Letreiros, "
         "telas de relógio e os primeiros videogames desenhavam assim, com memórias que guardam cada linha.",
         "Na versão com chaves, mude os bits de uma linha e veja o desenho mudar."},

        // More sums.
        {"multiplicador", kGroupMoreMath, "Multiplicador", "A × B em binário",
         {{kReady, [] { return readyBox("multiplier", 4); }, kReadyHow},
          {"Portas e somadores", multiplierArray,
           "O multiplicador em matriz: dezesseis E, uma para cada par de bits, dão os produtos parciais; cada linha, deslocada para a sua casa, é "
           "somada à anterior. É a conta de multiplicar do papel, em binário, onde cada algarismo é 0 ou 1."}},
         -1,
         "Multiplica dois números de N bits, aqui 4. O produto tem o dobro de bits: Baixo são os 4 de baixo e Alto os 4 de cima, então 15 × 15 = "
         "225 sai como Alto 14 e Baixo 1 (14 × 16 + 1).",
         "Faça 6 × 7: Alto mostra 2 e Baixo 10, ou seja 2 × 16 + 10 = 42."},
        {"divisor", kGroupMoreMath, "Divisor", "A ÷ B, com o resto",
         {{kReady, [] { return readyBox("divider", 4); }, kReadyHow},
          {"Subtratores", dividerSteps,
           "A divisão longa, em quatro passos, do bit mais alto de A para baixo. Em cada passo o resto recebe o próximo bit de A; um subtrator "
           "tenta tirar B, e um multiplexador fica com o resultado se coube, ou com o resto como estava se não coube. Coube vira 1 no quociente."}},
         -1,
         "Divide A por B, números de N bits, aqui 4: Q é o quociente e R o resto, de modo que A = Q × B + R. Dividir por zero não tem resposta; "
         "o circuito dá Q com todos os bits em 1.",
         "Faça 13 ÷ 4: Q mostra 3 e R mostra 1."},
        {"negador", kGroupMoreMath, "Negador", "Troca o sinal: −A",
         {{kReady, [] { return readyBox("negator", 4); }, kReadyHow},
          {"Portas e somador", negatorAdder,
           "Em complemento de dois, −A é NÃO A mais 1. Uma porta NÃO de 4 bits inverte todos os bits, e o somador soma o 1 pelo vai-um de "
           "entrada, que vem de uma constante."}},
         -1,
         "Dá o negativo de A em complemento de dois, o jeito como os computadores guardam números negativos: em 4 bits, −1 é 15 e −3 é 13. "
         "Somar A com −A dá sempre 0, com um vai-um que sobra.",
         "Ponha 3 em A: N mostra 13, que é −3 em 4 bits."},
        {"conta-bits", kGroupMoreMath, "Contador de bits", "Quantos bits estão em 1",
         {{kReady, [] { return readyBox("bit_count", 8); }, kReadyHow},
          {"Somadores", bitCountAdders,
           "Cada bit de D vira um número de 7 bits, 0 ou 1, e entra numa fila de somadores: o total cresce um bit de cada vez. Circuitos rápidos "
           "somam em árvore, aos pares, mas a conta é a mesma."}},
         -1,
         "Conta quantos bits de D estão em 1, a contagem de população. Serve para paridade, para medir a diferença entre duas palavras e em "
         "muitos algoritmos; processadores modernos têm uma instrução só para isso.",
         "Ligue bits de D e veja C contar."},
        {"acha-bit", kGroupMoreMath, "Localizador de bit", "Onde está o primeiro 1",
         {{kReady, [] { return readyBox("bit_find", 8); }, kReadyHow},
          {"Multiplexadores", bitFindMuxes,
           "Uma fila de multiplexadores, do bit 7 para o 0: cada bit ligado põe o seu próprio número no lugar do que veio antes. O último a "
           "fazer isso, o mais baixo, é o que sai em I. V é um OU de todos os bits."}},
         -1,
         "Dá em I o número do bit mais baixo de D que está em 1, e V diz se há algum. Encontrar o primeiro 1 ajuda a achar o primeiro espaço "
         "livre numa lista e a escolher a próxima tarefa de um sistema.",
         "Ponha 12 em D (bits 2 e 3): I mostra 2."},

        // Sums of one bit.
        {"meio-somador", kGroupOneBit, "Meio somador", "Soma dois bits",
         {{kReady, [] { return readyBox("half_adder"); }, kReadyHow},
          {kGates, halfAdderGates, "S é uma XOR, que dá 1 quando A e B são diferentes, e C é uma E, que dá 1 só quando os dois são 1. Duas portas bastam."},
          {kTransistors, halfAdderTransistors,
           "Cinco portas de transistores. As quatro NANDs do XOR dão S, e a primeira delas, NAND(A, B), passa por um inversor e vira o vai-um C: "
           "o mesmo sinal serve às duas saídas."}},
         -1,
         "Soma dois bits e dá um resultado de dois bits: a soma S e o vai-um C. Em binário, 1 + 1 = 10: S = 0 e C = 1. É meio somador porque "
         "não recebe o vai-um de uma coluna anterior.",
         "Teste as quatro combinações de A e B e compare com a tabela-verdade."},
        {"somador", kGroupOneBit, "Somador completo", "Soma três bits, com o vai-um",
         {{kReady, [] { return readyBox("full_adder"); }, kReadyHow},
          {kGates, fullAdderGates,
           "Dois meios somadores: o primeiro soma A e B, o segundo soma o resultado com Ce e dá S. Se qualquer um dos dois der vai-um, o OU leva "
           "esse vai-um para Cs."},
          {kTransistors, fullAdderTransistors,
           "Nove NANDs, dezoito transistores. Duas XORs de quatro NANDs, uma depois da outra, dão S, e uma última NAND junta dois resultados "
           "intermediários e dá Cs. É a versão de muitos chips lógicos."}},
         -1,
         "Soma três bits: A, B e o vai-um da coluna anterior, Ce. Dá a soma S e o vai-um seguinte, Cs. Enfileirando um por bit, com o Cs de "
         "cada um no Ce do próximo, somam-se números de qualquer tamanho.",
         "Ligue A, B e Ce juntos: 1 + 1 + 1 = 11 em binário, S e Cs acesos."},
        {"subtrator", kGroupOneBit, "Subtrator completo", "Subtrai um bit, com o empresta-um",
         {{kReady, [] { return readyBox("full_subtractor"); }, kReadyHow},
          {kGates, subtractorGates,
           "D vem de duas XORs, como a soma. Vs é 1 quando A é 0 e B é 1, ou quando A e B são iguais e chega um empresta-um: as duas condições, "
           "cada uma com uma E, juntam-se num OU."},
          {kTransistors, subtractorTransistors,
           "Nove NANDs, como no somador. As XORs dão D, e os dois sinais intermediários que valem B e não A, e Ve e não (A XOR B), juntam-se "
           "numa última NAND, que dá Vs."}},
         -1,
         "Calcula A − B − Ve, um bit por vez: Ve é o empresta-um que vem da coluna anterior. Dá a diferença D e o empresta-um seguinte, Vs, "
         "que vale 1 quando a conta fica abaixo de zero e precisa pedir à coluna de cima.",
         "Com A em 0 e B em 1, a conta 0 − 1 pede emprestado: D e Vs acendem."},
        {"comparador", kGroupOneBit, "Comparador de 1 bit", "Maior, igual ou menor",
         {{kReady, [] { return readyBox("comparator"); }, kReadyHow},
          {kGates, comparatorGates, "A>B é A E NÃO B; A<B é NÃO A E B; e A=B é a XNOR, que liga quando os dois são iguais. Nunca acendem duas juntas."},
          {kTransistors, comparatorTransistors,
           "Uma NAND de A e B alimenta outras duas, que valem NÃO(A>B) e NÃO(A<B). Dois inversores dão essas saídas, e uma NOR delas dá A=B: "
           "iguais é nem maior, nem menor."}},
         -1,
         "Compara dois bits e liga uma de três saídas: A>B, A=B ou A<B. Comparadores decidem os desvios: quando a CPU executa um BEQ, é uma "
         "comparação que escolhe o caminho do programa.",
         "Passe pelas quatro combinações: só uma saída acende de cada vez."},
        {"ula1", kGroupOneBit, "ULA de 1 bit", "Quatro operações num bit",
         {{kReady, [] { return readyBox("alu1"); }, kReadyHow},
          {"Blocos", aluBitBlocks,
           "As quatro operações são calculadas ao mesmo tempo, cada uma por seu circuito: uma E, uma OU, uma XOR e um somador. Um multiplexador "
           "4:1, comandado por Op, escolhe qual vai para R; o vai-um só sai quando a operação é a soma."},
          {kGates, aluBitGates,
           "O mesmo, só com portas: o multiplexador vira quatro E de três entradas, uma para cada código de Op, e um OU que junta as quatro. "
           "Cada E só deixa passar o seu resultado quando Op tem o seu código. Em transistores, seriam cerca de 70."}},
         -1,
         "A Unidade Lógica e Aritmética faz uma de quatro operações com A e B, escolhida por Op1 e Op0: 00 E, 01 OU, 10 XOR, 11 soma. R é o "
         "resultado e Cs, o vai-um da soma. Repetida lado a lado, forma a ULA de uma CPU.",
         "Escolha a operação com Op1 e Op0 e teste A, B e Ce."},

        // Sums of many bits.
        {"somador4", kGroupManyBits, "Somador de 4 bits", "Soma dois números de 0 a 15",
         {{kReady, adder4Ready, kReadyHow},
          {"Somadores de 1 bit", adder4Blocks,
           "Quatro somadores completos em fila, o somador de propagação: o Cs de cada coluna entra no Ce da seguinte, como o vai-um na conta de "
           "papel. O separador abre A e B em bits, e o juntador junta as somas no visor."},
          {kGates, adder4Gates,
           "Os quatro somadores abertos em portas: duas XORs, duas E e um OU por bit, vinte portas. O vai-um atravessa as quatro colunas uma "
           "depois da outra, e é por isso que somadores longos demoram mais."}},
         -1,
         "Soma dois números de 0 a 15, com o vai-um de entrada Ce. O resultado vai de 0 a 31: os quatro bits de S e o vai-um Cs, que vale 16.",
         "Clique nos bits de A e B e confira a conta no visor S. Com 15 + 1, S volta a 0 e Cs acende."},
        {"somadorn", kGroupManyBits, "Somador de N bits", "Soma de qualquer largura",
         {{kReady, adderNReady, kReadyHow},
          {"Dois de 4 bits", adderNHalves,
           "Dois somadores de 4 bits fazem um de 8: o Cs do de baixo entra no Ce do de cima. Somadores maiores se fazem assim, emendando os "
           "menores."}},
         -1,
         "O mesmo somador com a largura escolhida no painel (Bits), de 1 a 64. Aqui, 8 bits: números de 0 a 255. A CPU de 16 bits do jogo usa "
         "um destes, de 16.",
         "Some 200 + 100: o visor mostra 44 (300 − 256) e Cs acende."},
        {"subtratorn", kGroupManyBits, "Subtrator de N bits", "A − B, de qualquer largura",
         {{kReady, subNReady, kReadyHow},
          {"Subtratores de 1 bit", subNBlocks, "Quatro subtratores completos em fila: o Vs de cada coluna, o pedido de empréstimo, entra no Ve da seguinte."},
          {"Com um somador", subNWithAdder,
           "É assim que as CPUs subtraem: A − B = A + NÃO B + 1, o complemento de dois. Uma porta NÃO de 4 bits inverte B, o vai-um de entrada "
           "soma o 1, e o empresta-um é o vai-um invertido."}},
         -1,
         "Calcula A − B − Ve em N bits, aqui 4. Vs vale 1 quando o resultado seria negativo: a conta dá a volta e o visor mostra o complemento "
         "de dois do número.",
         "Faça 3 − 5: D mostra 14 (16 − 2) e Vs acende."},
        {"comparadorn", kGroupManyBits, "Comparador de N bits", "Compara dois números",
         {{kReady, cmpNReady, kReadyHow},
          {"Subtrator e portas", cmpNBlocks,
           "A<B é o empresta-um de A − B: se a conta fica negativa, A é menor. A=B é uma XNOR de 4 bits seguida de uma E de todos os bits. A>B "
           "é uma NOR das duas: nem menor, nem igual."}},
         -1,
         "Compara dois números de N bits, aqui 4, e liga uma de três saídas: A>B, A=B ou A<B.",
         "Mude A e B e veja qual saída acende."},
        {"ula4", kGroupManyBits, "ULA de 4 bits", "Quatro operações em 4 bits",
         {{kReady, alu4Ready, kReadyHow},
          {"ULAs de 1 bit", alu4Slices,
           "Op1 e Op0 vão para as quatro ULAs de 1 bit ao mesmo tempo, e o vai-um de cada uma entra na seguinte. Na soma ele se propaga; nas "
           "operações lógicas cada bit é calculado sozinho."}},
         -1,
         "Quatro fatias de ULA de 1 bit lado a lado: faz E, OU, XOR ou soma com números de 4 bits, escolhida por Op1 e Op0.",
         "Escolha a operação e mude A e B: o visor R mostra o resultado."},
        {"ulan", kGroupManyBits, "ULA de N bits", "A ULA da CPU, com oito operações",
         {{kReady, aluNReady, kReadyHow},
          {"Blocos", aluNBlocks,
           "Cada operação tem seu circuito, e todos calculam ao mesmo tempo: portas de 4 bits, um somador, um subtrator e dois deslocamentos "
           "feitos só de fios. Dois multiplexadores 4:1 escolhem por Op0 e Op1, um 2:1 escolhe por Op2, e Z é uma NOR dos bits de R."}},
         -1,
         "A ULA completa, a mesma da CPU de 16 bits: oito operações escolhidas por Op (0 E, 1 OU, 2 XOR, 3 soma, 4 subtração, 5 NÃO A, 6 desloca "
         "à esquerda, 7 à direita). Dá o resultado R, o vai-um Cs e Z, que liga quando R é zero.",
         "Ponha números em A e B e passe Op de 0 a 7."},

        // Memory.
        {"botao", kGroupMemory, "Botão e contador", "Conta cada aperto",
         {{"Circuito", buttonCounter,
           "O botão é o clock do contador: cada vez que é apertado, o sinal sobe de 0 para 1 e o contador soma 1. O decodificador mostra a "
           "contagem no display. Diferente da chave, o botão volta a 0 sozinho quando é solto."}},
         -1,
         "Um botão vale 1 só enquanto está apertado. Ligado no clock de um contador, cada aperto conta uma vez: é assim que um teclado ou um "
         "controle de jogo avisa o computador.",
         "Clique no botão algumas vezes e veja a contagem subir no display."},
        {"aleatorio", kGroupMemory, "Gerador aleatório", "Números que parecem sorteados",
         {{kReady, randomReady, kReadyHow},
          {"Registrador de deslocamento", randomShiftRegister,
           "Um registrador de 8 bits com realimentação: a cada pulso os bits sobem uma casa, e o que entra embaixo é uma XNOR dos bits 7, 5, 4 "
           "e 3. Com essas posições, ele passa por 255 valores antes de repetir."}},
         -1,
         "Gera uma sequência de números de 8 bits que parece sorteada, mas é sempre a mesma: um computador sozinho não sorteia, ele calcula. "
         "Jogos e simulações usam geradores assim, começando de um número diferente a cada vez.",
         "Aperte Rodar e veja Q pular de valor em valor."},
        {"teclado", kGroupMemory, "Teclado e terminal", "Escreva no circuito",
         {{"Circuito", keyboardTerminal,
           "O teclado entrega o código da tecla em 7 bits (Código) e liga Tecla enquanto ela está apertada. Tecla é o clock do terminal: cada "
           "tecla que desce grava o seu código na próxima posição da memória do terminal, e N conta quantas já foram."}},
         -1,
         "O teclado de verdade escreve no circuito enquanto o teclado da placa está selecionado. Cada tecla vira um número, o código ASCII: "
         "a é 97, A é 65, o espaço é 32. O terminal guarda esses números e os desenha como letras.",
         "Clique no teclado da placa para selecioná-lo e digite: o texto aparece no terminal. Esc solta o teclado."},
        {"latch", kGroupMemory, "Latch SR", "O menor circuito que lembra",
         {{kReady, [] { return readyBox("sr_latch"); }, kReadyHow},
          {kGates, latchGates,
           "Duas NORs, a saída de cada uma ligada na entrada da outra. Enquanto S e R estão em 0, cada porta mantém a outra no estado em que "
           "está: é essa realimentação que guarda o bit."}},
         -1,
         "Guarda um bit: S liga Q, R desliga, e com os dois em 0 o latch fica como estava. É a memória mais simples que existe, e o começo de "
         "registradores e da RAM.",
         "Ligue S e desligue: Q continua aceso. Depois faça o mesmo com R."},
        {"latch-d", kGroupMemory, "Latch D", "Copia D enquanto En está ligado",
         {{kReady, [] { return readyBox("d_latch"); }, kReadyHow},
          {kGates, dLatchGates,
           "Na frente de um latch SR de NORs, duas E: com En em 1, uma manda S = D e a outra R = NÃO D, e Q copia D. Com En em 0, as duas E "
           "ficam em 0 e o latch guarda o que tinha."}},
         -1,
         "Uma entrada de dado e uma de permissão: enquanto En vale 1, Q copia D; quando En volta a 0, Q guarda o último valor. Sem o risco de "
         "S e R juntos do latch SR.",
         "Ligue En, mude D e veja Q seguir. Desligue En e mude D: Q não muda mais."},
        {"flip-flop", kGroupMemory, "Flip-flop D", "Copia D só na subida do clock",
         {{kReady, [] { return readyBox("d_flip_flop"); }, kReadyHow},
          {"Dois latches", flipFlopLatches,
           "Dois latches D feitos de portas, em fila: o mestre e o escravo. Com Clk em 0, o mestre copia D e o escravo guarda; quando Clk sobe, "
           "o mestre fecha e o escravo abre, copiando o que o mestre guardou. Assim Q só muda na subida."}},
         -1,
         "O flip-flop D copia D no instante em que o clock passa de 0 para 1, e guarda o valor no resto do tempo. Registradores, contadores e "
         "a CPU inteira são feitos dele.",
         "Ligue D com Clk em 0: Q não muda. Ligue Clk: agora Q copia D. Mude D com Clk em 1: Q fica como estava."},
        {"contador4", kGroupMemory, "Contador de 4 bits", "Soma 1 a cada pulso",
         {{kReady, counterReady, kReadyHow},
          {"Registrador e somador", counterBlocks,
           "Um registrador guarda o valor, e um somador calcula esse valor mais 1, que o registrador guarda no próximo pulso. Conta entra no "
           "vai-um do somador e liga o registrador; uma E dos quatro bits dá Máximo."}},
         -1,
         "Conta os pulsos do clock de 0 a 15 e volta a 0. Zera leva a 0, e Máximo acende no 15. O PC da CPU é um contador assim, que diz qual "
         "instrução vem a seguir.",
         "Aperte Rodar e veja o valor subir; use o + da barra para acelerar."},
        {"registrador", kGroupMemory, "Registrador de 4 bits", "Guarda um número",
         {{kReady, registerReady, kReadyHow},
          {"Flip-flops", registerFlipFlops,
           "Quatro flip-flops D, um por bit, todos no mesmo clock. Na frente de cada um, um multiplexador escolhe: com Grava em 0, devolve ao "
           "flip-flop o próprio bit, e nada muda; com Grava em 1, entrega o bit novo do Dado."}},
         -1,
         "Guarda um número de 4 bits: com Grava ligado, copia o Dado no próximo pulso do clock; com Grava desligado, mantém o que tinha. Os "
         "registradores R0 a R7 da CPU são assim.",
         "Escolha um Dado, ligue Grava e aperte Passo; depois mude o Dado com Grava desligado."},
    };
    // Every gate built from NAND alone, as a version of its own: NAND is universal.
    const auto onlyNand = [&](const char* id, GalleryProject (*build)(), const char* how) {
        for (GalleryItem& item : items) {
            if (std::string_view(item.id) == id) item.versions.push_back({"Só NAND", build, how});
        }
    };
    onlyNand("nao", notOfNand, "Uma NAND com as duas entradas no mesmo fio: NÃO (A E A) é o mesmo que NÃO A. É assim que se tira um inversor de uma NAND.");
    onlyNand("e", andOfNand,
             "Duas NAND em fila: a primeira dá NÃO (A E B), e a segunda, com as duas entradas juntas, inverte de novo. Sobra A E B.");
    onlyNand("ou", orOfNand,
             "Duas NAND invertem A e B, e uma terceira junta as duas: NÃO (NÃO A E NÃO B), que pela lei de De Morgan é A OU B.");
    onlyNand("xor", xorOfNand,
             "Quatro NAND, o jeito clássico: a primeira dá NÃO (A E B), as duas do meio combinam esse resultado com A e com B, e a última junta "
             "tudo. Sai 1 só quando A e B são diferentes.");
    return items;
}

}  // namespace bancada::editor
