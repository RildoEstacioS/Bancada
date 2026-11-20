#include "editor/architecture.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "asm/mips.hpp"
#include "editor/computer16.hpp"
#include "editor/computer32.hpp"

namespace bancada::editor {

namespace {

using sim::NetId;
using sim::Op;

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr std::uint32_t kTie0 = 0xFFFFFFF0u;  // an input held at 0
constexpr std::uint32_t kTie1 = 0xFFFFFFF1u;  // an input held at 1
constexpr bool isSignal(std::uint32_t s) { return s < kTie0; }

// Where a bit of a net really comes from: through wires, slices and joins, to the node that makes it.
struct Source {
    NetId node = 0;
    unsigned bit = 0;
    bool valid = false;
};

Source resolve(const sim::Netlist& n, NetId net, unsigned bit) {
    const auto& nodes = n.nodes();
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const sim::Node& node = nodes[net];
        if (node.op == Op::Wire) {
            if (node.args.empty()) return {};
            net = node.args[0];
        } else if (node.op == Op::Slice) {
            bit += node.lo;
            net = node.args[0];
        } else if (node.op == Op::Concat) {
            bool found = false;
            for (const NetId part : node.args) {
                const unsigned w = nodes[part].width;
                if (bit < w) {
                    net = part;
                    found = true;
                    break;
                }
                bit -= w;
            }
            if (!found) return {};
        } else {
            return {net, bit, true};
        }
    }
    return {};
}

ArchGate gateOfLabel(const std::string& label) {
    static const std::map<std::string, ArchGate, std::less<>> kGates = {
        {"NOT", ArchGate::Not}, {"AND", ArchGate::And}, {"OR", ArchGate::Or},     {"NAND", ArchGate::Nand},
        {"NOR", ArchGate::Nor}, {"XOR", ArchGate::Xor}, {"XNOR", ArchGate::Xnor},
    };
    const auto found = kGates.find(label);
    return found != kGates.end() ? found->second : ArchGate::Nand;
}

std::uint64_t key(NetId node, unsigned bit) { return (std::uint64_t{node} << 8) | bit; }

unsigned banksOf(unsigned words, unsigned bits) {
    unsigned banks = 1;
    while (banks * 2 * bits * banks * 2 <= words) banks *= 2;
    return banks;
}

enum class Kind : std::uint8_t { Computer, Chip, Box, Detail, Cell };

// The room in a box, by what it is: between tracks, between parts, around them, and above for the name.
struct Room {
    float pitch, gap, pad, band;
};
Room roomOf(Kind kind) {
    switch (kind) {
        case Kind::Cell: return {0.2f, 0.3f, 0.15f, 0.0f};
        case Kind::Detail: return {0.3f, 0.5f, 0.4f, 1.1f};
        case Kind::Box: return {0.35f, 0.8f, 0.6f, 1.7f};
        case Kind::Chip: return {0.4f, 1.2f, 1.0f, 2.5f};
        case Kind::Computer: return {0.5f, 2.0f, 1.6f, 3.2f};
    }
    return {0.4f, 1.0f, 1.0f, 2.0f};
}

// A NAND or a memory: the parts that are not made of other parts.
struct Leaf {
    bool memory = false;
    std::uint32_t parent = 0;
    std::uint32_t index = 0;  // its place among its parent's parts
    std::vector<std::uint32_t> ins, outs;
    float w = 1.0f, h = 1.0f;
    std::vector<float> inY, outY;
    float x = 0.0f, y = 0.0f;
    std::uint32_t memoryIndex = 0;
};

struct Part {
    bool leaf = false;
    std::uint32_t id = 0;
};

// A signal as it goes through a box: from one part (or from outside, through an input pin of the box) to
// the parts that read it (and out through an output pin of the box, if something outside reads it too).
struct Net {
    std::int32_t source = -1;  // a part's number, or -1 for the box's input pin
    std::uint16_t sourcePin = 0;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> sinks;  // (part, pin)
    bool out = false;
};

struct Box {
    Kind kind = Kind::Box;
    std::string label;
    std::uint32_t parent = kNone;
    std::uint32_t index = 0;
    int depth = 0;
    ArchGate gate = ArchGate::Nand;
    std::vector<Part> parts;
    std::vector<std::uint32_t> ins, outs;  // the signals through its pins
    std::unordered_map<std::uint32_t, std::uint16_t> inPin, outPin;
    std::vector<std::pair<std::uint32_t, Net>> nets;  // in the order they were met
    std::unordered_map<std::uint32_t, std::size_t> netOf;
    float w = 0.0f, h = 0.0f, x = 0.0f, y = 0.0f;
    std::vector<float> inY, outY;
    std::vector<ArchSegment> segs;
    std::vector<ArchPoint> dots;
    std::vector<ArchTie> ties;

    Net& net(std::uint32_t signal) {
        const auto [where, added] = netOf.emplace(signal, nets.size());
        if (added) nets.push_back({signal, {}});
        return nets[where->second].second;
    }
};

struct Signal {
    ArchSignal rule;             // what it carries while the circuit runs
    std::uint32_t leaf = kNone;  // the leaf that drives it
    std::uint16_t pin = 0;
    bool outside = false;        // it comes from outside the computer
    std::string name;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> sinks;  // (leaf, pin)
};

class Planner {
public:
    explicit Planner(const sim::Netlist& netlist) : n_(netlist) {}

    Architecture build(const std::string& title) {
        boxes_.push_back({});
        boxes_[0].kind = Kind::Computer;
        boxes_[0].label = title;
        makeCells();
        makeFlipFlops();
        makeMemories();
        for (std::uint32_t l = 0; l < leaves_.size(); ++l) {
            for (std::size_t k = 0; k < leaves_[l].ins.size(); ++k) {
                if (isSignal(leaves_[l].ins[k])) signals_[leaves_[l].ins[k]].sinks.push_back({l, static_cast<std::uint16_t>(k)});
            }
        }
        makePins();
        for (std::size_t b = boxes_.size(); b-- > 0;) lay(static_cast<std::uint32_t>(b));
        place(0, 0.0f, 0.0f);
        return finish();
    }

private:
    const sim::Netlist& n_;
    std::vector<Box> boxes_;
    std::vector<Leaf> leaves_;
    std::vector<Signal> signals_;
    std::unordered_map<std::uint64_t, std::uint32_t> signalOf_;
    std::unordered_map<std::uint32_t, std::uint32_t> boxOfTag_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> cellsOfTag_;  // a gate's cells, one per bit
    std::size_t flipFlops_ = 0;

