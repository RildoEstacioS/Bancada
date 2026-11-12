// The electric solver against what's known for certain: closed-form answers from circuit theory, and laws
// no correct solution can break. A separate script holds it against ngspice on random circuits.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "check.hpp"
#include "electric/solver.hpp"

using namespace bancada;
using namespace bancada::electric;

namespace {

bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

#define CHECK_NEAR(a, b, tolerance)                                                            \
    do {                                                                                       \
        const double checkA = (a);                                                             \
        const double checkB = (b);                                                             \
        if (!near(checkA, checkB, (tolerance))) std::printf("  %g vs %g\n", checkA, checkB);   \
        CHECK(near(checkA, checkB, (tolerance)));                                              \
    } while (false)

// The net current into every node, from the elements themselves and not from what the solver worked out.
double worstImbalance(const Solver& solver) {
    const Circuit& circuit = solver.circuit();
    std::vector<double> total(static_cast<std::size_t>(circuit.nodeCount()) + 1, 0.0);
    const auto at = [&](Node node) -> double& { return total[static_cast<std::size_t>(node)]; };
    for (std::size_t i = 0; i < circuit.elements().size(); ++i) {
        const Element& e = circuit.elements()[i];
        const Currents c = solver.currents(i);
        at(e.a) += c.a;
        at(e.b) += c.b;
        at(e.c) += c.c;
    }
    double worst = 0.0;
    for (std::size_t node = 1; node < total.size(); ++node) worst = std::max(worst, std::abs(total[node]));
    return worst;
}

// The same, as a fraction of the largest current through each node: a node carrying amps can't be held to
// microamps, any more than a real meter could. The solver also ties every node to ground through 1 pS, so
// that none is left floating, and the picoamps that leak there aren't any element's: they're allowed for.
double worstRelativeImbalance(const Solver& solver) {
    const Circuit& circuit = solver.circuit();
    const auto nodes = static_cast<std::size_t>(circuit.nodeCount()) + 1;
    std::vector<double> total(nodes, 0.0);
    std::vector<double> largest(nodes, 0.0);
    for (std::size_t i = 0; i < circuit.elements().size(); ++i) {
        const Element& e = circuit.elements()[i];
        const Currents c = solver.currents(i);
        for (const auto& [node, amps] : {std::pair{e.a, c.a}, std::pair{e.b, c.b}, std::pair{e.c, c.c}}) {
            total[static_cast<std::size_t>(node)] += amps;
            largest[static_cast<std::size_t>(node)] = std::max(largest[static_cast<std::size_t>(node)], std::abs(amps));
        }
    }
    double worst = 0.0;
    for (std::size_t node = 1; node < nodes; ++node) {
        const double leak = 1e-12 * std::abs(solver.volts(static_cast<Node>(node))) * 1.01;
        constexpr double kPicoamp = 1e-12;  // below what any bench meter reads, and below the solver's own leak
        worst = std::max(worst, std::max(0.0, std::abs(total[node]) - leak - kPicoamp) / (largest[node] + 1e-15));
    }
    return worst;
}

// Solves w e^w = x.
double lambertW(double x) {
    double w = x < 1.0 ? x : std::log(x);
    for (int i = 0; i < 100; ++i) {
        const double e = std::exp(w);
        w -= (w * e - x) / (e * (w + 1.0));
    }
    return w;
}

}  // namespace

// --- straight lines ---------------------------------------------------------------------------------

TEST(a_divider_divides) {
    Circuit c;
    const Node top = c.addNode();
    const Node middle = c.addNode();
    c.addSource(top, kGround, 10.0);
    const std::size_t upper = c.addResistor(top, middle, 1000.0);
    c.addResistor(middle, kGround, 3000.0);
    Solver s(c);
    CHECK(s.solve());
    CHECK_NEAR(s.volts(middle), 7.5, 1e-6);
    CHECK_NEAR(s.currents(upper).a, 2.5e-3, 1e-9);
}

