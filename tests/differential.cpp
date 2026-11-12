// Runs random circuits on the reference interpreter and on the event simulator, and fails if they ever
// disagree on any net. The circuits are deliberately ugly: gate loops, clocks made of logic, flip-flops
// clocking flip-flops, undriven wires and wires with two drivers.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

#include "sim/backend.hpp"

using namespace bancada::sim;

namespace {

using Random = std::mt19937_64;

std::uint32_t below(Random& random, std::size_t limit) {
    return static_cast<std::uint32_t>(random() % limit);
}

NetId pick(Random& random, const std::vector<NetId>& nets) { return nets[below(random, nets.size())]; }

struct Circuit {
    Netlist netlist;
    std::vector<NetId> inputs;
};

// With gateLoops off, feedback can only pass through flip-flops. Those circuits settle, which random
// loops rarely do, and that exercises the paths where most gates are left alone.
Circuit randomCircuit(Random& random, bool gateLoops) {
    static constexpr std::array<unsigned, 7> kWidths{1, 1, 1, 2, 8, 16, 64};
    Circuit circuit;
    Netlist& n = circuit.netlist;
    std::map<unsigned, std::vector<NetId>> netsOfWidth;
    std::vector<NetId> wires;

    auto add = [&](NetId net) { netsOfWidth[n.width(net)].push_back(net); };
    auto randomWidth = [&] { return kWidths[below(random, kWidths.size())]; };
    auto netOfSomeWidth = [&] {
        for (;;) {
            const unsigned width = randomWidth();
            const auto found = netsOfWidth.find(width);
            if (found != netsOfWidth.end()) return pick(random, found->second);
        }
    };

    for (unsigned width : {1u, 1u, 1u, 1u, 2u, 8u, 16u, 64u}) {
        const NetId input = n.input("in", width);
        circuit.inputs.push_back(input);
        add(input);
    }
    const std::uint32_t wireCount = 2 + below(random, 16);
    for (std::uint32_t i = 0; i < wireCount; ++i) {
        const NetId wire = n.wire(randomWidth());
        wires.push_back(wire);
        add(wire);
    }

    const std::uint32_t nodeCount = 20 + below(random, 300);
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const std::uint32_t kind = below(random, 100);
        if (kind < 70) {
            const NetId a = netOfSomeWidth();
            add(n.nand(a, pick(random, netsOfWidth[n.width(a)])));
        } else if (kind < 80) {
            const NetId a = netOfSomeWidth();
            const unsigned width = 1 + below(random, n.width(a));
            add(n.slice(a, below(random, n.width(a) - width + 1), width));
        } else if (kind < 87) {
            const std::array parts{netOfSomeWidth(), netOfSomeWidth()};
            if (n.width(parts[0]) + n.width(parts[1]) <= kMaxWidth) add(n.concat(parts));
        } else if (kind < 90) {
            add(n.constant(random(), randomWidth()));
        } else if (kind < 96) {
            add(n.dff(netOfSomeWidth(), pick(random, netsOfWidth[1]), random()));
        } else {
            // A little memory: 2 address bits, or 8 when an 8-bit net is around to address it.
            const unsigned addressBits = below(random, 2) == 0 ? 2 : 8;
            const unsigned dataBits = randomWidth();
            std::vector<std::uint64_t> initial;
            for (std::uint32_t w = below(random, 4); w > 0; --w) initial.push_back(random());
            const std::uint32_t memory = n.memory(addressBits, dataBits, std::move(initial));
            const NetId address = pick(random, netsOfWidth[addressBits]);
            add(n.memoryRead(memory, address));
            const auto data = netsOfWidth.find(dataBits);
            if (data != netsOfWidth.end()) {
                n.memoryWrite(memory, pick(random, netsOfWidth[addressBits]), pick(random, data->second),
                              pick(random, netsOfWidth[1]), pick(random, netsOfWidth[1]));
            }
        }
    }

