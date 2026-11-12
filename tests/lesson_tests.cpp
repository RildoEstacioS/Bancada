// Every lesson can be finished: its board starts unsolved, a solution built part by part the way a player
// would build it passes, and the usual mistakes fail with a sentence that says what's wrong.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/boxes.hpp"
#include "editor/circuit.hpp"
#include "editor/course.hpp"
#include "editor/layout.hpp"
#include "editor/lessons.hpp"
#include "editor/truth_table.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

constexpr std::uint64_t kOhm = 1000;

const Lesson& lesson(const char* id) {
    const int index = lessonIndex(id);
    if (index < 0) throw std::logic_error(std::string("no lesson ") + id);
    return lessons()[static_cast<std::size_t>(index)];
}

PartId first(const Board& board, PartKind kind) {
    for (const Part& part : board.parts()) {
        if (part.kind == kind) return part.id;
    }
    throw std::logic_error("no part of that kind");
}

PartId named(const Board& board, const char* label, PartKind kind) {
    for (const Part& part : board.parts()) {
        if (part.label == label && part.kind == kind) return part.id;
    }
    throw std::logic_error(std::string("no part called ") + label);
}

PartId input(const Board& board, const char* label) { return named(board, label, PartKind::Toggle); }

PartId display(const Board& board, const char* label) { return named(board, label, PartKind::Probe); }

// The resistor in front of a real LED. Its free end, pin 0, is where a gate's output goes in; for an LED
// hanging from 5V its other end, pin 1, is the point a transistor pulls down.
PartId ledResistor(const Board& board, const char* label) {
    const PartId light = named(board, label, PartKind::LedBulb);
    for (const Wire& wire : board.wires()) {
        for (const auto& [a, b] : {std::pair{wire.from, wire.to}, std::pair{wire.to, wire.from}}) {
            if (a.kind == Terminal::Kind::Pin && a.id == light && a.pin == 0 && b.kind == Terminal::Kind::Pin &&
                board.part(b.id)->kind == PartKind::Resistor) {
                return b.id;
            }
        }
    }
    throw std::logic_error(std::string("no resistor in front of ") + label);
}

// A lesson's check, with the chips its board is built from when it has any: the computer's CPU.
LessonResult checkWith(const Lesson& chosen, const Board& board) {
    if (chosen.chips == nullptr) return checkLesson(chosen, board);
    const std::vector<ChipData> data = chosen.chips();
    ChipBoards chips;
    for (const ChipData& chip : data) chips[chip.id] = &chip.board;
    return checkLesson(chosen, board, &chips);
}

// The lesson's board with the player's additions, or the reason they couldn't be made.
LessonResult attempt(const char* id, const std::function<void(Board&, Layout&)>& build) {
    const Lesson& chosen = lesson(id);
    Board board = chosen.start();
    try {
        Layout l(board);
        build(board, l);
    } catch (const std::exception& error) {
        return {false, std::string("could not build: ") + error.what()};
    }
    return checkWith(chosen, board);
}

bool passes(const char* id, const std::function<void(Board&, Layout&)>& build) {
    const LessonResult result = attempt(id, build);
    if (!result.passed) std::printf("  %s: %s\n", id, result.message.c_str());
    return result.passed;
}

bool failsSaying(const char* id, const char* words, const std::function<void(Board&, Layout&)>& build) {
    const LessonResult result = attempt(id, build);
    const bool ok = !result.passed && result.message.find(words) != std::string::npos && result.message.find("could not build") == std::string::npos;
    if (!ok) std::printf("  %s: wanted a failure about \"%s\", got: %s\n", id, words, result.message.c_str());
    return ok;
}

void setValue(Board& board, PartId id, std::uint64_t value) {
    Part shape = *board.part(id);
    shape.value = value;
    if (!board.reconfigurePart(id, shape)) throw std::logic_error("could not change the value");
}

}  // namespace

TEST(every_lesson_has_a_board_and_none_starts_solved) {
    CHECK(lessons().size() >= 17);
    for (const Lesson& each : lessons()) {
        Board board;
        try {
            board = each.start();
        } catch (const std::exception& error) {
            std::printf("  %s: %s\n", each.id, error.what());
            CHECK(false);
            continue;
        }
        const LessonResult result = checkWith(each, board);
        if (result.passed) std::printf("  %s starts solved\n", each.id);
        CHECK(!result.passed);
        CHECK(!result.message.empty());
        CHECK(each.steps.size() >= 2);
        CHECK(!each.hints.empty());
        CHECK(each.act >= 0 && each.act < static_cast<int>(lessonActs().size()) && lessonActs()[static_cast<std::size_t>(each.act)].ready);
        if (each.check == LessonCheck::Table && each.expect == nullptr) {
            CHECK_EQ(std::string(each.table).size(), (std::size_t{1} << each.inputs.size()) * each.outputs.size());
        }
        for (const char* key : each.boxes) CHECK(boxFromKey(key) != kNoBox);
    }
}

TEST(lesson_ids_are_unique) {
    for (std::size_t i = 0; i < lessons().size(); ++i) {
        CHECK_EQ(lessonIndex(lessons()[i].id), i);
    }
}

TEST(every_output_from_the_transistors_on_is_a_real_led) {
    for (const Lesson& each : lessons()) {
        if (each.check != LessonCheck::Table) continue;
        const Board board = each.start();
        for (const char* name : each.outputs) {
            bool real = false;
            bool number = false;  // only a number of several bits shows on a display instead
            for (const Part& part : board.parts()) {
                real = real || (part.label == name && part.kind == PartKind::LedBulb);
                number = number || (part.label == name && part.kind == PartKind::Probe && part.width > 1);
            }
            if (!real && !number) std::printf("  %s: %s is not a real LED\n", each.id, name);
            CHECK(real || number);
        }
    }
}

TEST(the_truth_table_shows_real_leds_lit_or_dark) {
    Board board = lesson("porta-e").start();
    Layout l(board);
    const PartId gate = l.gate(PartKind::And, {20, 4});
    l.wire(input(board, "A"), 0u, gate, 0u);
    l.wire(input(board, "B"), 0u, gate, 1u);
    l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    const TruthTable table = computeTruthTable(board);
    CHECK(table.status == TruthTable::Status::Ready);
    CHECK_EQ(table.outputs.size(), 1);
    CHECK(table.outputNames.size() == 1 && table.outputNames[0] == "S");
    CHECK(table.rows == std::vector<std::string>({"0", "0", "0", "1"}));
    CHECK(table.labels.count(named(board, "S", PartKind::LedBulb)) == 1);
}

// --- act 1 --------------------------------------------------------------------------------------------

// The two ways of powering a circuit are both taught, and both are in every lesson's library: the
// battery as it looks, its current going back to its -, and the source whose - is the ground.
TEST(the_first_lesson_starts_with_the_battery_and_the_second_with_source_and_ground) {
    const Board first = lesson("acender-led").start();
    CHECK(first.parts().size() == 1 && first.parts()[0].kind == PartKind::BatteryCell);
    CHECK(failsSaying("acender-led", "Ponha um LED", [](Board&, Layout&) {}));
    const Board second = lesson("fonte-e-terra").start();
    bool source = false;
    bool ground = false;
    for (const Part& part : second.parts()) {
        source = source || part.kind == PartKind::Battery;
        ground = ground || part.kind == PartKind::Ground;
    }
    CHECK(source && ground);
    CHECK_EQ(lessonIndex("fonte-e-terra"), lessonIndex("acender-led") + 1);
    for (const Lesson& each : lessons()) {
        CHECK(lessonAllows(each, PartKind::BatteryCell, kNoBox));
        CHECK(lessonAllows(each, PartKind::Battery, kNoBox));
        CHECK(lessonAllows(each, PartKind::Ground, kNoBox));
    }
}