TEST(a_wheatstone_bridge_matches_its_formula) {
    Circuit c;
    const Node top = c.addNode();
    const Node left = c.addNode();
    const Node right = c.addNode();
    constexpr double r1 = 100, r2 = 220, r3 = 330, r4 = 470, rb = 1000, v = 12;
    c.addSource(top, kGround, v);
    c.addResistor(top, left, r1);
    c.addResistor(left, kGround, r2);
    c.addResistor(top, right, r3);
    c.addResistor(right, kGround, r4);
    c.addResistor(left, right, rb);
    Solver s(c);
    CHECK(s.solve());
    // By Thevenin: the bridge resistor sees two dividers and their parallel resistances.
    const double open = v * (r2 / (r1 + r2) - r4 / (r3 + r4));
    const double thevenin = r1 * r2 / (r1 + r2) + r3 * r4 / (r3 + r4);
    CHECK_NEAR(s.volts(left) - s.volts(right), open * rb / (rb + thevenin), 1e-6);
}

TEST(a_battery_short_circuited_is_limited_only_by_itself) {
    Circuit c;
    const Node a = c.addNode();
    const std::size_t battery = c.addSource(a, a, 5.0, 0.1);
    Solver s(c);
    CHECK(s.solve());
    CHECK_NEAR(std::abs(s.currents(battery).a), 50.0, 1e-6);
}

TEST(a_node_connected_to_nothing_is_still_a_number) {
    Circuit c;
    const Node floating = c.addNode();
    const Node a = c.addNode();
    c.addSource(a, kGround, 5.0);
    c.addResistor(a, kGround, 100.0);
    Solver s(c);
    CHECK(s.solve());
    CHECK_NEAR(s.volts(floating), 0.0, 1e-6);
}

TEST(two_sources_fighting_with_no_resistance_are_reported_not_invented) {
    Circuit c;
    const Node a = c.addNode();
    c.addSource(a, kGround, 5.0);
    c.addSource(a, kGround, 3.0);
    Solver s(c);
    CHECK(!s.solve());
}

// --- random networks of resistors --------------------------------------------------------------------

namespace {

struct RandomNetwork {
    Circuit circuit;
    std::vector<std::size_t> sources;
};

RandomNetwork randomLinear(std::mt19937& random, int nodes, int resistors, int sources) {
    RandomNetwork out;
    std::vector<Node> all{kGround};
    for (int i = 0; i < nodes; ++i) all.push_back(out.circuit.addNode());
    const auto pick = [&] { return all[random() % all.size()]; };
    std::uniform_real_distribution<double> ohms(1.0, 100000.0);
    for (std::size_t i = 1; i < all.size(); ++i) out.circuit.addResistor(all[i], all[random() % i], ohms(random));
    for (int i = 0; i < resistors; ++i) {
        const Node a = pick();
        Node b = pick();
        while (b == a) b = pick();
        out.circuit.addResistor(a, b, ohms(random));
    }
    for (int i = 0; i < sources; ++i) {
        const Node a = all[1 + static_cast<std::size_t>(i)];
        out.sources.push_back(out.circuit.addSource(a, kGround, static_cast<double>(random() % 20) - 10.0, 1.0));
    }
    return out;
}

}  // namespace

TEST(no_current_appears_or_vanishes_at_any_node_of_random_networks) {
    std::mt19937 random(11);
    for (int round = 0; round < 300; ++round) {
        RandomNetwork n = randomLinear(random, 3 + static_cast<int>(random() % 30), static_cast<int>(random() % 60), 1 + static_cast<int>(random() % 3));
        Solver s(n.circuit);
        CHECK(s.solve());
        CHECK(worstImbalance(s) < 1e-7);
    }
}