    std::uint32_t newSignal() {
        signals_.push_back({});
        return static_cast<std::uint32_t>(signals_.size() - 1);
    }

    std::uint32_t signalFor(NetId node, unsigned bit) {
        const auto [where, added] = signalOf_.emplace(key(node, bit), 0);
        if (added) {
            where->second = newSignal();
            signals_[where->second].rule = {ArchRule::Net, node, bit, 0, 0, 0};
            const sim::Node& made = n_.nodes()[node];
            if (made.op == Op::Input) {
                signals_[where->second].outside = true;
                signals_[where->second].name = made.name;
            }
        }
        return where->second;
    }

    // What an input of a gate is: a signal, a constant, or nothing at all (a wire nothing drives).
    std::uint32_t inputOf(NetId net, unsigned bit) {
        const Source source = resolve(n_, net, bit);
        if (!source.valid) return kNone;
        const sim::Node& node = n_.nodes()[source.node];
        if (node.op == Op::Const) return ((node.value >> source.bit) & 1u) != 0 ? kTie1 : kTie0;
        return signalFor(source.node, source.bit);
    }

    std::uint32_t newBox(Kind kind, std::string label, std::uint32_t parent) {
        Box box;
        box.kind = kind;
        box.label = std::move(label);
        box.parent = parent;
        box.depth = boxes_[parent].depth + 1;
        box.index = static_cast<std::uint32_t>(boxes_[parent].parts.size());
        const auto id = static_cast<std::uint32_t>(boxes_.size());
        boxes_.push_back(std::move(box));
        boxes_[parent].parts.push_back({false, id});
        return id;
    }

    std::uint32_t newLeaf(std::uint32_t parent) {
        Leaf leaf;
        leaf.parent = parent;
        leaf.index = static_cast<std::uint32_t>(boxes_[parent].parts.size());
        const auto id = static_cast<std::uint32_t>(leaves_.size());
        leaves_.push_back(std::move(leaf));
        boxes_[parent].parts.push_back({true, id});
        return id;
    }

    void newNand(std::uint32_t parent, std::uint32_t a, std::uint32_t b, std::uint32_t out) {
        const std::uint32_t leaf = newLeaf(parent);
        leaves_[leaf].ins = {a, b};
        leaves_[leaf].outs = {out};
        leaves_[leaf].inY = {0.25f, 0.75f};
        leaves_[leaf].outY = {0.5f};
        signals_[out].leaf = leaf;
        signals_[out].pin = 0;
    }

    // A gate of one NAND: a NOT has both its inputs on the same wire.
    void gateOfOne(std::uint32_t parent, ArchGate gate, std::uint32_t a, std::uint32_t b, std::uint32_t out) {
        const std::uint32_t cell = newBox(Kind::Cell, "", parent);
        boxes_[cell].gate = gate;
        newNand(cell, a, b, out);
    }

    // The box a tag stands for: chips, boxes and pieces become boxes; a gate's tag is the box it's in.
    std::uint32_t boxOfTag(std::uint32_t tag) {
        while (tag != sim::kNoTag && n_.tags()[tag].kind == sim::TagKind::Gate) tag = n_.tags()[tag].parent;
        if (tag == sim::kNoTag) return 0;
        const auto found = boxOfTag_.find(tag);
        if (found != boxOfTag_.end()) return found->second;
        const std::uint32_t parent = boxOfTag(n_.tags()[tag].parent);
        const sim::Tag& t = n_.tags()[tag];
        const Kind kind = t.kind == sim::TagKind::Chip ? Kind::Chip : t.kind == sim::TagKind::Box ? Kind::Box : Kind::Detail;
        const std::uint32_t box = newBox(kind, t.label, parent);
        boxOfTag_[tag] = box;
        return box;
    }

    // The gates: each gate tag's NANDs, one gate per bit; a NAND outside any gate is a gate of its own.
    void makeCells() {
        struct Group {
            std::vector<NetId> nodes;
            unsigned width = 1;
            ArchGate gate = ArchGate::Nand;
            std::uint32_t tag = sim::kNoTag;
        };
        std::vector<Group> groups;
        std::unordered_map<std::uint32_t, std::size_t> groupOfTag;
        const auto& nodes = n_.nodes();
        for (NetId i = 0; i < nodes.size(); ++i) {
            if (nodes[i].op != Op::Nand) continue;
            const std::uint32_t tag = n_.tagOf(i);
            const bool inGate = tag != sim::kNoTag && n_.tags()[tag].kind == sim::TagKind::Gate;
            std::size_t g = groups.size();
            if (inGate) {
                const auto [where, added] = groupOfTag.emplace(tag, groups.size());
                if (added) groups.push_back({{}, 1, gateOfLabel(n_.tags()[tag].label), tag});
                g = where->second;
            } else {
                groups.push_back({{}, 1, ArchGate::Nand, tag});
            }
            groups[g].nodes.push_back(i);
            groups[g].width = std::max<unsigned>(groups[g].width, nodes[i].width);
        }
        for (const Group& group : groups) {
            const std::uint32_t parent = boxOfTag(group.tag);
            for (unsigned bit = 0; bit < group.width; ++bit) {
                std::uint32_t cell = kNone;
                for (const NetId node : group.nodes) {
                    const sim::Node& nand = nodes[node];
                    if (nand.width <= bit && bit != 0) continue;
                    const unsigned b = nand.width > bit ? bit : 0;
                    if (cell == kNone) {
                        cell = newBox(Kind::Cell, "", parent);
                        boxes_[cell].gate = group.gate;
                        cellsOfTag_[group.tag].push_back(cell);
                    }
                    const unsigned argBit = nand.width > 1 ? b : 0;
                    newNand(cell, inputOf(nand.args[0], argBit), inputOf(nand.args[1], argBit), signalFor(node, b));
                }
            }
        }
    }

