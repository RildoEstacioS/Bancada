#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "sim/backend.hpp"
#include "sim/gates.hpp"

using namespace bancada::sim;
namespace g = bancada::sim::gates;

namespace {

// Every test runs on every backend.
using MakeBackend = std::unique_ptr<Backend> (*)(Program);
MakeBackend currentBackend = makeInterpreter;

std::unique_ptr<Backend> run(const Netlist& n) { return currentBackend(compile(n)); }

// Checks a two-input gate against its truth table, listed as out(a=0 b=0), (1 0), (0 1), (1 1).
template <typename Build>
void checkTwoInputs(Build build, std::array<int, 4> table) {
    Netlist n;
    const NetId a = n.input("a");
    const NetId b = n.input("b");
    const NetId out = build(n, a, b);
    auto sim = run(n);
    for (int i = 0; i < 4; ++i) {
        sim->set(a, i & 1);
        sim->set(b, (i >> 1) & 1);
        CHECK(!sim->settle().oscillating);
        CHECK_EQ(sim->get(out), table[static_cast<std::size_t>(i)]);
    }
}

void pulse(Backend& sim, NetId clock) {
    sim.set(clock, 1);
    sim.settle();
    sim.set(clock, 0);
    sim.settle();
}

bool has(const Program& program, Diagnostic::Kind kind) {
    return std::any_of(program.diagnostics.begin(), program.diagnostics.end(),
                       [&](const Diagnostic& d) { return d.kind == kind; });
}

}  // namespace

// --- gates -----------------------------------------------------------------------------------------

TEST(nand_truth_table) { checkTwoInputs([](Netlist& n, NetId a, NetId b) { return n.nand(a, b); }, {1, 1, 1, 0}); }
TEST(and_truth_table) { checkTwoInputs(g::andGate, {0, 0, 0, 1}); }
TEST(or_truth_table) { checkTwoInputs(g::orGate, {0, 1, 1, 1}); }
TEST(nor_truth_table) { checkTwoInputs(g::norGate, {1, 0, 0, 0}); }
TEST(xor_truth_table) { checkTwoInputs(g::xorGate, {0, 1, 1, 0}); }
TEST(xnor_truth_table) { checkTwoInputs(g::xnorGate, {1, 0, 0, 1}); }

TEST(not_truth_table) {
    Netlist n;
    const NetId a = n.input("a");
    const NetId out = g::notGate(n, a);
    auto sim = run(n);
    sim->set(a, 0);
    sim->settle();
    CHECK_EQ(sim->get(out), 1);
    sim->set(a, 1);
    sim->settle();
    CHECK_EQ(sim->get(out), 0);
}

TEST(mux_truth_table) {
    Netlist n;
    const NetId a = n.input("a");
    const NetId b = n.input("b");
    const NetId sel = n.input("sel");
    const NetId out = g::mux(n, a, b, sel);
    auto sim = run(n);
    for (int i = 0; i < 8; ++i) {
        const int va = i & 1, vb = (i >> 1) & 1, vs = (i >> 2) & 1;
        sim->set(a, va);
        sim->set(b, vb);
        sim->set(sel, vs);
        sim->settle();
        CHECK_EQ(sim->get(out), vs ? vb : va);
    }
}