// A lesson reopened after it changed in a new version of the game starts fresh: its version follows its board.
TEST(a_lesson_version_follows_its_first_board) {
    for (const Lesson& each : lessons()) CHECK_EQ(lessonVersion(each).size(), 8);
    CHECK(lessonVersion(lesson("acender-led")) == lessonVersion(lesson("acender-led")));
    CHECK(lessonVersion(lesson("acender-led")) != lessonVersion(lesson("fonte-e-terra")));
}

namespace {

// The first lesson's circuit, from the battery already there: the resistor if there is one, then the LED,
// the right way round or not, back to the battery's -. No ground anywhere.
void firstCircuit(Board& board, Layout& l, bool resistor, bool forwards) {
    const PartId battery = first(board, PartKind::BatteryCell);
    const PartId light = l.part(PartKind::LedBulb, {16, 2});
    const unsigned in = forwards ? 0u : 1u;
    if (resistor) {
        const PartId limit = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhm);
        l.wire(battery, 0u, limit, 0u);
        l.wire(limit, 1u, light, in);
    } else {
        l.wire(battery, 0u, light, in);
    }
    l.wire(light, 1u - in, battery, 1u);
}

}  // namespace

TEST(an_led_lights_through_a_resistor) {
    CHECK(passes("acender-led", [](Board& board, Layout& l) { firstCircuit(board, l, true, true); }));
}

// The same LED, powered the schematic way: the source's + to the resistor, the LED back through a ground.
TEST(the_source_and_ground_light_the_led_the_same) {
    const auto build = [](bool ownGround) {
        return [ownGround](Board& board, Layout& l) {
            const PartId source = first(board, PartKind::Battery);
            const PartId limit = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhm);
            const PartId light = l.part(PartKind::LedBulb, {16, 2});
            l.wire(source, 0u, limit, 0u);
            l.wire(limit, 1u, light, 0u);
            l.wire(light, 1u, ownGround ? l.part(PartKind::Ground, {22, 8}) : first(board, PartKind::Ground), 0u);
        };
    };
    CHECK(passes("fonte-e-terra", build(true)));
    CHECK(passes("fonte-e-terra", build(false)));
    CHECK(failsSaying("fonte-e-terra", "não acende", [](Board& board, Layout& l) {
        const PartId source = first(board, PartKind::Battery);
        const PartId limit = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhm);
        const PartId light = l.part(PartKind::LedBulb, {16, 2});
        l.wire(source, 0u, limit, 0u);
        l.wire(limit, 1u, light, 0u);  // and the LED's other leg left loose
    }));
}

TEST(an_led_without_a_resistor_burns_and_one_backwards_stays_off) {
    CHECK(failsSaying("acender-led", "queimou", [](Board& board, Layout& l) { firstCircuit(board, l, false, true); }));
    CHECK(failsSaying("acender-led", "não acende", [](Board& board, Layout& l) { firstCircuit(board, l, true, false); }));
}

TEST(the_right_resistor_keeps_the_led_in_range) {
    CHECK(failsSaying("resistor-certo", "queimou", [](Board&, Layout&) {}));
    CHECK(passes("resistor-certo", [](Board& board, Layout&) { setValue(board, first(board, PartKind::Resistor), 330 * kOhm); }));
    CHECK(failsSaying("resistor-certo", "pouco", [](Board& board, Layout&) { setValue(board, first(board, PartKind::Resistor), 1000 * kOhm); }));
    CHECK(failsSaying("resistor-certo", "demais", [](Board& board, Layout&) { setValue(board, first(board, PartKind::Resistor), 100 * kOhm); }));
}

TEST(a_switch_turns_the_led_on_and_off) {
    CHECK(failsSaying("chave", "uma chave", [](Board&, Layout&) {}));
    CHECK(passes("chave", [](Board& board, Layout& l) {
        const PartId toggle = l.part(PartKind::Switch, {4, 4});
        l.wire(first(board, PartKind::BatteryCell), 0u, toggle, 0u);
        l.wire(toggle, 1u, first(board, PartKind::Resistor), 0u);
    }));
}

namespace {

void twoSwitches(Board& board, Layout& l, bool parallel) {
    const PartId battery = first(board, PartKind::BatteryCell);
    const PartId resistor = first(board, PartKind::Resistor);
    const PartId a = l.part(PartKind::Switch, {2, 2});
    const PartId b = l.part(PartKind::Switch, {parallel ? 2 : 8, parallel ? 7 : 2});
    if (parallel) {
        l.wire(battery, 0u, a, 0u);
        l.wire(battery, 0u, b, 0u);
        l.wire(a, 1u, resistor, 0u);
        l.wire(b, 1u, resistor, 0u);
    } else {
        l.wire(battery, 0u, a, 0u);
        l.wire(a, 1u, b, 0u);
        l.wire(b, 1u, resistor, 0u);
    }
}

}  // namespace

TEST(switches_in_series_make_and_and_side_by_side_make_or) {
    CHECK(passes("chaves-e", [](Board& board, Layout& l) { twoSwitches(board, l, false); }));
    CHECK(passes("chaves-ou", [](Board& board, Layout& l) { twoSwitches(board, l, true); }));
    CHECK(failsSaying("chaves-e", "uma chave fechada", [](Board& board, Layout& l) { twoSwitches(board, l, true); }));
    CHECK(failsSaying("chaves-ou", "uma chave fechada", [](Board& board, Layout& l) { twoSwitches(board, l, false); }));
}

// --- act 2 --------------------------------------------------------------------------------------------