    // A flip-flop is one node to the simulator, for speed. Drawn, it's the two latches the lessons build:
    // a NOT on the clock, and in each latch a NOT and four NANDs.
    void makeFlipFlops() {
        const auto& nodes = n_.nodes();
        for (NetId i = 0; i < nodes.size(); ++i) {
            if (nodes[i].op != Op::Dff) continue;
            const std::uint32_t parent = boxOfTag(n_.tagOf(i));
            for (unsigned bit = 0; bit < nodes[i].width; ++bit) {
                ++flipFlops_;
                const std::uint32_t ff = newBox(Kind::Detail, "Flip-flop D", parent);
                const std::uint32_t d = inputOf(nodes[i].args[0], bit);
                const std::uint32_t clock = inputOf(nodes[i].args[1], 0);
                const std::uint32_t q = signalFor(i, bit);
                const std::uint32_t notClock = newSignal();
                signals_[notClock].rule = {ArchRule::Not, 0, 0, clock, 0, 0};
                gateOfOne(ff, ArchGate::Not, clock, clock, notClock);
                // A latch: while enable is 1 its output follows its input; while 0, it keeps what it had.
                const auto latch = [&](std::uint32_t in, std::uint32_t enable, std::uint32_t out) {
                    const std::uint32_t box = newBox(Kind::Detail, "Latch D", ff);
                    const std::uint32_t notIn = newSignal(), set = newSignal(), reset = newSignal(), notOut = newSignal();
                    signals_[notIn].rule = {ArchRule::Not, 0, 0, in, 0, 0};
                    signals_[set].rule = {ArchRule::Nand, 0, 0, in, enable, 0};
                    signals_[reset].rule = {ArchRule::Nand, 0, 0, notIn, enable, 0};
                    signals_[notOut].rule = {ArchRule::Not, 0, 0, out, 0, 0};
                    gateOfOne(box, ArchGate::Not, in, in, notIn);
                    gateOfOne(box, ArchGate::Nand, in, enable, set);
                    gateOfOne(box, ArchGate::Nand, notIn, enable, reset);
                    gateOfOne(box, ArchGate::Nand, set, notOut, out);
                    gateOfOne(box, ArchGate::Nand, reset, out, notOut);
                };
                // The first latch passes D while the clock is low and holds it when it rises: what Q took.
                const std::uint32_t middle = newSignal();
                signals_[middle].rule = {ArchRule::Latched, 0, 0, clock, q, d};
                latch(d, notClock, middle);
                latch(middle, clock, q);
            }
        }
    }

    // A memory: its address, data, write and clock pins on the left, what it reads on the right.
    void makeMemories() {
        const auto& nodes = n_.nodes();
        std::vector<std::vector<NetId>> reads(n_.memories().size());
        for (NetId i = 0; i < nodes.size(); ++i) {
            if (nodes[i].op == Op::MemRead) reads[static_cast<std::size_t>(nodes[i].value)].push_back(i);
        }
        for (std::uint32_t m = 0; m < n_.memories().size(); ++m) {
            const std::uint32_t leaf = newLeaf(boxOfTag(n_.memoryTag(m)));
            leaves_[leaf].memory = true;
            leaves_[leaf].memoryIndex = m;
            const auto addBits = [&](NetId net) {
                for (unsigned bit = 0; bit < nodes[net].width; ++bit) leaves_[leaf].ins.push_back(inputOf(net, bit));
            };
            for (const NetId read : reads[m]) addBits(nodes[read].args[0]);
            for (const sim::MemoryPort& port : n_.memoryPorts()) {
                if (port.memory != m) continue;
                for (const NetId net : {port.address, port.data, port.enable, port.clock}) addBits(net);
            }
            for (const NetId read : reads[m]) {
                for (unsigned bit = 0; bit < nodes[read].width; ++bit) {
                    const std::uint32_t s = signalFor(read, bit);
                    signals_[s].leaf = leaf;
                    signals_[s].pin = static_cast<std::uint16_t>(leaves_[leaf].outs.size());
                    leaves_[leaf].outs.push_back(s);
                }
            }
            const sim::Memory& made = n_.memories()[m];
            const unsigned words = 1u << made.addressBits;
            const unsigned banks = banksOf(words, made.dataBits);
            const float gridW = static_cast<float>(banks * made.dataBits) * kArchMemoryCell + static_cast<float>(banks - 1) * kArchMemoryCell * 2.0f;
            const float gridH = static_cast<float>(words / banks) * kArchMemoryCell;
            Leaf& placed = leaves_[leaf];
            placed.w = gridW + 2.0f;
            placed.h = std::max({gridH + 2.0f, static_cast<float>(placed.ins.size()) * 0.5f + 1.0f, static_cast<float>(placed.outs.size()) * 0.5f + 1.0f});
            for (std::size_t k = 0; k < placed.ins.size(); ++k) placed.inY.push_back(placed.h * (static_cast<float>(k) + 0.5f) / static_cast<float>(placed.ins.size()));
            for (std::size_t k = 0; k < placed.outs.size(); ++k) placed.outY.push_back(placed.h * (static_cast<float>(k) + 0.5f) / static_cast<float>(placed.outs.size()));
        }
    }

    // Which pins each box needs, and inside each box, where each signal comes from and goes to. A signal
    // goes up from the part that drives it, out of every box on the way, to the smallest box that holds
    // it and a part that reads it, and down again into that part.
    void makePins() {
        std::vector<std::uint32_t> onDriver(boxes_.size(), kNone);
        for (std::uint32_t s = 0; s < signals_.size(); ++s) {
            const Signal& signal = signals_[s];
            if (signal.sinks.empty() || (signal.leaf == kNone && !signal.outside)) continue;
            if (signal.leaf != kNone) {
                for (std::uint32_t b = leaves_[signal.leaf].parent; b != kNone; b = boxes_[b].parent) onDriver[b] = s;
            }
            std::uint32_t top = kNone;  // the highest box a sink's way up stopped in
            for (const auto& [leaf, pin] : signal.sinks) {
                std::uint32_t part = leaves_[leaf].index;
                std::uint16_t partPin = pin;
                std::uint32_t b = leaves_[leaf].parent;
                while (true) {
                    Net& net = boxes_[b].net(s);
                    const std::pair<std::uint32_t, std::uint16_t> end{part, partPin};
                    if (std::find(net.sinks.begin(), net.sinks.end(), end) == net.sinks.end()) net.sinks.push_back(end);
                    if (onDriver[b] == s) {
                        if (top == kNone || boxes_[b].depth < boxes_[top].depth) top = b;
                        break;
                    }
                    const bool known = boxes_[b].inPin.count(s) != 0;
                    if (!known) {
                        boxes_[b].inPin[s] = static_cast<std::uint16_t>(boxes_[b].ins.size());
                        boxes_[b].ins.push_back(s);
                    }
                    net.source = -1;
                    if (known || boxes_[b].parent == kNone) break;
                    part = boxes_[b].index;
                    partPin = boxes_[b].inPin[s];
                    b = boxes_[b].parent;
                }
            }
            if (signal.leaf == kNone || top == kNone) continue;
            // The driver's way up, to the highest box a sink reached.
            std::uint32_t part = leaves_[signal.leaf].index;
            std::uint16_t partPin = signal.pin;
            for (std::uint32_t b = leaves_[signal.leaf].parent;; b = boxes_[b].parent) {
                Net& net = boxes_[b].net(s);
                net.source = static_cast<std::int32_t>(part);
                net.sourcePin = partPin;
                if (b == top) break;
                net.out = true;
                if (boxes_[b].outPin.count(s) == 0) {
                    boxes_[b].outPin[s] = static_cast<std::uint16_t>(boxes_[b].outs.size());
                    boxes_[b].outs.push_back(s);
                }
                part = boxes_[b].index;
                partPin = boxes_[b].outPin[s];
            }
        }
    }