TEST(energy_is_conserved_in_random_networks) {
    std::mt19937 random(12);
    for (int round = 0; round < 200; ++round) {
        RandomNetwork n = randomLinear(random, 10, 30, 3);
        Solver s(n.circuit);
        CHECK(s.solve());
        double total = 0.0;
        for (std::size_t i = 0; i < n.circuit.elements().size(); ++i) {
            const Element& e = n.circuit.elements()[i];
            const Currents c = s.currents(i);
            total += c.a * s.volts(e.a) + c.b * s.volts(e.b);
        }
        CHECK(std::abs(total) < 1e-5);
    }
}

TEST(sources_add_up_independently_in_linear_networks) {
    std::mt19937 random(13);
    for (int round = 0; round < 100; ++round) {
        RandomNetwork n = randomLinear(random, 8, 20, 3);
        Solver both(n.circuit);
        CHECK(both.solve());
        std::vector<double> sum(static_cast<std::size_t>(n.circuit.nodeCount()), 0.0);
        for (std::size_t kept : n.sources) {
            Circuit alone = n.circuit;
            for (std::size_t other : n.sources) {
                if (other != kept) alone.setValue(other, 0.0);
            }
            Solver one(alone);
            CHECK(one.solve());
            for (int node = 1; node <= alone.nodeCount(); ++node) sum[static_cast<std::size_t>(node - 1)] += one.volts(node);
        }
        for (int node = 1; node <= n.circuit.nodeCount(); ++node) CHECK_NEAR(sum[static_cast<std::size_t>(node - 1)], both.volts(node), 1e-5);
    }
}

// --- diodes ------------------------------------------------------------------------------------------

TEST(a_diode_and_a_resistor_match_the_exact_solution) {
    for (double supply : {0.3, 0.7, 1.0, 5.0, 12.0, 100.0}) {
        for (double ohms : {10.0, 1000.0, 100000.0}) {
            for (double emission : {1.0, 2.0}) {
                Circuit c;
                const Node top = c.addNode();
                const Node anode = c.addNode();
                c.addSource(top, kGround, supply);
                const std::size_t r = c.addResistor(top, anode, ohms);
                const DiodeModel model{1e-14, emission};
                c.addDiode(anode, kGround, model);
                Solver s(c);
                CHECK(s.solve());
                const double thermal = emission * kThermalVoltage;
                const double argument = std::log(model.saturation * ohms / thermal) + (supply + model.saturation * ohms) / thermal;
                // In logs, since the number itself can be far too big to hold.
                double w = argument > 1.0 ? argument - std::log(argument) : lambertW(std::exp(argument));
                for (int i = 0; i < 100; ++i) w -= (w + std::log(w) - argument) / (1.0 + 1.0 / w);
                const double exact = thermal / ohms * w - model.saturation;
                CHECK(std::abs(s.currents(r).a - exact) <= 1e-9 + 1e-6 * std::abs(exact));
            }
        }
    }
}

TEST(a_backwards_diode_blocks) {
    Circuit c;
    const Node top = c.addNode();
    const Node cathode = c.addNode();
    c.addSource(top, kGround, -9.0);
    const std::size_t r = c.addResistor(top, cathode, 1000.0);
    c.addDiode(cathode, kGround);
    Solver s(c);
    CHECK(s.solve());
    CHECK(std::abs(s.currents(r).a) < 1e-9);
    CHECK_NEAR(s.volts(cathode), -9.0, 1e-3);
}

TEST(a_red_led_drops_about_two_volts) {
    Circuit c;
    const Node top = c.addNode();
    const Node anode = c.addNode();
    c.addSource(top, kGround, 5.0);
    const std::size_t r = c.addResistor(top, anode, 330.0);
    c.addDiode(anode, kGround, kLedModel);
    Solver s(c);
    CHECK(s.solve());
    CHECK(s.volts(anode) > 1.8 && s.volts(anode) < 2.2);
    CHECK(s.currents(r).a > 8e-3 && s.currents(r).a < 10e-3);
}

// --- bipolar transistors -----------------------------------------------------------------------------