TEST(a_transistor_switches_the_led) {
    const auto build = [](bool throughTransistor) {
        return [throughTransistor](Board& board, Layout& l) {
            const PartId transistor = first(board, PartKind::Npn);
            const PartId light = first(board, PartKind::LedBulb);
            const PartId ground = [&] {
                PartId last = 0;
                for (const Part& part : board.parts()) {
                    if (part.kind == PartKind::Ground) last = part.id;  // the one under the transistor
                }
                return last;
            }();
            const PartId supply = [&] {
                for (const Part& part : board.parts()) {
                    if (part.kind == PartKind::Tunnel && part.label == "5V" && part.position.x == 0) return part.id;
                }
                throw std::logic_error("no free 5V");
            }();
            const PartId toggle = l.part(PartKind::Switch, {8, 14});
            const PartId base = l.part(PartKind::Resistor, {15, 14}, 0, 10'000 * kOhm);
            l.wire(supply, 0u, toggle, 0u);
            l.wire(toggle, 1u, base, 0u);
            l.wire(base, 1u, transistor, 0u);
            l.wire(transistor, 2u, ground, 0u);
            if (throughTransistor) {
                l.wire(light, 1u, transistor, 1u);
            } else {
                // The LED straight to ground: it lights, but the transistor does nothing for it.
                const PartId other = l.part(PartKind::Ground, {30, 12});
                l.wire(light, 1u, other, 0u);
            }
        };
    };
    CHECK(passes("transistor-chave", build(true)));
    CHECK(failsSaying("transistor-chave", "", build(false)));
}

namespace {

// A resistor from a signal to a transistor's base.
PartId switchedTransistor(Layout& l, PartId signal, Point at) {
    const PartId base = l.part(PartKind::Resistor, {at.x - 12, at.y + 1}, 0, 10'000 * kOhm);
    const PartId transistor = l.part(PartKind::Npn, at);
    l.wire(signal, 0u, base, 0u);
    l.wire(base, 1u, transistor, 0u);
    return transistor;
}

// The transistor's collector on the point between the LED's resistor and the LED.
void pullsDown(const Board& board, Layout& l, PartId transistor) { l.wire(transistor, 1u, ledResistor(board, "S"), 1u); }

}  // namespace

TEST(one_transistor_makes_a_not) {
    CHECK(passes("inversor", [](Board& board, Layout& l) {
        const PartId transistor = switchedTransistor(l, input(board, "A"), {20, 4});
        const PartId ground = l.part(PartKind::Ground, {22, 12});
        l.wire(transistor, 2u, ground, 0u);
        pullsDown(board, l, transistor);
    }));
    // Without the emitter on ground the transistor has nowhere to send the current: the LED never goes out.
    CHECK(failsSaying("inversor", "devia ficar apagado", [](Board& board, Layout& l) {
        const PartId transistor = switchedTransistor(l, input(board, "A"), {20, 4});
        pullsDown(board, l, transistor);
    }));
    CHECK(failsSaying("inversor", "sem NOT", [](Board& board, Layout& l) {
        const PartId gate = l.gate(PartKind::Not, {20, 2});
        l.wire(input(board, "A"), 0u, gate, 0u);
        l.wire(gate, 1u, ledResistor(board, "S"), 1u);
    }));
}

TEST(two_transistors_in_series_make_a_nand_and_side_by_side_a_nor) {
    CHECK(passes("nand-transistores", [](Board& board, Layout& l) {
        const PartId top = switchedTransistor(l, input(board, "A"), {20, 4});
        const PartId bottom = switchedTransistor(l, input(board, "B"), {20, 14});
        const PartId ground = l.part(PartKind::Ground, {22, 21});
        l.wire(top, 2u, bottom, 1u);
        l.wire(bottom, 2u, ground, 0u);
        pullsDown(board, l, top);
    }));
    CHECK(passes("nor-transistores", [](Board& board, Layout& l) {
        const PartId upper = switchedTransistor(l, input(board, "A"), {20, 4});
        const PartId lower = switchedTransistor(l, input(board, "B"), {20, 14});
        const PartId groundUpper = l.part(PartKind::Ground, {26, 9});
        const PartId groundLower = l.part(PartKind::Ground, {22, 21});
        l.wire(upper, 2u, groundUpper, 0u);
        l.wire(lower, 2u, groundLower, 0u);
        pullsDown(board, l, upper);
        pullsDown(board, l, lower);
    }));
}

// --- act 3 --------------------------------------------------------------------------------------------

TEST(gates_light_the_led_the_way_their_tables_say) {
    CHECK(passes("porta-e", [](Board& board, Layout& l) {
        const PartId gate = l.gate(PartKind::And, {20, 4});
        l.wire(input(board, "A"), 0u, gate, 0u);
        l.wire(input(board, "B"), 0u, gate, 1u);
        l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    }));
    CHECK(failsSaying("porta-e", "devia ficar apagado", [](Board& board, Layout& l) {
        const PartId gate = l.gate(PartKind::Or, {20, 4});
        l.wire(input(board, "A"), 0u, gate, 0u);
        l.wire(input(board, "B"), 0u, gate, 1u);
        l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    }));
}

TEST(nand_makes_not_and_and_or) {
    CHECK(passes("nao-com-nand", [](Board& board, Layout& l) {
        const PartId gate = l.gate(PartKind::Nand, {20, 2});
        l.wire(input(board, "A"), 0u, gate, 0u);
        l.wire(input(board, "A"), 0u, gate, 1u);
        l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    }));
    CHECK(passes("e-com-nand", [](Board& board, Layout& l) {
        const PartId both = l.gate(PartKind::Nand, {12, 4});
        const PartId inverter = l.gate(PartKind::Nand, {22, 4});
        l.wire(input(board, "A"), 0u, both, 0u);
        l.wire(input(board, "B"), 0u, both, 1u);
        l.wire(both, 2u, inverter, 0u);
        l.wire(both, 2u, inverter, 1u);
        l.wire(inverter, 2u, ledResistor(board, "S"), 0u);
    }));
    CHECK(passes("ou-com-nand", [](Board& board, Layout& l) {
        const PartId notA = l.gate(PartKind::Nand, {12, 0});
        const PartId notB = l.gate(PartKind::Nand, {12, 10});
        const PartId both = l.gate(PartKind::Nand, {22, 4});
        l.wire(input(board, "A"), 0u, notA, 0u);
        l.wire(input(board, "A"), 0u, notA, 1u);
        l.wire(input(board, "B"), 0u, notB, 0u);
        l.wire(input(board, "B"), 0u, notB, 1u);
        l.wire(notA, 2u, both, 0u);
        l.wire(notB, 2u, both, 1u);
        l.wire(both, 2u, ledResistor(board, "S"), 0u);
    }));
    CHECK(failsSaying("e-com-nand", "sem AND", [](Board& board, Layout& l) {
        const PartId gate = l.gate(PartKind::And, {20, 4});
        l.wire(input(board, "A"), 0u, gate, 0u);
        l.wire(input(board, "B"), 0u, gate, 1u);
        l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    }));
}

TEST(xor_is_or_and_not_and) {
    CHECK(passes("xor", [](Board& board, Layout& l) {
        const PartId either = l.gate(PartKind::Or, {12, 0});
        const PartId notBoth = l.gate(PartKind::Nand, {12, 10});
        const PartId gate = l.gate(PartKind::And, {22, 4});
        l.wire(input(board, "A"), 0u, either, 0u);
        l.wire(input(board, "B"), 0u, either, 1u);
        l.wire(input(board, "A"), 0u, notBoth, 0u);
        l.wire(input(board, "B"), 0u, notBoth, 1u);
        l.wire(either, 2u, gate, 0u);
        l.wire(notBoth, 2u, gate, 1u);
        l.wire(gate, 2u, ledResistor(board, "S"), 0u);
    }));
}

// --- act 4 --------------------------------------------------------------------------------------------

TEST(gates_add_two_bits_then_three) {
    CHECK(passes("meio-somador", [](Board& board, Layout& l) {
        const PartId sum = l.gate(PartKind::Xor, {20, 0});
        const PartId carry = l.gate(PartKind::And, {20, 10});
        l.wire(input(board, "A"), 0u, sum, 0u);
        l.wire(input(board, "B"), 0u, sum, 1u);
        l.wire(input(board, "A"), 0u, carry, 0u);
        l.wire(input(board, "B"), 0u, carry, 1u);
        l.wire(sum, 2u, ledResistor(board, "S"), 0u);
        l.wire(carry, 2u, ledResistor(board, "C"), 0u);
    }));
    CHECK(passes("somador-completo", [](Board& board, Layout& l) {
        const PartId low = l.box("half_adder", {8, 2});
        const PartId high = l.box("half_adder", {22, 10});
        const PartId carry = l.gate(PartKind::Or, {36, 14});
        l.wire(input(board, "A"), 0u, low, "A");
        l.wire(input(board, "B"), 0u, low, "B");
        l.wire(low, "S", high, "A");
        l.wire(input(board, "Ce"), 0u, high, "B");
        l.wire(high, "S", ledResistor(board, "S"), 0u);
        l.wire(low, "C", carry, 0u);
        l.wire(high, "C", carry, 1u);
        l.wire(carry, 2u, ledResistor(board, "Cs"), 0u);
    }));
}

namespace {

void fourBitAdder(Board& board, Layout& l, bool halfAdderFirst) {
    const PartId splitA = l.bundle(PartKind::Splitter, {10, 0}, 4, 4);
    const PartId splitB = l.bundle(PartKind::Splitter, {10, 12}, 4, 4);
    const PartId join = l.bundle(PartKind::Joiner, {46, 2}, 4, 4);
    l.wire(input(board, "A"), 0u, splitA, 0u);
    l.wire(input(board, "B"), 0u, splitB, 0u);
    PartId carry = 0;
    const char* carryPin = "Cs";
    for (unsigned bit = 0; bit < 4; ++bit) {
        const bool half = halfAdderFirst && bit == 0;
        const PartId adder = l.box(half ? "half_adder" : "full_adder", {22, 22 + static_cast<int>(bit) * 9});
        l.wire(splitA, 1 + bit, adder, "A");
        l.wire(splitB, 1 + bit, adder, "B");
        if (carry != 0) l.wire(carry, carryPin, adder, "Ce");
        l.wire(adder, "S", join, bit);
        carry = adder;
        carryPin = half ? "C" : "Cs";
    }
    l.wire(carry, carryPin, ledResistor(board, "Cs"), 0u);
    l.wire(join, 4u, display(board, "S"), 0u);
}

}  // namespace

TEST(four_full_adders_add_two_four_bit_numbers) {
    // The way the hint says: the lowest adder's carry in left loose, which reads as 0.
    CHECK(passes("somador-4-bits", [](Board& board, Layout& l) { fourBitAdder(board, l, false); }));
    CHECK(passes("somador-4-bits", [](Board& board, Layout& l) { fourBitAdder(board, l, true); }));
}

// --- act 5 --------------------------------------------------------------------------------------------

namespace {

// Two NORs holding each other: S into one, R into the other, and the R one's output is Q. S and R come
// from these pins: a switch's is 0, a gate's output 2.
void srLatch(Board& board, Layout& l, PartId s, PartId r, Point at, unsigned from = 0u) {
    const PartId upper = l.gate(PartKind::Nor, at);
    const PartId lower = l.gate(PartKind::Nor, {at.x, at.y + 10});
    l.wire(s, from, upper, 0u);
    l.wire(r, from, lower, 1u);
    l.wire(upper, 2u, lower, 0u);
    l.wire(lower, 2u, upper, 1u);
    l.wire(lower, 2u, ledResistor(board, "Q"), 0u);
}

PartId clockOf(const Board& board) { return first(board, PartKind::Clock); }

}  // namespace

TEST(two_nors_remember_and_a_d_latch_keeps_its_bit) {
    CHECK(passes("latch-sr", [](Board& board, Layout& l) { srLatch(board, l, input(board, "S"), input(board, "R"), {12, 1}); }));
    // The other NOR's output on the LED shows the opposite.
    CHECK(failsSaying("latch-sr", "No passo 1", [](Board& board, Layout& l) {
        const PartId upper = l.gate(PartKind::Nor, {12, 1});
        const PartId lower = l.gate(PartKind::Nor, {12, 11});
        l.wire(input(board, "S"), 0u, upper, 0u);
        l.wire(input(board, "R"), 0u, lower, 1u);
        l.wire(upper, 2u, lower, 0u);
        l.wire(lower, 2u, upper, 1u);
        l.wire(upper, 2u, ledResistor(board, "Q"), 0u);
    }));
    CHECK(passes("latch-d", [](Board& board, Layout& l) {
        const PartId d = input(board, "D");
        const PartId enable = input(board, "En");
        const PartId notD = l.gate(PartKind::Not, {8, 6});
        const PartId set = l.gate(PartKind::And, {16, 0});
        const PartId reset = l.gate(PartKind::And, {16, 10});
        l.wire(d, 0u, set, 0u);
        l.wire(enable, 0u, set, 1u);
        l.wire(d, 0u, notD, 0u);
        l.wire(notD, 1u, reset, 0u);
        l.wire(enable, 0u, reset, 1u);
        srLatch(board, l, set, reset, {26, 0}, 2u);
    }));
}

TEST(a_flip_flop_a_register_and_a_counter_follow_the_clock) {
    CHECK(passes("flip-flop", [](Board& board, Layout& l) {
        const PartId flop = l.box("d_flip_flop", {10, 3});
        l.wire(input(board, "D"), 0u, flop, "D");
        l.wire(clockOf(board), 0u, flop, "Clk");
        l.wire(flop, "Q", ledResistor(board, "Q"), 0u);
    }));
    // A latch D instead of the flip-flop lets D through without waiting for the clock.
    CHECK(failsSaying("flip-flop", "sem Latch D", [](Board& board, Layout& l) {
        const PartId latch = l.box("d_latch", {10, 3});
        l.wire(input(board, "D"), 0u, latch, "D");
        l.wire(clockOf(board), 0u, latch, "En");
        l.wire(latch, "Q", ledResistor(board, "Q"), 0u);
    }));
    CHECK(passes("registrador", [](Board& board, Layout& l) {
        const PartId keep = l.box("register", {22, 4});
        l.wire(input(board, "Dado"), 0u, keep, "D");
        l.wire(input(board, "Grava"), 0u, keep, "En");
        l.wire(clockOf(board), 0u, keep, "Clk");
        l.wire(keep, "Q", display(board, "S"), 0u);
    }));
    CHECK(passes("contador", [](Board& board, Layout& l) {
        const PartId keep = l.box("register", {30, 4});
        const PartId adder = l.box("adder_n", {14, 14});
        const PartId count = input(board, "Conta");
        l.wire(keep, "Q", adder, "A");
        l.wire(adder, "S", keep, "D");
        l.wire(count, 0u, adder, "Ce");
        l.wire(count, 0u, keep, "En");
        l.wire(clockOf(board), 0u, keep, "Clk");
        l.wire(keep, "Q", display(board, "S"), 0u);
    }));
}

// --- act 6 --------------------------------------------------------------------------------------------

namespace {

// Writes a program into the computer's ROM, as Editar programa does.
void writeProgram(Board& board, const char* source) {
    PartId rom = 0;
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Box && part.box == boxFromKey("rom") && part.width == 16) rom = part.id;
    }
    const assembly::Result assembled = assembly::assemble(source);
    for (const assembly::Error& error : assembled.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    if (rom == 0 || !assembled.errors.empty()) throw std::logic_error("no program");
    Part shape = *board.part(rom);
    shape.rom = std::make_shared<const std::vector<std::uint64_t>>(assembled.words.begin(), assembled.words.end());
    shape.source = source;
    if (!board.reconfigurePart(rom, shape)) throw std::logic_error("the ROM refused the program");
}

std::function<void(Board&, Layout&)> program(const char* source) {
    return [source](Board& board, Layout&) { writeProgram(board, source); };
}

}  // namespace

// The machine code lessons take the words written by hand, _ between the fields or not, and refuse names.
TEST(machine_code_lessons_take_only_words) {
    CHECK(passes("binario-primeiro", program(".word 0b0010_001_000000111\n.word 0b1011_000_001_000000\n.word 0xF000\n")));
    CHECK(failsSaying("binario-primeiro", "binário", program("LI R1, 7\nOUT R1\nHLT\n")));
    CHECK(failsSaying("binario-primeiro", "mostrou 6", program(".word 0b0010_001_000000110\n.word 0b1011_000_001_000000\n.word 0xF000\n")));
    CHECK(passes("binario-soma", program(".word 0x2203\n.word 0x2404\n.word 0x0653\n.word 0b1011_000_011_000000\n.word 0xF000\n")));
    CHECK(passes("binario-laco", program("        .word 0x2203\nlaço:   .word 0xB040 ; OUT R1\n.word 0x127F\n.word 0x723D\n.word 0xF000\n")));
    CHECK(failsSaying("binario-laco", "BNE", program(".word 0x2201\n.word 0xB040\n.word 0xF000\n")));
}

TEST(programs_written_in_the_rom_finish_the_programming_lessons) {
    CHECK(passes("primeiro-programa", program("LI R1, 42\nOUT R1\nHLT\n")));
    CHECK(failsSaying("primeiro-programa", "mostrou 41", program("LI R1, 41\nOUT R1\nHLT\n")));
    CHECK(failsSaying("primeiro-programa", "OUT", program("LI R1, 42\nHLT\n")));
    CHECK(passes("conta-grande", program("LI R1, 300\nLI R2, 212\nADD R3, R1, R2\nOUT R3\nHLT\n")));
    CHECK(passes("conte-ate-cinco", program("LI R1, 1\nLI R2, 6\nlaço: OUT R1\nADDI R1, R1, 1\nBNE R1, R2, laço\nHLT\n")));
    CHECK(failsSaying("conte-ate-cinco", "HLT", program("LI R1, 1\nlaço: OUT R1\nADDI R1, R1, 1\nBNE R1, R0, laço\nHLT\n")));
    CHECK(passes("guarde-na-memoria", program("LI R1, 10\nST R1, 0(R0)\nLI R1, 20\nST R1, 1(R0)\nLI R1, 30\nST R1, 2(R0)\n"
                                              "LD R2, 0(R0)\nLD R3, 1(R0)\nADD R2, R2, R3\nLD R3, 2(R0)\nADD R2, R2, R3\nOUT R2\nHLT\n")));
    CHECK(failsSaying("guarde-na-memoria", "ST", program("LI R1, 60\nOUT R1\nHLT\n")));
    CHECK(passes("sub-rotina", program("LI R1, 3\nCALL dobra\nOUT R1\nCALL dobra\nOUT R1\nCALL dobra\nOUT R1\nHLT\ndobra: ADD R1, R1, R1\nJR R7\n")));
    CHECK(failsSaying("sub-rotina", "CALL", program("LI R1, 6\nOUT R1\nLI R1, 12\nOUT R1\nLI R1, 24\nOUT R1\nHLT\n")));
}

// --- act 7 --------------------------------------------------------------------------------------------

namespace {

constexpr const char* kTasksSolved =
    "; ---- O sistema operacional: não mexa daqui até o rótulo programa ----\n"
    "        JMP  programa\n"
    "; Chamada de sistema: R1 diz o serviço, e R2 leva ou traz o número.\n"
    ";   1: mostra R2   2: lê as chaves em R2   3: encerra\n"
    ";   4: cria a tarefa B, que começa no endereço R2\n"
    ";   5: passa a vez para a outra tarefa\n"
    "sistema: LI   R6, 1\n"
    "        BEQ  R1, R6, mostra\n"
    "        LI   R6, 2\n"
    "        BEQ  R1, R6, leia\n"
    "        LI   R6, 4\n"
    "        BEQ  R1, R6, cria\n"
    "        LI   R6, 5\n"
    "        BEQ  R1, R6, troca\n"
    "        HLT                 ; serviço 3: encerra\n"
    "mostra: OUT  R2\n"
    "        JR   R7\n"
    "leia:   IN   R2\n"
    "        JR   R7\n"
    "; A memória do sistema: no endereço 0, a tarefa que está rodando (0 é a A,\n"
    "; 1 é a B); nos endereços 1 e 2, onde a A e a B pararam.\n"
    "cria:   ST   R2, 2(R0)\n"
    "        JR   R7\n"
    "troca:  LD   R6, 0(R0)\n"
    "        BNE  R6, R0, eraB\n"
    "        ST   R7, 1(R0)      ; a A para aqui...\n"
    "        LI   R6, 1\n"
    "        ST   R6, 0(R0)\n"
    "        LD   R7, 2(R0)      ; ...e a B continua de onde parou\n"
    "        JR   R7\n"
    "eraB:   ST   R7, 2(R0)      ; a B para aqui...\n"
    "        ST   R0, 0(R0)\n"
    "        LD   R7, 1(R0)      ; ...e a A continua de onde parou\n"
    "        JR   R7\n"
    "; ---- O seu programa ----\n"
    "programa: LI  R1, 4         ; cria a tarefa B\n"
    "        LI   R2, tarefa_b\n"
    "        CALL sistema\n"
    "; A tarefa A mostra 1, 2 e 3, passando a vez depois de cada um, e encerra.\n"
    "; Ela usa R3 e R4: a tarefa B não pode mexer neles.\n"
    "tarefa_a: LI  R3, 1\n"
    "        LI   R4, 4\n"
    "volta_a: MOV  R2, R3\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R3, R3, 1\n"
    "        BNE  R3, R4, volta_a\n"
    "        LI   R1, 3          ; encerra\n"
    "        CALL sistema\n"
    "; A tarefa B mostra 10, 20, 30... passando a vez depois de cada um. Ela usa R5.\n"
    "tarefa_b: LI  R5, 10\n"
    "volta_b: MOV  R2, R5\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R5, R5, 10\n"
    "        JMP  volta_b\n";

constexpr const char* kMinimalSolved =
    "; ---- O sistema operacional ----\n"
    "        JMP  programa\n"
    "; Chamada de sistema: R1 diz o serviço, e R2 leva ou traz o número.\n"
    ";   1: mostra R2   2: lê as chaves em R2   3: encerra\n"
    ";   4: cria a tarefa B, que começa no endereço R2\n"
    ";   5: passa a vez para a outra tarefa\n"
    "sistema: LI   R6, 1\n"
    "        BEQ  R1, R6, mostra\n"
    "        LI   R6, 2\n"
    "        BEQ  R1, R6, leia\n"
    "        LI   R6, 4\n"
    "        BEQ  R1, R6, cria\n"
    "        LI   R6, 5\n"
    "        BEQ  R1, R6, troca\n"
    "        HLT                 ; serviço 3: encerra\n"
    "mostra: OUT  R2\n"
    "        JR   R7\n"
    "leia:   IN   R2\n"
    "        JR   R7\n"
    "; A memória do sistema: no endereço 0, a tarefa que está rodando (0 é a A,\n"
    "; 1 é a B); nos endereços 1 e 2, onde a A e a B pararam.\n"
    "cria:   ST   R2, 2(R0)\n"
    "        JR   R7\n"
    "troca:  LD   R6, 0(R0)\n"
    "        BNE  R6, R0, eraB\n"
    "        ST   R7, 1(R0)      ; a A para aqui...\n"
    "        LI   R6, 1\n"
    "        ST   R6, 0(R0)\n"
    "        LD   R7, 2(R0)      ; ...e a B continua de onde parou\n"
    "        JR   R7\n"
    "eraB:   ST   R7, 2(R0)      ; a B para aqui...\n"
    "        ST   R0, 0(R0)\n"
    "        LD   R7, 1(R0)      ; ...e a A continua de onde parou\n"
    "        JR   R7\n"
    "; ---- O seu programa ----\n"
    "programa: LI  R1, 4         ; cria a tarefa B\n"
    "        LI   R2, tarefa_b\n"
    "        CALL sistema\n"
    "; A tarefa A mostra 1, 2 e 3, passando a vez depois de cada um, e encerra.\n"
    "; Ela usa R3 e R4: a tarefa B não pode mexer neles.\n"
    "tarefa_a: LI  R3, 1\n"
    "        LI   R4, 4\n"
    "volta_a: MOV  R2, R3\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R3, R3, 1\n"
    "        BNE  R3, R4, volta_a\n"
    "        LI   R1, 3          ; encerra\n"
    "        CALL sistema\n"
    "; A tarefa B mostra 10, 20, 30... passando a vez depois de cada um. Ela usa R5.\n"
    "tarefa_b: LI  R5, 10\n"
    "volta_b: MOV  R2, R5\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R5, R5, 10\n"
    "        JMP  volta_b\n";

// The lesson's operating system, everything before its label "programa", followed by a program.
std::string systemThen(const char* id, std::string_view program) {
    const std::string_view start(lessons()[static_cast<std::size_t>(lessonIndex(id))].withSystem);
    return std::string(start.substr(0, start.find("\nprograma:") + 1)) + std::string(program);
}

std::function<void(Board&, Layout&)> programText(std::string source) {
    return [source](Board& board, Layout&) { writeProgram(board, source.c_str()); };
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) throw std::logic_error("nothing to replace");
    return text.replace(at, from.size(), to);
}

}  // namespace