    // A part's size and pins, wherever it is.
    struct Shape {
        float w, h;
        const std::vector<float>* inY;
        const std::vector<float>* outY;
    };
    Shape shapeOf(const Part& part) const {
        if (part.leaf) {
            const Leaf& leaf = leaves_[part.id];
            return {leaf.w, leaf.h, &leaf.inY, &leaf.outY};
        }
        const Box& box = boxes_[part.id];
        return {box.w, box.h, &box.inY, &box.outY};
    }

    // Lays out one box whose parts are laid out already: the parts in columns, the wires in channels.
    void lay(std::uint32_t id) {
        Box& box = boxes_[id];
        const Room room = roomOf(box.kind);
        const std::size_t count = box.parts.size();
        std::vector<Shape> shapes;
        for (const Part& part : box.parts) shapes.push_back(shapeOf(part));

        // The columns: each part one past the parts that feed it. Loops are cut where a depth-first search
        // meets a part still open: a part's place is decided by what feeds it from before.
        std::vector<std::vector<std::uint32_t>> next(count), prev(count);
        for (const auto& [signal, net] : box.nets) {
            if (net.source < 0) continue;
            for (const auto& [part, pin] : net.sinks) {
                if (part != static_cast<std::uint32_t>(net.source)) next[static_cast<std::size_t>(net.source)].push_back(part);
            }
        }
        std::vector<int> state(count, 0);
        std::vector<std::uint32_t> finished;
        for (std::uint32_t start = 0; start < count; ++start) {
            if (state[start] != 0) continue;
            std::vector<std::pair<std::uint32_t, std::size_t>> stack{{start, 0}};
            state[start] = 1;
            while (!stack.empty()) {
                const std::uint32_t at = stack.back().first;
                if (stack.back().second < next[at].size()) {
                    const std::uint32_t to = next[at][stack.back().second++];
                    if (state[to] == 0) {
                        state[to] = 1;
                        stack.push_back({to, 0});
                    }
                    continue;
                }
                state[at] = 2;
                finished.push_back(at);
                stack.pop_back();
            }
        }
        std::vector<std::size_t> rank(count, 0);
        for (std::size_t i = 0; i < finished.size(); ++i) rank[finished[i]] = i;
        for (std::uint32_t from = 0; from < count; ++from) {
            for (const std::uint32_t to : next[from]) {
                if (rank[to] < rank[from]) prev[to].push_back(from);  // the other way round would close a loop
            }
        }
        std::vector<int> col(count, 0);
        int columns = count > 0 ? 1 : 0;
        for (std::size_t i = finished.size(); i-- > 0;) {
            const std::uint32_t part = finished[i];
            for (const std::uint32_t from : prev[part]) col[part] = std::max(col[part], col[from] + 1);
            columns = std::max(columns, col[part] + 1);
        }

        // Down each column, the parts in the order of what feeds them, so wires cross as little as they can.
        std::vector<std::vector<std::uint32_t>> inColumn(static_cast<std::size_t>(columns));
        for (std::uint32_t part = 0; part < count; ++part) inColumn[static_cast<std::size_t>(col[part])].push_back(part);
        const auto inColumnOf = [&](int c) -> const std::vector<std::uint32_t>& { return inColumn[static_cast<std::size_t>(c)]; };
        std::vector<float> py(count, 0.0f);
        float tallest = 0.0f;
        const auto signalsIn = [&](std::uint32_t part) -> const std::vector<std::uint32_t>& {
            const Part& p = box.parts[part];
            return p.leaf ? leaves_[p.id].ins : boxes_[p.id].ins;
        };
        const auto signalsOut = [&](std::uint32_t part) -> const std::vector<std::uint32_t>& {
            const Part& p = box.parts[part];
            return p.leaf ? leaves_[p.id].outs : boxes_[p.id].outs;
        };
        // (height, signal) of every output pin of a column, and of every input pin of one.
        const auto outputsOf = [&](int c) {
            std::vector<std::pair<float, std::uint32_t>> list;
            for (const std::uint32_t part : inColumnOf(c)) {
                for (std::size_t k = 0; k < signalsOut(part).size(); ++k) list.push_back({py[part] + (*shapes[part].outY)[k], signalsOut(part)[k]});
            }
            return list;
        };
        const auto clashes = [](float y, std::uint32_t signal, const std::vector<std::pair<float, std::uint32_t>>& others) {
            return std::any_of(others.begin(), others.end(), [&](const auto& o) { return o.second != signal && std::abs(o.first - y) < 0.08f; });
        };
        std::vector<float> start(static_cast<std::size_t>(columns), 0.0f), height(static_cast<std::size_t>(columns), 0.0f);
        for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            // Each column centred on the tallest one, so the signals flow along the middle of the box.
            for (int c = 0; c < columns; ++c) start[static_cast<std::size_t>(c)] = (tallest - height[static_cast<std::size_t>(c)]) / 2.0f;
            tallest = 0.0f;
        }
        for (int c = 0; c < columns; ++c) {
            auto& list = inColumn[static_cast<std::size_t>(c)];
            if (c > 0) {
                std::vector<float> weight(count, 0.0f);
                for (const std::uint32_t part : list) {
                    float sum = 0.0f;
                    for (const std::uint32_t from : prev[part]) sum += py[from] + shapes[from].h / 2.0f;
                    weight[part] = prev[part].empty() ? -1e6f + static_cast<float>(part) : sum / static_cast<float>(prev[part].size());
                }
                std::stable_sort(list.begin(), list.end(), [&](std::uint32_t a, std::uint32_t b) { return weight[a] < weight[b]; });
            }
            const std::vector<std::pair<float, std::uint32_t>> before = c > 0 ? outputsOf(c - 1) : std::vector<std::pair<float, std::uint32_t>>{};
            float y = start[static_cast<std::size_t>(c)];
            for (const std::uint32_t part : list) {
                for (int tries = 0; tries < 200; ++tries) {
                    bool clear = true;
                    for (std::size_t k = 0; k < signalsIn(part).size() && clear; ++k) clear = !clashes(y + (*shapes[part].inY)[k], signalsIn(part)[k], before);
                    if (clear) break;
                    y += 0.1f;
                }
                py[part] = y;
                y += shapes[part].h + room.gap;
            }
            height[static_cast<std::size_t>(c)] = y - room.gap - start[static_cast<std::size_t>(c)];
            tallest = std::max(tallest, y - room.gap);
        }
        }