namespace {

struct BjtRig {
    Solver solver;
    Node base;
    Node collector;
    std::size_t transistor;
};

BjtRig commonEmitter(double baseSupply, double baseOhms, double supply, double collectorOhms) {
    Circuit c;
    const Node vcc = c.addNode();
    const Node vin = c.addNode();
    const Node base = c.addNode();
    const Node collector = c.addNode();
    c.addSource(vcc, kGround, supply);
    c.addSource(vin, kGround, baseSupply);
    c.addResistor(vin, base, baseOhms);
    c.addResistor(vcc, collector, collectorOhms);
    const std::size_t q = c.addNpn(base, collector, kGround);
    return {Solver(std::move(c)), base, collector, q};
}

}  // namespace

TEST(with_no_base_current_a_transistor_is_off) {
    BjtRig rig = commonEmitter(0.0, 10000.0, 5.0, 1000.0);
    CHECK(rig.solver.solve());
    CHECK(std::abs(rig.solver.currents(rig.transistor).b) < 1e-9);
    CHECK_NEAR(rig.solver.volts(rig.collector), 5.0, 1e-3);
}

TEST(a_small_base_current_controls_a_collector_current_beta_times_larger) {
    BjtRig rig = commonEmitter(1.5, 100000.0, 5.0, 100.0);
    CHECK(rig.solver.solve());
    const Currents i = rig.solver.currents(rig.transistor);
    CHECK(i.a > 0.0);
    CHECK_NEAR(i.b / i.a, 100.0, 0.5);
    CHECK_NEAR(i.c, -(i.a + i.b), 1e-12);
    CHECK(rig.solver.volts(rig.base) > 0.55 && rig.solver.volts(rig.base) < 0.75);
}

TEST(with_plenty_of_base_current_a_transistor_saturates_like_a_closed_switch) {
    BjtRig rig = commonEmitter(5.0, 1000.0, 5.0, 1000.0);
    CHECK(rig.solver.solve());
    CHECK(rig.solver.volts(rig.collector) < 0.2);
    const Currents i = rig.solver.currents(rig.transistor);
    CHECK(i.b / i.a < 100.0);  // below beta: the collector resistor is what limits it now
    CHECK_NEAR(i.b, (5.0 - rig.solver.volts(rig.collector)) / 1000.0, 1e-6);
}

TEST(a_pnp_is_an_npn_with_every_sign_flipped) {
    const auto build = [](double sign, bool pnp) {
        Circuit c;
        const Node vcc = c.addNode();
        const Node vin = c.addNode();
        const Node base = c.addNode();
        const Node collector = c.addNode();
        c.addSource(vcc, kGround, sign * 5.0);
        c.addSource(vin, kGround, sign * 1.2);
        c.addResistor(vin, base, 47000.0);
        c.addResistor(vcc, collector, 330.0);
        if (pnp) {
            c.addPnp(base, collector, kGround);
        } else {
            c.addNpn(base, collector, kGround);
        }
        return c;
    };
    Solver npn(build(1.0, false));
    Solver pnp(build(-1.0, true));
    CHECK(npn.solve());
    CHECK(pnp.solve());
    for (int node = 1; node <= 4; ++node) CHECK_NEAR(npn.volts(node), -pnp.volts(node), 1e-6);
}

// --- MOSFETs -----------------------------------------------------------------------------------------

namespace {

Currents mosCurrents(double vgs, double vds, bool pmos, const MosModel& model = {}) {
    Circuit c;
    const Node g = c.addNode();
    const Node d = c.addNode();
    c.addSource(g, kGround, vgs);
    c.addSource(d, kGround, vds);
    const std::size_t m = pmos ? c.addPmos(g, d, kGround, model) : c.addNmos(g, d, kGround, model);
    Solver s(std::move(c));
    CHECK(s.solve());
    return s.currents(m);
}

}  // namespace