TEST(input_and_output_answer_the_switches_and_wait_for_them) {
    CHECK(passes("entrada-e-saida", program("IN R1\nADD R1, R1, R1\nOUT R1\nHLT\n")));
    CHECK(failsSaying("entrada-e-saida", "Com as chaves em 5, a saída mostrou 5", program("IN R1\nOUT R1\nHLT\n")));
    CHECK(failsSaying("entrada-e-saida", "IN", program("LI R1, 10\nOUT R1\nHLT\n")));
    CHECK(passes("esperar-entrada", program("espera: IN R1\nBEQ R1, R0, espera\nADD R1, R1, R1\nOUT R1\nHLT\n")));
    // Reading once, at the start, sees the switches still at 0.
    CHECK(failsSaying("esperar-entrada", "parou antes de alguém mexer nas chaves", program("IN R1\nBEQ R1, R0, fim\nADD R1, R1, R1\nOUT R1\nfim: HLT\n")));
}

TEST(a_program_asks_the_operating_system_and_leaves_it_alone) {
    const std::string asks = systemThen("chamadas-de-sistema",
                                        "programa: LI R1, 2\nCALL sistema\nLI R1, 1\nCALL sistema\nADD R2, R2, R2\nCALL sistema\nLI R1, 3\nCALL sistema\n");
    CHECK(passes("chamadas-de-sistema", programText(asks)));
    CHECK(failsSaying("chamadas-de-sistema", "não pode usar OUT",
                      programText(systemThen("chamadas-de-sistema", "programa: LI R2, 4\nOUT R2\nADD R2, R2, R2\nOUT R2\nLI R1, 3\nCALL sistema\n"))));
    CHECK(failsSaying("chamadas-de-sistema", "não pode usar HLT",
                      programText(systemThen("chamadas-de-sistema", "programa: LI R1, 2\nCALL sistema\nLI R1, 1\nCALL sistema\nHLT\n"))));
    CHECK(failsSaying("chamadas-de-sistema", "O sistema operacional mudou", programText(replaced(asks, "LI   R6, 2", "LI   R6, 3"))));
    CHECK(failsSaying("chamadas-de-sistema", "Com as chaves em 4, a saída mostrou 4",
                      programText(systemThen("chamadas-de-sistema", "programa: LI R1, 2\nCALL sistema\nLI R1, 1\nCALL sistema\nLI R1, 3\nCALL sistema\n"))));
}