    // Flip-flop outputs are safe sources: nothing passes through them within a settle.
    std::map<unsigned, std::vector<NetId>> safeSources;
    for (NetId net = 0; net < n.nodes().size(); ++net) {
        const Op op = n.nodes()[net].op;
        if (op == Op::Dff || op == Op::Input) safeSources[n.width(net)].push_back(net);
    }

    for (NetId wire : wires) {
        const std::uint32_t kind = below(random, 10);
        if (kind == 0) continue;  // left undriven
        const unsigned width = n.width(wire);
        if (!gateLoops && safeSources[width].empty()) continue;
        const std::vector<NetId>& sameWidth = gateLoops ? netsOfWidth[width] : safeSources[width];
        n.drive(wire, pick(random, sameWidth));
        if (kind == 1) n.drive(wire, pick(random, sameWidth));  // two drivers
    }
    return circuit;
}

struct Totals {
    std::uint64_t circuits = 0;
    std::uint64_t withGateLoops = 0;
    std::uint64_t settles = 0;
    std::uint64_t oscillatingSettles = 0;
    std::uint64_t referenceEvaluations = 0;
    std::uint64_t eventEvaluations = 0;
};

bool same(const Backend& a, const Backend& b, std::size_t netCount, std::uint64_t seed, int step) {
    for (std::size_t net = 0; net < netCount; ++net) {
        const NetId id = static_cast<NetId>(net);
        if (a.get(id) == b.get(id)) continue;
        std::printf("seed %llu, step %d: net %zu is %llu on the reference and %llu on the event simulator\n",
                    static_cast<unsigned long long>(seed), step, net, static_cast<unsigned long long>(a.get(id)),
                    static_cast<unsigned long long>(b.get(id)));
        return false;
    }
    return true;
}

bool runCircuit(std::uint64_t seed, Totals& totals) {
    Random random(seed);
    const Circuit circuit = randomCircuit(random, seed % 2 == 0);
    const Program program = compile(circuit.netlist);
    const std::size_t netCount = circuit.netlist.nodes().size();
    auto reference = makeInterpreter(program);
    auto event = makeEventSimulator(program);

    ++totals.circuits;
    for (const Block& block : program.blocks) {
        if (block.loop) {
            ++totals.withGateLoops;
            break;
        }
    }

    if (!same(*reference, *event, netCount, seed, -1)) return false;
    for (int step = 0; step < 60; ++step) {
        if (below(random, 25) == 0) {
            reference->reset();
            event->reset();
        }
        const std::uint32_t changes = below(random, 4);  // sometimes nothing changes at all
        for (std::uint32_t i = 0; i < changes; ++i) {
            const NetId input = pick(random, circuit.inputs);
            const std::uint64_t value = random();
            reference->set(input, value);
            event->set(input, value);
        }
        const SettleResult a = reference->settle();
        const SettleResult b = event->settle();
        ++totals.settles;
        if (a.oscillating) ++totals.oscillatingSettles;
        if (a.oscillating != b.oscillating) {
            std::printf("seed %llu, step %d: reference oscillating=%d, event oscillating=%d\n",
                        static_cast<unsigned long long>(seed), step, a.oscillating, b.oscillating);
            return false;
        }
        if (!same(*reference, *event, netCount, seed, step)) return false;
    }
    totals.referenceEvaluations += reference->gateEvaluations();
    totals.eventEvaluations += event->gateEvaluations();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t circuits = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 3000;
    Totals totals;
    for (std::uint64_t seed = 1; seed <= circuits; ++seed) {
        if (!runCircuit(seed, totals)) return 1;
    }
    std::printf("%llu circuits, %llu with gate loops, %llu settles, %llu of them oscillating: no differences\n",
                static_cast<unsigned long long>(totals.circuits), static_cast<unsigned long long>(totals.withGateLoops),
                static_cast<unsigned long long>(totals.settles),
                static_cast<unsigned long long>(totals.oscillatingSettles));
    std::printf("gate evaluations: %llu on the reference, %llu on the event simulator\n",
                static_cast<unsigned long long>(totals.referenceEvaluations),
                static_cast<unsigned long long>(totals.eventEvaluations));
    return 0;
}