        // The box's own pins, each beside what it feeds or what feeds it.
        const auto spread = [&](const std::vector<std::uint32_t>& signals, const auto& wanted, std::vector<float>& ys,
                                const std::vector<std::pair<float, std::uint32_t>>& avoid) {
            std::vector<std::pair<float, std::size_t>> sorted;
            for (std::size_t k = 0; k < signals.size(); ++k) sorted.push_back({wanted(signals[k]), k});
            std::sort(sorted.begin(), sorted.end());
            ys.assign(signals.size(), 0.0f);
            float last = -1e9f;
            for (const auto& [want, k] : sorted) {
                float y = std::max(want, last + room.pitch * 2.0f);
                for (int tries = 0; tries < 200 && clashes(y, signals[k], avoid); ++tries) y += 0.1f;
                ys[k] = y;
                last = y;
            }
        };
        std::vector<std::pair<float, std::uint32_t>> firstInputs;
        if (columns > 0) {
            for (const std::uint32_t part : inColumnOf(0)) {
                for (std::size_t k = 0; k < signalsIn(part).size(); ++k) firstInputs.push_back({py[part] + (*shapes[part].inY)[k], signalsIn(part)[k]});
            }
        }
        const std::vector<std::pair<float, std::uint32_t>> lastOutputs = columns > 0 ? outputsOf(columns - 1) : std::vector<std::pair<float, std::uint32_t>>{};
        spread(box.ins, [&](std::uint32_t s) {
            const Net& net = box.nets[box.netOf.at(s)].second;
            float lowest = 1e9f;
            for (const auto& [part, pin] : net.sinks) lowest = std::min(lowest, py[part] + (*shapes[part].inY)[pin]);
            return lowest < 1e8f ? lowest : 0.0f;
        }, box.inY, firstInputs);
        spread(box.outs, [&](std::uint32_t s) {
            const Net& net = box.nets[box.netOf.at(s)].second;
            if (net.source < 0) return 0.0f;
            const auto from = static_cast<std::size_t>(net.source);
            return py[from] + (*shapes[from].outY)[net.sourcePin];
        }, box.outY, lastOutputs);
        for (const float y : box.inY) tallest = std::max(tallest, y);
        for (const float y : box.outY) tallest = std::max(tallest, y);