TEST(two_tasks_take_turns_and_the_player_writes_the_switch) {
    CHECK(passes("varias-tarefas", program(kTasksSolved)));
    // A task B that doesn't hand over keeps the CPU to itself.
    CHECK(failsSaying("varias-tarefas", "roda para sempre",
                      programText(replaced(kTasksSolved, "        LI   R1, 5          ; passa a vez\n        CALL sistema\n        ADDI R5", "        ADDI R5"))));
    CHECK(passes("sistema-minimo", program(kMinimalSolved)));
    // The switch that always goes back to the same task: B never runs.
    CHECK(failsSaying("sistema-minimo", "A saída mostrou 1, 2 e 3", programText(replaced(kMinimalSolved, "LD   R7, 2(R0)", "LD   R7, 1(R0)"))));
}

// --- act 8 --------------------------------------------------------------------------------------------

TEST(parity_a_multiplexer_and_priority_are_made_of_gates) {
    CHECK(passes("paridade", [](Board& board, Layout& l) {
        const PartId x1 = l.gate(PartKind::Xor, {8, 6});
        const PartId x2 = l.gate(PartKind::Xor, {14, 14});
        const PartId x3 = l.gate(PartKind::Xor, {20, 22});
        l.wire(input(board, "A"), 0u, x1, 0u);
        l.wire(input(board, "B"), 0u, x1, 1u);
        l.wire(x1, 2u, x2, 0u);
        l.wire(input(board, "C"), 0u, x2, 1u);
        l.wire(x2, 2u, x3, 0u);
        l.wire(input(board, "D"), 0u, x3, 1u);
        l.wire(x3, 2u, ledResistor(board, "P"), 0u);
    }));
    CHECK(failsSaying("paridade", "LED P", [](Board& board, Layout& l) {
        const PartId x1 = l.gate(PartKind::Xor, {8, 6});
        l.wire(input(board, "A"), 0u, x1, 0u);
        l.wire(input(board, "B"), 0u, x1, 1u);
        l.wire(x1, 2u, ledResistor(board, "P"), 0u);
    }));
    CHECK(passes("multiplexador", [](Board& board, Layout& l) {
        const PartId notSel = l.gate(PartKind::Not, {8, 20});
        const PartId fromA = l.gate(PartKind::And, {14, 2});
        const PartId fromB = l.gate(PartKind::And, {14, 10});
        const PartId either = l.gate(PartKind::Or, {22, 6});
        l.wire(input(board, "Sel"), 0u, notSel, 0u);
        l.wire(input(board, "A"), 0u, fromA, 0u);
        l.wire(notSel, 1u, fromA, 1u);
        l.wire(input(board, "B"), 0u, fromB, 0u);
        l.wire(input(board, "Sel"), 0u, fromB, 1u);
        l.wire(fromA, 2u, either, 0u);
        l.wire(fromB, 2u, either, 1u);
        l.wire(either, 2u, ledResistor(board, "S"), 0u);
    }));
    CHECK(passes("prioridade", [](Board& board, Layout& l) {
        const PartId high = l.gate(PartKind::Or, {14, 0});
        const PartId notD2 = l.gate(PartKind::Not, {10, 32});
        const PartId one = l.gate(PartKind::And, {20, 32});
        const PartId low = l.gate(PartKind::Or, {30, 10});
        const PartId any = l.add(makePart(PartKind::Or, {30, 20}, 0, 4));
        l.wire(input(board, "D3"), 0u, high, 0u);
        l.wire(input(board, "D2"), 0u, high, 1u);
        l.wire(input(board, "D2"), 0u, notD2, 0u);
        l.wire(notD2, 1u, one, 0u);
        l.wire(input(board, "D1"), 0u, one, 1u);
        l.wire(input(board, "D3"), 0u, low, 0u);
        l.wire(one, 2u, low, 1u);
        for (unsigned i = 0; i < 4; ++i) l.wire(input(board, ("D" + std::to_string(i)).c_str()), 0u, any, i);
        l.wire(high, 2u, ledResistor(board, "A1"), 0u);
        l.wire(low, 2u, ledResistor(board, "A0"), 0u);
        l.wire(any, 4u, ledResistor(board, "V"), 0u);
    }));
}

