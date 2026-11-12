#include "editor/circuit.hpp"

#include "electric/solver.hpp"
#include "electric/units.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <span>
#include <string>
#include <utility>

#include "sim/gates.hpp"

namespace bancada::editor {

namespace {

constexpr int kMaxChipDepth = 64;  // a safety net: chips that contain themselves are refused long before this

std::uint64_t pinKey(PartId part, unsigned pin) { return (std::uint64_t{part} << 8) | pin; }

// Which pins and joints are joined by wires.
class Connections {
public:
    explicit Connections(const Board& board) {
        std::uint32_t count = 0;
        for (const Part& part : board.parts()) {
            const unsigned pins = pinCount(part);
            for (unsigned pin = 0; pin < pins; ++pin) pinNode_[pinKey(part.id, pin)] = count++;
        }
        for (const Joint& joint : board.joints()) jointNode_[joint.id] = count++;
        parent_.resize(count);
        std::iota(parent_.begin(), parent_.end(), 0u);
        for (const Wire& wire : board.wires()) join(node(wire.from), node(wire.to));
        std::unordered_map<std::string, std::uint32_t> firstWithName;
        for (const Part& part : board.parts()) {
            if (part.kind != PartKind::Tunnel) continue;
            const std::uint32_t here = nodeOfPin(part.id, 0);
            const auto [found, isNew] = firstWithName.emplace(part.label, here);
            if (!isNew) join(found->second, here);
        }
    }

    std::uint32_t node(const Terminal& terminal) const {
        return terminal.kind == Terminal::Kind::Pin ? pinNode_.at(pinKey(terminal.id, terminal.pin))
                                                    : jointNode_.at(terminal.id);
    }
    std::uint32_t nodeOfPin(PartId part, unsigned pin) const { return pinNode_.at(pinKey(part, pin)); }

    std::uint32_t root(std::uint32_t node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    std::uint32_t count() const { return static_cast<std::uint32_t>(parent_.size()); }

private:
    void join(std::uint32_t a, std::uint32_t b) { parent_[root(a)] = root(b); }

    std::unordered_map<std::uint64_t, std::uint32_t> pinNode_;
    std::unordered_map<JointId, std::uint32_t> jointNode_;
    std::vector<std::uint32_t> parent_;
};

}  // namespace

// Turns a board into nets. For the top board it also fills in the circuit's maps; a chip's board is built
// the same way, with its switches replaced by the nets coming into the chip.
class CircuitBuilder {
public:
    using Group = BoardCircuit::Group;

    CircuitBuilder(BoardCircuit& circuit, sim::Netlist& netlist, const ChipBoards* chips)
        : circuit_(circuit), n_(netlist), chips_(chips) {}

