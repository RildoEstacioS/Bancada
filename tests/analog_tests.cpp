// Electric parts on real boards: what the player builds, with the logic around it, checked against what
// the circuits are known to do.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/electric_examples.hpp"
#include "editor/layout.hpp"
#include "editor/truth_table.hpp"
#include "electric/units.hpp"

using namespace bancada;
using namespace bancada::editor;
using electric::Quantity;

namespace {

constexpr std::uint64_t kOhm = 1000;

std::string row(const TruthTable& table, std::size_t index) { return table.rows[index]; }

}  // namespace

// --- values as people write them ----------------------------------------------------------------------

TEST(values_are_read_the_way_parts_are_marked) {
    CHECK(electric::parseQuantity("4k7", Quantity::Ohms) == 4'700'000u);
    CHECK(electric::parseQuantity("4,7 k\xCE\xA9", Quantity::Ohms) == 4'700'000u);
    CHECK(electric::parseQuantity("220", Quantity::Ohms) == 220'000u);
    CHECK(electric::parseQuantity("4R7", Quantity::Ohms) == 4'700u);
    CHECK(electric::parseQuantity("1M", Quantity::Ohms) == 1'000'000'000u);
    CHECK(electric::parseQuantity("10u", Quantity::Farads) == 10'000'000u);
    CHECK(electric::parseQuantity("10 \xC2\xB5" "F", Quantity::Farads) == 10'000'000u);
    CHECK(electric::parseQuantity("100n", Quantity::Farads) == 100'000u);
    CHECK(electric::parseQuantity("5V", Quantity::Volts) == 5'000u);
    CHECK(electric::parseQuantity("3.3", Quantity::Volts) == 3'300u);
    CHECK(electric::parseQuantity("9 V", Quantity::Volts) == 9'000u);
}

// A battery, a resistor and an LED in a loop, with no ground symbol: the battery's - is then 0 V.
TEST(a_loop_without_a_ground_measures_from_the_battery_minus) {
    Board board;
    Layout l(board);
    const PartId battery = l.part(PartKind::BatteryCell, {0, 8});
    const PartId resistor = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhm);
    const PartId light = l.part(PartKind::LedBulb, {16, 2});
    l.wire(battery, 0u, resistor, 0u);
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, battery, 1u);
    BoardCircuit circuit(board);
    circuit.settle();
    const std::optional<PartReading> reading = circuit.reading(light);
    CHECK(reading.has_value());
    if (!reading) return;
    CHECK(reading->amps > 0.008 && reading->amps < 0.010);
    CHECK(!reading->burnt);
    const std::optional<double> minus = circuit.pinVolts(battery, 1);
    const std::optional<double> plus = circuit.pinVolts(battery, 0);
    CHECK(minus.has_value() && std::abs(*minus) < 0.01);
    CHECK(plus.has_value() && std::abs(*plus - 5.0) < 0.05);
}

// The value field holds the number alone, with the unit drawn after it: what it shows reads back the same.
TEST(a_value_shown_without_its_unit_reads_back_the_same) {
    CHECK(electric::formatNumber(330'000u, Quantity::Ohms) == "330");
    CHECK(electric::formatNumber(4'700'000u, Quantity::Ohms) == "4,7k");
    CHECK(electric::formatNumber(5'000u, Quantity::Volts) == "5");
    CHECK(std::string(electric::unitSymbol(Quantity::Ohms)) == "\xCE\xA9");
    for (const auto& [value, quantity] : {std::pair{330'000ull, Quantity::Ohms}, std::pair{4'700'000ull, Quantity::Ohms}, std::pair{1'000'000'000ull, Quantity::Ohms},
                                          std::pair{10'000'000ull, Quantity::Farads}, std::pair{100'000ull, Quantity::Farads}, std::pair{9'000ull, Quantity::Volts}}) {
        const std::string shown = electric::formatNumber(value, quantity);
        if (electric::parseQuantity(shown, quantity) != value) std::printf("  %s did not read back\n", shown.c_str());
        CHECK(electric::parseQuantity(shown, quantity) == value);
    }
}

TEST(nonsense_and_values_out_of_range_are_refused) {
    for (const char* bad : {"", "abc", "4k7k", "-5", "0", "2000 V", "1x"}) CHECK(!electric::parseQuantity(bad, Quantity::Volts).has_value());
    CHECK(!electric::parseQuantity("4R7", Quantity::Volts).has_value());
    CHECK(!electric::parseQuantity("5 G", Quantity::Ohms).has_value());
}

TEST(values_are_written_back_with_a_comma_and_the_right_prefix) {
    CHECK(electric::formatQuantity(4'700'000, Quantity::Ohms) == "4,7 k\xCE\xA9");
    CHECK(electric::formatQuantity(330'000, Quantity::Ohms) == "330 \xCE\xA9");
    CHECK(electric::formatQuantity(10'000'000, Quantity::Farads) == "10 \xC2\xB5" "F");
    CHECK(electric::formatQuantity(5'000, Quantity::Volts) == "5 V");
    CHECK(electric::formatSi(0.00432, "A") == "4,32 mA");
    CHECK(electric::formatSi(0.0, "A") == "0 A");
    for (std::uint64_t stored : {1u, 47u, 330'000u, 4'700'000u, 999'000u}) {
        CHECK(electric::parseQuantity(electric::formatQuantity(stored, Quantity::Ohms), Quantity::Ohms) == stored);
    }
}

// --- the first circuit --------------------------------------------------------------------------------

TEST(the_led_lights_only_with_the_switch_closed) {
    LedExample example = buildLedExample();
    BoardCircuit circuit(example.board);
    std::optional<PartReading> light = circuit.reading(example.light);
    CHECK(light.has_value());
    CHECK(light->amps < 1e-9);
    CHECK_EQ(light->light, 0.0);

    circuit.setToggle(example.toggle, 1);
    circuit.settle();
    light = circuit.reading(example.light);
    // 5 V, about 2 V across the LED, the rest across 330 ohms.
    CHECK(light->amps > 0.008 && light->amps < 0.0095);
    CHECK(light->light > 0.8);
    CHECK(!light->burnt);
    const std::optional<PartReading> resistor = circuit.reading(example.resistor);
    CHECK(std::abs(resistor->amps - light->amps) < 1e-9);
    CHECK(std::abs(resistor->volts - 330.0 * resistor->amps) < 1e-6);
}

TEST(an_led_without_its_resistor_burns) {
    LedExample example = buildLedExample();
    Part tiny = *example.board.part(example.resistor);
    tiny.value = 1 * kOhm;
    example.board.reconfigurePart(example.resistor, tiny);
    BoardCircuit circuit(example.board);
    circuit.setToggle(example.toggle, 1);
    circuit.settle();
    CHECK(circuit.reading(example.light)->burnt);
}

TEST(wires_show_their_voltage_and_the_current_adds_up_along_them) {
    LedExample example = buildLedExample();
    BoardCircuit circuit(example.board);
    circuit.setToggle(example.toggle, 1);
    circuit.settle();
    double highest = 0.0;
    for (const Wire& wire : example.board.wires()) {
        const std::optional<double> volts = circuit.wireVolts(wire.id);
        CHECK(volts.has_value());
        if (volts) highest = std::max(highest, *volts);
    }
    CHECK(highest > 4.9 && highest < 5.01);
    // The current out of the battery's + is the current into the LED.
    CHECK(std::abs(circuit.pinAmps(example.light, 0) + circuit.reading(example.light)->amps) < 1e-9);
}

// --- transistors become logic ---------------------------------------------------------------------------

TEST(a_nand_of_two_transistors_has_the_nand_truth_table) {
    NandExample example = buildTransistorNand();
    const TruthTable table = computeTruthTable(example.board);
    CHECK(table.status == TruthTable::Status::Ready);
    CHECK_EQ(table.rows.size(), 4);
    if (table.rows.size() != 4) return;
    CHECK(row(table, 0) == "1");
    CHECK(row(table, 1) == "1");
    CHECK(row(table, 2) == "1");
    CHECK(row(table, 3) == "0");
}

TEST(a_transistor_circuit_works_as_a_chip_in_the_logic) {
    // Inside: a CMOS inverter. Outside: a switch, the chip, and an output.
    Board inside;
    {
        Layout l(inside);
        const PartId a = l.input("A", {0, 4});
        const PartId p = l.part(PartKind::Pmos, {10, 0});
        const PartId n = l.part(PartKind::Nmos, {10, 8});
        const PartId out = l.output("S", {24, 6});
        const PartId ground = l.part(PartKind::Ground, {12, 14});
        l.wire(a, 0u, p, 0u);
        l.wire(a, 0u, n, 0u);
        l.wire(p, 2u, n, 1u);
        l.wire(n, 2u, ground, 0u);
        l.wire(p, 2u, out, 0u);
        const PartId battery = l.part(PartKind::Battery, {20, -8});
        l.wire(battery, 0u, p, 1u);
        const PartId otherGround = l.part(PartKind::Ground, {28, -6});
        l.wire(battery, 1u, otherGround, 0u);
    }
    Board outside;
    Part chipPart = makePart(PartKind::Chip, {10, 0});
    chipPart.chip = 1;
    chipPart.interface = makeChipInterface(inside, "inversor");
    Layout l(outside);
    const PartId a = l.input("A", {0, 1});
    const PartId chip = l.add(chipPart);
    const PartId out = l.output("S", {30, 1});
    l.wire(a, 0u, chip, 0u);
    l.wire(chip, 1u, out, 0u);
    ChipBoards chips{{1, &inside}};
    BoardCircuit circuit(outside, &chips);
    CHECK(circuit.outputOn(out));
    circuit.setToggle(a, 1);
    circuit.settle();
    CHECK(!circuit.outputOn(out));
    CHECK(!circuit.electricUnsettled());
}

TEST(a_latch_of_transistors_remembers) {
    // Two resistor-transistor NOR gates, each feeding the other: the classic set-reset latch.
    Board board;
    Layout l(board);
    const PartId set = l.input("S", {0, 2});
    const PartId reset = l.input("R", {0, 30});
    const auto norGate = [&](int y, const char* name) {
        struct Gate {
            PartId inA, inB, out, q;
        };
        const PartId baseA = l.part(PartKind::Resistor, {8, y}, 0, 10'000 * kOhm);
        const PartId baseB = l.part(PartKind::Resistor, {8, y + 6}, 0, 10'000 * kOhm);
        const PartId q1 = l.part(PartKind::Npn, {16, y - 1});
        const PartId q2 = l.part(PartKind::Npn, {24, y + 5});
        const PartId load = l.part(PartKind::Resistor, {30, y - 8}, 1, 1'000 * kOhm);
        const PartId g1 = l.part(PartKind::Ground, {18, y + 4});
        const PartId g2 = l.part(PartKind::Ground, {26, y + 10});
        const PartId shown = l.output(name, {40, y - 2});
        l.wire(baseA, 1u, q1, 0u);
        l.wire(baseB, 1u, q2, 0u);
        l.wire(q1, 2u, g1, 0u);
        l.wire(q2, 2u, g2, 0u);
        l.wire(load, 1u, q1, 1u);
        l.wire(q2, 1u, q1, 1u);
        l.name(load, 0, "5V", true);
        l.wire(q1, 1u, shown, 0u);
        return Gate{baseA, baseB, q1, shown};
    };
    const auto top = norGate(2, "Q");      // Q = NOR(R, Qbar)
    const auto bottom = norGate(30, "NQ");  // Qbar = NOR(S, Q)
    l.wire(reset, 0u, top.inA, 0u);
    l.wire(set, 0u, bottom.inA, 0u);
    l.wire(bottom.out, 1u, top.inB, 0u);
    l.wire(top.out, 1u, bottom.inB, 0u);
    const PartId battery = l.part(PartKind::Battery, {50, 50});
    l.name(battery, 0, "5V", true);
    const PartId ground = l.part(PartKind::Ground, {56, 53});
    l.wire(battery, 1u, ground, 0u);

    BoardCircuit circuit(board);
    const auto press = [&](PartId which) {
        circuit.setToggle(which, 1);
        circuit.settle();
        circuit.setToggle(which, 0);
        circuit.settle();
    };
    press(set);
    CHECK(circuit.outputOn(top.q));
    CHECK(!circuit.outputOn(bottom.q));
    press(reset);
    CHECK(!circuit.outputOn(top.q));
    CHECK(circuit.outputOn(bottom.q));
    press(set);
    CHECK(circuit.outputOn(top.q));
    CHECK(circuit.hasMemory());
}

// --- time -------------------------------------------------------------------------------------------------

TEST(the_blinker_blinks_by_itself) {
    BlinkerExample example = buildBlinker();
    BoardCircuit circuit(example.board);
    CHECK(circuit.analogStepping());
    int changes = 0;
    bool wasLit = circuit.reading(example.left)->light > 0.5;
    for (int frame = 0; frame < 300; ++frame) {
        circuit.advanceAnalog(1.0 / 60.0);
        const bool lit = circuit.reading(example.left)->light > 0.5;
        if (lit != wasLit) ++changes;
        wasLit = lit;
    }
    std::printf("  the left LED changed %d times in 5 seconds\n", changes);
    CHECK(changes >= 6);
}

TEST(every_example_builds_without_conflicts) {
    const LedExample led = buildLedExample();
    const NandExample nand = buildTransistorNand();
    const BlinkerExample blinker = buildBlinker();
    for (const Board* board : {&led.board, &nand.board, &blinker.board}) {
        BoardCircuit circuit(*board);
        CHECK(!circuit.hasConflict());
        CHECK(!circuit.hasWidthConflict());
        CHECK(!circuit.electricTooBig());
    }
}

int main() { return check::runAll(); }