TEST(below_the_threshold_a_mosfet_is_off) {
    CHECK_EQ(mosCurrents(0.5, 5.0, false).b, 0.0);
    CHECK_EQ(mosCurrents(1.0, 5.0, false).b, 0.0);
}

TEST(a_mosfet_follows_the_square_law_in_each_region) {
    const MosModel m;
    for (double vgs : {1.5, 2.0, 3.0, 5.0}) {
        for (double vds : {0.1, 0.5, 1.0, 2.0, 4.0, 5.0}) {
            const double drive = vgs - m.threshold;
            const double channel = 1.0 + m.lambda * vds;
            const double expected = vds < drive ? m.gain * (drive * vds - vds * vds / 2.0) * channel : m.gain / 2.0 * drive * drive * channel;
            CHECK_NEAR(mosCurrents(vgs, vds, false).b, expected, 1e-8);
        }
    }
}

TEST(a_mosfet_conducts_both_ways) {
    const Currents forward = mosCurrents(4.0, 0.5, false);
    CHECK(forward.b > 0.0);
    const Currents backward = mosCurrents(3.5, -0.5, false);
    CHECK_NEAR(backward.b, -forward.b, 1e-8);
}

TEST(a_pmos_is_an_nmos_mirrored) {
    for (double vgs : {1.5, 3.0, 5.0}) {
        for (double vds : {0.5, 2.0, 5.0}) {
            CHECK_NEAR(mosCurrents(-vgs, -vds, true).b, -mosCurrents(vgs, vds, false).b, 1e-8);
        }
    }
}

// --- gates, from transistors -------------------------------------------------------------------------

namespace {

struct Gate {
    Circuit circuit;
    std::vector<std::size_t> inputs;
    Node output = kGround;
    Node supply = kGround;
    std::size_t rail = 0;
};

Gate cmosInverter() {
    Gate g;
    g.supply = g.circuit.addNode();
    const Node in = g.circuit.addNode();
    g.output = g.circuit.addNode();
    g.rail = g.circuit.addSource(g.supply, kGround, 5.0);
    g.inputs.push_back(g.circuit.addSource(in, kGround, 0.0));
    g.circuit.addPmos(in, g.output, g.supply);
    g.circuit.addNmos(in, g.output, kGround);
    return g;
}

Gate cmosNand() {
    Gate g;
    g.supply = g.circuit.addNode();
    const Node a = g.circuit.addNode();
    const Node b = g.circuit.addNode();
    const Node middle = g.circuit.addNode();
    g.output = g.circuit.addNode();
    g.rail = g.circuit.addSource(g.supply, kGround, 5.0);
    g.inputs.push_back(g.circuit.addSource(a, kGround, 0.0));
    g.inputs.push_back(g.circuit.addSource(b, kGround, 0.0));
    g.circuit.addPmos(a, g.output, g.supply);
    g.circuit.addPmos(b, g.output, g.supply);
    g.circuit.addNmos(a, g.output, middle);
    g.circuit.addNmos(b, middle, kGround);
    return g;
}

// Two NPNs in series under a pull-up: the classic resistor-transistor NAND.
Gate rtlNand() {
    Gate g;
    g.supply = g.circuit.addNode();
    const Node a = g.circuit.addNode();
    const Node b = g.circuit.addNode();
    const Node baseA = g.circuit.addNode();
    const Node baseB = g.circuit.addNode();
    const Node middle = g.circuit.addNode();
    g.output = g.circuit.addNode();
    g.rail = g.circuit.addSource(g.supply, kGround, 5.0);
    g.inputs.push_back(g.circuit.addSource(a, kGround, 0.0));
    g.inputs.push_back(g.circuit.addSource(b, kGround, 0.0));
    g.circuit.addResistor(a, baseA, 10000.0);
    g.circuit.addResistor(b, baseB, 10000.0);
    g.circuit.addResistor(g.supply, g.output, 1000.0);
    g.circuit.addNpn(baseA, g.output, middle);
    g.circuit.addNpn(baseB, middle, kGround);
    return g;
}

bool outputHigh(Solver& solver, const Gate& g, std::uint32_t inputs) {
    for (std::size_t i = 0; i < g.inputs.size(); ++i) solver.circuit().setValue(g.inputs[i], ((inputs >> i) & 1) != 0 ? 5.0 : 0.0);
    CHECK(solver.solve());
    const double v = solver.volts(g.output);
    CHECK(v > 4.0 || v < 1.0);
    return v > 2.5;
}

}  // namespace