        // Channels: 0 is left of the first column, `columns` right of the last. Each net has a track of its
        // own in each channel it uses; a net that uses two channels or more also has a lane on top.
        struct Use {
            std::size_t net;
            int channel;
            float lo, hi;
            int track = 0;
        };
        struct Route {
            int sourceChannel = 0;
            float sourceY = 0.0f;
            std::vector<std::pair<int, float>> ends;  // (channel, y) of each sink, then of the output pin
            std::vector<bool> straight;               // this end is reached straight across, from the source's channel
            std::vector<std::pair<int, float>> jogs;  // (channel, y): across from the source's channel at y, to a track there
            bool lane = false;
            bool below = false;  // its lane runs under the parts: it goes back, against the flow
            int laneIndex = 0;
            int lo = 0, hi = 0;
        };
        std::vector<Route> routes(box.nets.size());
        std::vector<Use> uses;
        // A wire that goes forward, past columns where nothing stands at its height, goes straight across
        // instead of over the top: no part is in its way, and no other wire runs at that height there.
        std::vector<std::vector<std::pair<float, float>>> standing(static_cast<std::size_t>(columns));
        for (std::uint32_t part = 0; part < count; ++part) standing[static_cast<std::size_t>(col[part])].push_back({py[part], py[part] + shapes[part].h});
        struct Run {
            float y;
            int from, to;
        };
        std::vector<Run> runs;
        // The heights where other signals' short wires leave the parts of a column, or the box's input pins.
        std::vector<std::vector<std::pair<float, std::uint32_t>>> leaving(static_cast<std::size_t>(columns) + 1);
        for (std::size_t k = 0; k < box.ins.size(); ++k) leaving[0].push_back({box.inY[k], box.ins[k]});
        for (int c = 0; c < columns; ++c) leaving[static_cast<std::size_t>(c) + 1] = outputsOf(c);
        // The heights where other signals' short wires go into the parts of a column, or out to the box's pins.
        std::vector<std::vector<std::pair<float, std::uint32_t>>> entering(static_cast<std::size_t>(columns) + 1);
        for (int c = 0; c < columns; ++c) {
            for (const std::uint32_t part : inColumnOf(c)) {
                for (std::size_t k = 0; k < signalsIn(part).size(); ++k) entering[static_cast<std::size_t>(c)].push_back({py[part] + (*shapes[part].inY)[k], signalsIn(part)[k]});
            }
        }
        for (std::size_t k = 0; k < box.outs.size(); ++k) entering[static_cast<std::size_t>(columns)].push_back({box.outY[k], box.outs[k]});
        const auto clear = [&](float y, int from, int to, std::uint32_t signal) {
            for (int c = from; c < to; ++c) {
                for (const auto& [a, b] : standing[static_cast<std::size_t>(c)]) {
                    if (y > a - 0.08f && y < b + 0.08f) return false;
                }
            }
            if (clashes(y, signal, leaving[static_cast<std::size_t>(from)])) return false;
            return std::none_of(runs.begin(), runs.end(), [&](const Run& r) { return std::abs(r.y - y) < room.pitch * 0.75f && r.from <= to && from <= r.to; });
        };
        for (std::size_t i = 0; i < box.nets.size(); ++i) {
            const Net& net = box.nets[i].second;
            Route& route = routes[i];
            if (net.source >= 0) {
                const auto from = static_cast<std::size_t>(net.source);
                route.sourceChannel = col[from] + 1;
                route.sourceY = py[from] + (*shapes[from].outY)[net.sourcePin];
            } else {
                route.sourceChannel = 0;
                route.sourceY = box.inY[box.inPin.at(box.nets[i].first)];
            }
            for (const auto& [part, pin] : net.sinks) route.ends.push_back({col[part], py[part] + (*shapes[part].inY)[pin]});
            if (net.out) route.ends.push_back({columns, box.outY[box.outPin.at(box.nets[i].first)]});
            std::map<int, std::pair<float, float>> spans;
            const auto touch = [&](int channel, float y) {
                const auto [where, added] = spans.emplace(channel, std::pair<float, float>{y, y});
                if (!added) where->second = {std::min(where->second.first, y), std::max(where->second.second, y)};
            };
            touch(route.sourceChannel, route.sourceY);
            route.straight.assign(route.ends.size(), false);
            const std::uint32_t signal = box.nets[i].first;
            std::vector<int> linked{route.sourceChannel};  // channels joined to the source's without the lane
            const auto isLinked = [&](int channel) { return std::find(linked.begin(), linked.end(), channel) != linked.end(); };
            for (std::size_t e = 0; e < route.ends.size(); ++e) {
                const auto [channel, y] = route.ends[e];
                const int from = route.sourceChannel;
                if (channel > from && clear(y, from, channel, signal)) {  // straight across into the pin
                    route.straight[e] = true;
                    runs.push_back({y, from, channel});
                    touch(from, y);
                    continue;
                }
                touch(channel, y);
                if (channel <= from || isLinked(channel)) continue;
                // Across through the nearest gap between the parts, then down the channel to the pin.
                for (int step = 1; step < 160; ++step) {
                    const float across = y + static_cast<float>((step + 1) / 2) * 0.05f * (step % 2 == 0 ? 1.0f : -1.0f);
                    if (!clear(across, from, channel, signal) || clashes(across, signal, entering[static_cast<std::size_t>(channel)])) continue;
                    runs.push_back({across, from, channel});
                    route.jogs.push_back({channel, across});
                    touch(from, across);
                    touch(channel, across);
                    linked.push_back(channel);
                    break;
                }
            }
            route.lane = std::any_of(spans.begin(), spans.end(), [&](const auto& span) { return !isLinked(span.first); });
            route.below = route.lane && spans.begin()->first < route.sourceChannel;
            route.lo = spans.begin()->first;
            route.hi = spans.rbegin()->first;
            for (const auto& [channel, span] : spans) {
                const float lo = route.lane && !route.below ? -1e9f : span.first;
                const float hi = route.lane && route.below ? 1e9f : span.second;
                uses.push_back({i, channel, lo, hi});
            }
        }
        std::vector<int> tracks(static_cast<std::size_t>(columns) + 1, 0);
        std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return std::tie(a.channel, a.lo, a.hi) < std::tie(b.channel, b.lo, b.hi); });
        for (std::size_t i = 0; i < uses.size();) {
            const int channel = uses[i].channel;
            std::vector<float> free;  // where each track is free again
            for (; i < uses.size() && uses[i].channel == channel; ++i) {
                int chosen = -1;
                for (std::size_t t = 0; t < free.size(); ++t) {
                    if (free[t] < uses[i].lo - room.pitch) {
                        chosen = static_cast<int>(t);
                        break;
                    }
                }
                if (chosen < 0) {
                    chosen = static_cast<int>(free.size());
                    free.push_back(0.0f);
                }
                free[static_cast<std::size_t>(chosen)] = uses[i].hi;
                uses[i].track = chosen;
            }
            tracks[static_cast<std::size_t>(channel)] = static_cast<int>(free.size());
        }
        int lanes = 0, lanesBelow = 0;
        for (const bool below : {false, true}) {
            std::vector<std::size_t> withLane;
            for (std::size_t i = 0; i < routes.size(); ++i) {
                if (routes[i].lane && routes[i].below == below) withLane.push_back(i);
            }
            std::sort(withLane.begin(), withLane.end(), [&](std::size_t a, std::size_t b) { return std::tie(routes[a].lo, routes[a].hi) < std::tie(routes[b].lo, routes[b].hi); });
            std::vector<int> free;
            for (const std::size_t i : withLane) {
                int chosen = -1;
                for (std::size_t l = 0; l < free.size(); ++l) {
                    if (free[l] < routes[i].lo) {
                        chosen = static_cast<int>(l);
                        break;
                    }
                }
                if (chosen < 0) {
                    chosen = static_cast<int>(free.size());
                    free.push_back(0);
                }
                free[static_cast<std::size_t>(chosen)] = routes[i].hi;
                routes[i].laneIndex = chosen;
            }
            (below ? lanesBelow : lanes) = static_cast<int>(free.size());
        }

        // Across: the channels and the columns, one after the other.
        std::vector<float> channelX(static_cast<std::size_t>(columns) + 1, 0.0f), columnX(static_cast<std::size_t>(columns), 0.0f);
        std::vector<float> columnW(static_cast<std::size_t>(columns), 0.0f);
        for (std::uint32_t part = 0; part < count; ++part) {
            float& widest = columnW[static_cast<std::size_t>(col[part])];
            widest = std::max(widest, shapes[part].w);
        }
        float x = 0.0f;
        for (int c = 0; c <= columns; ++c) {
            channelX[static_cast<std::size_t>(c)] = x;
            const float least = c == 0 || c == columns ? room.pad : room.gap;
            x += std::max(least, static_cast<float>(tracks[static_cast<std::size_t>(c)] + 1) * room.pitch);
            if (c < columns) {
                columnX[static_cast<std::size_t>(c)] = x;
                x += columnW[static_cast<std::size_t>(c)];
            }
        }
        box.w = x;
        const float top = room.band + (lanes > 0 ? static_cast<float>(lanes) * room.pitch + room.gap : room.pad);
        const float bottom = top + tallest + room.gap;  // where the lanes under the parts start
        box.h = top + tallest + (lanesBelow > 0 ? room.gap + static_cast<float>(lanesBelow) * room.pitch : 0.0f) + room.pad;
        for (float& y : box.inY) y += top;
        for (float& y : box.outY) y += top;
        for (std::uint32_t part = 0; part < count; ++part) {
            const Part& p = box.parts[part];
            const float px = columnX[static_cast<std::size_t>(col[part])];
            const float partY = py[part] + top;
            if (p.leaf) {
                leaves_[p.id].x = px;
                leaves_[p.id].y = partY;
            } else {
                boxes_[p.id].x = px;
                boxes_[p.id].y = partY;
            }
        }

        // The wires: out of the source across to its track, along the tracks and the lane, and across into
        // each pin that reads it.
        std::map<std::pair<std::size_t, int>, int> trackOf;
        for (const Use& use : uses) trackOf[{use.net, use.channel}] = use.track;
        const auto trackX = [&](std::size_t net, int channel) {
            return channelX[static_cast<std::size_t>(channel)] + static_cast<float>(trackOf.at({net, channel}) + 1) * room.pitch;
        };
        const auto add = [&](float x0, float y0, float x1, float y1, std::uint32_t signal) {
            if (std::abs(x0 - x1) < 1e-4f && std::abs(y0 - y1) < 1e-4f) return;
            box.segs.push_back({x0, y0, x1, y1, signal});
        };
        for (std::size_t i = 0; i < box.nets.size(); ++i) {
            const std::uint32_t signal = box.nets[i].first;
            const Net& net = box.nets[i].second;
            const Route& route = routes[i];
            const float laneY = (route.below ? bottom : room.band) + (static_cast<float>(route.laneIndex) + 0.5f) * room.pitch;
            std::map<int, std::vector<float>> attach;  // in each channel, where wires meet the track
            const float sourceY = route.sourceY + top;
            float sourceX = 0.0f;
            if (net.source >= 0) {
                const auto from = static_cast<std::size_t>(net.source);
                sourceX = columnX[static_cast<std::size_t>(col[from])] + shapes[from].w;
            }
            add(sourceX, sourceY, trackX(i, route.sourceChannel), sourceY, signal);
            attach[route.sourceChannel].push_back(sourceY);
            for (const auto& [channel, across] : route.jogs) {
                add(trackX(i, route.sourceChannel), across + top, trackX(i, channel), across + top, signal);
                attach[route.sourceChannel].push_back(across + top);
                attach[channel].push_back(across + top);
            }
            for (std::size_t e = 0; e < route.ends.size(); ++e) {
                const int channel = route.straight[e] ? route.sourceChannel : route.ends[e].first;
                const float y = route.ends[e].second + top;
                const float to = e < net.sinks.size() ? columnX[static_cast<std::size_t>(col[net.sinks[e].first])] : box.w;
                add(trackX(i, channel), y, to, y, signal);
                attach[channel].push_back(y);
            }
            float laneLeft = 1e9f, laneRight = -1e9f;
            for (auto& [channel, ys] : attach) {
                const float tx = trackX(i, channel);
                if (route.lane) {
                    ys.push_back(laneY);
                    laneLeft = std::min(laneLeft, tx);
                    laneRight = std::max(laneRight, tx);
                }
                const auto [low, high] = std::minmax_element(ys.begin(), ys.end());
                const float lowY = *low, highY = *high;
                add(tx, lowY, tx, highY, signal);
                // A dot where a wire leaves a track between its ends: there it branches.
                for (const float y : ys) {
                    if (y > lowY + 1e-4f && y < highY - 1e-4f) box.dots.push_back({tx, y});
                }
            }
            if (route.lane) {
                add(laneLeft, laneY, laneRight, laneY, signal);
                for (const auto& [channel, ys] : attach) {
                    const float tx = trackX(i, channel);
                    if (tx > laneLeft + 1e-4f && tx < laneRight - 1e-4f) box.dots.push_back({tx, laneY});
                }
            }
        }
        // Pins held at 0 or 1: a mark beside the pin.
        for (std::uint32_t part = 0; part < count; ++part) {
            const Part& p = box.parts[part];
            if (!p.leaf) continue;
            const Leaf& leaf = leaves_[p.id];
            for (std::size_t k = 0; k < leaf.ins.size(); ++k) {
                if (leaf.ins[k] == kTie0 || leaf.ins[k] == kTie1) {
                    box.ties.push_back({leaf.x, leaf.y + leaf.inY[k], static_cast<std::uint8_t>(leaf.ins[k] == kTie1 ? 1 : 0)});
                }
            }
        }
    }

    // From positions inside parents to positions on the plan.
    void place(std::uint32_t id, float ox, float oy) {
        Box& box = boxes_[id];
        box.x += ox;
        box.y += oy;
        for (float& y : box.inY) y += box.y;
        for (float& y : box.outY) y += box.y;
        for (ArchSegment& s : box.segs) {
            s.x0 += box.x;
            s.x1 += box.x;
            s.y0 += box.y;
            s.y1 += box.y;
        }
        for (ArchPoint& p : box.dots) {
            p.x += box.x;
            p.y += box.y;
        }
        for (ArchTie& t : box.ties) {
            t.x += box.x;
            t.y += box.y;
        }
        for (const Part& part : box.parts) {
            if (part.leaf) {
                leaves_[part.id].x += box.x;
                leaves_[part.id].y += box.y;
            } else {
                place(part.id, box.x, box.y);
            }
        }
    }

    Architecture finish() {
        Architecture plan;
        plan.width = boxes_[0].w;
        plan.height = boxes_[0].h;
        const auto wiresOf = [&](const Box& box) {
            ArchWires w;
            w.firstSegment = static_cast<std::uint32_t>(plan.segments.size());
            w.segments = static_cast<std::uint32_t>(box.segs.size());
            w.firstDot = static_cast<std::uint32_t>(plan.dots.size());
            w.dots = static_cast<std::uint32_t>(box.dots.size());
            plan.segments.insert(plan.segments.end(), box.segs.begin(), box.segs.end());
            plan.dots.insert(plan.dots.end(), box.dots.begin(), box.dots.end());
            plan.ties.insert(plan.ties.end(), box.ties.begin(), box.ties.end());
            return w;
        };
        for (const Box& box : boxes_) {
            if (box.kind == Kind::Cell) {
                ArchCell cell;
                cell.rect = {box.x, box.y, box.w, box.h};
                cell.gate = box.gate;
                cell.inputs = box.inY;
                cell.output = box.outY.empty() ? box.y + box.h / 2.0f : box.outY.front();
                cell.firstNand = static_cast<std::uint32_t>(plan.nands.size());
                for (const Part& part : box.parts) {
                    if (part.leaf) plan.nands.push_back({{leaves_[part.id].x, leaves_[part.id].y, 1.0f, 1.0f}});
                }
                cell.nands = static_cast<std::uint32_t>(plan.nands.size()) - cell.firstNand;
                cell.wires = wiresOf(box);
                plan.cells.push_back(std::move(cell));
                continue;
            }
            ArchFrame frame;
            frame.rect = {box.x, box.y, box.w, box.h};
            frame.label = box.label;
            frame.depth = box.depth;
            frame.kind = box.kind == Kind::Computer ? ArchFrameKind::Computer
                         : box.kind == Kind::Chip  ? ArchFrameKind::Chip
                         : box.kind == Kind::Box   ? ArchFrameKind::Box
                                                   : ArchFrameKind::Detail;
            frame.wires = wiresOf(box);
            plan.frames.push_back(std::move(frame));
        }
        for (const Leaf& leaf : leaves_) {
            for (std::size_t k = 0; k < leaf.ins.size(); ++k) {
                if (isSignal(leaf.ins[k])) plan.pins.push_back({leaf.x, leaf.y + leaf.inY[k], leaf.ins[k], false});
            }
            for (std::size_t k = 0; k < leaf.outs.size(); ++k) plan.pins.push_back({leaf.x + leaf.w, leaf.y + leaf.outY[k], leaf.outs[k], true});
            if (!leaf.memory) continue;
            const sim::Memory& made = n_.memories()[leaf.memoryIndex];
            ArchMemory memory;
            memory.rect = {leaf.x, leaf.y, leaf.w, leaf.h};
            memory.words = 1u << made.addressBits;
            memory.bits = made.dataBits;
            memory.banks = banksOf(memory.words, memory.bits);
            memory.rom = std::none_of(n_.memoryPorts().begin(), n_.memoryPorts().end(), [&](const sim::MemoryPort& p) { return p.memory == leaf.memoryIndex; });
            const float gridH = static_cast<float>(memory.words / memory.banks) * kArchMemoryCell;
            memory.grid = {leaf.x + 1.0f, leaf.y + (leaf.h - gridH) / 2.0f, leaf.w - 2.0f, gridH};
            memory.label = std::to_string(memory.words) + " × " + std::to_string(memory.bits);
            plan.counts.memoryBits += std::size_t{memory.words} * memory.bits;
            plan.counts.transistors += std::size_t{memory.words} * memory.bits * (memory.rom ? 1u : 6u);
            plan.memories.push_back(std::move(memory));
        }
        // What comes into the computer from outside: a pin of the computer's own box, named.
        const Box& root = boxes_[0];
        for (std::size_t k = 0; k < root.ins.size(); ++k) {
            plan.ports.push_back({root.x, root.inY[k], signals_[root.ins[k]].name});
            plan.pins.push_back({root.x, root.inY[k], root.ins[k], true});
        }
        for (const Signal& signal : signals_) plan.signals.push_back(signal.rule);
        // Where each part of the board on top went: its box, or for a gate the cells of its bits together.
        for (std::uint32_t t = 0; t < n_.tags().size(); ++t) {
            const sim::Tag& tag = n_.tags()[t];
            if (tag.parent != sim::kNoTag) continue;
            std::vector<std::uint32_t> made;
            if (const auto box = boxOfTag_.find(t); box != boxOfTag_.end()) made.push_back(box->second);
            if (const auto cells = cellsOfTag_.find(t); cells != cellsOfTag_.end()) made.insert(made.end(), cells->second.begin(), cells->second.end());
            if (made.empty()) continue;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const std::uint32_t b : made) {
                x0 = std::min(x0, boxes_[b].x);
                y0 = std::min(y0, boxes_[b].y);
                x1 = std::max(x1, boxes_[b].x + boxes_[b].w);
                y1 = std::max(y1, boxes_[b].y + boxes_[b].h);
            }
            plan.places.push_back({tag.x, tag.y, {x0, y0, x1 - x0, y1 - y0}});
        }
        plan.counts.gates = plan.cells.size();
        plan.counts.flipFlops = flipFlops_;
        plan.counts.nands = plan.nands.size();
        plan.counts.transistors += 4 * plan.counts.nands;
        return plan;
    }
};

}  // namespace