    void buildTop(const Board& board) { build(board, {}, {}, 0, true); }

private:
    void build(const Board& board, std::span<const sim::NetId> chipInputs, std::span<sim::NetId> chipOutputs, int depth, bool top) {
        Connections connections(board);
        std::vector<Group> groups;
        std::vector<std::uint32_t> groupOfRoot(connections.count(), UINT32_MAX);
        const auto groupOf = [&](std::uint32_t node) {
            const std::uint32_t root = connections.root(node);
            if (groupOfRoot[root] == UINT32_MAX) {
                groupOfRoot[root] = static_cast<std::uint32_t>(groups.size());
                groups.push_back({});
            }
            return groupOfRoot[root];
        };

        std::unordered_map<std::uint64_t, std::uint32_t> pinGroup;
        for (const Part& part : board.parts()) {
            const unsigned pins = pinCount(part);
            for (unsigned pin = 0; pin < pins; ++pin) {
                const std::uint32_t index = groupOf(connections.nodeOfPin(part.id, pin));
                pinGroup[pinKey(part.id, pin)] = index;
                Group& group = groups[index];
                const unsigned bits = pinBits(part, pin);
                if (group.bits == 0) group.bits = bits;
                if (group.bits != bits) group.mismatch = true;
            }
        }
        std::unordered_map<WireId, std::uint32_t> wireGroup;
        for (const Wire& wire : board.wires()) wireGroup[wire.id] = groupOf(connections.node(wire.from));

        for (Group& group : groups) {
            if (group.bits == 0) group.bits = 1;
            group.net = n_.wire(group.bits);
            if (group.mismatch) circuit_.widthConflict_ = true;
        }

        // A pin on a wire whose widths disagree reads 0, and drives nothing.
        const auto pinNet = [&](const Part& part, unsigned pin) {
            const Group& group = groups[pinGroup.at(pinKey(part.id, pin))];
            if (group.mismatch) return n_.constant(0, pinBits(part, pin));
            return group.net;
        };
        const auto drive = [&](const Part& part, unsigned pin, sim::NetId source) {
            Group& group = groups[pinGroup.at(pinKey(part.id, pin))];
            if (group.mismatch) return;
            n_.drive(group.net, source);
            ++group.drivers;
            circuit_.conflict_ = circuit_.conflict_ || group.drivers > 1;
        };

        const std::vector<PartId> switches = inputsInOrder(board);
        std::unordered_map<PartId, std::size_t> switchIndex;
        for (std::size_t i = 0; i < switches.size(); ++i) switchIndex[switches[i]] = i;

        namespace g = sim::gates;
        for (const Part& part : board.parts()) {
            const unsigned output = inputCount(part);
            const auto fold = [&](auto combine) {
                sim::NetId result = pinNet(part, 0);
                for (unsigned pin = 1; pin < part.inputs; ++pin) result = combine(n_, result, pinNet(part, pin));
                return result;
            };
            // Every gate becomes NANDs here: the simulator never sees anything else.
            switch (part.kind) {
                case PartKind::And: drive(part, output, fold(g::andGate)); break;
                case PartKind::Or: drive(part, output, fold(g::orGate)); break;
                case PartKind::Xor: drive(part, output, fold(g::xorGate)); break;
                case PartKind::Nand: drive(part, output, g::notGate(n_, fold(g::andGate))); break;
                case PartKind::Nor: drive(part, output, g::notGate(n_, fold(g::orGate))); break;
                case PartKind::Xnor: drive(part, output, g::notGate(n_, fold(g::xorGate))); break;
                case PartKind::Not: drive(part, output, g::notGate(n_, pinNet(part, 0))); break;
                case PartKind::Toggle: {
                    if (top) {
                        const sim::NetId input = n_.input("toggle", part.width);
                        circuit_.toggles_[part.id] = input;
                        drive(part, 0, input);
                    } else {
                        const std::size_t index = switchIndex.at(part.id);
                        drive(part, 0, index < chipInputs.size() ? chipInputs[index] : n_.constant(0, part.width));
                    }
                    break;
                }
                case PartKind::Button: {
                    // Held down only by the player, on the board itself: inside a chip it stays up.
                    if (top) {
                        const sim::NetId input = n_.input("button", 1);
                        circuit_.toggles_[part.id] = input;
                        drive(part, 0, input);
                    } else {
                        drive(part, 0, n_.constant(0));
                    }
                    break;
                }
                case PartKind::Keyboard: {
                    // One input of eight bits: the key's code in the low seven, and the top bit while it's down.
                    const sim::NetId keys = top ? n_.input("keyboard", 8) : n_.constant(0, 8);
                    if (top) circuit_.toggles_[part.id] = keys;
                    drive(part, 0, n_.slice(keys, 0, 7));
                    drive(part, 1, n_.slice(keys, 7, 1));
                    break;
                }
                case PartKind::Constant:
                    drive(part, 0, n_.constant(part.value, part.width));
                    break;
                case PartKind::Clock: {
                    const sim::NetId clock = n_.input("clock");
                    circuit_.clocks_.push_back(clock);
                    drive(part, 0, clock);
                    break;
                }
                case PartKind::Splitter: {
                    const sim::NetId bus = pinNet(part, 0);
                    unsigned low = 0;
                    for (unsigned branch = 0; branch < part.inputs; ++branch) {
                        const unsigned bits = branchBits(part, branch);
                        drive(part, 1 + branch, n_.slice(bus, low, bits));
                        low += bits;
                    }
                    break;
                }
                case PartKind::Joiner: {
                    std::vector<sim::NetId> branches;
                    for (unsigned branch = 0; branch < part.inputs; ++branch) branches.push_back(pinNet(part, branch));
                    drive(part, part.inputs, n_.concat(branches));
                    break;
                }
                case PartKind::Box: {
                    const BoxSpec& spec = boxSpec(part.box);
                    std::vector<sim::NetId> in;
                    for (unsigned pin = 0; pin < output; ++pin) in.push_back(pinNet(part, pin));
                    std::vector<sim::NetId> out(spec.outputs.size());
                    const BoxParams params{part.width, part.address, part.rom.get()};
                    const std::size_t memoriesBefore = n_.memories().size();
                    spec.build(n_, in, out, params);
                    if (n_.memories().size() > memoriesBefore) {
                        std::vector<PartId> where = path_;
                        where.push_back(part.id);
                        circuit_.memoryPlaces_.push_back({std::move(where), static_cast<std::uint32_t>(memoriesBefore)});
                    }
                    for (unsigned i = 0; i < out.size(); ++i) drive(part, output + i, out[i]);
                    break;
                }
                case PartKind::Chip: {
                    if (chips_ == nullptr || depth >= kMaxChipDepth) break;
                    const auto found = chips_->find(part.chip);
                    if (found == chips_->end()) break;
                    std::vector<sim::NetId> in;
                    for (unsigned pin = 0; pin < output; ++pin) in.push_back(pinNet(part, pin));
                    std::vector<sim::NetId> out(outputCount(part));
                    for (unsigned i = 0; i < out.size(); ++i) out[i] = n_.constant(0, pinBits(part, output + i));
                    path_.push_back(part.id);
                    build(*found->second, in, out, depth + 1, false);
                    path_.pop_back();
                    for (unsigned i = 0; i < out.size(); ++i) drive(part, output + i, out[i]);
                    break;
                }
                case PartKind::Led:
                case PartKind::Probe:
                case PartKind::Tunnel:
                case PartKind::Display7:  // only shows its pins: drawn from their values
                    break;
                default:  // electric: all of them at once, below
                    break;
            }
        }
        buildElectric(board, groups, pinGroup, top);

        if (!top) {
            const std::vector<PartId> outputs = outputsInOrder(board);
            for (std::size_t i = 0; i < outputs.size() && i < chipOutputs.size(); ++i) {
                const Group& group = groups[pinGroup.at(pinKey(outputs[i], 0))];
                if (!group.mismatch && n_.width(group.net) == n_.width(chipOutputs[i])) chipOutputs[i] = group.net;
            }
            return;
        }
        circuit_.groups_ = std::move(groups);
        circuit_.pinGroup_ = std::move(pinGroup);
        circuit_.wireGroup_ = std::move(wireGroup);
    }