TEST(a_cmos_inverter_inverts_and_draws_nothing_while_still) {
    Gate g = cmosInverter();
    Solver s(g.circuit);
    CHECK(outputHigh(s, g, 0));
    CHECK(std::abs(s.currents(g.rail).a) < 1e-9);
    CHECK(!outputHigh(s, g, 1));
    CHECK(std::abs(s.currents(g.rail).a) < 1e-9);
}

TEST(the_inverter_output_only_falls_as_the_input_rises) {
    Gate g = cmosInverter();
    Solver s(g.circuit);
    double last = 6.0;
    for (int step = 0; step <= 500; ++step) {
        s.circuit().setValue(g.inputs[0], 5.0 * step / 500.0);
        CHECK(s.solve());
        CHECK(s.volts(g.output) <= last + 1e-6);
        last = s.volts(g.output);
    }
    CHECK(last < 0.01);
}

TEST(a_nand_made_of_four_mosfets_is_a_nand) {
    Gate g = cmosNand();
    Solver s(g.circuit);
    for (std::uint32_t inputs = 0; inputs < 4; ++inputs) CHECK_EQ(outputHigh(s, g, inputs), inputs != 3);
}

TEST(a_nand_made_of_two_npn_transistors_is_a_nand) {
    Gate g = rtlNand();
    Solver s(g.circuit);
    for (std::uint32_t inputs = 0; inputs < 4; ++inputs) CHECK_EQ(outputHigh(s, g, inputs), inputs != 3);
}

// --- things that remember ----------------------------------------------------------------------------

namespace {

struct Latch {
    Circuit circuit;
    Node q = 0;
    Node notQ = 0;
};

// Two inverters feeding each other, the smallest thing that can remember a bit.
Latch crossCoupled() {
    Latch l;
    const Node vcc = l.circuit.addNode();
    l.q = l.circuit.addNode();
    l.notQ = l.circuit.addNode();
    l.circuit.addSource(vcc, kGround, 5.0);
    l.circuit.addPmos(l.q, l.notQ, vcc);
    l.circuit.addNmos(l.q, l.notQ, kGround);
    l.circuit.addPmos(l.notQ, l.q, vcc);
    l.circuit.addNmos(l.notQ, l.q, kGround);
    return l;
}

}  // namespace

TEST(two_inverters_in_a_ring_settle_into_either_of_two_states) {
    Latch l = crossCoupled();
    const auto seeded = [&](double q, double notQ) {
        std::vector<double> seed(3, 0.0);
        seed[0] = 5.0;
        seed[static_cast<std::size_t>(l.q - 1)] = q;
        seed[static_cast<std::size_t>(l.notQ - 1)] = notQ;
        Solver s(l.circuit);
        CHECK(s.relax(seed));
        return std::pair{s.volts(l.q), s.volts(l.notQ)};
    };
    const auto high = seeded(3.0, 2.0);
    CHECK(high.first > 4.9 && high.second < 0.1);
    const auto low = seeded(2.0, 3.0);
    CHECK(low.first < 0.1 && low.second > 4.9);
}