std::vector<std::uint8_t> archValues(const Architecture& plan, const BoardCircuit& circuit) {
    std::vector<std::uint8_t> values(plan.signals.size(), 0);
    const auto of = [&](std::uint32_t s) -> std::uint8_t {
        if (s == kTie1) return 1;
        return s < values.size() ? values[s] : 0;
    };
    // A rule only reads signals made before it, so one pass in order does.
    for (std::size_t s = 0; s < plan.signals.size(); ++s) {
        const ArchSignal& rule = plan.signals[s];
        switch (rule.rule) {
            case ArchRule::Net: values[s] = static_cast<std::uint8_t>((circuit.netValue(rule.net) >> rule.bit) & 1u); break;
            case ArchRule::Not: values[s] = of(rule.a) != 0 ? 0 : 1; break;
            case ArchRule::Nand: values[s] = of(rule.a) != 0 && of(rule.b) != 0 ? 0 : 1; break;
            case ArchRule::Latched: values[s] = of(rule.a) != 0 ? of(rule.b) : of(rule.c); break;
        }
    }
    return values;
}

Architecture buildArchitecture(const Board& board, const ChipBoards* chips, const std::string& title) {
    const sim::Netlist netlist = BoardCircuit::tagged(board, chips);
    return Planner(netlist).build(title);
}

Architecture architecture16() {
    const ComputerProject project = buildComputer16({}, "");
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    return buildArchitecture(project.board, &chips, "Computador 16 bits");
}

Architecture architecture32() {
    const Computer32Project project = buildComputer32(mips::Program{}, "");
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    return buildArchitecture(project.board, &chips, "Computador 32 bits");
}

}  // namespace bancada::editor