    static constexpr std::size_t kMaxElectricInputs = 8;
    enum : std::int8_t { kLow = 0, kHigh = 1, kHold = 2, kUnknown = 3 };

    // Electric parts become a circuit for the solver. Every combination of the signals coming in, from the
    // logic around them and from their switches, is solved, and each wire the logic reads becomes a table:
    // what it shows for each combination. A circuit that remembers, a latch made of transistors, settles
    // differently depending on where it came from, and there the table keeps whatever it was.
    void buildElectric(const Board& board, std::vector<Group>& groups, const std::unordered_map<std::uint64_t, std::uint32_t>& pinGroup, bool top) {
        std::vector<const Part*> parts;
        for (const Part& part : board.parts()) {
            if (isElectric(part.kind)) parts.push_back(&part);
        }
        if (parts.empty()) return;
        const auto groupAt = [&](const Part& part, unsigned pin) { return pinGroup.at(pinKey(part.id, pin)); };

        ElectricModel model;
        model.nodeOfGroup.assign(groups.size(), -1);
        bool grounded = false;
        for (const Part* part : parts) {
            if (part->kind != PartKind::Ground) continue;
            model.nodeOfGroup[groupAt(*part, 0)] = electric::kGround;
            grounded = true;
        }
        // A battery, a resistor and an LED in a loop need no ground symbol: then the battery's - is 0 V,
        // the way a meter would measure it.
        if (!grounded) {
            for (const Part* part : parts) {
                if (!isBattery(part->kind)) continue;
                model.nodeOfGroup[groupAt(*part, 1)] = electric::kGround;
                break;
            }
        }
        const auto node = [&](std::uint32_t group) {
            int& n = model.nodeOfGroup[group];
            if (n < 0) n = model.circuit.addNode();
            return n;
        };
        std::vector<sim::NetId> switchNets;
        using electric::Quantity;
        for (const Part* part : parts) {
            const auto at = [&](unsigned pin) { return node(groupAt(*part, pin)); };
            electric::Circuit& c = model.circuit;
            std::size_t element = 0;
            switch (part->kind) {
                case PartKind::Ground: break;
                case PartKind::Battery:
                case PartKind::BatteryCell: element = c.addSource(at(0), at(1), electric::toSi(part->value, Quantity::Volts), kBatteryOhms); break;
                case PartKind::Resistor: element = c.addResistor(at(0), at(1), electric::toSi(part->value, Quantity::Ohms)); break;
                case PartKind::Diode: element = c.addDiode(at(0), at(1)); break;
                case PartKind::Light:
                case PartKind::LedBulb: element = c.addDiode(at(0), at(1), electric::kLedModel); break;
                case PartKind::Switch: {
                    element = c.addResistor(at(0), at(1), kSwitchOpenOhms);
                    model.switches.push_back({element, part->id});
                    sim::NetId net = n_.constant(0);
                    if (top) {
                        net = n_.input("switch");
                        circuit_.toggles_[part->id] = net;
                    }
                    switchNets.push_back(net);
                    break;
                }
                case PartKind::Npn: element = c.addNpn(at(0), at(1), at(2)); break;
                case PartKind::Pnp: element = c.addPnp(at(0), at(1), at(2)); break;
                case PartKind::Nmos: element = c.addNmos(at(0), at(1), at(2)); break;
                case PartKind::Pmos: element = c.addPmos(at(0), at(1), at(2)); break;
                case PartKind::Capacitor:
                    element = c.addCapacitor(at(0), at(1), electric::toSi(part->value, Quantity::Farads));
                    model.capacitors = true;
                    break;
                default: break;
            }
            model.parts[part->id] = {element, part->kind};
        }

        // The logic's signals on electric wires: each one a little battery that follows it.
        std::vector<sim::NetId> sourceNets;
        for (std::uint32_t g = 0; g < groups.size(); ++g) {
            if (model.nodeOfGroup[g] < 0 || groups[g].drivers == 0) continue;
            if (model.nodeOfGroup[g] == electric::kGround || groups[g].mismatch) {
                circuit_.conflict_ = true;
                continue;
            }
            ElectricModel::Source source;
            source.group = g;
            source.element = model.circuit.addSource(model.nodeOfGroup[g], electric::kGround, 0.0, kLogicOutputOhms);
            for (const Part& part : board.parts()) {
                if (isElectric(part.kind)) continue;
                for (unsigned pin = inputCount(part); pin < pinCount(part); ++pin) {
                    if (groupAt(part, pin) == g && source.driver == 0) {
                        source.driver = part.id;
                        source.driverPin = pin;
                    }
                }
            }
            model.sources.push_back(source);
            sourceNets.push_back(groups[g].net);
        }

        // What the logic reads from electric wires.
        std::vector<std::uint32_t> outputs;
        std::vector<char> isOutput(groups.size(), 0);
        for (const Part& part : board.parts()) {
            if (isElectric(part.kind) || part.kind == PartKind::Tunnel) continue;
            for (unsigned pin = 0; pin < inputCount(part); ++pin) {
                const std::uint32_t g = groupAt(part, pin);
                if (model.nodeOfGroup[g] > 0 && groups[g].drivers == 0 && !groups[g].mismatch && isOutput[g] == 0) {
                    isOutput[g] = 1;
                    outputs.push_back(g);
                }
            }
        }

        std::vector<sim::NetId> inputs = sourceNets;
        inputs.insert(inputs.end(), switchNets.begin(), switchNets.end());
        if (inputs.size() > kMaxElectricInputs) {
            circuit_.electricTooBig_ = true;
        } else if (!outputs.empty()) {
            const std::vector<std::vector<std::int8_t>> table = tabulate(model, outputs, inputs.size());
            for (std::size_t k = 0; k < outputs.size(); ++k) {
                Group& group = groups[outputs[k]];
                std::vector<std::int8_t> column;
                for (const auto& row : table) column.push_back(row[k]);
                n_.drive(group.net, tree(column, inputs, inputs.size(), 0, group.net));
                ++group.drivers;
            }
        }
        if (top) {
            circuit_.analog_ = std::make_unique<AnalogBoard>(std::move(model));
            circuit_.analogSources_ = std::move(sourceNets);
            circuit_.analogSwitches_ = std::move(switchNets);
        }
    }

