// Measures the backends on two kinds of circuit, because real ones are mixtures of both:
//   busy:  a counter feeding a chain of NAND adders, where most of the circuit moves on every tick
//   quiet: the same adders fed by a register that changes once in 32768 ticks, so nearly all of the
//          circuit sits still, the way most of a computer does on any one tick
// A tick is a rise and a fall of the clock. Later backends get measured on these same circuits.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>

#include "sim/backend.hpp"
#include "sim/gates.hpp"

using namespace bancada::sim;
namespace g = bancada::sim::gates;

namespace {

constexpr unsigned kWidth = 16;

struct Circuit {
    Netlist netlist;
    NetId clock = 0;
};

NetId counter(Netlist& n, NetId clock) {
    const NetId next = n.wire(kWidth);
    const NetId count = n.dff(next, clock);
    n.drive(next, g::rippleAdder(n, count, n.constant(1, kWidth), n.constant(0)).sum);
    return count;
}

NetId adderChain(Netlist& n, NetId value, int adders) {
    for (int i = 0; i < adders; ++i) {
        value = g::rippleAdder(n, value, n.constant(0x5A5A, kWidth), n.constant(0)).sum;
    }
    return value;
}

Circuit busy(int adders) {
    Circuit c;
    c.clock = c.netlist.input("clock");
    adderChain(c.netlist, counter(c.netlist, c.clock), adders);
    return c;
}

Circuit quiet(int adders) {
    Circuit c;
    Netlist& n = c.netlist;
    c.clock = n.input("clock");
    const NetId count = counter(n, c.clock);
    const NetId held = n.dff(count, n.slice(count, kWidth - 1, 1));
    adderChain(n, held, adders);
    return c;
}

struct Result {
    double ticksPerSecond;
    double evaluationsPerTick;
};

Result measure(std::unique_ptr<Backend> sim, NetId clock) {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t ticks = 0;
    double seconds = 0;
    do {
        for (int i = 0; i < 64; ++i) {
            sim->set(clock, 1);
            sim->settle();
            sim->set(clock, 0);
            sim->settle();
        }
        ticks += 64;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    } while (seconds < 0.5);
    return {static_cast<double>(ticks) / seconds,
            static_cast<double>(sim->gateEvaluations()) / static_cast<double>(ticks)};
}

void row(const char* name, const Circuit& circuit) {
    const Result reference = measure(makeInterpreter(compile(circuit.netlist)), circuit.clock);
    const Result event = measure(makeEventSimulator(compile(circuit.netlist)), circuit.clock);
    std::printf("%-6s %9zu %13.0f %13.0f %8.2fx %12.0f %12.0f\n", name, circuit.netlist.nandCount(),
                reference.ticksPerSecond, event.ticksPerSecond, event.ticksPerSecond / reference.ticksPerSecond,
                reference.evaluationsPerTick, event.evaluationsPerTick);
}

}  // namespace

int main() {
    std::printf("%-6s %9s %13s %13s %9s %12s %12s\n", "", "nands", "ticks/s ref", "ticks/s event", "gain",
                "evals/t ref", "evals/t ev");
    for (int adders : {1, 8, 64, 512, 2048}) row("busy", busy(adders));
    for (int adders : {1, 8, 64, 512, 2048}) row("quiet", quiet(adders));
    return 0;
}
