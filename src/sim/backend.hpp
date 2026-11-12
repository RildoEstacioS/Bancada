// The one door to a running circuit. The game runs on makeEventSimulator. makeInterpreter stays as the
// plain reference that faster backends are checked against, and neither may behave differently.
//
// Timing model: gates have no delay. settle() brings the circuit to rest for the inputs it has now.
// A flip-flop built from NANDs works anyway, because a clock change reaches both of its latches
// before they are evaluated. Set D before raising the clock: changing both in one settle() captures
// the new D.
#pragma once

#include <cstdint>
#include <memory>

#include "sim/program.hpp"

namespace bancada::sim {

struct SettleResult {
    bool oscillating = false;  // some logic never came to rest
};

class Backend {
public:
    virtual ~Backend() = default;

    // All nets to 0, flip-flops to their reset values, then settled, with no clock edge counted.
    virtual void reset() = 0;

    virtual void set(NetId input, std::uint64_t value) = 0;
    virtual std::uint64_t get(NetId net) const = 0;

    // A word of a memory, for showing what's in it. 0 for a memory or address that doesn't exist.
    virtual std::uint64_t memoryWord(std::uint32_t memory, std::uint64_t address) const = 0;
    virtual SettleResult settle() = 0;

    // Gates evaluated since the last reset, for benchmarks.
    virtual std::uint64_t gateEvaluations() const = 0;
};

std::unique_ptr<Backend> makeInterpreter(Program program);     // evaluates everything on every settle
std::unique_ptr<Backend> makeEventSimulator(Program program);  // evaluates only what can have changed

}  // namespace bancada::sim