    // For each combination of inputs, low bit first, what each output shows.
    std::vector<std::vector<std::int8_t>> tabulate(const ElectricModel& model, const std::vector<std::uint32_t>& outputs, std::size_t inputCount) {
        electric::Solver solver(model.circuit);
        const std::size_t rows = std::size_t{1} << inputCount;
        const auto nodes = static_cast<std::size_t>(model.circuit.nodeCount());
        const auto apply = [&](std::size_t row) {
            for (std::size_t i = 0; i < model.sources.size(); ++i) solver.circuit().setValue(model.sources[i].element, ((row >> i) & 1u) != 0 ? kLogicHigh : 0.0);
            for (std::size_t j = 0; j < model.switches.size(); ++j) {
                const bool closed = ((row >> (model.sources.size() + j)) & 1u) != 0;
                solver.circuit().setValue(model.switches[j].element, closed ? kSwitchClosedOhms : kSwitchOpenOhms);
            }
        };
        const auto read = [&]() {
            std::vector<std::int8_t> out;
            for (const std::uint32_t g : outputs) {
                const double v = solver.volts(model.nodeOfGroup[g]);
                out.push_back(v > kLogicHighAbove ? kHigh : v < kLogicLowBelow ? kLow : kUnknown);
            }
            return out;
        };
        const auto defined = [](const std::vector<std::int8_t>& values) {
            return std::none_of(values.begin(), values.end(), [](std::int8_t v) { return v == kUnknown; });
        };

        std::vector<std::vector<std::int8_t>> table(rows);
        std::vector<std::vector<double>> settled(rows);
        std::vector<char> resolved(rows, 0);
        const std::vector<double> zeros(nodes, 0.0);
        const std::vector<double> highs(nodes, kLogicHigh);
        // First the plain way, from everything low and from everything high. Most circuits are done here.
        for (std::size_t row = 0; row < rows; ++row) {
            apply(row);
            const bool fromLow = solver.solveFrom(zeros);
            std::vector<std::int8_t> low = fromLow ? read() : std::vector<std::int8_t>{};
            if (fromLow) settled[row] = solver.allVolts();
            const bool fromHigh = solver.solveFrom(highs);
            const std::vector<std::int8_t> high = fromHigh ? read() : std::vector<std::int8_t>{};
            if (!fromLow && fromHigh) settled[row] = solver.allVolts();
            if (fromLow && fromHigh && defined(low) && low == high) {
                table[row] = low;
                resolved[row] = 1;
            }
        }
        // The rest settle in time, starting from where each neighbouring combination left them: that's how a
        // latch is reached in practice, and where it can come out two ways, it holds.
        for (std::size_t row = 0; row < rows; ++row) {
            if (resolved[row] != 0) continue;
            apply(row);
            std::vector<std::vector<double>> seeds{zeros, highs};
            for (std::size_t bit = 0; bit < inputCount; ++bit) {
                const std::size_t other = row ^ (std::size_t{1} << bit);
                if (!settled[other].empty()) seeds.push_back(settled[other]);
            }
            std::vector<std::vector<std::int8_t>> results;
            for (const std::vector<double>& seed : seeds) {
                if (solver.relax(seed)) results.push_back(read());
            }
            table[row].assign(outputs.size(), kLow);
            for (std::size_t k = 0; k < outputs.size(); ++k) {
                bool sawLow = false;
                bool sawHigh = false;
                for (const auto& result : results) {
                    sawLow = sawLow || result[k] == kLow;
                    sawHigh = sawHigh || result[k] == kHigh;
                }
                table[row][k] = sawLow && sawHigh ? kHold : sawHigh ? kHigh : kLow;
                if (!sawLow && !sawHigh) circuit_.electricUnsettled_ = true;
            }
        }
        return table;
    }

