// A board, running. Rebuilt whenever the board changes: everything here is derived and disposable. Chips
// are flattened into one netlist, so a chip costs what its contents cost and nothing more.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "editor/analog.hpp"
#include "editor/board.hpp"
#include "sim/backend.hpp"

namespace bancada::editor {

// The project's other circuits, by id, for the chips on a board to be built from.
using ChipBoards = std::unordered_map<ChipId, const Board*>;

class BoardCircuit {
public:
    explicit BoardCircuit(const Board& board, const ChipBoards* chips = nullptr);

    void setToggle(PartId toggle, std::uint64_t value);
    void settle();

    // Every switch and clock low, and everything that remembers back in its starting state.
    void reset();

    bool hasClock() const { return !clocks_.empty(); }
    void setClocks(bool high);

    bool oscillating() const { return oscillating_; }
    bool hasConflict() const { return conflict_; }
    bool hasWidthConflict() const { return widthConflict_; }

    // Something that remembers: what the circuit does can depend on what it did before.
    bool hasMemory() const { return memory_; }

    bool wireOn(WireId wire) const;
    bool wireFloating(WireId wire) const;     // nothing drives this wire
    bool wireConflicted(WireId wire) const;   // two outputs drive this wire, or its pins have different widths
    unsigned wireBits(WireId wire) const;
    std::uint64_t wireValue(WireId wire) const;
    bool pinOn(PartId part, unsigned pin) const;
    std::uint64_t pinValue(PartId part, unsigned pin) const;
    // Which electrical net a wire or pin is on, to tell whether two of them are connected.
    std::optional<std::uint32_t> netOfWire(WireId wire) const;
    std::optional<std::uint32_t> netOfPin(PartId part, unsigned pin) const;

    // A word of the memory inside a box, found by the chips on the way to it from this board and then the box.
    // 0 if there's no such memory or word.
    std::uint64_t memoryWord(std::span<const PartId> path, std::uint64_t address) const;

    bool outputOn(PartId output) const;  // what an LED or an output is showing

    // Electric parts. Inside chips they become logic and nothing more; on this board they can be looked at.
    bool electricUnsettled() const { return electricUnsettled_; }  // an output the logic reads sat between 0 and 1
    bool electricTooBig() const { return electricTooBig_; }        // too many signals into one electric circuit
    bool hasAnalog() const { return analog_ != nullptr; }
    bool analogStepping() const { return analog_ != nullptr && analog_->stepping(); }
    void advanceAnalog(double seconds);
    std::optional<double> wireVolts(WireId wire) const;
    std::optional<double> pinVolts(PartId part, unsigned pin) const;
    double pinAmps(PartId part, unsigned pin) const;  // from the pin into its wire
    std::optional<PartReading> reading(PartId part) const;
    std::uint64_t outputValue(PartId output) const;

private:
    struct Group {
        sim::NetId net = 0;
        int drivers = 0;
        unsigned bits = 0;
        bool mismatch = false;
    };

    friend class CircuitBuilder;

    const Group* groupOfWire(WireId wire) const;
    const Group* groupOfPin(PartId part, unsigned pin) const;

    struct MemoryPlace {
        std::vector<PartId> path;
        std::uint32_t memory = 0;
    };

    void refreshAnalog();

    std::vector<Group> groups_;
    std::unique_ptr<AnalogBoard> analog_;
    std::vector<sim::NetId> analogSources_;
    std::vector<sim::NetId> analogSwitches_;
    bool electricUnsettled_ = false;
    bool electricTooBig_ = false;
    std::vector<MemoryPlace> memoryPlaces_;
    std::unordered_map<std::uint64_t, std::uint32_t> pinGroup_;
    std::unordered_map<WireId, std::uint32_t> wireGroup_;
    std::unordered_map<PartId, sim::NetId> toggles_;
    std::vector<sim::NetId> clocks_;
    std::unique_ptr<sim::Backend> simulator_;
    bool oscillating_ = false;
    bool conflict_ = false;
    bool widthConflict_ = false;
    bool memory_ = false;
};

}  // namespace bancada::editor