TEST(newton_alone_can_stop_on_the_balance_point_but_relaxing_never_does) {
    Latch l = crossCoupled();
    const std::vector<double> balanced{5.0, 2.5, 2.5};
    Solver newton(l.circuit);
    CHECK(newton.solveFrom(balanced));
    CHECK_NEAR(newton.volts(l.q), 2.5, 0.01);
    Solver settled(l.circuit);
    const std::vector<double> nudged{5.0, 2.51, 2.49};
    CHECK(settled.relax(nudged));
    CHECK(settled.volts(l.q) > 4.9);
}

// --- capacitors --------------------------------------------------------------------------------------

namespace {

double chargedAfter(double seconds, double stepSize) {
    constexpr double r = 1000.0, capacitance = 1e-6;
    Circuit c;
    const Node top = c.addNode();
    const Node middle = c.addNode();
    c.addSource(top, kGround, 5.0);
    c.addResistor(top, middle, r);
    c.addCapacitor(middle, kGround, capacitance);
    Solver s(std::move(c));
    CHECK(s.solve());
    s.dischargeCapacitors();
    const int steps = static_cast<int>(std::lround(seconds / stepSize));
    for (int i = 0; i < steps; ++i) CHECK(s.step(stepSize));
    return s.volts(middle);
}

}  // namespace

TEST(a_capacitor_is_open_in_the_steady_state) {
    Circuit c;
    const Node top = c.addNode();
    const Node middle = c.addNode();
    c.addSource(top, kGround, 5.0);
    const std::size_t r = c.addResistor(top, middle, 1000.0);
    c.addCapacitor(middle, kGround, 1e-6);
    Solver s(std::move(c));
    CHECK(s.solve());
    CHECK_NEAR(s.volts(middle), 5.0, 1e-3);
    CHECK(std::abs(s.currents(r).a) < 1e-8);
}

TEST(a_capacitor_charges_on_the_exponential_curve) {
    constexpr double tau = 1e-3;
    for (double t : {0.5e-3, 1e-3, 2e-3, 5e-3}) {
        CHECK_NEAR(chargedAfter(t, tau / 1000.0), 5.0 * (1.0 - std::exp(-t / tau)), 2e-3);
    }
}

TEST(making_the_time_step_half_as_big_makes_the_error_half_as_big) {
    constexpr double tau = 1e-3;
    const double exact = 5.0 * (1.0 - std::exp(-1.0));
    const double coarse = std::abs(chargedAfter(tau, tau / 50.0) - exact);
    const double fine = std::abs(chargedAfter(tau, tau / 100.0) - exact);
    CHECK(coarse / fine > 1.9 && coarse / fine < 2.1);
}

TEST(current_out_of_a_capacitor_matches_the_current_through_its_resistor) {
    Circuit c;
    const Node top = c.addNode();
    const Node middle = c.addNode();
    c.addSource(top, kGround, 5.0);
    c.addResistor(top, middle, 1000.0);
    const std::size_t cap = c.addCapacitor(middle, kGround, 1e-6);
    Solver s(std::move(c));
    CHECK(s.solve());
    s.dischargeCapacitors();
    for (int i = 0; i < 300; ++i) {
        CHECK(s.step(1e-5));
        CHECK(worstImbalance(s) < 1e-8);
        CHECK_NEAR(s.currents(cap).a, (5.0 - s.volts(2)) / 1000.0, 1e-8);
    }
}