    // A table as logic: split on one input at a time, down to the leaves, which are 0, 1, or the wire itself.
    sim::NetId tree(const std::vector<std::int8_t>& column, const std::vector<sim::NetId>& inputs, std::size_t level, std::size_t base, sim::NetId self) {
        const std::size_t span = std::size_t{1} << level;
        const std::int8_t first = column[base];
        if (std::all_of(column.begin() + static_cast<std::ptrdiff_t>(base), column.begin() + static_cast<std::ptrdiff_t>(base + span),
                        [&](std::int8_t v) { return v == first; })) {
            if (first == kHold) return self;
            return n_.constant(first == kHigh ? 1 : 0);
        }
        const sim::NetId low = tree(column, inputs, level - 1, base, self);
        const sim::NetId high = tree(column, inputs, level - 1, base + span / 2, self);
        return sim::gates::mux(n_, low, high, inputs[level - 1]);
    }

    BoardCircuit& circuit_;
    sim::Netlist& n_;
    const ChipBoards* chips_;
    std::vector<PartId> path_;  // the chips being built, from the top board in
};

BoardCircuit::BoardCircuit(const Board& board, const ChipBoards* chips) {
    sim::Netlist n;
    CircuitBuilder(*this, n, chips).buildTop(board);

    sim::Program program = sim::compile(n);
    memory_ = !program.dffs.empty() || !program.memories.empty() ||
              std::any_of(program.blocks.begin(), program.blocks.end(), [](const sim::Block& block) { return block.loop; });
    simulator_ = sim::makeEventSimulator(std::move(program));
    settle();
}

void BoardCircuit::setToggle(PartId toggle, std::uint64_t value) {
    const auto found = toggles_.find(toggle);
    if (found != toggles_.end()) simulator_->set(found->second, value);
}

void BoardCircuit::setClocks(bool high) {
    for (sim::NetId clock : clocks_) simulator_->set(clock, high ? 1 : 0);
}

void BoardCircuit::settle() {
    oscillating_ = simulator_->settle().oscillating;
    if (analog_ != nullptr) refreshAnalog();
}

void BoardCircuit::refreshAnalog() {
    std::vector<bool> high;
    for (const sim::NetId net : analogSources_) high.push_back(simulator_->get(net) != 0);
    std::vector<bool> closed;
    for (const sim::NetId net : analogSwitches_) closed.push_back(simulator_->get(net) != 0);
    analog_->update(high, closed);
}

void BoardCircuit::advanceAnalog(double seconds) {
    if (analog_ != nullptr) analog_->advance(seconds);
}

std::optional<double> BoardCircuit::wireVolts(WireId wire) const {
    if (analog_ == nullptr) return std::nullopt;
    const std::optional<std::uint32_t> net = netOfWire(wire);
    return net ? analog_->groupVolts(*net) : std::nullopt;
}

std::optional<double> BoardCircuit::pinVolts(PartId part, unsigned pin) const {
    if (analog_ == nullptr) return std::nullopt;
    const std::optional<std::uint32_t> net = netOfPin(part, pin);
    return net ? analog_->groupVolts(*net) : std::nullopt;
}

double BoardCircuit::pinAmps(PartId part, unsigned pin) const { return analog_ == nullptr ? 0.0 : analog_->pinAmps(part, pin); }

std::optional<PartReading> BoardCircuit::reading(PartId part) const {
    return analog_ == nullptr ? std::nullopt : analog_->reading(part);
}

void BoardCircuit::reset() {
    simulator_->reset();
    settle();
}

const BoardCircuit::Group* BoardCircuit::groupOfWire(WireId wire) const {
    const auto found = wireGroup_.find(wire);
    return found == wireGroup_.end() ? nullptr : &groups_[found->second];
}

const BoardCircuit::Group* BoardCircuit::groupOfPin(PartId part, unsigned pin) const {
    const auto found = pinGroup_.find(pinKey(part, pin));
    return found == pinGroup_.end() ? nullptr : &groups_[found->second];
}

bool BoardCircuit::wireOn(WireId wire) const { return wireValue(wire) != 0; }

std::uint64_t BoardCircuit::wireValue(WireId wire) const {
    const Group* group = groupOfWire(wire);
    return group == nullptr ? 0 : simulator_->get(group->net);
}

unsigned BoardCircuit::wireBits(WireId wire) const {
    const Group* group = groupOfWire(wire);
    return group == nullptr ? 1 : group->bits;
}

bool BoardCircuit::wireFloating(WireId wire) const {
    const Group* group = groupOfWire(wire);
    return group != nullptr && group->drivers == 0;
}

bool BoardCircuit::wireConflicted(WireId wire) const {
    const Group* group = groupOfWire(wire);
    return group != nullptr && (group->drivers > 1 || group->mismatch);
}

bool BoardCircuit::pinOn(PartId part, unsigned pin) const { return pinValue(part, pin) != 0; }

std::uint64_t BoardCircuit::pinValue(PartId part, unsigned pin) const {
    const Group* group = groupOfPin(part, pin);
    return group == nullptr ? 0 : simulator_->get(group->net);
}

std::uint64_t BoardCircuit::memoryWord(std::span<const PartId> path, std::uint64_t address) const {
    for (const MemoryPlace& place : memoryPlaces_) {
        if (std::equal(place.path.begin(), place.path.end(), path.begin(), path.end())) return simulator_->memoryWord(place.memory, address);
    }
    return 0;
}

std::optional<std::uint32_t> BoardCircuit::netOfWire(WireId wire) const {
    const Group* group = groupOfWire(wire);
    if (group == nullptr) return std::nullopt;
    return static_cast<std::uint32_t>(group - groups_.data());
}

std::optional<std::uint32_t> BoardCircuit::netOfPin(PartId part, unsigned pin) const {
    const Group* group = groupOfPin(part, pin);
    if (group == nullptr) return std::nullopt;
    return static_cast<std::uint32_t>(group - groups_.data());
}

bool BoardCircuit::outputOn(PartId output) const { return pinOn(output, 0); }

std::uint64_t BoardCircuit::outputValue(PartId output) const { return pinValue(output, 0); }

}  // namespace bancada::editor