TEST(nand_works_on_whole_buses) {
    Netlist n;
    const NetId a = n.input("a", 8);
    const NetId b = n.input("b", 8);
    const NetId out = n.nand(a, b);
    auto sim = run(n);
    sim->set(a, 0b1100'1010);
    sim->set(b, 0b1010'0110);
    sim->settle();
    CHECK_EQ(sim->get(out), 0b0111'1101);
}

TEST(sixty_four_bit_values_keep_every_bit) {
    Netlist n;
    const NetId a = n.input("a", 64);
    const NetId out = g::notGate(n, a);
    auto sim = run(n);
    sim->set(a, 0x0123'4567'89AB'CDEFull);
    sim->settle();
    CHECK_EQ(sim->get(out), ~0x0123'4567'89AB'CDEFull);
}

TEST(inputs_are_cut_to_their_width) {
    Netlist n;
    const NetId a = n.input("a", 4);
    auto sim = run(n);
    sim->set(a, 0xFF);
    CHECK_EQ(sim->get(a), 0xF);
}

TEST(slice_and_concat_split_and_join_a_bus) {
    Netlist n;
    const NetId a = n.input("a", 16);
    const NetId low = n.slice(a, 0, 8);
    const NetId high = n.slice(a, 8, 8);
    const std::array swapped{high, low};
    const NetId out = n.concat(swapped);
    auto sim = run(n);
    sim->set(a, 0xABCD);
    sim->settle();
    CHECK_EQ(sim->get(low), 0xCD);
    CHECK_EQ(sim->get(high), 0xAB);
    CHECK_EQ(sim->get(out), 0xCDAB);
}

// --- arithmetic ------------------------------------------------------------------------------------

TEST(full_adder_every_input) {
    Netlist n;
    const NetId a = n.input("a");
    const NetId b = n.input("b");
    const NetId c = n.input("c");
    const g::Sum out = g::fullAdder(n, a, b, c);
    auto sim = run(n);
    for (int i = 0; i < 8; ++i) {
        sim->set(a, i & 1);
        sim->set(b, (i >> 1) & 1);
        sim->set(c, (i >> 2) & 1);
        sim->settle();
        const int total = (i & 1) + ((i >> 1) & 1) + ((i >> 2) & 1);
        CHECK_EQ(sim->get(out.sum), total & 1);
        CHECK_EQ(sim->get(out.carry), total >> 1);
    }
}

TEST(adder_8_bit_every_pair) {
    Netlist n;
    const NetId a = n.input("a", 8);
    const NetId b = n.input("b", 8);
    const g::Sum out = g::rippleAdder(n, a, b, n.constant(0));
    auto sim = run(n);
    for (unsigned x = 0; x < 256; ++x) {
        for (unsigned y = 0; y < 256; ++y) {
            sim->set(a, x);
            sim->set(b, y);
            sim->settle();
            CHECK_EQ(sim->get(out.sum), (x + y) & 0xFF);
            CHECK_EQ(sim->get(out.carry), (x + y) >> 8);
        }
    }
}

TEST(adder_16_bit_random_values) {
    Netlist n;
    const NetId a = n.input("a", 16);
    const NetId b = n.input("b", 16);
    const NetId carryIn = n.input("c");
    const g::Sum out = g::rippleAdder(n, a, b, carryIn);
    auto sim = run(n);
    std::mt19937 random(12345);
    for (int i = 0; i < 100000; ++i) {
        const std::uint32_t x = random() & 0xFFFF, y = random() & 0xFFFF, c = random() & 1;
        sim->set(a, x);
        sim->set(b, y);
        sim->set(carryIn, c);
        sim->settle();
        CHECK_EQ(sim->get(out.sum), (x + y + c) & 0xFFFF);
        CHECK_EQ(sim->get(out.carry), (x + y + c) >> 16);
    }
}

// The order parts are placed on a board must not change what the circuit does.
TEST(adder_built_in_shuffled_order_still_adds) {
    constexpr unsigned kBits = 8;
    std::vector<unsigned> order(kBits);
    std::iota(order.begin(), order.end(), 0u);
    std::mt19937 random(99);
    std::shuffle(order.begin(), order.end(), random);

    Netlist n;
    const NetId a = n.input("a", kBits);
    const NetId b = n.input("b", kBits);
    std::vector<NetId> carry(kBits + 1), bits(kBits);
    for (NetId& net : carry) net = n.wire();
    n.drive(carry[0], n.constant(0));
    for (unsigned i : order) {
        const g::Sum bit = g::fullAdder(n, n.slice(a, i, 1), n.slice(b, i, 1), carry[i]);
        bits[i] = bit.sum;
        n.drive(carry[i + 1], bit.carry);
    }
    const NetId sum = n.concat(bits);
    auto sim = run(n);
    for (unsigned x = 0; x < 256; x += 3) {
        for (unsigned y = 0; y < 256; y += 5) {
            sim->set(a, x);
            sim->set(b, y);
            CHECK(!sim->settle().oscillating);
            CHECK_EQ(sim->get(sum), (x + y) & 0xFF);
        }
    }
}

// --- memory from gates -----------------------------------------------------------------------------

TEST(sr_latch_sets_holds_and_resets) {
    Netlist n;
    const NetId notSet = n.input("notSet");
    const NetId notReset = n.input("notReset");
    const g::Latch latch = g::srLatch(n, notSet, notReset);
    auto sim = run(n);

    auto apply = [&](int s, int r) {
        sim->set(notSet, s);
        sim->set(notReset, r);
        CHECK(!sim->settle().oscillating);
    };
    apply(0, 1);
    CHECK_EQ(sim->get(latch.q), 1);
    CHECK_EQ(sim->get(latch.notQ), 0);
    apply(1, 1);
    CHECK_EQ(sim->get(latch.q), 1);
    CHECK_EQ(sim->get(latch.notQ), 0);
    apply(1, 0);
    CHECK_EQ(sim->get(latch.q), 0);
    CHECK_EQ(sim->get(latch.notQ), 1);
    apply(1, 1);
    CHECK_EQ(sim->get(latch.q), 0);
    CHECK_EQ(sim->get(latch.notQ), 1);
}

TEST(d_latch_follows_while_open_and_holds_while_shut) {
    Netlist n;
    const NetId d = n.input("d");
    const NetId enable = n.input("enable");
    const g::Latch latch = g::dLatch(n, d, enable);
    auto sim = run(n);

    sim->set(d, 1);
    sim->set(enable, 1);
    sim->settle();
    CHECK_EQ(sim->get(latch.q), 1);
    sim->set(enable, 0);
    sim->settle();
    sim->set(d, 0);
    sim->settle();
    CHECK_EQ(sim->get(latch.q), 1);
    sim->set(enable, 1);
    sim->settle();
    CHECK_EQ(sim->get(latch.q), 0);
}

TEST(nand_flip_flop_changes_only_on_the_rising_edge) {
    Netlist n;
    const NetId d = n.input("d");
    const NetId clock = n.input("clock");
    const g::Latch ff = g::dFlipFlop(n, d, clock);
    auto sim = run(n);

    // Crossed NANDs wake up in a state nobody chose, so store a known value first.
    pulse(*sim, clock);
    CHECK_EQ(sim->get(ff.q), 0);

    sim->set(d, 1);
    sim->settle();
    CHECK_EQ(sim->get(ff.q), 0);  // the clock hasn't moved
    sim->set(clock, 1);
    sim->settle();
    CHECK_EQ(sim->get(ff.q), 1);
    sim->set(d, 0);
    sim->settle();
    CHECK_EQ(sim->get(ff.q), 1);  // d changed while the clock was high
    sim->set(clock, 0);
    sim->settle();
    CHECK_EQ(sim->get(ff.q), 1);  // the falling edge does nothing
    sim->set(clock, 1);
    sim->settle();
    CHECK_EQ(sim->get(ff.q), 0);
}

TEST(nand_flip_flop_divides_a_clock_by_two) {
    Netlist n;
    const NetId clock = n.input("clock");
    const NetId d = n.wire();
    const g::Latch ff = g::dFlipFlop(n, d, clock);
    n.drive(d, ff.notQ);
    auto sim = run(n);
    sim->settle();
    const std::uint64_t start = sim->get(ff.q);
    for (int i = 1; i <= 16; ++i) {
        sim->set(clock, 1);
        CHECK(!sim->settle().oscillating);
        CHECK_EQ(sim->get(ff.q), (start + static_cast<unsigned>(i)) & 1);
        sim->set(clock, 0);
        CHECK(!sim->settle().oscillating);
        CHECK_EQ(sim->get(ff.q), (start + static_cast<unsigned>(i)) & 1);
    }
}

TEST(nand_ripple_counter_counts) {
    constexpr unsigned kBits = 4;
    Netlist n;
    const NetId clock = n.input("clock");
    std::vector<NetId> bits;
    NetId stageClock = clock;
    for (unsigned i = 0; i < kBits; ++i) {
        const NetId d = n.wire();
        const g::Latch ff = g::dFlipFlop(n, d, stageClock);
        n.drive(d, ff.notQ);
        bits.push_back(ff.q);
        stageClock = ff.notQ;  // the next bit moves when this one falls
    }
    const NetId count = n.concat(bits);
    auto sim = run(n);
    sim->settle();
    const std::uint64_t start = sim->get(count);
    for (unsigned i = 1; i <= 40; ++i) {
        pulse(*sim, clock);
        CHECK_EQ(sim->get(count), (start + i) & 0xF);
    }
}

// --- loops that never rest -------------------------------------------------------------------------

TEST(a_not_feeding_itself_is_reported) {
    Netlist n;
    const NetId loop = n.wire();
    n.drive(loop, g::notGate(n, loop));
    auto sim = run(n);
    CHECK(sim->settle().oscillating);
}

TEST(an_odd_ring_oscillates_and_an_even_ring_does_not) {
    auto ring = [](int length) {
        Netlist n;
        const NetId start = n.wire();
        NetId at = start;
        for (int i = 0; i < length; ++i) at = g::notGate(n, at);
        n.drive(start, at);
        return run(n)->settle().oscillating;
    };
    CHECK(ring(3));
    CHECK(ring(101));
    CHECK(!ring(2));
    CHECK(!ring(100));
}

TEST(a_stable_loop_reports_nothing_after_an_oscillating_one_stops) {
    Netlist n;
    const NetId run_ = n.input("run");
    const NetId loop = n.wire();
    n.drive(loop, n.nand(run_, loop));  // NOT of itself while run is 1, stuck at 1 while run is 0
    auto sim = run(n);
    sim->set(run_, 1);
    CHECK(sim->settle().oscillating);
    sim->set(run_, 0);
    CHECK(!sim->settle().oscillating);
    CHECK_EQ(sim->get(loop), 1);
}

// --- built-in flip-flops ---------------------------------------------------------------------------

TEST(dff_counter_with_a_nand_adder) {
    Netlist n;
    const NetId clock = n.input("clock");
    const NetId next = n.wire(8);
    const NetId count = n.dff(next, clock);
    n.drive(next, g::rippleAdder(n, count, n.constant(1, 8), n.constant(0)).sum);
    auto sim = run(n);
    for (unsigned i = 1; i <= 300; ++i) {
        pulse(*sim, clock);
        CHECK_EQ(sim->get(count), i & 0xFF);
    }
}

TEST(dffs_on_one_clock_move_as_one) {
    Netlist n;
    const NetId clock = n.input("clock");
    const NetId wireA = n.wire(4);
    const NetId wireB = n.wire(4);
    const NetId a = n.dff(wireB, clock, 3);
    const NetId b = n.dff(wireA, clock, 9);
    n.drive(wireA, a);
    n.drive(wireB, b);
    auto sim = run(n);
    pulse(*sim, clock);
    CHECK_EQ(sim->get(a), 9);
    CHECK_EQ(sim->get(b), 3);
    pulse(*sim, clock);
    CHECK_EQ(sim->get(a), 3);
    CHECK_EQ(sim->get(b), 9);
}

TEST(dff_clocked_by_another_dff_moves_in_the_same_settle) {
    Netlist n;
    const NetId clock = n.input("clock");
    const NetId d0 = n.wire();
    const NetId q0 = n.dff(d0, clock);
    const NetId notQ0 = g::notGate(n, q0);
    n.drive(d0, notQ0);
    const NetId d1 = n.wire();
    const NetId q1 = n.dff(d1, notQ0);
    n.drive(d1, g::notGate(n, q1));
    const std::array bits{q0, q1};
    const NetId count = n.concat(bits);
    auto sim = run(n);
    for (unsigned i = 1; i <= 12; ++i) {
        pulse(*sim, clock);
        CHECK_EQ(sim->get(count), i & 3);
    }
}

TEST(dff_that_clocks_itself_forever_is_reported) {
    // One flip-flop moves on the clock's rise, the other on its fall, and each move flips the clock.
    Netlist n;
    const NetId enable = n.input("enable");
    const NetId clock = n.wire();
    const NetId notClock = g::notGate(n, clock);
    const NetId d1 = n.wire();
    const NetId d2 = n.wire();
    const NetId q1 = n.dff(d1, clock);
    const NetId q2 = n.dff(d2, notClock);
    n.drive(d1, g::notGate(n, q1));
    n.drive(d2, g::notGate(n, q2));
    n.drive(clock, g::andGate(n, enable, g::xnorGate(n, q1, q2)));
    auto sim = run(n);
    CHECK(!sim->settle().oscillating);
    sim->set(enable, 1);
    CHECK(sim->settle().oscillating);
}

TEST(reset_restores_the_start) {
    Netlist n;
    const NetId clock = n.input("clock");
    const NetId next = n.wire(4);
    const NetId count = n.dff(next, clock, 5);
    n.drive(next, g::rippleAdder(n, count, n.constant(1, 4), n.constant(0)).sum);
    auto sim = run(n);
    pulse(*sim, clock);
    pulse(*sim, clock);
    CHECK_EQ(sim->get(count), 7);
    sim->reset();
    CHECK_EQ(sim->get(count), 5);
    pulse(*sim, clock);
    CHECK_EQ(sim->get(count), 6);
}

TEST(reset_with_the_clock_high_is_not_an_edge) {
    Netlist n;
    const NetId one = n.constant(1);
    const NetId q = n.dff(n.constant(1), one);
    auto sim = run(n);
    sim->settle();
    CHECK_EQ(sim->get(q), 0);
}

// --- memory ----------------------------------------------------------------------------------------

TEST(memory_reads_at_once_and_writes_on_the_rising_edge) {
    Netlist n;
    const NetId address = n.input("a", 4);
    const NetId data = n.input("d", 8);
    const NetId write = n.input("w");
    const NetId clock = n.input("c");
    const std::uint32_t memory = n.memory(4, 8, {11, 22, 33});
    const NetId out = n.memoryRead(memory, address);
    n.memoryWrite(memory, address, data, write, clock);
    auto sim = run(n);

    sim->settle();
    CHECK_EQ(sim->get(out), 11);
    sim->set(address, 2);
    sim->settle();
    CHECK_EQ(sim->get(out), 33);

    sim->set(data, 99);
    sim->set(write, 0);
    sim->settle();
    pulse(*sim, clock);
    CHECK_EQ(sim->get(out), 33);  // write wasn't on

    sim->set(write, 1);
    sim->settle();
    CHECK_EQ(sim->get(out), 33);  // no edge yet
    sim->set(clock, 1);
    sim->settle();
    CHECK_EQ(sim->get(out), 99);
    sim->set(clock, 0);
    sim->settle();

    sim->set(address, 1);
    sim->settle();
    CHECK_EQ(sim->get(out), 22);
    sim->reset();
    sim->set(address, 2);
    sim->settle();
    CHECK_EQ(sim->get(out), 33);  // reset brings back the initial words
}

TEST(memory_and_flip_flops_on_one_edge_see_the_same_moment) {
    // The write address comes from a counter: the word must land at the old count, not the new one.
    Netlist n;
    const NetId clock = n.input("c");
    const NetId next = n.wire(2);
    const NetId count = n.dff(next, clock);
    n.drive(next, g::rippleAdder(n, count, n.constant(1, 2), n.constant(0)).sum);
    const NetId look = n.input("look", 2);
    const std::uint32_t memory = n.memory(2, 4);
    n.memoryWrite(memory, count, n.constant(7, 4), n.constant(1), clock);
    const NetId out = n.memoryRead(memory, look);
    auto sim = run(n);
    pulse(*sim, clock);
    sim->set(look, 0);
    sim->settle();
    CHECK_EQ(sim->get(out), 7);
    sim->set(look, 1);
    sim->settle();
    CHECK_EQ(sim->get(out), 0);
}

TEST(a_memory_read_can_feed_its_own_address) {
    // A linked list in memory: each word is the address of the next, with a register walking it.
    Netlist n;
    const NetId clock = n.input("c");
    const NetId next = n.wire(2);
    const NetId at = n.dff(next, clock);
    const std::uint32_t memory = n.memory(2, 2, {2, 0, 3, 1});
    n.drive(next, n.memoryRead(memory, at));
    auto sim = run(n);
    const std::array<std::uint64_t, 5> walk{2, 3, 1, 0, 2};
    for (std::uint64_t expected : walk) {
        pulse(*sim, clock);
        CHECK_EQ(sim->get(at), expected);
    }
}

TEST(memory_mistakes_are_refused) {
    auto refused = [](auto action) {
        try {
            action();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    Netlist n;
    const NetId a = n.input("a", 4);
    const NetId bad = n.input("b", 3);
    const NetId bit = n.input("c");
    CHECK(refused([&] { n.memory(0, 8); }));
    CHECK(refused([&] { n.memory(25, 8); }));
    CHECK(refused([&] { n.memory(4, 65); }));
    CHECK(refused([&] { n.memory(1, 8, {1, 2, 3}); }));
    const std::uint32_t memory = n.memory(4, 8);
    CHECK(refused([&] { n.memoryRead(memory, bad); }));
    CHECK(refused([&] { n.memoryRead(99, a); }));
    CHECK(refused([&] { n.memoryWrite(memory, a, a, bit, bit); }));
}

// --- half-built circuits ---------------------------------------------------------------------------

TEST(undriven_wire_reads_0_and_is_reported) {
    Netlist n;
    const NetId floating = n.wire();
    const NetId out = g::notGate(n, floating);
    Program program = compile(n);
    CHECK(has(program, Diagnostic::Kind::Undriven));
    auto sim = currentBackend(std::move(program));
    sim->settle();
    CHECK_EQ(sim->get(floating), 0);
    CHECK_EQ(sim->get(out), 1);
}

TEST(two_drivers_are_reported_and_the_first_wins) {
    Netlist n;
    const NetId wire = n.wire();
    n.drive(wire, n.constant(1));
    n.drive(wire, n.constant(0));
    Program program = compile(n);
    CHECK(has(program, Diagnostic::Kind::MultipleDrivers));
    auto sim = currentBackend(std::move(program));
    sim->settle();
    CHECK_EQ(sim->get(wire), 1);
}

TEST(wires_joined_only_to_each_other_are_reported) {
    Netlist n;
    const NetId a = n.wire();
    const NetId b = n.wire();
    n.drive(a, b);
    n.drive(b, a);
    Program program = compile(n);
    CHECK(has(program, Diagnostic::Kind::WireLoop));
    auto sim = currentBackend(std::move(program));
    CHECK(!sim->settle().oscillating);
    CHECK_EQ(sim->get(a), 0);
    CHECK_EQ(sim->get(b), 0);
}

TEST(mistakes_the_editor_must_prevent_are_refused) {
    auto refused = [](auto action) {
        try {
            action();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    Netlist n;
    const NetId a = n.input("a", 4);
    const NetId b = n.input("b", 8);
    CHECK(refused([&] { n.nand(a, b); }));
    CHECK(refused([&] { n.slice(a, 2, 3); }));
    CHECK(refused([&] { n.input("too wide", 65); }));
    CHECK(refused([&] { n.dff(a, b); }));
    CHECK(refused([&] { n.drive(a, b); }));
    CHECK(refused([&] { n.nand(a, 12345); }));
    auto sim = run(n);
    CHECK(refused([&] { sim->set(12345, 0); }));
}

TEST(only_inputs_can_be_set) {
    Netlist n;
    const NetId a = n.input("a");
    const NetId out = g::notGate(n, a);
    auto sim = run(n);
    bool refused = false;
    try {
        sim->set(out, 1);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    CHECK(refused);
}

// --- scale -----------------------------------------------------------------------------------------

TEST(very_long_chains_compile_and_run) {
    constexpr int kLength = 200000;
    Netlist n;
    const NetId a = n.input("a");
    NetId at = a;
    for (int i = 0; i < kLength; ++i) at = g::notGate(n, at);
    auto sim = run(n);
    sim->set(a, 1);
    sim->settle();
    CHECK_EQ(sim->get(at), 1);  // an even number of NOTs
}

TEST(a_very_long_even_ring_settles) {
    constexpr int kLength = 50000;
    Netlist n;
    const NetId start = n.wire();
    NetId at = start;
    for (int i = 0; i < kLength; ++i) at = g::notGate(n, at);
    n.drive(start, at);
    CHECK(!run(n)->settle().oscillating);
}

TEST(the_same_circuit_always_runs_the_same_way) {
    auto trace = [] {
        Netlist n;
        const NetId notSet = n.input("s");
        const NetId notReset = n.input("r");
        const g::Latch latch = g::srLatch(n, notSet, notReset);
        auto sim = run(n);
        std::vector<std::uint64_t> values;
        std::mt19937 random(7);
        for (int i = 0; i < 1000; ++i) {
            sim->set(notSet, random() & 1);  // both may go 0 and rise together: a real race
            sim->set(notReset, random() & 1);
            sim->settle();
            values.push_back(sim->get(latch.q));
        }
        return values;
    };
    CHECK(trace() == trace());
}

TEST(a_settle_with_nothing_changed_evaluates_nothing) {
    if (currentBackend != makeEventSimulator) return;
    Netlist n;
    const NetId a = n.input("a", 8);
    const NetId clock = n.input("clock");
    const NetId b = g::notGate(n, a);
    const NetId q = n.dff(b, clock);
    auto sim = run(n);
    sim->set(a, 5);
    sim->settle();
    const std::uint64_t before = sim->gateEvaluations();
    for (int i = 0; i < 100; ++i) {
        sim->set(a, 5);
        sim->settle();
    }
    CHECK_EQ(sim->gateEvaluations(), before);
    sim->set(clock, 1);
    sim->settle();
    CHECK_EQ(sim->gateEvaluations(), before);  // only the flip-flop moved, and nothing reads it
    CHECK_EQ(sim->get(q), 250);
}

TEST(an_oscillating_loop_keeps_being_reported_with_nothing_changed) {
    Netlist n;
    const NetId loop = n.wire();
    n.drive(loop, g::notGate(n, loop));
    auto sim = run(n);
    for (int i = 0; i < 5; ++i) CHECK(sim->settle().oscillating);
}

int main() {
    std::puts("--- reference interpreter");
    currentBackend = makeInterpreter;
    int result = check::runAll();
    std::puts("--- event simulator");
    currentBackend = makeEventSimulator;
    result |= check::runAll();
    return result;
}