TEST(negative_numbers_and_multiplication_are_sums) {
    CHECK(passes("negativo", [](Board& board, Layout& l) {
        Part invert = makePart(PartKind::Not, {14, 1}, 0, 1);
        invert.width = 4;
        const PartId notA = l.add(std::move(invert));
        const PartId adder = l.box("adder_n", {24, 2}, 4);
        const PartId one = l.constant(1, {14, 10}, 1);
        l.wire(input(board, "A"), 0u, notA, 0u);
        l.wire(notA, 1u, adder, "A");
        l.wire(one, 0u, adder, "Ce");
        l.wire(adder, "S", display(board, "N"), 0u);
    }));
    // Without the one, it's only NOT A: one less than it should be.
    CHECK(failsSaying("negativo", "N devia ser", [](Board& board, Layout& l) {
        Part invert = makePart(PartKind::Not, {14, 1}, 0, 1);
        invert.width = 4;
        const PartId notA = l.add(std::move(invert));
        l.wire(input(board, "A"), 0u, notA, 0u);
        l.wire(notA, 1u, display(board, "N"), 0u);
    }));
    CHECK(passes("multiplicar", [](Board& board, Layout& l) {
        const PartId splitA = l.bundle(PartKind::Splitter, {8, 1}, 2, 2);
        const PartId splitB = l.bundle(PartKind::Splitter, {8, 11}, 2, 2);
        l.wire(input(board, "A"), 0u, splitA, 0u);
        l.wire(input(board, "B"), 0u, splitB, 0u);
        const auto product = [&](unsigned a, unsigned b, Point at) {
            const PartId gate = l.gate(PartKind::And, at);
            l.wire(splitA, 1 + a, gate, 0u);
            l.wire(splitB, 1 + b, gate, 1u);
            return gate;
        };
        const PartId p0 = product(0, 0, {16, 0});
        const PartId p10 = product(1, 0, {16, 6});
        const PartId p01 = product(0, 1, {16, 12});
        const PartId p11 = product(1, 1, {16, 18});
        const PartId first = l.box("half_adder", {26, 6});
        const PartId second = l.box("half_adder", {26, 16});
        l.wire(p10, 2u, first, "A");
        l.wire(p01, 2u, first, "B");
        l.wire(p11, 2u, second, "A");
        l.wire(first, "C", second, "B");
        const PartId join = l.bundle(PartKind::Joiner, {44, 4}, 4, 4);
        l.wire(p0, 2u, join, 0u);
        l.wire(first, "S", join, 1u);
        l.wire(second, "S", join, 2u);
        l.wire(second, "C", join, 3u);
        l.wire(join, 4u, display(board, "P"), 0u);
    }));
}