// The textbook two-transistor flasher: nothing in it says "oscillate", and it does.
TEST(two_transistors_and_two_capacitors_blink_by_themselves) {
    Circuit c;
    const Node vcc = c.addNode();
    const Node c1 = c.addNode();
    const Node c2 = c.addNode();
    const Node b1 = c.addNode();
    const Node b2 = c.addNode();
    c.addSource(vcc, kGround, 5.0);
    c.addResistor(vcc, c1, 1000.0);
    c.addResistor(vcc, c2, 1000.0);
    c.addResistor(vcc, b1, 47000.0);
    c.addResistor(vcc, b2, 50000.0);
    c.addNpn(b1, c1, kGround);
    c.addNpn(b2, c2, kGround);
    c.addCapacitor(c1, b2, 10e-6);
    c.addCapacitor(c2, b1, 10e-6);
    Solver s(std::move(c));
    CHECK(s.solve());
    s.dischargeCapacitors();
    int rises = 0;
    bool wasHigh = s.volts(c1) > 2.5;
    double firstRise = -1.0;
    double secondRise = -1.0;
    constexpr double kStep = 1e-3;
    for (int i = 0; i < 8000; ++i) {
        CHECK(s.step(kStep));
        const bool high = s.volts(c1) > 2.5;
        if (high && !wasHigh) {
            ++rises;
            if (rises == 3) firstRise = i * kStep;
            if (rises == 4) secondRise = i * kStep;
        }
        wasHigh = high;
    }
    CHECK(rises >= 6);
    // A flasher's period is about 1.4 R C for each half, and this one has two nearly equal halves.
    const double expected = 0.69 * (47000.0 + 50000.0) * 10e-6;
    CHECK(firstRise > 0.0 && secondRise > firstRise);
    CHECK_NEAR(secondRise - firstRise, expected, 0.2 * expected);
}

// --- random circuits with everything in them ---------------------------------------------------------

// A junction straight across a battery would carry 10^69 amps, which no real part survives and no floating
// point number can balance against picoamps elsewhere. Those are short circuits: the game shows them as
// burnt parts, and here they're counted apart.
TEST(random_circuits_with_every_kind_of_part_solve_and_obey_kirchhoff) {
    int solved = 0;
    int shorted = 0;
    constexpr int kRounds = 2000;
    for (int round = 0; round < kRounds; ++round) {
        std::mt19937 random(static_cast<std::uint32_t>(round + 100));
        Circuit c;
        std::vector<Node> all{kGround};
        const int nodes = 3 + static_cast<int>(random() % 12);
        for (int i = 0; i < nodes; ++i) all.push_back(c.addNode());
        const auto pick = [&] { return all[random() % all.size()]; };
        std::uniform_real_distribution<double> logOhms(1.0, 6.0);
        c.addSource(all[1], kGround, 5.0);
        for (std::size_t i = 2; i < all.size(); ++i) c.addResistor(all[i], all[random() % i], std::pow(10.0, logOhms(random)));
        const int parts = 2 + static_cast<int>(random() % 15);
        for (int i = 0; i < parts; ++i) {
            const Node a = pick();
            const Node b = pick();
            const Node d = pick();
            switch (random() % 7) {
                case 0: c.addDiode(a, b); break;
                case 1: c.addDiode(a, b, kLedModel); break;
                case 2: c.addNpn(a, b, d); break;
                case 3: c.addPnp(a, b, d); break;
                case 4: c.addNmos(a, b, d); break;
                case 5: c.addPmos(a, b, d); break;
                default:
                    if (a != b) c.addResistor(a, b, std::pow(10.0, logOhms(random)));
                    break;
            }
        }
        Solver s(std::move(c));
        if (!s.solve()) continue;
        ++solved;
        double largest = 0.0;
        for (std::size_t i = 0; i < s.circuit().elements().size(); ++i) {
            const Currents k = s.currents(i);
            largest = std::max({largest, std::abs(k.a), std::abs(k.b), std::abs(k.c)});
        }
        if (largest > 100.0) {
            ++shorted;
            continue;
        }
        const double imbalance = worstRelativeImbalance(s);
        if (imbalance >= 1e-5) std::printf("  seed %d: imbalance %g\n", round + 100, imbalance);
        CHECK(imbalance < 1e-5);
    }
    std::printf("  solved %d of %d, %d of them short circuits\n", solved, kRounds, shorted);
    // These are thrown together at random, and some may have no resting state at all, the way an oscillator
    // has none. Which of the rest SPICE can solve is for tools/compare_spice.py to say.
    CHECK(solved >= kRounds * 98 / 100);
    CHECK(shorted < solved / 2);
}

int main() { return check::runAll(); }
