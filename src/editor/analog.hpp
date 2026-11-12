// The electric side of a board. Its parts become a circuit for the solver, with the logic around it as
// little batteries that follow each signal. It's solved again whenever the logic changes, and stepped
// through time when it has capacitors.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "editor/board.hpp"
#include "electric/solver.hpp"

namespace bancada::editor {

inline constexpr double kLogicHigh = 5.0;
inline constexpr double kLogicHighAbove = 3.5;  // what the logic reads as 1
inline constexpr double kLogicLowBelow = 1.5;   // and as 0. In between it can't tell.
inline constexpr double kLogicOutputOhms = 25.0;  // what a real logic output has inside it
inline constexpr double kBatteryOhms = 0.1;
inline constexpr double kSwitchClosedOhms = 0.01;
inline constexpr double kSwitchOpenOhms = 1e12;
inline constexpr double kLedLitAbove = 0.1;  // an LED with less light than this looks off: about 1 mA

struct ElectricModel {
    electric::Circuit circuit;
    std::vector<int> nodeOfGroup;  // the solver's node for each net of the board, or -1 if no electric part is on it

    // A logic signal on an electric wire, and the part pin that drives it.
    struct Source {
        std::size_t element = 0;
        std::uint32_t group = 0;
        PartId driver = 0;
        unsigned driverPin = 0;
    };
    std::vector<Source> sources;

    struct Switch {
        std::size_t element = 0;
        PartId part = 0;
    };
    std::vector<Switch> switches;

    struct Placed {
        std::size_t element = 0;
        PartKind kind = PartKind::Resistor;
    };
    std::unordered_map<PartId, Placed> parts;
    bool capacitors = false;
};

struct PartReading {
    PartKind kind = PartKind::Resistor;
    double volts = 0.0;    // across it: first pin minus second; for transistors, base or gate minus emitter or source
    double amps = 0.0;     // through it; for transistors, the collector or drain current
    double control = 0.0;  // transistors: the base current
    double power = 0.0;    // watts turned into heat or light
    double light = 0.0;    // LEDs: from 0 to 1
    bool burnt = false;    // more than a real part of its kind survives
    const char* state = "";
};

class AnalogBoard {
public:
    explicit AnalogBoard(ElectricModel model);

    // The logic's signals and the switches as they are now. Solves for the new state unless time is being
    // stepped, in which case the next step will catch up.
    void update(const std::vector<bool>& sourcesHigh, const std::vector<bool>& switchesClosed);
    void advance(double seconds);
    bool stepping() const { return model_.capacitors; }

    std::optional<double> groupVolts(std::uint32_t group) const;
    std::optional<PartReading> reading(PartId part) const;
    double pinAmps(PartId part, unsigned pin) const;  // from the pin into its wire
    const ElectricModel& model() const { return model_; }

private:
    ElectricModel model_;
    electric::Solver solver_;
    bool started_ = false;
};

}  // namespace bancada::editor