// --- input and output -----------------------------------------------------------------------------------

TEST(segments_a_button_and_random_numbers) {
    CHECK(passes("sete-segmentos", [](Board& board, Layout& l) {
        const PartId notB0 = l.gate(PartKind::Not, {8, 14});
        const PartId notB1 = l.gate(PartKind::Not, {8, 22});
        const PartId a = l.gate(PartKind::Or, {16, 4});
        const PartId c = l.gate(PartKind::Or, {16, 20});
        const PartId f = l.gate(PartKind::Nor, {16, 42});
        const PartId one = l.constant(1, {20, 12}, 1);
        l.wire(input(board, "B0"), 0u, notB0, 0u);
        l.wire(input(board, "B1"), 0u, notB1, 0u);
        l.wire(input(board, "B1"), 0u, a, 0u);
        l.wire(notB0, 1u, a, 1u);
        l.wire(notB1, 1u, c, 0u);
        l.wire(input(board, "B0"), 0u, c, 1u);
        l.wire(input(board, "B1"), 0u, f, 0u);
        l.wire(input(board, "B0"), 0u, f, 1u);
        l.wire(a, 2u, ledResistor(board, "a"), 0u);
        l.wire(one, 0u, ledResistor(board, "b"), 0u);
        l.wire(c, 2u, ledResistor(board, "c"), 0u);
        l.wire(a, 2u, ledResistor(board, "d"), 0u);
        l.wire(notB0, 1u, ledResistor(board, "e"), 0u);
        l.wire(f, 2u, ledResistor(board, "f"), 0u);
        l.wire(input(board, "B1"), 0u, ledResistor(board, "g"), 0u);
    }));
    CHECK(passes("contador-botao", [](Board& board, Layout& l) {
        const PartId counter = l.box("counter", {12, 0}, 4);
        const PartId one = l.constant(1, {0, 12}, 1);
        l.wire(named(board, "Botão", PartKind::Button), 0u, counter, "Clk");
        l.wire(one, 0u, counter, "En");
        l.wire(counter, "Q", display(board, "S"), 0u);
    }));
    // Without En on, nothing counts.
    CHECK(failsSaying("contador-botao", "S devia mostrar 1", [](Board& board, Layout& l) {
        const PartId counter = l.box("counter", {12, 0}, 4);
        l.wire(named(board, "Botão", PartKind::Button), 0u, counter, "Clk");
        l.wire(counter, "Q", display(board, "S"), 0u);
    }));
    CHECK(passes("aleatorio", [](Board& board, Layout& l) {
        const PartId keep = l.box("register", {30, 0}, 4);
        const PartId split = l.bundle(PartKind::Splitter, {44, 8}, 4, 4);
        const PartId mix = l.gate(PartKind::Xnor, {10, 14});
        const PartId join = l.bundle(PartKind::Joiner, {20, 2}, 4, 4);
        const PartId one = l.constant(1, {20, 14}, 1);
        l.wire(keep, "Q", split, 0u);
        l.wire(split, 4u, mix, 0u);
        l.wire(split, 3u, mix, 1u);
        l.wire(mix, 2u, join, 0u);
        l.wire(split, 1u, join, 1u);
        l.wire(split, 2u, join, 2u);
        l.wire(split, 3u, join, 3u);
        l.wire(join, 4u, keep, "D");
        l.wire(one, 0u, keep, "En");
        l.wire(first(board, PartKind::Clock), 0u, keep, "Clk");
        l.wire(keep, "Q", display(board, "S"), 0u);
    }));
}

TEST(the_keyboard_writes_on_the_terminal) {
    CHECK(passes("teclado-terminal", [](Board& board, Layout& l) {
        const PartId screen = l.box("terminal", {18, 0});
        const PartId keyboard = named(board, "Teclado", PartKind::Keyboard);
        const PartId one = l.constant(1, {8, 10}, 1);
        l.wire(keyboard, 0u, screen, "Char");
        l.wire(keyboard, 1u, screen, "Clk");
        l.wire(one, 0u, screen, "Write");
        l.wire(screen, "N", display(board, "S"), 0u);
    }));
    CHECK(failsSaying("teclado-terminal", "S devia mostrar 1", [](Board& board, Layout& l) {
        const PartId screen = l.box("terminal", {18, 0});
        const PartId keyboard = named(board, "Teclado", PartKind::Keyboard);
        l.wire(keyboard, 0u, screen, "Char");
        l.wire(keyboard, 1u, screen, "Clk");
        l.wire(screen, "N", display(board, "S"), 0u);
    }));
}

// --- the ready parts, made by hand ---------------------------------------------------------------------

TEST(the_ready_parts_are_made_of_gates_in_their_lessons) {
    CHECK(passes("decodificador", [](Board& board, Layout& l) {
        const PartId not1 = l.gate(PartKind::Not, {8, 36});
        const PartId not0 = l.gate(PartKind::Not, {8, 42});
        l.wire(input(board, "A1"), 0u, not1, 0u);
        l.wire(input(board, "A0"), 0u, not0, 0u);
        const struct {
            const char* name;
            bool high;
            bool low;
        } ys[] = {{"Y0", false, false}, {"Y1", false, true}, {"Y2", true, false}, {"Y3", true, true}};
        for (int i = 0; i < 4; ++i) {
            const PartId gate = l.gate(PartKind::And, {20, 4 + 8 * i});
            if (ys[i].high) l.wire(input(board, "A1"), 0u, gate, 0u); else l.wire(not1, 1u, gate, 0u);
            if (ys[i].low) l.wire(input(board, "A0"), 0u, gate, 1u); else l.wire(not0, 1u, gate, 1u);
            l.wire(gate, 2u, ledResistor(board, ys[i].name), 0u);
        }
    }));
    CHECK(passes("mux4", [](Board& board, Layout& l) {
        const PartId notS1 = l.gate(PartKind::Not, {8, 50});
        const PartId notS0 = l.gate(PartKind::Not, {8, 56});
        l.wire(input(board, "S1"), 0u, notS1, 0u);
        l.wire(input(board, "S0"), 0u, notS0, 0u);
        const PartId any = l.add(makePart(PartKind::Or, {24, 4}, 0, 4));
        for (unsigned i = 0; i < 4; ++i) {
            const PartId gate = l.add(makePart(PartKind::And, {14, 14 + 8 * static_cast<int>(i)}, 0, 3));
            l.wire(input(board, ("D" + std::to_string(i)).c_str()), 0u, gate, 0u);
            if (i & 2u) l.wire(input(board, "S1"), 0u, gate, 1u); else l.wire(notS1, 1u, gate, 1u);
            if (i & 1u) l.wire(input(board, "S0"), 0u, gate, 2u); else l.wire(notS0, 1u, gate, 2u);
            l.wire(gate, 3u, any, i);
        }
        l.wire(any, 4u, ledResistor(board, "Y"), 0u);
    }));
    CHECK(passes("subtrator", [](Board& board, Layout& l) {
        const PartId x1 = l.gate(PartKind::Xor, {8, 4});
        const PartId d = l.gate(PartKind::Xor, {16, 4});
        const PartId notA = l.gate(PartKind::Not, {8, 26});
        const PartId borrowAB = l.gate(PartKind::And, {16, 24});
        const PartId notX = l.gate(PartKind::Not, {8, 32});
        const PartId borrowIn = l.gate(PartKind::And, {16, 32});
        const PartId vs = l.gate(PartKind::Or, {24, 14});
        l.wire(input(board, "A"), 0u, x1, 0u);
        l.wire(input(board, "B"), 0u, x1, 1u);
        l.wire(x1, 2u, d, 0u);
        l.wire(input(board, "Ve"), 0u, d, 1u);
        l.wire(d, 2u, ledResistor(board, "D"), 0u);
        l.wire(input(board, "A"), 0u, notA, 0u);
        l.wire(notA, 1u, borrowAB, 0u);
        l.wire(input(board, "B"), 0u, borrowAB, 1u);
        l.wire(x1, 2u, notX, 0u);
        l.wire(notX, 1u, borrowIn, 0u);
        l.wire(input(board, "Ve"), 0u, borrowIn, 1u);
        l.wire(borrowAB, 2u, vs, 0u);
        l.wire(borrowIn, 2u, vs, 1u);
        l.wire(vs, 2u, ledResistor(board, "Vs"), 0u);
    }));
    CHECK(passes("comparador", [](Board& board, Layout& l) {
        const PartId notB = l.gate(PartKind::Not, {8, 30});
        const PartId notA = l.gate(PartKind::Not, {8, 36});
        const PartId gt = l.gate(PartKind::And, {18, 2});
        const PartId eq = l.gate(PartKind::Xnor, {18, 10});
        const PartId lt = l.gate(PartKind::And, {18, 18});
        l.wire(input(board, "B"), 0u, notB, 0u);
        l.wire(input(board, "A"), 0u, notA, 0u);
        l.wire(input(board, "A"), 0u, gt, 0u);
        l.wire(notB, 1u, gt, 1u);
        l.wire(input(board, "A"), 0u, eq, 0u);
        l.wire(input(board, "B"), 0u, eq, 1u);
        l.wire(notA, 1u, lt, 0u);
        l.wire(input(board, "B"), 0u, lt, 1u);
        l.wire(gt, 2u, ledResistor(board, "A>B"), 0u);
        l.wire(eq, 2u, ledResistor(board, "A=B"), 0u);
        l.wire(lt, 2u, ledResistor(board, "A<B"), 0u);
    }));
    CHECK(failsSaying("comparador", "LED A=B", [](Board& board, Layout& l) {
        const PartId gt = l.gate(PartKind::And, {18, 2});
        l.wire(input(board, "A"), 0u, gt, 0u);
        l.wire(input(board, "B"), 0u, gt, 1u);
        l.wire(gt, 2u, ledResistor(board, "A=B"), 0u);
    }));
    CHECK(passes("ula-1-bit", [](Board& board, Layout& l) {
        const PartId both = l.gate(PartKind::And, {8, 44});
        const PartId either = l.gate(PartKind::Or, {8, 50});
        const PartId differ = l.gate(PartKind::Xor, {8, 56});
        const PartId adder = l.box("full_adder", {6, 64});
        const PartId choose = l.box("mux4", {22, 44});
        const PartId adding = l.gate(PartKind::And, {22, 62});
        const PartId carry = l.gate(PartKind::And, {32, 62});
        for (const PartId gate : {both, either, differ}) {
            l.wire(input(board, "A"), 0u, gate, 0u);
            l.wire(input(board, "B"), 0u, gate, 1u);
        }
        l.wire(input(board, "A"), 0u, adder, "A");
        l.wire(input(board, "B"), 0u, adder, "B");
        l.wire(input(board, "Ce"), 0u, adder, "Ce");
        l.wire(both, 2u, choose, "D0");
        l.wire(either, 2u, choose, "D1");
        l.wire(differ, 2u, choose, "D2");
        l.wire(adder, "S", choose, "D3");
        l.wire(input(board, "Op0"), 0u, choose, "S0");
        l.wire(input(board, "Op1"), 0u, choose, "S1");
        l.wire(input(board, "Op0"), 0u, adding, 0u);
        l.wire(input(board, "Op1"), 0u, adding, 1u);
        l.wire(adder, "Cs", carry, 0u);
        l.wire(adding, 2u, carry, 1u);
        l.wire(choose, "Y", ledResistor(board, "R"), 0u);
        l.wire(carry, 2u, ledResistor(board, "Cs"), 0u);
    }));
}

// The course: every reading names exercises that exist, in its own act, each in one reading only; and
// every lesson of an act that has readings belongs to one of them.
TEST(the_readings_name_their_exercises) {
    std::vector<std::string> named;
    for (const Topic& topic : topics()) {
        CHECK(topicIndex(topic.id) >= 0);
        CHECK(!topic.blocks.empty());
        CHECK(!topic.exercises.empty());
        for (const char* id : topic.exercises) {
            const int lesson = lessonIndex(id);
            CHECK(lesson >= 0);
            if (lesson >= 0) CHECK_EQ(lessons()[static_cast<std::size_t>(lesson)].act, topic.act);
            CHECK(std::find(named.begin(), named.end(), id) == named.end());
            named.emplace_back(id);
            CHECK_EQ(topicOfLesson(lesson), topicIndex(topic.id));
        }
    }
    for (const Topic& topic : topics()) {
        for (const int lesson : actLessons(topic.act)) CHECK(topicOfLesson(lesson) >= 0);
    }
}

// Every figure builds, has something on it, and closes only switches it has; every table is a rectangle.
TEST(the_readings_figures_and_tables_hold_together) {
    for (const Topic& topic : topics()) {
        for (const TheoryBlock& block : topic.blocks) {
            if (block.kind == TheoryBlockKind::Figure) {
                CHECK(block.figure != nullptr);
                CHECK(block.height > 0);
                if (block.figure == nullptr) continue;
                const TheoryFigure figure = block.figure();
                CHECK(!figure.board.parts().empty());
                for (const PartId id : figure.closed) {
                    const Part* part = figure.board.part(id);
                    CHECK(part != nullptr && (part->kind == PartKind::Switch || part->kind == PartKind::Toggle));
                }
            }
            if (block.kind == TheoryBlockKind::Table) {
                const std::string table(block.text);
                std::size_t columns = 0;
                std::size_t start = 0;
                while (start <= table.size()) {
                    std::size_t end = table.find('\n', start);
                    if (end == std::string::npos) end = table.size();
                    const std::string row = table.substr(start, end - start);
                    const auto count = static_cast<std::size_t>(std::count(row.begin(), row.end(), '|')) + 1;
                    if (columns == 0) columns = count;
                    CHECK_EQ(count, columns);
                    start = end + 1;
                }
            }
        }
    }
}

// The first act is open from the start; the next opens when every exercise before it is done, or when the
// player had already finished something in it.
TEST(an_act_opens_when_the_one_before_is_done) {
    std::vector<std::string> finished;
    CHECK(actOpen(0, finished));
    CHECK(!actOpen(1, finished));
    for (const int lesson : actLessons(0)) {
        CHECK(!actOpen(1, finished));
        finished.emplace_back(lessons()[static_cast<std::size_t>(lesson)].id);
    }
    CHECK(actOpen(1, finished));
    CHECK(!actOpen(2, finished));
    CHECK_EQ(actDone(0, finished), static_cast<int>(actLessons(0).size()));
    finished.emplace_back(lessons()[static_cast<std::size_t>(actLessons(3).front())].id);
    CHECK(actOpen(3, finished));
    CHECK(!actOpen(2, finished));
    // A reading done doesn't count as an exercise.
    std::vector<std::string> read{readingId(topics().front())};
    CHECK_EQ(actDone(0, read), 0);
}

int main() { return check::runAll(); }
