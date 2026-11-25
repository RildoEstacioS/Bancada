#include "editor/lessons.hpp"

#include <algorithm>
#include <string_view>
#include <cctype>
#include <bit>
#include <cstdio>
#include <optional>

#include "asm/assembler.hpp"
#include "asm/mips.hpp"
#include "editor/boxes.hpp"
#include "editor/computer16.hpp"
#include "editor/computer32.hpp"
#include "editor/computer_view.hpp"
#include "editor/circuit.hpp"
#include "editor/layout.hpp"
#include "editor/truth_table.hpp"
#include "i18n/i18n.hpp"

namespace bancada::editor {

namespace {

constexpr std::uint64_t kOhm = 1000;  // resistors keep milliohms
constexpr std::uint8_t kUpright = 1;  // a quarter turn: first pin at the top

// --- The boards the lessons start from ----------------------------------------------------------------


// There are two ways to power a circuit, and the lessons show both. The battery as it looks, whose
// current goes out of its + and back into its -, a loop anyone can follow with a finger. And the way
// schematics draw it: a source whose - is the ground, so that every part that needs to get back to it
// just goes to a ground symbol, wherever it is. From the transistors on, the lessons use the second.

// A source labelled 5V with its - on the ground, and one more 5V label to take the supply from anywhere.
void supplyWithLabel(Layout& l, Point battery, Point extra) {
    const PartId source = l.part(PartKind::Battery, battery);
    l.name(source, 0, "5V", true);
    const PartId ground = l.part(PartKind::Ground, {battery.x + 6, battery.y + 3});
    l.wire(source, 1u, ground, 0u);
    Part label = makePart(PartKind::Tunnel, extra);
    label.label = "5V";
    l.add(std::move(label));
}

// The very first lesson starts with the battery, and nothing else.
Board onlyBattery() {
    Board board;
    Layout l(board);
    l.part(PartKind::BatteryCell, {0, 10});
    return board;
}

// The second way: a source whose - already goes to a ground.
Board sourceAndGround() {
    Board board;
    Layout l(board);
    const PartId source = l.part(PartKind::Battery, {0, 10});
    const PartId ground = l.part(PartKind::Ground, {9, 13});
    l.wire(source, 1u, ground, 0u);
    return board;
}

// A complete LED circuit whose resistor lets far too much through.
Board burningLed() {
    Board board;
    Layout l(board);
    const PartId battery = l.part(PartKind::BatteryCell, {0, 10});
    const PartId resistor = l.part(PartKind::Resistor, {8, 0}, 0, 10 * kOhm);
    const PartId light = l.part(PartKind::LedBulb, {16, 0});
    l.wire(battery, 0u, resistor, 0u);
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, battery, 1u);
    return board;
}

// The LED circuit with the + of the battery left free, for switches to go in between.
Board ledWithOpenPlus() {
    Board board;
    Layout l(board);
    const PartId battery = l.part(PartKind::BatteryCell, {0, 14});
    const PartId resistor = l.part(PartKind::Resistor, {14, 0}, 0, 330 * kOhm);
    const PartId light = l.part(PartKind::LedBulb, {22, 0});
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, battery, 1u);
    return board;
}

// An LED hanging from 5V, a transistor and a ground: the transistor still has to be wired in.
Board transistorParts() {
    Board board;
    Layout l(board);
    supplyWithLabel(l, {40, 24}, {0, 14});
    // The LED stands upright under its resistor, dome up, its legs going out to each side.
    const PartId load = l.part(PartKind::Resistor, {26, 0}, kUpright, 330 * kOhm);
    l.name(load, 0, "5V", true);
    const PartId light = l.part(PartKind::LedBulb, {28, 5});
    l.wire(load, 1u, light, 0u);
    l.part(PartKind::Npn, {24, 15});
    l.part(PartKind::Ground, {26, 24});
    return board;
}

PartId namedLight(Layout& l, const char* name, Point at, std::uint8_t rotation = 0) {
    Part light = makePart(PartKind::LedBulb, at, rotation);
    light.label = name;
    return l.add(std::move(light));
}

// A real LED as an output: whatever drives the resistor's free end lights it, through to ground.
void ledOutput(Layout& l, const char* name, Point at) {
    const PartId resistor = l.part(PartKind::Resistor, at, 0, 330 * kOhm);
    const PartId light = namedLight(l, name, {at.x + 7, at.y});
    const PartId ground = l.part(PartKind::Ground, {at.x + 13, at.y + 3});
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, ground, 0u);
}

// A real LED from 5V to ground, lit all the time, until a transistor pulls the point between its
// resistor and itself down to ground: the current then goes the easy way, and the LED goes out.
void ledLoad(Layout& l, const char* name, Point at) {
    const PartId resistor = l.part(PartKind::Resistor, at, kUpright, 330 * kOhm);
    l.name(resistor, 0, "5V", true);
    const PartId light = namedLight(l, name, {at.x + 2, at.y + 5});
    const PartId ground = l.part(PartKind::Ground, {at.x + 5, at.y + 11});
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, ground, 0u);
}

// Named switches on the left, and a real LED for each output on the right.
Board namedSignals(std::initializer_list<const char*> inputs, std::initializer_list<const char*> outputs) {
    Board board;
    Layout l(board);
    int y = 2;
    for (const char* name : inputs) {
        l.input(name, {0, y});
        y += 8;
    }
    y = static_cast<int>(inputs.size()) == 1 ? 3 : 5;
    for (const char* name : outputs) {
        ledOutput(l, name, {30, y});
        y += 8;
    }
    return board;
}

// The transistor lessons: named switches, an LED hanging from 5V, and the battery that gives the 5V.
Board transistorGate(std::initializer_list<const char*> inputs) {
    Board board;
    Layout l(board);
    int y = 4;
    for (const char* name : inputs) {
        l.input(name, {0, y});
        y += 10;
    }
    ledLoad(l, "S", {30, -4});
    const PartId cell = l.part(PartKind::Battery, {44, 18});
    l.name(cell, 0, "5V", true);
    const PartId ground = l.part(PartKind::Ground, {50, 21});
    l.wire(cell, 1u, ground, 0u);
    return board;
}

// --- Bits ---------------------------------------------------------------------------------------------

// Four switches, one for each bit, named by what the bit is worth; and a display for the number they make.
Board fourSwitches() {
    Board board;
    Layout l(board);
    // The bit worth 1 on top, as the joiner takes it: the wires go straight across.
    const char* names[] = {"1", "2", "4", "8"};
    for (int i = 0; i < 4; ++i) l.input(names[i], {0, 2 + 7 * i});
    l.output("N", {44, 12}, 4);  // the number, in a digit
    return board;
}

// A number of four bits coming in on one bus, and a real LED for each of its bits.
Board fourLeds() {
    Board board;
    Layout l(board);
    l.input("N", {0, 12}, 4);
    const char* names[] = {"1", "2", "4", "8"};
    for (int i = 0; i < 4; ++i) ledOutput(l, names[i], {36, 1 + 9 * i});
    return board;
}

Board noBoard() { return Board{}; }

// The calculator: Op and the two numbers' bits on the left, the result's LEDs far on the right, room for gates between.
Board calculatorStart() {
    Board board;
    Layout l(board);
    const char* inputs[] = {"Op", "A1", "A0", "B1", "B0"};
    for (int i = 0; i < 5; ++i) l.input(inputs[i], {0, 2 + 8 * i});
    const char* outputs[] = {"C", "S1", "S0"};
    for (int i = 0; i < 3; ++i) ledOutput(l, outputs[i], {80, 5 + 8 * i});
    return board;
}

// Op, A1, A0, B1, B0 in; C, S1, S0 out: A + B with Op at 0, A - B (A plus B's two's complement) with Op at 1.
std::uint64_t calculator(std::span<const std::uint64_t> in, std::size_t output) {
    const std::uint64_t op = in[0] & 1u;
    const std::uint64_t a = 2 * in[1] + in[2];
    const std::uint64_t b = 2 * in[3] + in[4];
    const std::uint64_t total = a + ((op != 0 ? ~b : b) & 3u) + op;
    return (total >> (2 - output)) & 1u;
}

// The four switches worth 8, 4, 2 and 1, as one number.
std::uint64_t weighed(std::span<const std::uint64_t> in, std::size_t) { return 8 * in[0] + 4 * in[1] + 2 * in[2] + in[3]; }

// The bit of the number each LED is worth: 8, 4, 2, then 1.
std::uint64_t bitOfNumber(std::span<const std::uint64_t> in, std::size_t output) { return (in[0] >> (3 - output)) & 1u; }

Board notTransistor() { return transistorGate({"A"}); }

// The NPN's first lesson again, with an NMOS in the transistor's place.
Board mosParts() {
    Board board;
    Layout l(board);
    supplyWithLabel(l, {40, 24}, {0, 14});
    const PartId load = l.part(PartKind::Resistor, {26, 0}, kUpright, 330 * kOhm);
    l.name(load, 0, "5V", true);
    const PartId light = l.part(PartKind::LedBulb, {28, 5});
    l.wire(load, 1u, light, 0u);
    l.part(PartKind::Nmos, {24, 15});
    l.part(PartKind::Ground, {26, 24});
    return board;
}

// The CMOS inverter's start: the input, the LED it drives, and the 5V with a label to take it from.
Board cmosStart() {
    Board board;
    Layout l(board);
    l.input("A", {0, 8});
    ledOutput(l, "S", {34, 7});
    supplyWithLabel(l, {44, 24}, {14, -4});
    return board;
}
Board twoTransistors() { return transistorGate({"A", "B"}); }
Board gateOneInput() { return namedSignals({"A"}, {"S"}); }
Board gateTwoInputs() { return namedSignals({"A", "B"}, {"S"}); }
Board halfAdder() { return namedSignals({"A", "B"}, {"S", "C"}); }
Board fullAdder() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2});
    l.input("B", {0, 10});
    l.input("Ce", {0, 18});
    ledOutput(l, "S", {42, 1});
    ledOutput(l, "Cs", {42, 9});
    return board;
}

Board fourBitAdder() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2}, 4);
    l.input("B", {0, 12}, 4);
    l.output("S", {56, 4}, 4);  // a number: shown in digits
    ledOutput(l, "Cs", {52, 12});
    return board;
}

std::uint64_t sumOfFour(std::span<const std::uint64_t> in, std::size_t output) {
    const std::uint64_t total = in[0] + in[1];
    return output == 0 ? total & 15u : total >> 4;
}

// --- More parts ----------------------------------------------------------------------------------------

// D0 to D3 after S1 and S0: the one S1 S0 numbers.
std::uint64_t fourWayChoice(std::span<const std::uint64_t> in, std::size_t) { return in[2 + 2 * in[0] + in[1]]; }

// A − B − Ve on one bit: the difference's bit, and the borrow when it went below zero.
std::uint64_t subtractBit(std::span<const std::uint64_t> in, std::size_t output) {
    const int difference = static_cast<int>(in[0]) - static_cast<int>(in[1]) - static_cast<int>(in[2]);
    return output == 0 ? static_cast<std::uint64_t>(difference & 1) : difference < 0 ? 1u : 0u;
}

std::uint64_t compareBits(std::span<const std::uint64_t> in, std::size_t output) {
    if (output == 0) return in[0] > in[1] ? 1u : 0u;
    if (output == 1) return in[0] == in[1] ? 1u : 0u;
    return in[0] < in[1] ? 1u : 0u;
}

// A, B, Ce, Op1, Op0: AND, OR, XOR or the sum, and the sum's carry.
std::uint64_t aluOneBit(std::span<const std::uint64_t> in, std::size_t output) {
    const std::uint64_t a = in[0], b = in[1], carry = in[2], op = 2 * in[3] + in[4];
    const std::uint64_t sum = a + b + carry;
    if (output == 1) return op == 3 ? sum >> 1 : 0u;
    switch (op) {
        case 0: return a & b;
        case 1: return a | b;
        case 2: return a ^ b;
        default: return sum & 1u;
    }
}

std::uint64_t oddParity(std::span<const std::uint64_t> in, std::size_t) { return (in[0] ^ in[1] ^ in[2] ^ in[3]) & 1u; }

// D0 to D3 on, and A1 A0 the number of the highest; V if any is.
std::uint64_t priorityOfFour(std::span<const std::uint64_t> in, std::size_t output) {
    int highest = -1;
    for (int i = 0; i < 4; ++i) {
        if (in[static_cast<std::size_t>(i)] != 0) highest = i;
    }
    if (output == 2) return highest >= 0 ? 1u : 0u;
    if (highest < 0) return 0;
    return output == 0 ? (static_cast<unsigned>(highest) >> 1) & 1u : static_cast<unsigned>(highest) & 1u;
}

std::uint64_t minusOf(std::span<const std::uint64_t> in, std::size_t) { return (16u - in[0]) & 15u; }
std::uint64_t productOf(std::span<const std::uint64_t> in, std::size_t) { return in[0] * in[1]; }
std::uint64_t shiftedOnce(std::span<const std::uint64_t> in, std::size_t) { return in[1] != 0 ? in[0] >> 1 : (in[0] << 1) & 15u; }
std::uint64_t extendedOf(std::span<const std::uint64_t> in, std::size_t) { return (in[0] & 8u) != 0 ? in[0] | 0xF0u : in[0]; }

Board shiftOnceBoard() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2}, 4);
    l.input("Dir", {0, 14});
    l.output("Q", {56, 7}, 4);
    return board;
}

Board extendBoard() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2}, 4);
    l.output("Q", {56, 4}, 8);
    return board;
}

Board parityBoard() { return namedSignals({"A", "B", "C", "D"}, {"P"}); }
Board muxBoard() { return namedSignals({"A", "B", "Sel"}, {"S"}); }
Board priorityBoard() {
    Board board;
    Layout l(board);
    for (int i = 0; i < 4; ++i) l.input(("D" + std::to_string(i)).c_str(), {0, 2 + 8 * i});
    ledOutput(l, "A1", {40, 3});
    ledOutput(l, "A0", {40, 11});
    ledOutput(l, "V", {40, 19});
    return board;
}

Board negativeBoard() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2}, 4);
    l.output("N", {44, 4}, 4);
    return board;
}

Board multiplyBoard() {
    Board board;
    Layout l(board);
    l.input("A", {0, 2}, 2);
    l.input("B", {0, 12}, 2);
    l.output("P", {56, 7}, 4);
    return board;
}

Board segmentsBoard() { return namedSignals({"B1", "B0"}, {"a", "b", "c", "d", "e", "f", "g"}); }

Board buttonBoard() {
    Board board;
    Layout l(board);
    Part key = makePart(PartKind::Button, {0, 4});
    key.label = "Botão";
    l.add(std::move(key));
    l.output("S", {40, 4}, 4);
    return board;
}

Board keyboardBoard() {
    Board board;
    Layout l(board);
    Part keys = makePart(PartKind::Keyboard, {0, 2});
    keys.label = "Teclado";
    l.add(std::move(keys));
    l.output("S", {56, 5}, 6);
    return board;
}

Board shiftBoard() {
    Board board;
    Layout l(board);
    board.addPart(PartKind::Clock, {0, 4});
    l.output("S", {56, 4}, 4);
    return board;
}

// --- Memory: circuits that remember ---------------------------------------------------------------------

Board latchBoard() {
    Board board;
    Layout l(board);
    l.input("S", {0, 2});
    l.input("R", {0, 12});
    ledOutput(l, "Q", {30, 5});
    return board;
}

Board dLatchBoard() {
    Board board;
    Layout l(board);
    l.input("D", {0, 2});
    l.input("En", {0, 12});
    ledOutput(l, "Q", {44, 5});
    return board;
}

Board flipFlopBoard() {
    Board board;
    Layout l(board);
    l.input("D", {0, 2});
    l.part(PartKind::Clock, {0, 10});
    ledOutput(l, "Q", {30, 4});
    return board;
}

Board registerBoard() {
    Board board;
    Layout l(board);
    l.input("Dado", {0, 1}, 8);
    l.input("Grava", {0, 8});
    l.part(PartKind::Clock, {0, 14});
    l.output("S", {44, 6}, 8);
    return board;
}

// D, a clock and the LED Q, with room between them for two latches and a NOT.
Board flipFlopLatchesBoard() {
    Board board;
    Layout l(board);
    l.input("D", {0, 2});
    l.part(PartKind::Clock, {0, 12});
    ledOutput(l, "Q", {54, 4});
    return board;
}

// Four switches, D0 to D3, a clock, and an LED for each bit.
Board fourFlipFlopsBoard() {
    Board board;
    Layout l(board);
    static constexpr const char* kIn[] = {"D0", "D1", "D2", "D3"};
    static constexpr const char* kOut[] = {"Q0", "Q1", "Q2", "Q3"};
    for (int i = 0; i < 4; ++i) {
        l.input(kIn[i], {0, 2 + 9 * i});
        ledOutput(l, kOut[i], {40, 1 + 9 * i});
    }
    l.part(PartKind::Clock, {0, 38});
    return board;
}

// An address, a word and its write switch, a clock, and the display of what's read.
Board ramBoard() {
    Board board;
    Layout l(board);
    l.input("A", {0, 1}, 8);
    l.input("Dado", {0, 8}, 8);
    l.input("Grava", {0, 15});
    l.part(PartKind::Clock, {0, 21});
    l.output("S", {50, 8}, 8);
    return board;
}

// The register file's inputs as switches and its two outputs as displays.
Board registerFileBoard() {
    Board board;
    Layout l(board);
    l.input("Ra", {0, 0}, 3);
    l.input("Rb", {0, 6}, 3);
    l.input("Rw", {0, 12}, 3);
    l.input("Dado", {0, 18}, 16);
    l.input("Grava", {0, 24});
    l.part(PartKind::Clock, {0, 30});
    l.output("A", {56, 6}, 16);
    l.output("B", {56, 18}, 16);
    return board;
}

Board counterBoard() {
    Board board;
    Layout l(board);
    l.input("Conta", {0, 2});
    l.part(PartKind::Clock, {0, 10});
    l.output("S", {50, 6}, 8);
    return board;
}

// --- Programming the computer -----------------------------------------------------------------------------

constexpr const char* kStarterProgram =
    "; Escreva o seu programa aqui.\n"
    "; Ctrl+Enter monta e grava na ROM.\n"
    "\n"
    "        HLT\n";

Board computerStart() {
    const assembly::Result program = assembly::assemble(kStarterProgram);
    return buildComputer16(program.words, kStarterProgram).board;
}

std::vector<ChipData> computerChips() { return buildCpuChips(1); }

// --- Programming the 32-bit computer ----------------------------------------------------------------------

constexpr const char* kMipsStarter =
    "# Escreva o seu programa aqui.\n"
    "# Ctrl+Enter monta e grava na ROM.\n"
    "        .text\n"
    "main:   li   $v0, 10       # termina\n"
    "        syscall\n";

constexpr const char* kMipsArray =
    "# Some as palavras do vetor e mostre a soma.\n"
    "        .data\n"
    "vetor:  .word 3, 5, 7, 9\n"
    "n:      .word 4\n"
    "\n"
    "        .text\n"
    "main:   la   $t0, vetor    # o endereço da primeira palavra\n"
    "        lw   $t1, n        # quantas faltam\n"
    "        li   $t2, 0        # a soma\n"
    "\n"
    "        li   $v0, 10       # termina\n"
    "        syscall\n";

Board mipsBoard(const char* source) { return buildComputer32(mips::assemble(source), source).board; }
Board mipsStart() { return mipsBoard(kMipsStarter); }
Board mipsArrayStart() { return mipsBoard(kMipsArray); }
std::vector<ChipData> mipsChips() { return buildComputer32(mips::Program{}, "").chips; }

// --- Machine code: the programs of Act 6, to be written as numbers -----------------------------------

constexpr const char* kMachineFirst =
    "; Nesta aula o programa vai direto em binário: uma palavra .word por instrução.\n"
    "; Em assembly, o programa seria:\n"
    ";     LI  R1, 7    ; LI é o op 2:   op(4) rd(3) imm9(9)\n"
    ";     OUT R1       ; OUT é o op 11: op(4) ---(3) rs(3) ------(6)\n"
    ";     HLT          ; HLT é o op 15, e o resto fica em 0\n"
    "; O _ só separa os campos, para ler melhor. Troque cada palavra pela certa:\n"
    "        .word 0b0000_000_000000000    ; LI  R1, 7\n"
    "        .word 0b0000_000_000_000000   ; OUT R1\n"
    "        .word 0b0000_000000000000     ; HLT\n";

constexpr const char* kMachineSum =
    "; Uma conta na ULA, em binário. Em assembly:\n"
    ";     LI  R1, 3\n"
    ";     LI  R2, 4\n"
    ";     ADD R3, R1, R2   ; op 0: op(4) rd(3) rs(3) rt(3) func(3); ADD é a func 3\n"
    ";     OUT R3\n"
    ";     HLT\n"
    "        .word 0b0000_000_000000000    ; LI  R1, 3\n"
    "        .word 0b0000_000_000000000    ; LI  R2, 4\n"
    "        .word 0b0000_000_000_000_000  ; ADD R3, R1, R2\n"
    "        .word 0b0000_000_000_000000   ; OUT R3\n"
    "        .word 0b0000_000000000000     ; HLT\n";

constexpr const char* kMachineLoop =
    "; Um laço que mostra 3, 2 e 1, em binário. Em assembly:\n"
    ";       LI   R1, 3\n"
    "; laço: OUT  R1\n"
    ";       ADDI R1, R1, -1    ; op 1: op(4) rd(3) rs(3) imm6(6)\n"
    ";       BNE  R1, R0, laço  ; op 7, o mesmo formato: imm6 é a distância\n"
    ";       HLT\n"
    "; A distância do BNE: endereço de laço - (endereço do BNE + 1).\n"
    "; Um imm6 negativo vai em complemento de dois: -1 é 111111.\n"
    "        .word 0b0000_000_000000000    ; LI   R1, 3          (endereço 0)\n"
    "        .word 0b0000_000_000_000000   ; OUT  R1             (endereço 1)\n"
    "        .word 0b0000_000_000_000000   ; ADDI R1, R1, -1     (endereço 2)\n"
    "        .word 0b0000_000_000_000000   ; BNE  R1, R0, laço   (endereço 3)\n"
    "        .word 0b0000_000000000000     ; HLT                 (endereço 4)\n";

Board machineBoard(const char* source) { return buildComputer16(assembly::assemble(source).words, source).board; }
Board machineFirstStart() { return machineBoard(kMachineFirst); }
Board machineSumStart() { return machineBoard(kMachineSum); }
Board machineLoopStart() { return machineBoard(kMachineLoop); }

// --- The operating system ---------------------------------------------------------------------------
// A small system at the start of the ROM: it jumps to the player's program, and serves it through
// system calls, a CALL to its label with the service's number in R1. The second one also keeps two tasks
// and takes turns between them, the way a real system shares one CPU.

constexpr const char* kCallsProgram =
    "; ---- O sistema operacional: não mexa daqui até o rótulo programa ----\n"
    "; Ao ligar, o sistema passa a vez ao programa.\n"
    "        JMP  programa\n"
    "; Chamada de sistema: R1 diz o serviço, e R2 leva ou traz o número.\n"
    ";   1: mostra R2 na saída   2: lê as chaves em R2   3: encerra\n"
    "sistema: LI   R6, 1\n"
    "        BEQ  R1, R6, mostra\n"
    "        LI   R6, 2\n"
    "        BEQ  R1, R6, leia\n"
    "        HLT                 ; serviço 3: encerra\n"
    "mostra: OUT  R2\n"
    "        JR   R7\n"
    "leia:   IN   R2\n"
    "        JR   R7\n"
    "; ---- O seu programa ----\n"
    "programa: LI  R1, 3         ; por enquanto, só pede ao sistema para encerrar\n"
    "        CALL sistema\n";

constexpr const char* kTasksProgram =
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
    "; A tarefa B: escreva aqui. Ela mostra 10, 20 e 30, passando a vez depois de cada um.\n"
    "tarefa_b: LI  R1, 5         ; por enquanto, só passa a vez\n"
    "        CALL sistema\n"
    "        JMP  tarefa_b\n";

constexpr const char* kMinimalSystemProgram =
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
    "troca:  ; escreva aqui a troca de tarefas\n"
    "        HLT\n"
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

Board programStart(const char* source) {
    const assembly::Result program = assembly::assemble(source);
    return buildComputer16(program.words, source).board;
}

Board callsStart() { return programStart(kCallsProgram); }
Board tasksStart() { return programStart(kTasksProgram); }
Board minimalSystemStart() { return programStart(kMinimalSystemProgram); }

// --- The lessons --------------------------------------------------------------------------------------

const std::vector<PartKind> kElectric{PartKind::Battery, PartKind::Ground, PartKind::BatteryCell, PartKind::Resistor, PartKind::LedBulb, PartKind::Tunnel};
const std::vector<PartKind> kLogic{PartKind::And, PartKind::Or, PartKind::Not};
const std::vector<PartKind> kGates{PartKind::And, PartKind::Or, PartKind::Not, PartKind::Nand, PartKind::Nor, PartKind::Xor};

std::vector<PartKind> with(std::vector<PartKind> base, std::initializer_list<PartKind> more) {
    base.insert(base.end(), more);
    return base;
}

std::vector<Lesson> makeLessons() {
    std::vector<Lesson> all;

    // Act 1: electricity.
    {
        Lesson l;
        l.id = "acender-led";
        l.act = 0;
        l.title = "Acenda um LED";
        l.story = "Tudo começa com uma bateria: ela empurra a corrente para fora pelo + e a recebe de volta pelo -. Um LED é uma luzinha que só "
                  "acende quando a corrente passa por ele no sentido certo: entra pela perna do + e sai pela outra.";
        l.steps = {"A bateria já está na placa. Arraste um resistor e um LED da biblioteca.",
                   "Ligue o + da bateria ao resistor, e o resistor à perna do + do LED.", "Ligue a outra perna do LED de volta ao - da bateria."};
        l.hints = {"O + da bateria é a ponta prateada, com o biquinho e o sinal de mais.", "O LED também tem um +: a perna marcada com o sinal de mais.",
                   "Sem o resistor a corrente fica grande demais e o LED queima. Ele vai entre a bateria e o LED.",
                   "Passe o mouse em cima do LED para ver a corrente que passa por ele."};
        l.success = "Acendeu! A corrente sai do + da bateria, passa pelo resistor e pelo LED e volta ao -. Um caminho fechado assim é um circuito.";
        l.start = onlyBattery;
        l.parts = {PartKind::Resistor, PartKind::LedBulb, PartKind::BatteryCell};
        l.check = LessonCheck::LightOn;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "fonte-e-terra";
        l.act = 0;
        l.title = "A fonte e o terra";
        l.story = "Os esquemas desenham a bateria de outro jeito: uma fonte, com traços de tamanhos diferentes. O - dela vai num terra, o símbolo "
                  "das três linhas. Todo terra é o mesmo ponto: em vez de puxar um fio comprido de volta até o -, basta ligar num terra.";
        l.steps = {"Arraste um resistor e um LED, e ligue o + da fonte ao resistor e o resistor à perna do + do LED.",
                   "Arraste um terra para perto do LED.", "Ligue a outra perna do LED nesse terra: o caminho fecha pelo terra até o - da fonte."};
        l.hints = {"O + da fonte é o lado do traço comprido, com o sinal de mais.", "Pode haver quantos terras quiser: são todos o mesmo ponto.",
                   "É o mesmo circuito da aula anterior, desenhado do jeito dos esquemas."};
        l.success = "Acendeu do mesmo jeito! A bateria e a fonte com terra são duas formas de desenhar a mesma coisa. Os circuitos grandes usam o terra.";
        l.start = sourceAndGround;
        l.parts = {PartKind::Resistor, PartKind::LedBulb, PartKind::Ground, PartKind::Battery};
        l.check = LessonCheck::LightOn;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "lei-de-ohm";
        l.act = 0;
        l.title = "Contas com a lei de Ohm";
        l.story = "Sem montar nada desta vez: contas com tensão, corrente e resistência. Antes de escolher o resistor do LED, vale treinar a conta que diz quanto de corrente passa.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"I = V ÷ R, com V em volts e R em ohms, dá I em ampères. Vezes 1000, em miliampères.", "No circuito do LED, o resistor fica com a tensão que sobra: 5 V - 2 V."};
        l.success = "A lei de Ohm é a conta mais usada da eletrônica, e agora você a faz de cabeça.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Um resistor de 100 Ω tem 5 V sobre ele. Qual a corrente, em mA?", "", 10, "50", "I = 5 ÷ 100 = 0,05 A = 50 mA.", "I = V ÷ R, e depois vezes 1000 para passar a miliampères."},
            {"A bateria dá 5 V e o LED aceso fica com 2 V. Quantos volts sobram para o resistor?", "", 10, "3", "5 - 2 = 3 V.", "A tensão da bateria se divide entre o LED e o resistor."},
            {"Com esses 3 V num resistor de 300 Ω, qual a corrente, em mA?", "", 10, "10", "I = 3 ÷ 300 = 0,01 A = 10 mA: o LED acende bem.", "I = V ÷ R, e depois vezes 1000."},
            {"Que resistor, em ohms, deixa passar 15 mA com 3 V sobre ele?", "", 10, "200", "R = 3 ÷ 0,015 = 200 Ω.", "R = V ÷ I, com I em ampères: 15 mA = 0,015 A."},
            {"Quantos ohms tem um resistor de 2,2 kΩ?", "", 10, "2200", "k quer dizer mil: 2,2 × 1000 = 2200 Ω.", "1 kΩ são 1000 Ω."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "resistor-certo";
        l.act = 0;
        l.title = "O resistor certo";
        l.story = "Este LED está queimando: o resistor de 10 Ω deixa passar corrente demais. Quem decide a corrente é o resistor, e a lei de Ohm "
                  "diz quanto: corrente = tensão ÷ resistência.";
        l.steps = {"Selecione o resistor e use Editar valor para trocar a resistência.",
                   "Deixe a corrente do LED entre 5 e 15 mA.",
                   "O LED fica com uns 2 V, então sobram 3 V para o resistor: 3 V ÷ 0,01 A = 300 Ω."};
        l.hints = {"Passe o mouse no LED para ver quantos mA passam por ele.", "Escreva 330 no valor do resistor: 330 Ω é um valor que existe nas lojas.",
                   "1k (1000 Ω) deixa passar só uns 3 mA: pouco. 100 Ω deixa uns 30 mA: demais."};
        l.success = "Na medida! Com mais ohms passa menos corrente. Todo LED de verdade tem um resistor assim do lado.";
        l.start = burningLed;
        l.parts = {PartKind::Resistor};
        l.check = LessonCheck::LightCurrent;
        l.minMilliamps = 5.0;
        l.maxMilliamps = 15.0;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "chave";
        l.act = 0;
        l.title = "Liga e desliga";
        l.story = "Uma chave abre ou fecha o caminho da corrente. Fechada, a corrente passa e o LED acende. Aberta, o caminho fica cortado e o LED apaga.";
        l.steps = {"Arraste uma chave para a placa.", "Ligue a chave entre o + da bateria e o resistor.",
                   "Clique na chave para abrir e fechar: o LED tem que obedecer."};
        l.hints = {"A chave tem dois lados: um vai no +, o outro no resistor.", "Fechada, a chave é como um pedaço de fio."};
        l.success = "Isso! A chave decide se a corrente passa. Um interruptor de parede é exatamente isso.";
        l.start = ledWithOpenPlus;
        l.parts = {PartKind::Switch};
        l.check = LessonCheck::LightSwitches;
        l.switches = 1;
        l.lit = "01";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "chaves-e";
        l.act = 0;
        l.title = "Duas chaves: E";
        l.story = "Com duas chaves uma depois da outra, em série, a corrente só passa se as duas estiverem fechadas. "
                  "É a ideia da porta E (AND): a saída só liga se A E B estiverem ligadas.";
        l.steps = {"Arraste duas chaves.", "Ligue o + da bateria à primeira chave, a primeira à segunda, e a segunda ao resistor.",
                   "Teste: o LED só pode acender com as duas fechadas."};
        l.hints = {"Em série quer dizer em fila: a corrente atravessa uma chave e depois a outra."};
        l.success = "Você fez uma porta E com as mãos: uma chave E a outra. Logo os transistores vão fazer isso sozinhos.";
        l.start = ledWithOpenPlus;
        l.parts = {PartKind::Switch};
        l.check = LessonCheck::LightSwitches;
        l.switches = 2;
        l.lit = "001";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "chaves-ou";
        l.act = 0;
        l.title = "Duas chaves: OU";
        l.story = "Lado a lado, em paralelo, cada chave abre um caminho só dela. Basta uma fechada para o LED acender. Essa é a porta OU (OR).";
        l.steps = {"Arraste duas chaves.", "Ligue um lado das duas no + da bateria, e o outro lado das duas no resistor.",
                   "Teste: qualquer chave fechada acende o LED."};
        l.hints = {"Um fio pode sair de um pino e se dividir: clique no meio de um fio para tirar outro dele.",
                   "Em paralelo quer dizer lado a lado: as duas chaves ligam os mesmos dois pontos."};
        l.success = "Uma OU a outra: qualquer caminho fechado basta. Com E e OU já dá para pensar em lógica.";
        l.start = ledWithOpenPlus;
        l.parts = {PartKind::Switch};
        l.check = LessonCheck::LightSwitches;
        l.switches = 2;
        l.lit = "011";
        all.push_back(l);
    }

    // Act 2: transistors.
    {
        Lesson l;
        l.id = "transistor-chave";
        l.act = 1;
        l.title = "O transistor é uma chave";
        l.story = "O transistor NPN é uma chave que se liga com eletricidade, sem mão nenhuma: uma corrente pequena na base (B) deixa passar uma "
                  "corrente grande do coletor (C) para o emissor (E). Todo computador é feito disso. Daqui em diante os circuitos usam a fonte com "
                  "terra, e mais um atalho: o rótulo 5V é o + da fonte.";
        l.steps = {"Ligue a perna sem o + do LED ao coletor (C) do transistor, e o emissor (E) ao terra.",
                   "Ponha uma chave e um resistor em fila, do rótulo 5V até a base (B).", "Feche a chave: o transistor liga e o LED acende."};
        l.hints = {"Dois rótulos com o mesmo nome são o mesmo fio: o 5V daqui é o + da fonte, lá embaixo.",
                   "O coletor é a perna de cima, o emissor a de baixo, com a setinha. A base é a do meio, à esquerda.",
                   "O resistor da base protege o transistor: 1 kΩ ou 10 kΩ funcionam.",
                   "O LED não pode estar ligado à chave: quem acende o LED é o transistor."};
        l.success = "A chave só manda um sinal fraquinho para a base, e o transistor faz o trabalho pesado. Agora um fio pode ligar outro fio.";
        l.start = transistorParts;
        l.parts = with(kElectric, {PartKind::Switch, PartKind::Npn});
        l.required = {PartKind::Npn};
        l.check = LessonCheck::LightSwitches;
        l.switches = 1;
        l.lit = "01";
        l.transistorCarries = true;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "nmos-chave";
        l.act = 1;
        l.title = "A NMOS é uma chave";
        l.story = "O mesmo LED da primeira aula do transistor, agora com uma NMOS. Ela liga com tensão na porta (G), sem precisar de corrente: a chave leva o 5V até a porta, e a NMOS deixa a corrente do LED ir do dreno (D) até a fonte (S). Com a chave aberta, um resistor da porta até o terra segura a porta em 0.";
        l.steps = {"Ligue a perna sem o + do LED ao dreno (D) da NMOS, e a fonte (S) ao terra.", "Ponha uma chave do rótulo 5V até a porta (G). Não precisa de resistor: a porta não puxa corrente.", "Ponha um resistor de 10 kΩ da porta (G) até o terra, e feche a chave."};
        l.hints = {"Passe o mouse nas pernas da NMOS para ver os nomes: G, D e S.", "O resistor do terra é o pull-down: sem ele, com a chave aberta, a porta fica solta.", "O LED não pode estar ligado à chave: quem acende o LED é a NMOS."};
        l.success = "A chave só põe uma tensão na porta, e quase nenhuma corrente entra por ela. A NMOS faz o resto: é assim nos chips.";
        l.start = mosParts;
        l.parts = with(kElectric, {PartKind::Switch, PartKind::Nmos});
        l.required = {PartKind::Nmos};
        l.check = LessonCheck::LightSwitches;
        l.switches = 1;
        l.lit = "01";
        l.transistorCarries = true;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "inversor-cmos";
        l.act = 1;
        l.title = "O inversor CMOS";
        l.story = "Monte o inversor dos chips: uma PMOS entre o 5V e a saída, uma NMOS entre a saída e o terra, e as duas portas em A. Com A em 0 a PMOS liga e o LED S acende; com A em 1 a NMOS liga e o LED apaga. Sem resistor de pull-up.";
        l.steps = {"Ponha uma PMOS e uma NMOS e ligue as duas portas (G) em A.", "A PMOS vai do rótulo 5V até o ponto de saída; a NMOS vai do ponto de saída até o terra.", "Ligue o ponto de saída, entre as duas, na ponta livre do resistor do LED S."};
        l.hints = {"A PMOS é a que tem a bolinha na porta: ela fica em cima, do lado do 5V.", "Passe o mouse nas pernas para ver os nomes: G, D e S.", "Não precisa de pull-up: quem puxa a saída para cima é a PMOS."};
        l.success = "Um inversor CMOS de verdade: em cada estado um transistor está cortado, e quase nenhuma corrente se perde. Os processadores têm bilhões desses.";
        l.start = cmosStart;
        l.parts = with(kElectric, {PartKind::Nmos, PartKind::Pmos});
        l.required = {PartKind::Nmos, PartKind::Pmos};
        l.check = LessonCheck::Table;
        l.inputs = {"A"};
        l.outputs = {"S"};
        l.table = "10";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "inversor";
        l.act = 3;
        l.title = "NÃO feito de um transistor";
        l.story = "Agora a chave vira um sinal: a entrada A vale 0 ou 1. O LED S já está aceso, ligado do 5V até o terra. "
                  "Com A em 1, o transistor liga e abre um atalho para o terra: a corrente passa por ele e o LED apaga. "
                  "Com A em 0, o LED acende. S é o contrário de A: uma porta NÃO (NOT).";
        l.steps = {"Ligue A a um resistor, e o resistor à base (B) do NPN.", "Ligue o coletor (C) no ponto entre o resistor e o LED S.",
                   "Ligue o emissor (E) ao terra."};
        l.hints = {"O ponto entre o resistor e o LED é o pino de baixo do resistor: ligue o coletor direto nele.",
                   "Quando o transistor liga, a corrente prefere o caminho dele: é muito mais fácil do que passar pelo LED.",
                   "A tabela-verdade ao lado mostra o LED S para cada valor de A."};
        l.success = "Uma porta lógica de verdade: um transistor decide se o LED acende. A tabela confirma: S é sempre o contrário de A.";
        l.start = notTransistor;
        l.parts = with(kElectric, {PartKind::Npn});
        l.required = {PartKind::Npn};
        l.check = LessonCheck::Table;
        l.inputs = {"A"};
        l.outputs = {"S"};
        l.table = "10";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "tabelas-transistores";
        l.act = 3;
        l.title = "As tabelas dos transistores";
        l.story = "Sem montar nada desta vez: pense nos transistores ligando e desligando e diga o que S mostra. Responda com 0 ou 1.";
        l.steps = {"Leia a pergunta e digite 0 ou 1.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Os transistores só puxam S para 0. Se nenhum caminho até o terra se fecha, o pull-up deixa S em 1.", "Em série precisa dos dois ligados; em paralelo basta um."};
        l.success = "Você lê o circuito como o transistor lê: caminho fechado até o terra, 0; sem caminho, 1.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"No inversor, com A = 0, quanto vale S?", "", 10, "1", "O transistor fica desligado e o pull-up puxa S para 5 V.", "Com A em 0, o transistor liga?"},
            {"Na porta NAND, com A = 1 e B = 1, quanto vale S?", "", 10, "0", "Os dois em série ligam e fecham o caminho até o terra.", "Em série, os dois ligados fecham o caminho."},
            {"Na porta NAND, com A = 0 e B = 1, quanto vale S?", "", 10, "1", "O transistor de A está desligado e corta o caminho: o pull-up ganha.", "Um transistor desligado em série corta o caminho inteiro."},
            {"Na porta NOR, com A = 0 e B = 1, quanto vale S?", "", 10, "0", "Em paralelo, o transistor de B sozinho já liga S ao terra.", "Em paralelo, basta um transistor ligado."},
            {"Na porta NOR, com A = 0 e B = 0, quanto vale S?", "", 10, "1", "Nenhum transistor liga: o pull-up mantém S em 1.", "Algum caminho até o terra se fecha?"},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "nand-transistores";
        l.act = 3;
        l.title = "NAND com dois transistores";
        l.story = "Agora são dois transistores em fila, um em cima do outro. O atalho para o terra só se abre quando os dois "
                  "estão ligados: A E B. Então o LED só apaga com A e B em 1. Isso é a porta NAND, a porta que sozinha faz todas "
                  "as outras.";
        l.steps = {"Cada entrada vai por um resistor até a base de um transistor.",
                   "Ligue o coletor do transistor de cima no ponto entre o resistor e o LED S.",
                   "Ligue o emissor de cima ao coletor de baixo, e o emissor de baixo ao terra."};
        l.hints = {"É a porta NÃO da aula anterior com mais um transistor embaixo, em série.",
                   "Confira a tabela: só a linha A = 1, B = 1 pode apagar o LED."};
        l.success = "NAND pronta! Com NANDs dá para construir um computador inteiro, e é isso que você vai fazer.";
        l.start = twoTransistors;
        l.parts = with(kElectric, {PartKind::Npn});
        l.required = {PartKind::Npn};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "1110";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "nor-transistores";
        l.act = 3;
        l.title = "NOR com dois transistores";
        l.story = "Lado a lado, em paralelo, cada transistor abre o seu próprio atalho para o terra. Qualquer um ligado já apaga "
                  "o LED: ele só acende quando A e B são 0. Isso é uma porta NOR.";
        l.steps = {"Cada entrada vai por um resistor até a base de um transistor.", "Ligue os dois coletores no ponto entre o resistor e o LED S.",
                   "Ligue os dois emissores ao terra."};
        l.hints = {"É como as duas chaves em paralelo da primeira parte, só que apagando o LED em vez de acender."};
        l.success = "NOR pronta. Repare: em série deu NAND, em paralelo deu NOR. A forma de ligar muda a lógica.";
        l.start = twoTransistors;
        l.parts = with(kElectric, {PartKind::Npn});
        l.required = {PartKind::Npn};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "1000";
        all.push_back(l);
    }

    // Act 3: bits. What a wire's 0 and 1 are, and how a few of them make a number.
    {
        Lesson l;
        l.id = "dois-bits";
        l.act = 2;
        l.title = "Dois bits, quatro números";
        l.story = "Cada chave é um bit: ligada vale 1, desligada vale 0. Com dois bits dá para escrever quatro números: 00, 01, 10 e 11, ou "
                  "seja, 0, 1, 2 e 3. Ligue cada chave ao seu LED e veja os quatro números aparecerem, um bit em cada luz.";
        l.steps = {"Ligue a chave B1 ao resistor do LED L1, e a chave B0 ao resistor do LED L0.",
                   "Ligue as chaves de todos os jeitos e veja: o fio de um bit 1 fica claro, o de um bit 0 fica escuro.",
                   "A tabela-verdade ao lado mostra as quatro combinações: 00, 01, 10 e 11."};
        l.hints = {"O fio sai do pino da direita da chave e entra no pino de cima do resistor de cada LED.",
                   "B1 é o bit da esquerda, que vale 2; B0 é o da direita, que vale 1. Com os dois ligados: 2 + 1 = 3."};
        l.success = "Dois fios, quatro números. Cada bit a mais dobra a conta: três bits fazem 8 números, quatro fazem 16.";
        l.start = [] { return namedSignals({"B1", "B0"}, {"L1", "L0"}); };
        l.check = LessonCheck::Table;
        l.inputs = {"B1", "B0"};
        l.outputs = {"L1", "L0"};
        l.table = "00011011";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "ligar-com-rotulos";
        l.act = 2;
        l.title = "Rótulos no lugar de fios";
        l.story = "As saídas estão trocadas: a chave A tem de acender o LED T, embaixo, e a chave B tem de acender o S, em cima. Com fios, os dois se cruzariam no meio da placa. Use rótulos: cada sinal com o seu nome, sem fio nenhum atravessando.";
        l.steps = {"Arraste um Rótulo (Fios) para a direita da chave A, ligue a chave a ele e, no painel da direita, troque o nome dele para a.", "Arraste outro Rótulo para a esquerda do resistor do LED T, ligue-o ao resistor e dê a ele também o nome a.", "Faça o mesmo com a chave B e o LED S, com o nome b, e teste as quatro combinações."};
        l.hints = {"Clique num rótulo, só nele: os que têm o mesmo nome ganham moldura laranja. Se só ele ficar laranja, o nome do outro está diferente.", "Todo rótulo novo se chama sinal. Se você não trocar os nomes, as chaves A e B viram o mesmo fio.", "Maiúscula conta: a e A são nomes diferentes. Os dois rótulos de um sinal têm de ter o nome escrito igual."};
        l.success = "Nenhum fio cruzando, e cada sinal no lugar certo. Nos circuitos grandes, como os computadores, os rótulos levam sinais como o Clk a dezenas de peças.";
        l.start = [] { return namedSignals({"A", "B"}, {"S", "T"}); };
        l.parts = {PartKind::Tunnel};
        l.required = {PartKind::Tunnel};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S", "T"};
        l.table = "00100111";  // S is B and T is A: the wires would cross
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "quatro-bits-numero";
        l.act = 2;
        l.title = "Quatro bits viram um número";
        l.story = "Cada chave tem o seu peso: 8, 4, 2 e 1. O Juntador junta os quatro fios num só fio grosso, o barramento, e a Saída N mostra o "
                  "número que eles formam. Ligue as chaves dos pesos 4 e 1: N mostra 5, porque 4 + 1 = 5.";
        l.steps = {"Arraste um Juntador (Fios) para o meio. No painel da direita, deixe-o com 4 bits e 4 entradas.",
                   "Ligue a chave 1 na entrada de cima do Juntador, a 2 na seguinte, depois a 4, e a 8 na de baixo.",
                   "Ligue a saída do Juntador à Saída N, e teste: 4 e 1 ligadas mostram 5."};
        l.hints = {"A entrada de cima do Juntador é o bit 0, o que vale 1. A de baixo é o bit 3, o que vale 8.",
                   "Se N mostrar 10 em vez de 5, os fios estão de cabeça para baixo: a chave 1 tem de ir na entrada de cima.",
                   "A Saída mostra o número em hexadecimal: de 10 a 15 aparecem as letras A a F. A próxima leitura conta por quê."};
        l.success = "Quatro fios, um número de 0 a 15. O barramento é o mesmo número, num fio só: o fio grosso fica azul-claro quando algum bit é 1.";
        l.start = fourSwitches;
        l.parts = {PartKind::Joiner};
        l.required = {PartKind::Joiner};
        l.check = LessonCheck::Table;
        l.inputs = {"8", "4", "2", "1"};
        l.outputs = {"N"};
        l.expect = weighed;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "numero-em-bits";
        l.act = 2;
        l.title = "Um número vira quatro bits";
        l.story = "Agora o contrário: o número N chega num barramento, e cada LED tem de mostrar um bit dele. O Separador abre o barramento em "
                  "fios de um bit. Com N = 5, ou 0101, acendem o 4 e o 1.";
        l.steps = {"Arraste um Separador (Fios) e deixe-o com 4 bits e 4 saídas.",
                   "Ligue a entrada N no Separador. A saída de cima vai ao LED 1, a seguinte ao 2, depois ao 4, e a de baixo ao 8.",
                   "Mude N clicando nos seus bits: os LEDs acendem como os bits do número."};
        l.hints = {"Como no Juntador, a saída de cima do Separador é o bit 0, o que vale 1.",
                   "Cada LED tem o seu resistor: o fio do Separador entra no pino de cima do resistor."};
        l.success = "Juntador e Separador fazem o caminho de ida e volta: de fios a número e de número a fios. A CPU faz isso o tempo todo.";
        l.start = fourLeds;
        l.parts = {PartKind::Splitter};
        l.required = {PartKind::Splitter};
        l.check = LessonCheck::Table;
        l.inputs = {"N"};
        l.outputs = {"8", "4", "2", "1"};
        l.expect = bitOfNumber;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "quanto-vale";
        l.act = 2;
        l.title = "Quanto vale?";
        l.story = "Sem montar nada desta vez: ler e escrever números em binário, de cabeça. As luzes acesas são os bits 1; embaixo de cada uma "
                  "está o quanto ela vale. Some o valor das acesas.";
        l.steps = {"Leia as luzes e digite a resposta no teclado.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Os pesos dobram da direita para a esquerda: 1, 2, 4, 8, 16, 32, 64, 128.",
                   "Para escrever um número em binário, tire o maior peso que cabe, depois o seguinte, até sobrar zero. 9 = 8 + 1: 1001."};
        l.success = "Agora você lê e escreve binário. É só somar pesos.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quanto vale este número? Digite em decimal.", "0011", 10, "3", "2 + 1 = 3.",
             "Some os números embaixo das luzes acesas: aqui estão acesas a do 2 e a do 1."},
            {"E este?", "0101", 10, "5", "4 + 1 = 5.", "Acesas estão a luz que vale 4 e a que vale 1. Some as duas."},
            {"E este?", "1010", 10, "10", "8 + 2 = 10.", "Acesas estão a luz que vale 8 e a que vale 2."},
            {"E este, com as quatro luzes acesas?", "1111", 10, "15", "8 + 4 + 2 + 1 = 15: o maior número de 4 bits.",
             "Todas acesas: some 8, 4, 2 e 1."},
            {"Agora com 8 bits, um byte. Quanto vale?", "0010 1010", 10, "42", "32 + 8 + 2 = 42.",
             "Com 8 bits os pesos vão até 128. Some os das luzes acesas: 32, 8 e 2."},
            {"E este byte?", "1000 0000", 10, "128", "Só o bit que vale 128 está aceso.", "Só uma luz está acesa: o número é o que está embaixo dela."},
            {"Escreva 9 em binário, com 4 bits.", "", 2, "1001", "9 = 8 + 1: acendem o 8 e o 1.",
             "As casas valem 8, 4, 2 e 1. Que casas somam 9? Escreva 1 nelas e 0 nas outras, da casa do 8 para a do 1."},
            {"Escreva 6 em binário, com 4 bits.", "", 2, "0110", "6 = 4 + 2.", "6 = 4 + 2. As casas são 8, 4, 2 e 1: ponha 1 nas do 4 e do 2."},
            {"Escreva 200 em binário, com 8 bits.", "", 2, "11001000", "200 = 128 + 64 + 8.",
             "Tire o maior peso que cabe: 200 - 128 = 72, 72 - 64 = 8, 8 - 8 = 0. As casas são 128, 64, 32, 16, 8, 4, 2 e 1."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "contar-binario";
        l.act = 2;
        l.title = "Contar e o hexadecimal";
        l.story = "Contar em binário é somar 1, com vai-um: 1 + 1 dá 0 e vai 1 para a casa da esquerda. E como os números ficam compridos, "
                  "eles costumam ser escritos em hexadecimal, um dígito de 0 a F para cada 4 bits. A Saída do simulador mostra assim.";
        l.steps = {"Leia a pergunta e digite a resposta: em binário, decimal ou hexadecimal, como ela pede.",
                   "Enter (ou Responder) confere."};
        l.hints = {"Somar 1: troque os 1 da direita por 0 até achar um 0, e troque esse 0 por 1. 0111 + 1 = 1000.",
                   "Hexadecimal: 10 = A, 11 = B, 12 = C, 13 = D, 14 = E, 15 = F. Cada grupo de 4 bits vira um dígito."};
        l.success = "Binário, decimal e hexadecimal são o mesmo número escrito de três jeitos. Agora as portas lógicas vão fazer contas com eles.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Qual é o número seguinte? Digite em binário.", "0111", 2, "1000", "0111 + 1: o vai-um anda até o bit que vale 8. 7 + 1 = 8.",
             "O seguinte é este mais 1. Troque os 1 da direita por 0 até achar um 0, e troque esse 0 por 1. Responda só com 0 e 1."},
            {"E depois deste?", "1001", 2, "1010", "1 + 1 dá 0 e vai 1: 1001 + 1 = 1010. 9 + 1 = 10.",
             "O último bit é 1: vira 0, e o vai-um passa para o bit ao lado, que é 0 e vira 1."},
            {"E depois deste byte?", "0011 1111", 2, "01000000", "Seis bits 1 viram 0, e o vai-um acende o que vale 64. 63 + 1 = 64.",
             "Os seis 1 da direita viram 0, e o primeiro 0 depois deles vira 1."},
            {"Com só 4 bits, o que vem depois de 1111?", "1111", 2, "0000",
             "15 + 1 = 16 não cabe em 4 bits: o vai-um sai pela esquerda e sobra 0000. É o estouro.",
             "Todos os 1 viram 0, e o vai-um não tem mais casa para onde ir: fica só o que cabe nos 4 bits."},
            {"Escreva este número em hexadecimal.", "1011", 16, "B", "1011 = 11, e 11 em hexadecimal é B.",
             "Primeiro some os pesos das luzes acesas. Depois: em hexadecimal, 10 é A, 11 é B, 12 é C, 13 é D, 14 é E e 15 é F."},
            {"E este byte, em hexadecimal?", "0010 1010", 16, "2A", "Quatro bits por dígito: 0010 = 2 e 1010 = A. 0x2A é 42.",
             "Cada grupo de 4 luzes vira um dígito. Leia cada grupo com os pesos 8, 4, 2 e 1, e escreva os dois dígitos lado a lado."},
            {"Quanto vale 0x1F em decimal?", "", 10, "31", "1 × 16 + F (15) = 31.", "O 1 está na casa do 16, e o F vale 15. Some 16 com 15."},
            {"Uma Saída de 8 bits mostra FF. Quanto é em decimal?", "", 10, "255", "15 × 16 + 15 = 255: os oito bits acesos.",
             "F vale 15. O primeiro F está na casa do 16: 15 × 16, mais o segundo F."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "hexadecimal-perguntas";
        l.act = 2;
        l.title = "Tamanhos e hexadecimal";
        l.story = "Sem montar nada desta vez: quantos números cabem em tantos bits, e números escritos em hexadecimal.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Cada dígito hexadecimal são 4 bits. A vale 10, B 11, C 12, D 13, E 14 e F 15.", "n bits: 2 elevado a n combinações, e o maior número é uma a menos."};
        l.success = "Tamanhos e hexadecimal, de cabeça: é assim que se lê a memória de um computador.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quantos números diferentes cabem em 8 bits? Em decimal.", "", 10, "256", "2 elevado a 8 = 256: de 0 a 255.", "Dobre 8 vezes, começando de 1."},
            {"Qual é o maior número de 16 bits? Em decimal.", "", 10, "65535", "2 elevado a 16 = 65.536 combinações; o maior é uma a menos.", "O maior é 2 elevado a 16, menos 1."},
            {"Escreva 1011 em hexadecimal.", "", 16, "B", "1011 = 8 + 2 + 1 = 11, que é B.", "Calcule o valor e lembre: 10 é A, 11 é B..."},
            {"Quanto vale 0x2F em decimal?", "", 10, "47", "2 × 16 + 15 = 47.", "O 2 está na casa do 16; F vale 15."},
            {"Escreva 200 em hexadecimal.", "", 16, "C8", "200 = 12 × 16 + 8: os dígitos são C (12) e 8.", "Quantas vezes 16 cabe em 200? E quanto sobra?"},
            {"Escreva 0xA5 em binário, com 8 bits.", "", 2, "10100101", "A é 1010 e 5 é 0101: 1010 0101.", "Troque cada dígito pelos seus 4 bits."},
        };
        all.push_back(l);
    }

    // Act 4: logic gates.
    {
        Lesson l;
        l.id = "portas-perguntas";
        l.act = 3;
        l.title = "As regras das portas";
        l.story = "Sem montar nada desta vez: diga o que cada porta entrega. Responda com 0 ou 1.";
        l.steps = {"Leia a pergunta e digite 0 ou 1.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Porta E: só 1 com todas em 1. Porta OU: 1 com pelo menos uma em 1. Porta XOR: 1 com as entradas diferentes.", "NAND e NOR são a porta E e a porta OU com a saída invertida."};
        l.success = "As seis regras na ponta da língua: com elas você lê qualquer circuito de portas.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Com A = 1 e B = 0, quanto dá A E B?", "", 2, "0", "A porta E só dá 1 quando as duas são 1.", "B está em 0."},
            {"E A OU B, com as mesmas entradas?", "", 2, "1", "Basta uma em 1: A está em 1.", "Pelo menos uma está em 1?"},
            {"E A XOR B, ainda com A = 1 e B = 0?", "", 2, "1", "As entradas são diferentes: a porta XOR dá 1.", "As duas são iguais ou diferentes?"},
            {"Com A = 1 e B = 1, quanto dá A NAND B?", "", 2, "0", "1 E 1 = 1, e a porta NAND inverte: 0.", "Calcule a porta E e inverta."},
            {"Com A = 0 e B = 0, quanto dá A NOR B?", "", 2, "1", "0 OU 0 = 0, e a porta NOR inverte: 1.", "Calcule a porta OU e inverta."},
            {"Quanto dá NÃO (1 OU 0)?", "", 2, "0", "1 OU 0 = 1, e NÃO 1 = 0.", "Faça primeiro o que está entre parênteses."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-e";
        l.act = 3;
        l.title = "Acenda um LED com uma porta";
        l.story = "Daqui em diante cada porta é uma caixinha pronta da biblioteca; mais adiante, neste ato, você monta cada uma com transistores. A saída de uma porta tem força para acender um LED de verdade, com o resistor de sempre. A porta E (AND) liga a saída só quando todas as entradas estão ligadas.";
        l.steps = {"Arraste uma porta AND.", "Ligue A e B nas entradas da porta.",
                   "Ligue a saída da porta na ponta livre do resistor do LED S, e teste clicando em A e B."};
        l.hints = {"As entradas da porta ficam à esquerda e a saída à direita.", "A ponta livre do resistor é a da esquerda: o outro lado já vai para o LED."};
        l.success = "O LED S só acende com A e B ligadas. Por dentro dessa porta há uma porta NAND e uma porta NÃO, ou seja, três "
             "transistores.";
        l.start = gateTwoInputs;
        l.parts = kLogic;
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "0001";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-ou";
        l.act = 3;
        l.title = "Faça uma porta OU";
        l.story = "A porta OU (OR) liga a saída quando pelo menos uma das entradas está ligada. É a versão em porta das duas chaves em paralelo do Ato 1: qualquer caminho basta.";
        l.steps = {"Arraste uma porta OR.", "Ligue A e B nas entradas da porta.", "Ligue a saída da porta no resistor do LED S e teste as quatro combinações."};
        l.hints = {"A tabela-verdade à direita mostra o que o LED deve fazer em cada linha.", "A porta OU só apaga com A e B em 0."};
        l.success = "O LED acende com A, com B ou com os dois. Só apaga quando nenhuma entrada está ligada.";
        l.start = gateTwoInputs;
        l.parts = kGates;
        l.required = {PartKind::Or};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "0111";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-nao";
        l.act = 3;
        l.title = "Faça uma porta NÃO";
        l.story = "A porta NÃO (NOT), o inversor, tem uma entrada só e entrega o contrário dela: 0 vira 1, e 1 vira 0.";
        l.steps = {"Arraste uma porta NOT.", "Ligue A na entrada e a saída no resistor do LED S.", "Clique em A: o LED faz o contrário."};
        l.hints = {"A porta NOT é o triângulo com a bolinha na ponta."};
        l.success = "S é sempre o contrário de A. Com A desligada o LED acende: a porta NÃO de 0 é 1.";
        l.start = gateOneInput;
        l.parts = kGates;
        l.required = {PartKind::Not};
        l.check = LessonCheck::Table;
        l.inputs = {"A"};
        l.outputs = {"S"};
        l.table = "10";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-nand";
        l.act = 3;
        l.title = "Faça uma porta NAND";
        l.story = "A porta NAND é uma porta E com a saída invertida: dá 0 só quando todas as entradas estão em 1, e 1 em todo o "
             "resto. Parece estranha, mas é a porta mais importante de todas: com ela se faz qualquer outra.";
        l.steps = {"Arraste uma porta NAND, a porta E com a bolinha na ponta.", "Ligue A e B nas entradas.", "Ligue a saída no resistor do LED S e confira a tabela."};
        l.hints = {"Compare com a tabela da porta E: cada linha é o contrário."};
        l.success = "O LED só apaga com A e B ligadas: o contrário da porta E. Guarde esta porta: daqui a pouco ela faz todas as "
             "outras.";
        l.start = gateTwoInputs;
        l.parts = kGates;
        l.required = {PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "1110";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-nor";
        l.act = 3;
        l.title = "Faça uma porta NOR";
        l.story = "A porta NOR é uma porta OU com a saída invertida: dá 1 só quando nenhuma entrada está ligada.";
        l.steps = {"Arraste uma porta NOR, a porta OU com a bolinha na ponta.", "Ligue A e B nas entradas.", "Ligue a saída no resistor do LED S e confira a tabela."};
        l.hints = {"Compare com a tabela da porta OU: cada linha é o contrário."};
        l.success = "O LED só acende com tudo desligado: o contrário da porta OU.";
        l.start = gateTwoInputs;
        l.parts = kGates;
        l.required = {PartKind::Nor};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "1000";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "porta-tres-entradas";
        l.act = 3;
        l.title = "Uma porta E de três entradas";
        l.story = "Uma porta pode ter mais de duas entradas. Uma porta E de três entradas só acende a saída quando A, B e C "
             "estão todas ligadas: A · B · C.";
        l.steps = {"Arraste uma porta AND e, com ela selecionada, aumente as Entradas para 3 no painel da direita.", "Ligue A, B e C nas três entradas.", "Ligue a saída no resistor do LED S."};
        l.hints = {"Também vale fazer com duas portas E em fila: (A · B) · C dá o mesmo, pela regra associativa.", "Das oito linhas da tabela, só a última acende o LED."};
        l.success = "Uma porta faz o trabalho de duas: A · B · C. E a regra associativa garante que a ordem não importa.";
        l.start = [] { return namedSignals({"A", "B", "C"}, {"S"}); };
        l.parts = kGates;
        l.required = {PartKind::And};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B", "C"};
        l.outputs = {"S"};
        l.table = "00000001";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "expressao";
        l.act = 3;
        l.title = "Monte S = AB + C";
        l.story = "Agora é você quem vai de uma expressão até um circuito. S = AB + C quer dizer: faça a porta E de A e B, e "
             "depois a porta OU desse resultado com C. A porta E vem primeiro, como a multiplicação.";
        l.steps = {"Uma porta AND recebe A e B.", "Uma porta OR recebe a saída da AND e C.", "A saída da OR vai ao resistor do LED S."};
        l.hints = {"Siga a expressão de dentro para fora: primeiro AB, depois o + C.", "O LED acende sempre que C está ligada, e também quando A e B estão ligadas."};
        l.success = "Da expressão ao circuito, porta por porta. É assim que se projeta qualquer circuito digital.";
        l.start = [] { return namedSignals({"A", "B", "C"}, {"S"}); };
        l.parts = kGates;
        l.required = {PartKind::And, PartKind::Or};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B", "C"};
        l.outputs = {"S"};
        l.table = "01010111";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "nao-com-nand";
        l.act = 3;
        l.title = "NÃO feito de portas NAND";
        l.story = "A porta NAND é universal: com ela dá para fazer qualquer porta. Comece pela mais simples. Se as duas entradas "
                  "de uma porta NAND recebem o mesmo sinal, o que ela faz?";
        l.steps = {"Arraste uma porta NAND.", "Ligue A nas duas entradas dela.", "Ligue a saída no resistor do LED S."};
        l.hints = {"NAND de A com A: 0 vira 1 e 1 vira 0."};
        l.success = "Uma porta NAND com as entradas juntas é uma porta NÃO. Guarde esse truque: ele vai aparecer muito.";
        l.start = gateOneInput;
        l.parts = {PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A"};
        l.outputs = {"S"};
        l.table = "10";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "e-com-nand";
        l.act = 3;
        l.title = "E feito de portas NAND";
        l.story = "NAND quer dizer NÃO-E: é uma porta E com a saída invertida. Então, para ter uma porta E, basta inverter a "
             "saída de uma porta NAND de novo.";
        l.steps = {"Ligue A e B numa NAND.", "Faça uma porta NÃO com outra porta NAND, como na aula anterior.",
                   "Passe a saída da primeira pela porta NÃO até o resistor do LED S."};
        l.hints = {"Duas inversões se anulam."};
        l.success = "E feito só de portas NAND. Cada porta nova é feita das anteriores: é assim que o computador cresce.";
        l.start = gateTwoInputs;
        l.parts = {PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "0001";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "ou-com-nand";
        l.act = 3;
        l.title = "OU feito de portas NAND";
        l.story = "Um desafio: o LED tem que acender quando A OU B estiver ligada, usando só NANDs. A porta NAND só dá 0 quando "
                  "as duas entradas são 1... e se as entradas chegarem invertidas?";
        l.steps = {"Inverta A com uma porta NAND, e B com outra.", "Ligue as duas entradas invertidas numa terceira NAND.",
                   "Ligue a saída dela no resistor do LED S."};
        l.hints = {"Pense ao contrário: S só é 0 quando A e B são 0, ou seja, quando NÃO A e NÃO B são 1."};
        l.success = "Três NANDs fazem uma porta OU. Isso tem nome: lei de De Morgan.";
        l.start = gateTwoInputs;
        l.parts = {PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "0111";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "xor";
        l.act = 3;
        l.title = "OU exclusivo";
        l.story = "A porta XOR liga quando as entradas são diferentes: uma ou outra, mas não as duas. Ele é o coração da soma: "
             "em binário, 1 + 1 dá 0 e vai 1.";
        l.steps = {"Monte com as portas que quiser: E, OU, NÃO e NAND.", "O LED S acende só quando A e B são diferentes."};
        l.hints = {"XOR é (A OU B) E NÃO (A E B).", "Também dá com quatro NANDs, se quiser um desafio."};
        l.success = "XOR pronto. Na próxima aula ele vira a primeira peça de uma calculadora.";
        l.start = gateTwoInputs;
        l.parts = {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S"};
        l.table = "0110";
        all.push_back(l);
    }

    // Two ready parts of act 3, made of gates: the decoder and the multiplexer of four.
    const auto gatesLesson = [&](const char* id, int act, const char* title, const char* story, std::vector<const char*> steps,
                                 std::vector<const char*> hints, const char* success, Board (*start)(), std::vector<PartKind> parts,
                                 std::vector<const char*> boxes, std::vector<const char*> inputs, std::vector<const char*> outputs) {
        Lesson l;
        l.id = id;
        l.act = act;
        l.title = title;
        l.story = story;
        l.steps = std::move(steps);
        l.hints = std::move(hints);
        l.success = success;
        l.start = start;
        l.parts = std::move(parts);
        l.boxes = std::move(boxes);
        l.check = LessonCheck::Table;
        l.inputs = std::move(inputs);
        l.outputs = std::move(outputs);
        return l;
    };
    {
        Lesson l = gatesLesson("decodificador", 3, "Decodificador: um fio por número",
                               "Dois bits, A1 e A0, formam um número de 0 a 3. Um decodificador acende só a saída desse número: Y0 com 00, Y3 "
                               "com 11. A CPU usa um destes para escolher qual registrador vai ser escrito.",
                               {"Faça NÃO A1 e NÃO A0 com duas portas NOT.", "Cada saída é uma porta E: Y0 quer NÃO A1 e NÃO A0; Y3 quer A1 e A0.",
                                "Y1 é 01 (NÃO A1 e A0) e Y2 é 10 (A1 e NÃO A0). Ligue cada porta E no resistor do seu LED."},
                               {"Cada porta E reconhece um número só: as quatro nunca acendem juntas.", "Os inversores podem alimentar várias portas."},
                               "Um decodificador de portas, igual à peça pronta Decodificador 2:4.",
                               [] { return namedSignals({"A1", "A0"}, {"Y0", "Y1", "Y2", "Y3"}); },
                               {PartKind::And, PartKind::Not}, {}, {"A1", "A0"}, {"Y0", "Y1", "Y2", "Y3"});
        l.table = "1000" "0100" "0010" "0001";
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("mux4", 3, "Escolher entre quatro",
                               "Um multiplexador 4:1 escolhe uma de quatro entradas, D0 a D3, pelo número que S1 e S0 formam. É ele que, dentro "
                               "da ULA, escolhe qual operação vai para o resultado.",
                               {"Faça NÃO S1 e NÃO S0.", "Para cada D, uma porta E de três entradas que só deixa passar com o seu número: D2, por exemplo, com S1 e NÃO "
                                                         "S0.",
                                "Junte as quatro portas E num OU de quatro entradas e ligue no resistor do LED Y."},
                               {"Selecione uma porta e use + no painel para dar mais entradas a ela.", "É o decodificador da aula anterior, com "
                                                                                                       "cada saída abrindo a passagem de um D."},
                               "Um multiplexador de quatro entradas. A ULA do computador tem vários deles.",
                               [] { return namedSignals({"S1", "S0", "D0", "D1", "D2", "D3"}, {"Y"}); },
                               {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"S1", "S0", "D0", "D1", "D2", "D3"}, {"Y"});
        l.expect = fourWayChoice;
        all.push_back(l);
    }

    // Act 5: arithmetic.
    {
        Lesson l;
        l.id = "somas-binarias";
        l.act = 4;
        l.title = "Somas em binário";
        l.story = "Sem montar nada desta vez: somas no papel, em binário, com vai-um. Atenção à base que cada pergunta pede.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Some casa por casa, da direita para a esquerda: 1 + 1 = 10, fica 0 e vai 1.", "Com 4 bits, o vai-um que sai da última casa se perde: é o estouro."};
        l.success = "Você soma em binário como o somador da CPU: casa por casa, com vai-um.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quanto dá 0101 + 0011? Em binário, com 4 bits.", "", 2, "1000", "5 + 3 = 8: 1000.", "Some casa por casa; três vai-uns seguidos."},
            {"Quanto dá 0110 + 0111? Em decimal.", "", 10, "13", "6 + 7 = 13, ou 1101.", "Leia os dois números em decimal e some."},
            {"Quanto dá 1011 + 0101, com 4 bits? Em binário, sem o vai-um que sobra.", "", 2, "0000", "11 + 5 = 16 = 10000: com 4 bits sobra 0000, e o vai-um se perde.", "16 cabe em 4 bits?"},
            {"E nessa soma, quanto vale o vai-um que sai da última casa? 0 ou 1.", "", 2, "1", "A soma passou de 15: o último Cs acende. É o sinal do estouro.", "O resultado coube em 4 bits?"},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "meio-somador";
        l.act = 4;
        l.title = "Meio somador";
        l.story = "Somar dois bits dá um resultado de dois bits: 0+0=0, 0+1=1, 1+1=10 (dois, em binário). O bit de baixo é a soma S, "
                  "o de cima é o vai-um C.";
        l.steps = {"O LED S acende quando A e B são diferentes: que porta faz isso?", "O LED C acende só quando A e B são 1.",
                   "Ligue as duas portas nos resistores dos LEDs S e C."};
        l.hints = {"S é uma porta XOR, C é uma porta E."};
        l.success = "Seu circuito soma! É o primeiro pedaço da ULA, a parte do computador que faz contas.";
        l.start = halfAdder;
        l.parts = {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Xor, PartKind::Nand};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S", "C"};
        l.table = "00101001";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "somador-completo";
        l.act = 4;
        l.title = "Somador completo";
        l.story = "Para somar números de vários bits, cada coluna recebe o vai-um da coluna anterior: são três bits para somar, A, B e Ce. "
                  "O resultado é a soma S e o novo vai-um Cs.";
        l.steps = {"Some A e B com um meio somador.", "Some o resultado com Ce em outro meio somador: a soma dele vai para o LED S.",
                   "O LED Cs acende se algum dos dois meios somadores der vai-um."};
        l.hints = {"O meio somador pronto está na biblioteca. Os dois vai-uns se juntam num OU."};
        l.success = "Somador completo! Enfileirando estes, um para cada bit, dá para somar números de qualquer tamanho.";
        l.start = fullAdder;
        l.parts = {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Xor};
        l.boxes = {"half_adder"};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B", "Ce"};
        l.outputs = {"S", "Cs"};
        l.table = "0010100110010111";
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "somador-4-bits";
        l.act = 4;
        l.title = "Somador de 4 bits";
        l.story = "A e B agora são números de 0 a 15, quatro fios cada. O separador abre um barramento em fios de 1 bit, e o juntador faz o contrário. "
                  "Com quatro somadores completos em fila, o vai-um de cada um entra no próximo.";
        l.steps = {"Separe A e B em bits com dois separadores.", "Ligue um somador completo por bit; o Cs de um vai no Ce do próximo.",
                   "Junte as quatro somas com um juntador no visor S, e ligue o último vai-um no resistor do LED Cs."};
        l.hints = {"O primeiro somador, o dos bits de baixo, não tem vai-um chegando: deixe o Ce dele solto, que vale 0.",
                   "No separador, o bit 0 é o de baixo."};
        l.success = "Uma calculadora de 4 bits feita de portas! A ULA do computador de 16 bits é isso, maior e com mais operações.";
        l.start = fourBitAdder;
        l.parts = {PartKind::Splitter, PartKind::Joiner, PartKind::And, PartKind::Or, PartKind::Xor};
        l.boxes = {"full_adder", "half_adder"};
        l.check = LessonCheck::Table;
        l.inputs = {"A", "B"};
        l.outputs = {"S", "Cs"};
        l.expect = sumOfFour;
        all.push_back(l);
    }

    // Act 5, more: subtracting, comparing, and the ALU slice.
    {
        Lesson l;
        l.id = "complemento-de-dois";
        l.act = 4;
        l.title = "Complemento de dois";
        l.story = "Sem montar nada desta vez: números negativos em 4 bits. -N é NÃO N mais 1.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Para achar -N: escreva N em 4 bits, inverta todos os bits e some 1.", "A - B é A + (-B). O vai-um que sobra da última casa se descarta."};
        l.success = "Negativos e subtração com o mesmo somador: é assim que a ULA da CPU faz SUB.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Como se escreve -1 com 4 bits? Em binário.", "", 2, "1111", "1 é 0001; invertendo, 1110; mais 1, 1111.", "Inverta 0001 e some 1."},
            {"E -5 com 4 bits?", "", 2, "1011", "5 é 0101; invertendo, 1010; mais 1, 1011.", "Inverta os bits de 0101 e some 1."},
            {"Para fazer 7 - 3, a ULA soma 7 com qual número de 4 bits? Em binário.", "", 2, "1101", "7 - 3 = 7 + (-3), e -3 é 1101.", "Ache -3 em complemento de dois."},
            {"Quanto dá 0110 + 1111 com 4 bits, em decimal? (6 + (-1))", "", 10, "5", "0110 + 1111 = 10101; com 4 bits sobra 0101, que é 5.", "Some em binário e descarte o vai-um que sai da última casa."},
        };
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("subtrator", 4, "Subtrator completo",
                               "Subtrair um bit é parecido com somar: D é A − B − Ve, e Vs, o empresta-um, liga quando a conta fica abaixo de "
                               "zero e precisa pedir à coluna de cima. Ve é o pedido que chega da coluna de baixo.",
                               {"D é A XOR B XOR Ve: duas XORs, como na soma.", "Vs liga com NÃO A e B, ou com NÃO (A XOR B) e Ve.",
                                "Junte essas duas portas E num OU e ligue no resistor do LED Vs."},
                               {"Teste 0 − 1: D e Vs acendem, porque 0 − 1 dá 1 pedindo emprestado.", "NÃO (A XOR B) é 1 quando A e B são "
                                                                                                       "iguais."},
                               "Subtrator pronto. Enfileirados, eles subtraem números de qualquer tamanho, como os somadores.",
                               [] { return namedSignals({"A", "B", "Ve"}, {"D", "Vs"}); },
                               {PartKind::Xor, PartKind::And, PartKind::Or, PartKind::Not}, {}, {"A", "B", "Ve"}, {"D", "Vs"});
        l.expect = subtractBit;
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("comparador", 4, "Comparar dois bits",
                               "Um comparador diz se A é maior, igual ou menor que B. É com ele que um programa decide: o BEQ da CPU desvia "
                               "quando dois números são iguais.",
                               {"A>B é A E NÃO B.", "A<B é NÃO A E B.", "A=B é a porta XNOR de A e B. Ligue cada uma no resistor do seu LED."},
                               {"Só um dos três LEDs pode acender de cada vez.", "A=B também é uma porta NOR de A>B com A<B: nem maior, nem menor."},
                               "Comparador pronto. Para números de muitos bits, compara-se do bit mais alto para baixo.",
                               [] { return namedSignals({"A", "B"}, {"A>B", "A=B", "A<B"}); },
                               {PartKind::And, PartKind::Not, PartKind::Xnor, PartKind::Nor}, {}, {"A", "B"}, {"A>B", "A=B", "A<B"});
        l.expect = compareBits;
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("ula-1-bit", 4, "Uma fatia da ULA",
                               "A ULA faz uma de quatro operações com A e B, escolhida por Op1 e Op0: 00 E, 01 OU, 10 XOR, 11 soma. Monte as "
                               "quatro e deixe um multiplexador escolher. Cs é o vai-um, mas só na soma.",
                               {"Faça A E B, A OU B e A XOR B com portas, e A + B + Ce com um somador completo.",
                                "Num MUX 4:1 pronto, ligue as quatro em D0 a D3, Op0 no S0 e Op1 no S1. A saída vai no LED R.",
                                "Cs é o Cs do somador E (Op1 E Op0): o vai-um só sai na soma."},
                               {"O MUX 4:1 e o somador completo estão na biblioteca desta aula.", "A ordem dos D segue os códigos: D0 é 00, a porta E; D3 é 11, a soma."},
                               "Uma fatia da ULA! Quatro delas lado a lado são a ULA de 4 bits, e a CPU de 16 bits usa dezesseis.",
                               [] { return namedSignals({"A", "B", "Ce", "Op1", "Op0"}, {"R", "Cs"}); },
                               {PartKind::And, PartKind::Or, PartKind::Xor}, {"full_adder", "mux4"}, {"A", "B", "Ce", "Op1", "Op0"},
                               {"R", "Cs"});
        l.expect = aluOneBit;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "calculadora-bits";
        l.act = 4;
        l.title = "Projeto: uma calculadora de 2 bits";
        l.story = "Uma calculadora só de portas: com Op em 0 ela faz A + B, e com Op em 1, A − B. A e B têm 2 bits (A1 A0 e B1 B0); o resultado sai em S1 S0, e C é o vai-um (na subtração, C aceso quer dizer A ≥ B).";
        l.steps = {"Subtrair é somar o complemento de dois. Ponha uma porta XOR entre cada bit de B e Op: com Op em 1, o bit sai "
             "invertido.", "Monte um somador completo para o bit 0, com A0, o B0 que sai da porta XOR e, como vai-um de entrada, o "
             "próprio Op: é o + 1 do complemento.", "Monte outro somador completo para o bit 1, com A1, o B1 da porta XOR e o vai-um do bit 0. As somas vão em S0 "
             "e S1, e o último vai-um em C."};
        l.hints = {"Somador completo: S = A XOR B XOR Ve, e Vs = (A E B) OU (Ve E (A XOR B)). São 2 XOR, 2 E e 1 OU.", "A porta XOR com Op é um inversor que se liga e desliga: com Op em 0 o bit passa igual; com Op em 1 sai "
             "invertido.", "Teste 3 − 1: A = 11, B = 01 e Op = 1. Deve dar S1 S0 = 10, e C aceso.", "A tabela tem 32 linhas. Se uma falhar, a linha vermelha mostra quais chaves testar."};
        l.success = "Uma calculadora de verdade, feita só de portas: soma e subtração no mesmo circuito, escolhidas por um bit. A ULA de um processador é isso, com mais bits e mais contas.";
        l.start = calculatorStart;
        l.parts = kGates;
        l.required = {PartKind::Xor};
        l.check = LessonCheck::Table;
        l.inputs = {"Op", "A1", "A0", "B1", "B0"};
        l.outputs = {"C", "S1", "S0"};
        l.expect = calculator;
        all.push_back(l);
    }

    // Act 6: memory.
    {
        Lesson l;
        l.id = "latch-sr";
        l.act = 5;
        l.title = "Um circuito que lembra";
        l.story = "Até aqui a saída dependia só das entradas de agora. Ligando a saída de cada porta na entrada da outra, o circuito passa a "
                  "lembrar: S liga, R desliga, e com os dois em 0 ele fica como estava. É o começo da memória.";
        l.steps = {"Arraste duas portas NOR. Ligue S numa delas e R na outra.", "A saída de cada porta NOR vai na entrada livre da outra.",
                   "A saída da porta NOR do R vai no resistor do LED Q. Teste: S acende, R apaga, e com os dois em 0 o LED fica "
             "como estava."};
        l.hints = {"As duas NORs se seguram uma na outra: é isso que guarda o 1 ou o 0.",
                   "Se o LED acende ao contrário, ligue nele a saída da outra porta NOR."};
        l.success = "Lembrou! Com S e R em 0 o LED continua como estava. Isso é um latch SR, a menor memória que existe: um bit.";
        l.start = latchBoard;
        l.parts = {PartKind::Nor};
        l.check = LessonCheck::Sequence;
        l.outputs = {"Q"};
        l.sequence = {{{{"S", 1}, {"R", 0}}, false, {{"Q", 1}}}, {{{"S", 0}, {"R", 0}}, false, {{"Q", 1}}}, {{{"S", 0}, {"R", 1}}, false, {{"Q", 0}}},
                      {{{"S", 0}, {"R", 0}}, false, {{"Q", 0}}}, {{{"S", 1}, {"R", 0}}, false, {{"Q", 1}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "latch-d";
        l.act = 5;
        l.title = "Latch D: guardar um dado";
        l.story = "O latch SR tem dois botões. O latch D tem um dado, D, e um porteiro, En: com En ligado, Q copia D; com En desligado, Q guarda "
                  "o que tinha, mude D como mudar.";
        l.steps = {"Faça S = D E En, e R = (NÃO D) E En, com portas AND e uma NOT.", "Ligue S e R num latch SR de duas NORs, como na aula anterior.",
                   "O Q do latch vai no resistor do LED Q."};
        l.hints = {"São duas ANDs: cada uma só deixa passar quando En está ligado.", "O R recebe o contrário do D: assim nunca são os dois 1 ao mesmo tempo."};
        l.success = "Um latch D! Com En desligado, mexer em D não muda nada: o bit fica guardado.";
        l.start = dLatchBoard;
        l.parts = {PartKind::Nor, PartKind::And, PartKind::Not};
        l.check = LessonCheck::Sequence;
        l.outputs = {"Q"};
        l.sequence = {{{{"D", 1}, {"En", 1}}, false, {{"Q", 1}}}, {{{"D", 0}, {"En", 1}}, false, {{"Q", 0}}}, {{{"D", 1}, {"En", 0}}, false, {{"Q", 0}}},
                      {{{"D", 1}, {"En", 1}}, false, {{"Q", 1}}}, {{{"D", 0}, {"En", 0}}, false, {{"Q", 1}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "clock-perguntas";
        l.act = 5;
        l.title = "Contar os pulsos";
        l.story = "Sem montar nada desta vez: contas com o clock. Frequência é quantos ciclos por segundo; o período é quanto dura um ciclo.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Hz quer dizer por segundo: 4 Hz são 4 bordas de subida a cada segundo.", "Período = 1 ÷ frequência. Um segundo tem 1000 milissegundos."};
        l.success = "Frequência, período e bordas: a matemática do compasso do computador.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Um clock de 4 Hz faz quantas bordas de subida em 3 segundos?", "", 10, "12", "4 por segundo, vezes 3 segundos: 12.", "Hz é por segundo: 4 Hz são 4 subidas a cada segundo. Multiplique pelos segundos."},
            {"Quanto dura um ciclo desse clock de 4 Hz, em milissegundos?", "", 10, "250", "1 segundo = 1000 ms, dividido em 4 ciclos: 250 ms cada.", "Período = 1 ÷ frequência. Divida 1000 milissegundos por 4."},
            {"Um processador de 2 GHz faz quantos milhões de ciclos por segundo?", "", 10, "2000", "Giga é bilhão: 2 bilhões = 2000 milhões.", "1 GHz é mil milhões de ciclos por segundo."},
            {"O clock passa por 0, 1, 0, 1, 1, 0, 1. Quantas bordas de subida há?", "", 10, "3", "Ele sobe de 0 para 1 na 2ª, na 4ª e na 7ª posição. Ficar em 1 não é borda.", "Conte só as passagens de 0 para 1."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "flip-flop";
        l.act = 5;
        l.title = "O flip-flop e o clock";
        l.story = "O flip-flop D só olha o D no instante em que o clock sobe. Entre um pulso e outro, D pode mudar à vontade que Q não muda. "
                  "É assim que o computador inteiro anda em compasso.";
        l.steps = {"Arraste um flip-flop D, da pasta Memória.", "Ligue D no D dele, o clock no Clk, e o Q no resistor do LED Q.",
                   "Use Avançar meio ciclo (N) e veja: Q só muda quando o clock sobe."};
        l.hints = {"O flip-flop por dentro são dois latches D, um abrindo quando o outro fecha."};
        l.success = "O compasso do computador: tudo muda junto, na subida do clock, e espera o próximo pulso.";
        l.start = flipFlopBoard;
        l.boxes = {"d_flip_flop"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"Q"};
        // Step 4 holds the clock high while D changes: a latch would let the new D through, the flip-flop waits.
        l.sequence = {{{{"D", 0}}, true, {{"Q", 0}}},  {{{"D", 1}}, false, {{"Q", 0}}},           {{}, true, {{"Q", 1}}},
                      {{{"Clock", 1}, {"D", 0}}, false, {{"Q", 1}}}, {{{"Clock", 0}}, false, {{"Q", 1}}}, {{}, true, {{"Q", 0}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "flip-flop-latches";
        l.act = 5;
        l.title = "Flip-flop com dois latches";
        l.story = "Agora por dentro: um flip-flop D é feito de dois latches D em fila. O primeiro, o mestre, fica aberto com o clock em 0; o segundo, o escravo, com o clock em 1. Os dois nunca abrem juntos.";
        l.steps = {"Arraste dois Latch D, da pasta Memória, e uma porta NOT.", "Mestre: o D no D dele, e o clock passando pela NOT no En. Escravo: o Q do mestre no D dele, e o clock direto no En.", "O Q do escravo vai no resistor do LED Q. Teste com Avançar meio ciclo (N)."};
        l.hints = {"A NOT faz o mestre abrir com o clock em 0 e fechar quando ele sobe.", "Se Q muda sem esperar a subida, o clock está chegando direto nos dois latches."};
        l.success = "Você montou o flip-flop por dentro: o mestre segura D até a borda, e o escravo o entrega na saída. Só muda na subida.";
        l.start = flipFlopLatchesBoard;
        l.parts = {PartKind::Not};
        l.boxes = {"d_latch"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"Q"};
        l.sequence = {{{{"D", 0}}, true, {{"Q", 0}}},  {{{"D", 1}}, false, {{"Q", 0}}},           {{}, true, {{"Q", 1}}},
                      {{{"Clock", 1}, {"D", 0}}, false, {{"Q", 1}}}, {{{"Clock", 0}}, false, {{"Q", 1}}}, {{}, true, {{"Q", 0}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "registrador";
        l.act = 5;
        l.title = "Registrador de 8 bits";
        l.story = "Oito flip-flops lado a lado guardam um número de 0 a 255: é um registrador. Com Grava ligado, ele guarda o Dado no próximo pulso "
                  "do clock; com Grava desligado, continua com o que tinha.";
        l.steps = {"Arraste um registrador, da pasta Memória.", "Ligue o Dado no D, o Grava no En, o clock no Clk e o Q no visor S.",
                   "Escolha um número no Dado, ligue Grava e dê um pulso: o visor mostra o número guardado."};
        l.hints = {"O pino Rst pode ficar solto: solto ele vale 0.", "O Dado tem 8 chaves: clique em cada bit para formar o número."};
        l.success = "É assim que os registradores R0 a R7 da CPU guardam os números enquanto o programa roda.";
        l.start = registerBoard;
        l.boxes = {"register"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{{"Dado", 5}, {"Grava", 1}}, true, {{"S", 5}}}, {{{"Dado", 9}, {"Grava", 0}}, true, {{"S", 5}}}, {{{"Grava", 1}}, true, {{"S", 9}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "registrador-flip-flops";
        l.act = 5;
        l.title = "Quatro flip-flops, um número";
        l.story = "Um registrador de 4 bits é isto: quatro flip-flops D, um por bit, todos no mesmo clock. Na borda, os quatro guardam juntos.";
        l.steps = {"Arraste quatro Flip-flop D, da pasta Memória.", "Ligue cada chave (D0 a D3) no D de um flip-flop, e o Q dele no LED do mesmo número.", "Ligue o clock no Clk dos quatro. Teste: escolha os bits e dê um pulso."};
        l.hints = {"O mesmo fio do clock pode ir aos quatro Clk.", "D0 vai com Q0, D1 com Q1, e assim por diante."};
        l.success = "Quatro bits guardados juntos, na mesma borda: um registrador. Com um multiplexador na frente de cada bit, ele ganha a entrada Grava.";
        l.start = fourFlipFlopsBoard;
        l.boxes = {"d_flip_flop"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"Q0", "Q1", "Q2", "Q3"};
        l.sequence = {{{{"D0", 1}, {"D1", 0}, {"D2", 1}, {"D3", 0}}, true, {{"Q0", 1}, {"Q1", 0}, {"Q2", 1}, {"Q3", 0}}},
                      {{{"D0", 0}, {"D1", 1}, {"D2", 1}, {"D3", 1}}, false, {{"Q0", 1}, {"Q1", 0}, {"Q2", 1}, {"Q3", 0}}},
                      {{}, true, {{"Q0", 0}, {"Q1", 1}, {"Q2", 1}, {"Q3", 1}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "contador";
        l.act = 5;
        l.title = "Monte um contador";
        l.story = "Um contador é um registrador que guarda o próprio valor mais 1 a cada pulso. Ele é o coração do PC, que conta qual instrução "
                  "vem a seguir.";
        l.steps = {"Arraste um registrador e um somador de N bits.", "Ligue o Q do registrador no A do somador, e o S do somador no D do registrador.",
                   "Ligue Conta no Ce do somador e no En do registrador, o clock no Clk, e o Q no visor S. Cada pulso soma 1."};
        l.hints = {"O B do somador fica solto, valendo 0: quem soma o 1 é o vai-um de entrada, Ce.",
                   "Os dois já têm 8 bits, iguais ao visor: não precisa mudar a largura."};
        l.success = "1, 2, 3, 4... Um contador feito de um registrador e um somador. O PC da CPU é isso.";
        l.start = counterBoard;
        l.boxes = {"register", "adder_n"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{{"Conta", 1}}, true, {{"S", 1}}}, {{}, true, {{"S", 2}}}, {{}, true, {{"S", 3}}}, {{{"Conta", 0}}, true, {{"S", 3}}},
                      {{{"Conta", 1}}, true, {{"S", 4}}}};
        all.push_back(l);
    }

    {
        Lesson l;
        l.id = "ram-perguntas";
        l.act = 5;
        l.title = "Endereços e palavras";
        l.story = "Sem montar nada desta vez: contas com endereços. Cada bit de endereço dobra o número de palavras.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Com n bits de endereço há 2 elevado a n palavras: 1 bit dá 2, 2 bits dão 4, 3 bits dão 8.", "Os endereços começam no 0."};
        l.success = "Endereços, palavras e tamanhos: é assim que se mede uma memória.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Uma memória com 3 bits de endereço tem quantas palavras?", "", 10, "8", "2 × 2 × 2 = 8: os endereços vão de 0 a 7.", "Cada bit de endereço dobra: 1 bit dá 2, 2 bits dão 4..."},
            {"E com 10 bits de endereço?", "", 10, "1024", "2 elevado a 10 = 1024.", "Dobre 10 vezes, começando de 1: 2, 4, 8, 16..."},
            {"Uma memória de 16 palavras precisa de quantos bits de endereço?", "", 10, "4", "2 × 2 × 2 × 2 = 16.", "Quantas vezes você dobra 1 até chegar a 16?"},
            {"Uma RAM de 16 palavras de 8 bits guarda quantos bits ao todo?", "", 10, "128", "16 palavras × 8 bits = 128 bits.", "Multiplique o número de palavras pelos bits de cada uma."},
            {"Qual é o maior endereço de uma memória de 256 palavras?", "", 10, "255", "Os endereços começam no 0: vão de 0 a 255.", "O primeiro endereço é 0, não 1."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "ram";
        l.act = 5;
        l.title = "Guardar e ler na RAM";
        l.story = "A RAM guarda uma palavra em cada endereço. Lê na hora: Q mostra a palavra do endereço A. Grava só na subida do clock, e só com Grava ligado.";
        l.steps = {"Arraste uma RAM, da pasta Memória.", "Ligue A no A dela, o Dado no D, o Grava no We, o clock no Clk, e o Q no visor S.", "Teste: grave 5 no endereço 1, grave 9 no 2, e volte ao 1 para ler."};
        l.hints = {"A RAM já vem com 8 bits de endereço e 8 de dado, iguais às chaves: não precisa mudar nada.", "We é a permissão de gravar: é ali que vai o Grava."};
        l.success = "Cada endereço guarda a sua palavra, e o visor mostra a do endereço escolhido. A memória do computador é isso, com mais endereços.";
        l.start = ramBoard;
        l.boxes = {"ram"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{{"A", 1}, {"Dado", 5}, {"Grava", 1}}, true, {{"S", 5}}}, {{{"A", 2}, {"Dado", 9}}, true, {{"S", 9}}},
                      {{{"A", 1}, {"Grava", 0}}, false, {{"S", 5}}}, {{{"Dado", 7}}, true, {{"S", 5}}}, {{{"A", 3}}, false, {{"S", 0}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "banco";
        l.act = 5;
        l.title = "O banco de registradores";
        l.story = "Oito registradores numa peça, como na CPU: Rw, Dado e Grava gravam um deles na subida do clock; Ra e Rb escolhem os dois que aparecem em A e B.";
        l.steps = {"Arraste um Banco de registradores, da pasta Memória.", "Ligue Ra, Rb e Rw nos pinos de mesmo nome, o Dado no D, o Grava no We e o clock no Clk.", "Ligue as saídas A e B nos visores A e B. Teste: grave 7 no R2 e 3 no R5, e leia os dois de uma vez."};
        l.hints = {"O banco já vem com 16 bits e 3 bits para escolher o registrador, como as chaves.", "Ra e Rb só leem, e na hora; gravar, só com Grava ligado e na borda."};
        l.success = "Dois registradores lidos e um gravado, no mesmo ciclo: é o que a CPU faz em ADD R3, R1, R2.";
        l.start = registerFileBoard;
        l.boxes = {"regfile"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"A", "B"};
        l.sequence = {{{{"Ra", 2}, {"Rw", 2}, {"Dado", 7}, {"Grava", 1}}, true, {{"A", 7}}}, {{{"Rw", 5}, {"Dado", 3}}, true, {{"A", 7}}},
                      {{{"Rb", 5}, {"Grava", 0}}, false, {{"A", 7}, {"B", 3}}}, {{{"Rw", 2}, {"Dado", 1}}, true, {{"A", 7}, {"B", 3}}}};
        all.push_back(l);
    }

    // Act 8: programming the computer.
    const auto program = [&](const char* id, const char* title, const char* story, std::vector<const char*> steps, std::vector<const char*> hints,
                             const char* success, std::vector<std::uint16_t> shows, std::vector<NeededInstruction> needs) {
        Lesson l;
        l.id = id;
        l.act = 7;
        l.title = title;
        l.story = story;
        l.steps = std::move(steps);
        l.hints = std::move(hints);
        l.success = success;
        l.start = computerStart;
        l.chips = computerChips;
        l.check = LessonCheck::Program;
        l.shows = std::move(shows);
        l.needs = std::move(needs);
        return l;
    };
    // Act 7: machine code. The same computer, programmed with the numbers themselves.
    const auto machine = [&](Lesson l, Board (*start)()) {
        l.act = 6;
        l.start = start;
        l.machineCode = true;
        return l;
    };
    {
        Lesson l;
        l.id = "cpu-perguntas";
        l.act = 6;
        l.title = "As peças e o op";
        l.story = "Sem montar nada desta vez: o ciclo da instrução e os códigos de operação.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Os 4 bits de cima da palavra são o op. A tabela de ops está na aula.", "Em hexadecimal, cada dígito são 4 bits."};
        l.success = "Você já lê uma instrução como a CPU: o op primeiro, depois os detalhes.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quantas etapas o Raio-X mostra para cada instrução?", "", 10, "6", "Busca, decodifica, executa, memória, escreve e próximo PC.", "Conte as linhas da tabela do ciclo."},
            {"Quantos bits tem o op?", "", 10, "4", "Os 4 bits de cima da palavra de 16.", "Com ele há 16 códigos."},
            {"Qual é o op de HLT, em decimal?", "", 10, "15", "HLT é 1111, o último código.", "Procure HLT na tabela de ops."},
            {"Escreva LI R1, 7 em hexadecimal.", "", 16, "2207", "op 2 = 0010, rd 1 = 001, imm9 7 = 000000111: 0010 0010 0000 0111 = 2207.", "Monte os 16 bits e agrupe de quatro em quatro."},
        };
        all.push_back(l);
    }
    all.push_back(machine(program("binario-primeiro", "Três instruções em binário",
                                  "Para a CPU, um programa é uma lista de números de 16 bits. Escreva à mão as três palavras de LI R1, 7, OUT R1 e HLT, "
                                  "campo por campo, para a saída mostrar 7.",
                                  {"O editor de programa abre sozinho: o programa em assembly está nos comentários.",
                                   "Troque cada .word pela palavra certa. LI R1, 7: op 2 (0010), rd 1 (001), imm9 7 (000000111).",
                                   "Faça o mesmo com OUT R1 e HLT, monte com Ctrl+Enter e aperte Rodar."},
                                  {"LI R1, 7 é 0b0010_001_000000111.", "OUT R1: op 11 é 1011, os 3 bits seguintes não são usados, e rs é 001.",
                                   "HLT é 1111 seguido de doze zeros. No Raio-X, a faixa de bits mostra se cada campo ficou certo."},
                                  "7! Você escreveu um programa em linguagem de máquina, do jeito que se fazia antes de existirem montadores.", {7},
                                  {{assembly::kOpOut, -1, "OUT", -1}}),
                          machineFirstStart));
    {
        Lesson l;
        l.id = "campos-perguntas";
        l.act = 6;
        l.title = "Campos e imediatos";
        l.story = "Sem montar nada desta vez: monte e leia os campos das instruções.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Formato R: op(4) rd(3) rs(3) rt(3) func(3). Contas da ULA usam op 0.", "O imm6 tem sinal: -1 é 111111."};
        l.success = "Campos, func e imediatos: o decodificador não sabe nada além disso.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Escreva ADD R3, R1, R2 em hexadecimal.", "", 16, "0653", "0000 011 001 010 011 = 0000 0110 0101 0011 = 0653.", "op 0, rd 3, rs 1, rt 2, func 3. Junte e agrupe de quatro em quatro."},
            {"Qual é o func de SUB, em decimal?", "", 10, "4", "SUB é 100, o func 4.", "Procure SUB na tabela do func."},
            {"Qual é o maior número que cabe no imm9?", "", 10, "511", "9 bits: 2 elevado a 9 = 512 valores, de 0 a 511.", "O maior é 2 elevado a 9, menos 1."},
            {"Em ADDI R1, R1, -1, quanto vale o campo imm6, em binário?", "", 2, "111111", "-1 em complemento de dois, com 6 bits, é tudo 1.", "Ache -1 com 6 bits: inverta 000001 e some 1."},
        };
        all.push_back(l);
    }
    all.push_back(machine(program("binario-soma", "Uma soma em binário",
                                  "Agora uma instrução da ULA: ADD tem cinco campos, e o último, func, diz qual conta fazer. Escreva em binário um "
                                  "programa que guarda 3 e 4, soma os dois e mostra 7.",
                                  {"Os dois LI são como na aula anterior: só mudam rd e imm9.",
                                   "ADD R3, R1, R2: op 0000, rd 011, rs 001, rt 010, func 011 (ADD).",
                                   "Depois, OUT R3 e HLT. Monte e rode."},
                                  {"LI R2, 4 é 0b0010_010_000000100.", "ADD R3, R1, R2 é 0b0000_011_001_010_011.", "OUT R3 é 0b1011_000_011_000000."},
                                  "7 de novo, mas agora quem somou foi a ULA, mandada por uma palavra que você montou bit a bit.", {7},
                                  {{assembly::kOpAlu, assembly::kAdd, "ADD", -1}}),
                          machineSumStart));
    {
        Lesson l;
        l.id = "desvios-perguntas";
        l.act = 6;
        l.title = "Desvios e saltos";
        l.story = "Sem montar nada desta vez: para onde vão os desvios e os saltos.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Desvio: destino = PC + 1 + imm6.", "CALL guarda em R7 o endereço da instrução seguinte."};
        l.success = "Distâncias e endereços: é com eles que o programa anda pela ROM.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Um BEQ no endereço 3, com imm6 = 4, desvia para qual endereço?", "", 10, "8", "3 + 1 + 4 = 8.", "Destino = PC + 1 + imm6."},
            {"Um CALL 10 no endereço 4: que valor fica em R7?", "", 10, "5", "O endereço da instrução seguinte: 4 + 1 = 5.", "R7 guarda para onde voltar."},
            {"Escreva JMP 20 em hexadecimal.", "", 16, "8014", "op 8 = 1000, e 20 em 12 bits é 0000 0001 0100: 8014.", "O op de JMP é 8; o endereço vai nos 12 bits de baixo."},
            {"Um BNE no endereço 4 volta para o endereço 2. Quanto vale o imm6, em binário com 6 bits?", "", 2, "111101", "2 = 4 + 1 + imm6, então imm6 = -3, que com 6 bits é 111101.", "Ache a distância a partir do endereço 5, depois escreva o negativo com 6 bits."},
        };
        all.push_back(l);
    }
    all.push_back(machine(program("binario-laco", "Um laço em binário",
                                  "Um desvio guarda a distância até o destino, e não o endereço. Escreva em binário um laço que mostra 3, 2 e 1: conte "
                                  "para baixo com ADDI e volte com BNE enquanto R1 não for 0.",
                                  {"ADDI R1, R1, -1: op 0001, rd 001, rs 001, e imm6 = -1, que em 6 bits é 111111.",
                                   "BNE R1, R0, laço: o laço está no endereço 1, e o BNE no 3. A distância é 1 - (3 + 1) = -3.",
                                   "-3 em 6 bits é 64 - 3 = 61, ou 111101. BNE é op 0111, rd 001 (R1), rs 000 (R0)."},
                                  {"BNE R1, R0, laço é 0b0111_001_000_111101.", "ADDI R1, R1, -1 é 0b0001_001_001_111111.",
                                   "No Raio-X, a etapa Próximo PC mostra a conta do desvio: PC+1 mais a distância."},
                                  "3, 2, 1! É exatamente esse cálculo de distância que o montador faz por você toda vez que escreve um rótulo.", {3, 2, 1},
                                  {{assembly::kOpBne, -1, "BNE ou BEQ", assembly::kOpBeq}}),
                          machineLoopStart));

    {
        Lesson l;
        l.id = "assembly-perguntas";
        l.act = 7;
        l.title = "Seguindo programas";
        l.story = "Antes de escrever, ler: siga cada programa à mão, anotando os registradores, e diga o que a saída mostra.";
        l.steps = {"Siga o programa à mão, linha por linha, e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Anote o valor de cada registrador depois de cada linha.", "R0 vale sempre 0."};
        l.success = "Quem consegue seguir um programa à mão consegue achar o erro de qualquer um.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"LI R1, 10, depois ADDI R1, R1, -3, depois OUT R1. O que a saída mostra?", "", 10, "7", "10 - 3 = 7.", "ADDI soma um número, que pode ser negativo."},
            {"LI R1, 4, ADD R2, R1, R1, ADD R2, R2, R1, OUT R2. O que a saída mostra?", "", 10, "12", "R2 = 4 + 4 = 8, depois R2 = 8 + 4 = 12.", "Faça uma linha de cada vez."},
            {"Depois de LI R0, 9, quanto vale R0?", "", 10, "0", "R0 vale sempre 0: o 9 se perde.", "R0 é especial."},
            {"LI R1, 300, LI R2, 200, ADD R3, R1, R2, OUT R3. O que a saída mostra?", "", 10, "500", "300 + 200 = 500. Juntando dois LI, passa do limite de 511 de um só.", "Some os dois."},
        };
        all.push_back(l);
    }
    all.push_back(program("primeiro-programa", "O primeiro programa",
                          "O computador de 16 bits está pronto na placa: ele lê as instruções da ROM, uma por uma. Para mudar o que ele faz você não "
                          "mexe em fio nenhum: escreve um programa. Faça a saída mostrar 42.",
                          {"O editor de programa abre sozinho com o exercício; se o fechar, Programar a ROM o abre de novo.", "Escreva LI R1, 42, depois OUT R1, e por fim HLT.",
                           "Ctrl+Enter monta e grava. Feche o editor e aperte Rodar: a saída mostra 42."},
                          {"LI coloca um número num registrador; OUT mostra um registrador na saída.", "Cada instrução numa linha. HLT para o computador."},
                          "Seu primeiro programa rodou! Os fios são os mesmos de antes: só mudou o que está escrito na ROM.", {42},
                          {{assembly::kOpOut, -1, "OUT", -1}}));
    all.push_back(program("conta-grande", "Uma conta na ULA",
                          "LI só aceita números até 511. Para chegar em 512, a ULA tem que trabalhar: guarde 300 num registrador, 212 em outro, some os "
                          "dois e mostre o resultado.",
                          {"LI R1, 300 e LI R2, 212.", "ADD R3, R1, R2 soma e guarda em R3.", "OUT R3, e HLT."},
                          {"No Raio-X dá para ver a ULA somando os dois números."}, "512! A ULA somou dois registradores, do jeito que você montou no Ato 5.",
                          {512}, {{assembly::kOpAlu, assembly::kAdd, "ADD", -1}}));
    {
        Lesson l;
        l.id = "lacos-perguntas";
        l.act = 7;
        l.title = "Seguindo laços";
        l.story = "Siga os laços e os desvios à mão, volta por volta, e responda.";
        l.steps = {"Siga o programa à mão, linha por linha, e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Faça uma tabela: a cada volta, o valor do contador e o que a saída mostrou.", "BNE volta enquanto os dois forem diferentes."};
        l.success = "Laços e memória: com isso já dá para escrever quase qualquer programa.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"LI R1, 1; LI R2, 6; laço: OUT R1; ADDI R1, R1, 1; BNE R1, R2, laço; HLT. Quantos números a saída mostra?", "", 10, "5", "Mostra 1, 2, 3, 4 e 5; com R1 em 6, para.", "Conte as voltas até R1 ficar igual a R2."},
            {"Trocando LI R2, 6 por LI R2, 10, qual é o último número mostrado?", "", 10, "9", "Mostra de 1 a 9; quando R1 chega a 10, o BNE para de voltar.", "O laço para antes de mostrar o valor de R2."},
            {"Com R4 = 10, em que endereço LD R1, 3(R4) lê?", "", 10, "13", "10 + 3 = 13.", "Endereço = base + deslocamento."},
            {"LI R1, 5; LI R2, 5; BEQ R1, R2, igual; LI R3, 0; JMP fim; igual: LI R3, 1; fim: OUT R3. O que a saída mostra?", "", 10, "1", "R1 e R2 são iguais: o BEQ desvia para igual, e R3 = 1.", "O BEQ desvia?"},
        };
        all.push_back(l);
    }
    all.push_back(program("conte-ate-cinco", "Um laço: conte até 5",
                          "Em vez de escrever OUT cinco vezes, um laço repete as mesmas instruções. O desvio BNE volta para cima enquanto a conta não "
                          "chega ao fim.",
                          {"Comece com LI R1, 1 e LI R2, 6: R2 é onde parar.", "Um rótulo marca o começo do laço: laço: OUT R1, ADDI R1, R1, 1.",
                           "BNE R1, R2, laço volta enquanto R1 for diferente de R2. Depois, HLT."},
                          {"Um rótulo é um nome seguido de dois-pontos, no começo da linha.", "BNE quer dizer: desvie se não for igual."},
                          "1, 2, 3, 4, 5: um laço. Quase todo programa é feito de laços como esse.", {1, 2, 3, 4, 5},
                          {{assembly::kOpBne, -1, "BNE ou BEQ", assembly::kOpBeq}}));
    {
        Lesson l = program("guarde-na-memoria", "Guardar na memória",
                           "Os registradores são só oito. A RAM guarda milhares de números: ST grava um registrador na memória, e LD traz de volta. "
                           "Grave 10, 20 e 30 nos endereços 0, 1 e 2, leia de volta e mostre a soma.",
                           {"ST R1, 0(R0) grava R1 no endereço 0; ST R1, 1(R0) no endereço 1.", "Grave 10, 20 e 30, depois leia com LD R1, 0(R0) e some.",
                            "Mostre a soma, 60, com OUT, e termine com HLT."},
                           {"O número antes dos parênteses soma no endereço: 2(R0) é o endereço 2.", "No Painel, a memória aparece no depurador à esquerda."},
                           "A memória guardou os três números e eles voltaram. Variáveis de um programa moram assim, na RAM.", {60},
                           {{assembly::kOpSt, -1, "ST", -1}, {assembly::kOpLd, -1, "LD", -1}});
        l.memory = {{0, 10}, {1, 20}, {2, 30}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "sub-rotinas-perguntas";
        l.act = 7;
        l.title = "Seguindo chamadas";
        l.story = "Siga as chamadas de sub-rotina à mão: para onde o PC vai, o que fica em R7 e o que a saída mostra.";
        l.steps = {"Siga o programa à mão, linha por linha, e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"CALL guarda em R7 o endereço da instrução seguinte.", "Conte os endereços a partir de 0, uma instrução por endereço."};
        l.success = "Ir e voltar sem se perder: é o que toda função faz por baixo.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"LI R1, 3; CALL dobra; OUT R1; HLT; dobra: ADD R1, R1, R1; JR R7. O que a saída mostra?", "", 10, "6", "A sub-rotina dobra R1: 3 + 3 = 6.", "Siga o CALL até o JR R7 e volte."},
            {"Nesse programa, o CALL está no endereço 1. Que valor ele guarda em R7?", "", 10, "2", "O endereço da instrução seguinte, o OUT: 1 + 1 = 2.", "R7 guarda para onde voltar."},
            {"E em que endereço começa a sub-rotina dobra?", "", 10, "4", "LI é 0, CALL é 1, OUT é 2, HLT é 3: dobra é 4.", "Conte as instruções antes do rótulo."},
            {"Se o programa chamar dobra duas vezes seguidas antes do OUT, o que a saída mostra?", "", 10, "12", "3 vira 6, e 6 vira 12.", "Cada chamada dobra R1."},
        };
        all.push_back(l);
    }
    all.push_back(program("sub-rotina", "Sub-rotinas",
                          "Um pedaço de programa que se usa várias vezes vira uma sub-rotina. CALL pula para ela e guarda em R7 o endereço de volta; "
                          "JR R7, no fim dela, volta para logo depois do CALL.",
                          {"Escreva uma sub-rotina dobra: ADD R1, R1, R1 e depois JR R7.", "Comece R1 em 3 e chame CALL dobra três vezes, com OUT R1 depois de cada uma.",
                           "Ponha o HLT antes da sub-rotina, para o programa não cair dentro dela."},
                          {"A saída deve mostrar 6, 12 e 24.", "O rótulo da sub-rotina vai na linha da primeira instrução dela: dobra: ADD R1, R1, R1."},
                          "Sub-rotinas são as funções dos programas. O sistema operacional é feito de muitas delas.", {6, 12, 24},
                          {{assembly::kOpCall, -1, "CALL", -1}, {assembly::kOpJr, -1, "JR", -1}}));

    // Act 9: the operating system. The computer talks to the world, waits for it, and then gets a small
    // system that serves the programs and takes turns between them; the last lesson has the player write
    // the turn-taking itself.
    const auto inAct7 = [&](Lesson l) {
        l.act = 8;
        return l;
    };
    {
        Lesson l;
        l.id = "espera-perguntas";
        l.act = 8;
        l.title = "Esperando o mundo";
        l.story = "Antes de escrever: siga a espera ocupada à mão e conte o tempo que a CPU gasta olhando.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"O laço de espera tem duas instruções: IN e BEQ. Uma instrução por ciclo de clock.", "BEQ volta enquanto R1 for 0."};
        l.success = "Agora você sabe por que esperar desse jeito custa caro: a CPU não faz mais nada.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"espera: IN R1; BEQ R1, R0, espera; OUT R1. As chaves valem 0, 0, 0 e depois 9. O que a saída mostra?", "", 10, "9", "O laço sai quando lê o 9, e o OUT o mostra.", "Quando o BEQ deixa de voltar?"},
            {"Nessa história, quantas vezes o IN executou?", "", 10, "4", "Três leituras de 0 e uma de 9.", "Conte todas as leituras, inclusive a que deu 9."},
            {"Num clock de 1000 Hz, quantas vezes por segundo esse laço lê as chaves?", "", 10, "500", "O laço tem 2 instruções: 1000 ÷ 2 = 500 voltas por segundo.", "Cada volta leva duas instruções."},
        };
        all.push_back(l);
    }
    {
        Lesson l = inAct7(program("entrada-e-saida", "Entrada e saída",
                                  "Um computador precisa conversar com o mundo. Este tem duas portas: IN lê o número das chaves, e OUT mostra um número "
                                  "na saída. Faça um programa que lê as chaves e mostra o dobro do número.",
                                  {"IN R1 lê as chaves e guarda o número em R1.", "ADD R1, R1, R1 dobra o número.",
                                   "OUT R1 mostra, e HLT termina. Mude as chaves e aperte Zerar para testar outros números."},
                                  {"As chaves ficam na placa, à esquerda: clique nos bits para formar um número.",
                                   "A verificação testa com as chaves em 5, 21 e 100."},
                                  "O programa respondeu às chaves! Entrada, conta e saída: é o que todo programa faz.", {},
                                  {{assembly::kOpIn, -1, "IN", -1}, {assembly::kOpOut, -1, "OUT", -1}}));
        l.runs = {{5, -1, 0, {10}}, {21, -1, 0, {42}}, {100, -1, 0, {200}}};
        all.push_back(l);
    }
    {
        Lesson l = inAct7(program("esperar-entrada", "Esperar pela entrada",
                                  "O programa não sabe quando alguém vai mexer nas chaves. Uma saída é ficar perguntando: lê as chaves e, se ainda "
                                  "estiverem em 0, lê de novo. Isso se chama espera ocupada. Faça o programa esperar as chaves saírem do 0 e então "
                                  "mostrar o dobro.",
                                  {"Comece com um rótulo: espera: IN R1.", "BEQ R1, R0, espera volta a ler enquanto R1 for 0.",
                                   "Depois dobre, mostre com OUT e termine com HLT. Rode, e só então mexa nas chaves."},
                                  {"Computadores de verdade também têm interrupções: o hardware avisa a CPU quando algo muda, e ela não precisa "
                                   "ficar perguntando. Este computador ainda não tem.",
                                   "Na verificação, as chaves começam em 0 e mudam depois de algumas instruções."},
                                  "Esperou e respondeu. Enquanto espera, a CPU não faz mais nada: é por isso que os sistemas dão a vez a outros "
                                  "programas.",
                                  {}, {{assembly::kOpIn, -1, "IN", -1}, {assembly::kOpBeq, -1, "BEQ ou BNE", assembly::kOpBne}}));
        l.runs = {{0, 40, 7, {14}}, {0, 90, 12, {24}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "sistema-perguntas";
        l.act = 8;
        l.title = "Serviços e contextos";
        l.story = "Antes de escrever: os serviços do sistema e o que guardar ao trocar de tarefa.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Serviços: 1 mostra, 2 lê as chaves, 3 encerra.", "O contexto é o PC e os registradores que a tarefa usa."};
        l.success = "Serviços e contextos: o coração de todo sistema operacional.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Para mostrar 42 pelo sistema, que número vai em R1?", "", 10, "1", "O serviço 1 mostra o número que está em R2.", "Olhe a tabela de serviços."},
            {"E para ler as chaves?", "", 10, "2", "O serviço 2 lê as chaves e volta com o número em R2.", "Olhe a tabela de serviços."},
            {"Uma tarefa usa R1 a R4 e parou no endereço 30. Quantos valores o sistema guarda no contexto dela, contando o PC?", "", 10, "5", "Os quatro registradores e o PC: 5.", "Some os registradores usados e o PC."},
            {"Num computador de 1000 Hz, se um relógio troca de tarefa a cada 100 instruções, quantas trocas há por segundo?", "", 10, "10", "1000 instruções por segundo ÷ 100 = 10 trocas.", "Uma instrução por ciclo de clock."},
        };
        all.push_back(l);
    }
    {
        Lesson l = inAct7(program("chamadas-de-sistema", "Chamadas de sistema",
                                  "Com um sistema operacional, os programas não mexem direto nas chaves nem na saída: pedem ao sistema, que já está no "
                                  "começo da ROM. Para pedir, ponha o número do serviço em R1 e chame CALL sistema.",
                                  {"Leia as chaves: LI R1, 2 e CALL sistema. O número volta em R2.",
                                   "Mostre R2 (LI R1, 1 e CALL sistema), dobre com ADD R2, R2, R2 e mostre de novo.",
                                   "Encerre com LI R1, 3 e CALL sistema. O seu programa não pode usar IN, OUT nem HLT."},
                                  {"O seu programa começa no rótulo programa, depois do sistema. Escreva só dali para baixo.",
                                   "O sistema usa R6 e R7: guarde o que precisar em R1 a R5."},
                                  "O programa pediu e o sistema fez. Os programas de verdade também conversam com o sistema assim, por chamadas de "
                                  "sistema.",
                                  {}, {{assembly::kOpCall, -1, "CALL", -1}}));
        l.start = callsStart;
        l.withSystem = kCallsProgram;
        l.runs = {{4, -1, 0, {4, 8}}, {9, -1, 0, {9, 18}}};
        all.push_back(l);
    }
    {
        Lesson l = inAct7(program("varias-tarefas", "Duas tarefas ao mesmo tempo",
                                  "Uma CPU faz uma coisa por vez, mas o sistema pode revezar: cada tarefa roda um pouco e passa a vez (serviço 5), e o "
                                  "sistema continua a outra de onde ela parou. A tarefa A já está escrita; escreva a tarefa B.",
                                  {"Na tarefa B, mostre 10 com o serviço 1 e passe a vez com o serviço 5.",
                                   "Faça o mesmo com 20 e 30: um laço com ADDI R5, R5, 10 resolve.",
                                   "A saída deve alternar: 1, 10, 2, 20, 3, 30. O serviço 4, no começo do programa, já cria a tarefa B."},
                                  {"Os registradores são de todas as tarefas: a A usa R3 e R4, então a B usa R5. Um sistema de verdade guarda todos "
                                   "os registradores de cada tarefa na troca.",
                                   "A tarefa B pode nunca terminar: quando a A encerra, o computador para."},
                                  "Duas tarefas revezando a CPU! Rápido o bastante, parece que as duas rodam ao mesmo tempo. É assim que um computador "
                                  "roda vários programas.",
                                  {1, 10, 2, 20, 3, 30}, {{assembly::kOpCall, -1, "CALL", -1}}));
        l.start = tasksStart;
        l.withSystem = kTasksProgram;
        all.push_back(l);
    }
    {
        Lesson l = inAct7(program("sistema-minimo", "O seu sistema operacional",
                                  "Agora o coração do sistema é com você: a troca de tarefas. A memória do sistema guarda, no endereço 0, a tarefa que "
                                  "está rodando (0 é a A, 1 é a B), e nos endereços 1 e 2 onde a A e a B pararam. Quando uma tarefa pede para passar a "
                                  "vez, R7 diz onde ela parou.",
                                  {"Em troca, leia o endereço 0 com LD R6, 0(R0) e veja se é a vez da A ou da B.",
                                   "Se for a A: guarde R7 no endereço 1, marque a B no endereço 0 e pegue no endereço 2 onde a B parou.",
                                   "Se for a B, o contrário. Termine voltando com JR R7."},
                                  {"Para a A: ST R7, 1(R0), depois LI R6, 1 e ST R6, 0(R0), depois LD R7, 2(R0) e JR R7.",
                                   "Para a B: ST R7, 2(R0), ST R0, 0(R0), LD R7, 1(R0) e JR R7. Use BNE R6, R0, ... para separar os dois casos.",
                                   "O sistema pode usar R6 e R7 à vontade: as tarefas não guardam nada neles."},
                                  "Você escreveu um sistema operacional! Ele cria tarefas, atende chamadas e reveza a CPU. Os sistemas de verdade fazem o "
                                  "mesmo, com muito mais tarefas, memória protegida e interrupções.",
                                  {1, 10, 2, 20, 3, 30},
                                  {{assembly::kOpSt, -1, "ST", -1}, {assembly::kOpLd, -1, "LD", -1}, {assembly::kOpJr, -1, "JR", -1}}));
        l.start = minimalSystemStart;
        all.push_back(l);
    }

    // Act 10: more parts, each built from the ones before.
    std::vector<Lesson> extra;
    const auto table = [&](const char* id, const char* title, const char* story, std::vector<const char*> steps, std::vector<const char*> hints,
                           const char* success, Board (*start)(), std::vector<PartKind> parts, std::vector<const char*> boxes,
                           std::vector<const char*> inputs, std::vector<const char*> outputs,
                           std::uint64_t (*expect)(std::span<const std::uint64_t>, std::size_t)) {
        Lesson l;
        l.id = id;
        l.act = 9;
        l.title = title;
        l.story = story;
        l.steps = std::move(steps);
        l.hints = std::move(hints);
        l.success = success;
        l.start = start;
        l.parts = std::move(parts);
        l.boxes = std::move(boxes);
        l.check = LessonCheck::Table;
        l.inputs = std::move(inputs);
        l.outputs = std::move(outputs);
        l.expect = expect;
        return l;
    };
    {
        Lesson l;
        l.id = "paridade-perguntas";
        l.act = 9;
        l.title = "Paridade, MUX e prioridade";
        l.story = "Antes de montar: as três peças do ato, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Paridade ímpar: P é escolhido para que o total de 1s, contando o P, seja ímpar.", "No codificador de prioridade, vence a entrada ligada de número mais alto."};
        l.success = "As três peças, entendidas. Agora é montá-las.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Com paridade ímpar, qual é o bit P para os dados 1011?", "", 10, "0", "1011 já tem três 1, uma quantidade ímpar: com P = 0 o total continua ímpar.", "Conte os 1 dos dados: o total, com P, tem de ficar ímpar."},
            {"E para os dados 0110?", "", 10, "1", "0110 tem dois 1: com P = 1 o total vira três, ímpar.", "Conte os 1 dos dados: o total, com P, tem de ficar ímpar."},
            {"Um MUX de 4 bits tem S = 1, A = 5 e B = 9. Que número sai?", "", 10, "9", "S = 1 escolhe B.", "S = 0 passa A; S = 1 passa B."},
            {"Quantos MUX de 1 bit formam um MUX de 16 bits?", "", 10, "16", "Um por bit, todos com o mesmo S.", "Cada MUX escolhe um bit."},
            {"No codificador de prioridade de 4 entradas, D0, D1 e D2 estão ligadas. Que número sai?", "", 10, "2", "D2 é a ligada de número mais alto.", "Vence a de número mais alto."},
        };
        extra.push_back(l);
    }
    extra.push_back(table("paridade", "Paridade: achar erros",
                          "Quando dados viajam, um bit pode chegar trocado. Um jeito de perceber é a paridade par: junto com os dados vai um bit P "
                          "escolhido para que o total de 1s, contando o P, seja par. Então o LED P acende quando há uma quantidade ímpar de 1s entre "
                          "A, B, C e D. Se um bit mudar no caminho, o total fica ímpar.",
                          {"Ligue A e B numa XOR.", "Ligue o resultado e C em outra porta XOR, e esse resultado e D numa terceira.",
                           "A última XOR vai no resistor do LED P."},
                          {"Cada porta XOR inverte o resultado quando o bit que chega é 1.", "A ordem dos bits não importa: a porta XOR troca de lado sem mudar nada."},
                          "Paridade pronta! Memórias e redes usam um bit assim para avisar que algo chegou errado.", parityBoard,
                          {PartKind::Xor, PartKind::Xnor, PartKind::Not}, {}, {"A", "B", "C", "D"}, {"P"}, oddParity));
    {
        Lesson l = table("multiplexador", "Escolher um sinal",
                         "Um multiplexador escolhe qual entrada vai para a saída: com Sel em 0, S mostra A; com Sel em 1, S mostra B. É como uma "
                         "chave de linha de trem, e a CPU usa dezenas deles para escolher o que vai para cada lugar.",
                         {"Faça A E (NÃO Sel): A só passa com Sel em 0.", "Faça B E Sel: B só passa com Sel em 1.",
                          "Junte os dois num OU e ligue no resistor do LED S."},
                         {"Só uma das duas portas E deixa passar de cada vez, então a porta OU entrega a que passou.",
                          "No Raio-X, os blocos que escolhem o caminho dos números são multiplexadores."},
                         "Um multiplexador de portas. A peça pronta MUX 2:1 faz exatamente isso.", muxBoard,
                         {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"A", "B", "Sel"}, {"S"}, nullptr);
        l.table = "00011011";
        extra.push_back(l);
    }
    extra.push_back(table("prioridade", "Quem pede primeiro",
                          "Quando vários aparelhos pedem a atenção da CPU juntos, o mais importante passa na frente. Um codificador de "
                          "prioridade diz, em binário, qual é a entrada mais alta ligada (A1 A0), e V avisa que alguma está.",
                          {"A1 acende com D3 ou D2: uma porta OU.", "A0 acende com D3, ou com D1 quando D2 está apagada.",
                           "V é uma porta OU das quatro entradas. Ligue cada saída no resistor do seu LED."},
                          {"Para D1 só valer sem D2, use uma porta E com NÃO D2.", "Uma porta OU pode ter mais entradas: selecione-o e use + no painel."},
                          "Prioridade decidida. É assim que um sistema escolhe qual interrupção atender primeiro.", priorityBoard,
                          {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"D0", "D1", "D2", "D3"}, {"A1", "A0", "V"}, priorityOfFour));
    {
        Lesson l;
        l.id = "multiplicar-perguntas";
        l.act = 9;
        l.title = "Negativos e multiplicação";
        l.story = "Antes de montar: trocar o sinal e multiplicar somando, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"−N é NÃO N + 1. Leia o resultado como número sem sinal.", "Para cada bit 1 de B, some A deslocado até aquela casa."};
        l.success = "Sinal e multiplicação, à mão. Agora é montar os circuitos.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Em 4 bits, −5 fica guardado como que número sem sinal?", "", 10, "11", "5 é 0101; invertendo, 1010; somando 1, 1011, que é 11.", "Inverta 0101 e some 1."},
            {"Em 8 bits, −1 fica guardado como que número sem sinal?", "", 10, "255", "1 é 00000001; invertendo, 11111110; somando 1, 11111111, que é 255.", "Todos os bits ficam 1."},
            {"Quanto vale 5 deslocado 3 casas para a esquerda?", "", 10, "40", "Cada casa multiplica por 2: 5 × 8 = 40.", "3 casas: × 2 × 2 × 2."},
            {"6 × 5, somando 6 deslocado nas casas dos bits 1 de 5 (101). Quanto dá?", "", 10, "30", "Bits 0 e 2: 6 + 24 = 30.", "Bit 0: 6 sem deslocar. Bit 2: 6 deslocado 2 casas."},
            {"Multiplicando A por 7, quantas parcelas são somadas, sem contar as que dão 0?", "", 10, "3", "7 é 111: três bits ligados, três parcelas.", "Uma parcela por bit 1 de 7."},
        };
        extra.push_back(l);
    }
    extra.push_back(table("negativo", "Números negativos",
                          "Os computadores guardam −A como NÃO A + 1, o complemento de dois: em 4 bits, −1 é 15 e −3 é 13. A vantagem é que a soma "
                          "comum já funciona com eles. Faça N mostrar −A.",
                          {"Ponha uma porta NÃO e mude para 4 bits (Bits, no painel). Ligue A nela.",
                           "Ligue a saída no A de um somador de N bits com 4 bits.", "Uma constante 1 no Ce soma o 1. Ligue S no visor N."},
                          {"O B do somador fica solto, valendo 0.", "A constante fica na pasta Fios."},
                          "Complemento de dois! A + (−A) dá 0: a mesma soma serve para subtrair.", negativeBoard,
                          {PartKind::Not, PartKind::Constant}, {"adder_n"}, {"A"}, {"N"}, minusOf));
    extra.push_back(table("multiplicar", "Multiplicar em binário",
                          "Em binário, multiplicar bits é uma porta E: 1 × 1 = 1, o resto dá 0. Para multiplicar A e B de 2 bits, faz-se "
                          "como no papel: os produtos de cada par de bits, somados na casa certa. O visor P mostra o resultado, de 0 a 9.",
                          {"Separe A e B em bits. P0 é a0 E b0.", "P1 é a soma de a1 E b0 com a0 E b1: um meio somador.",
                           "P2 soma a1 E b1 com o vai-um de antes, noutro meio somador; o vai-um dele é P3. Junte os quatro no visor."},
                          {"No separador e no juntador, o bit 0 é o de baixo.", "O multiplicador pronto da biblioteca faz isso para N bits."},
                          "Multiplicação feita de portas E e somas: é o multiplicador em matriz, igual ao do exemplo Multiplicador.", multiplyBoard,
                          {PartKind::And, PartKind::Splitter, PartKind::Joiner}, {"half_adder"}, {"A", "B"}, {"P"}, productOf));
    {
        Lesson l;
        l.id = "deslocar-perguntas";
        l.act = 9;
        l.title = "Deslocar e estender à mão";
        l.story = "Antes de montar: deslocar e estender alguns números, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta: em decimal, ou em binário quando ela pedir.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"Uma casa para a esquerda dobra; para a direita, divide por 2 e joga fora o resto.", "Estender com sinal repete o bit de cima."};
        l.success = "Deslocar e estender, à mão. Agora é montar com fios.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Em 8 bits, quanto dá 6 deslocado 3 casas para a esquerda?", "", 10, "48", "Cada casa dobra: 6 × 8 = 48.", "3 casas: × 2 × 2 × 2."},
            {"Em 4 bits, quanto dá 12 (1100) deslocado uma casa para a esquerda?", "", 10, "8", "1100 vira 1000: o 1 de cima se perde, e 24 não cabe em 4 bits.", "Faça em binário e corte em 4 bits."},
            {"Quanto dá 45 deslocado 2 casas para a direita?", "", 10, "11", "45 ÷ 4 = 11, resto 1, e o resto se perde.", "2 casas para a direita: divide por 4."},
            {"Um deslocador com N de 4 bits tem quantos estágios de MUX?", "", 10, "4", "Um por bit de N: de 1, 2, 4 e 8 casas.", "Cada bit de N liga um estágio."},
            {"Estendendo 1110 (4 bits) para 8 bits com sinal, o que sai? Responda em binário.", "", 2, "11111110", "O bit de sinal é 1 e se repete: 11111110, o −2 de 8 bits.", "Repita o bit de cima nos quatro bits novos."},
            {"E sem sinal? Responda em binário.", "", 2, "00001110", "Sem sinal, os bits novos são 0: 00001110, 14.", "Encha de 0."},
        };
        extra.push_back(l);
    }
    extra.push_back(table("deslocar", "Deslocar uma casa",
                          "Deslocar uma casa para a esquerda dobra um número; para a direita, divide por 2. Com Dir em 0, Q mostra A deslocado "
                          "para a esquerda; com Dir em 1, para a direita. O bit que sai se perde, e o que entra é 0.",
                          {"Separe A em bits com um separador de 4.",
                           "Num juntador, ligue um 0 no bit 0 e a0, a1, a2 nos bits 1, 2 e 3: é A para a esquerda. Noutro, a1, a2, a3 nos bits 0, "
                           "1 e 2 e um 0 no bit 3: A para a direita.",
                           "Um MUX 2:1 de 4 bits escolhe entre os dois, com Dir no Sel. Ligue a saída no visor Q."},
                          {"Deslocar uma casa não precisa de porta nenhuma: é só ligar cada bit uma casa ao lado.",
                           "A constante fica na pasta Fios; ponha o valor 0. Uma só serve aos dois juntadores."},
                          "Um deslocador de uma casa. O Deslocador da biblioteca repete isso em estágios de 1, 2 e 4 casas, com um MUX por "
                          "estágio.",
                          shiftOnceBoard, {PartKind::Splitter, PartKind::Joiner, PartKind::Constant}, {"mux2_n"}, {"A", "Dir"}, {"Q"},
                          shiftedOnce));
    extra.push_back(table("estender", "Estender o sinal",
                          "Um número negativo de 4 bits tem de continuar negativo em 8 bits. Para isso, repete-se o bit de cima, o bit de sinal: "
                          "1101 (−3) vira 11111101, que também é −3. Faça Q mostrar A estendido para 8 bits, com o sinal.",
                          {"Separe A em bits com um separador de 4.", "Num juntador de 8, ligue a0 a a3 nos bits 0 a 3.",
                           "Ligue a3, o bit de sinal, também nos bits 4, 5, 6 e 7. Ligue a saída no visor Q."},
                          {"Uma saída pode ir para vários lugares: puxe vários fios da mesma saída do separador.",
                           "Com A positivo, a3 é 0 e os bits de cima ficam 0; com A negativo, ficam 1."},
                          "Extensão de sinal! O Extensor de bits da biblioteca faz isso, e a CPU usa um para completar os imediatos.",
                          extendBoard, {PartKind::Splitter, PartKind::Joiner}, {}, {"A"}, {"Q"}, extendedOf));
    for (Lesson& l : extra) all.push_back(std::move(l));

    // Act 11: input and output.
    {
        Lesson l;
        l.id = "display-perguntas";
        l.act = 10;
        l.title = "Contando segmentos";
        l.story = "Antes de montar: os segmentos de cada algarismo, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"a em cima, b e c à direita, d embaixo, e e f à esquerda, g no meio.", "A tabela da leitura diz quais acendem em cada número."};
        l.success = "Segmentos contados. Agora é montar as portas de cada um.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quantos segmentos acendem para o 8?", "", 10, "7", "O 8 acende todos.", "São sete barras."},
            {"E para o 1?", "", 10, "2", "Só b e c.", "A barra da direita."},
            {"Em quantos algarismos de 0 a 9 o segmento a acende?", "", 10, "8", "Em todos, menos no 1 e no 4.", "Quais algarismos não têm a barra de cima?"},
            {"E o segmento g, o do meio?", "", 10, "7", "Em todos, menos no 0, no 1 e no 7.", "Quais algarismos não têm traço no meio?"},
        };
        all.push_back(l);
    }
    {
        Lesson l = table("sete-segmentos", "Desenhar números",
                         "Um display de 7 segmentos desenha algarismos com sete barras, a até g: a em cima, b e c à direita, d embaixo, e e f "
                         "à esquerda, g no meio. Faça os LEDs a até g desenharem 0, 1, 2 e 3, o número que B1 e B0 formam.",
                         {"Para cada segmento, veja em quais números ele acende: o 0 usa a, b, c, d, e, f; o 1 só b e c; o 2 a, b, d, e, g; o 3 "
                          "a, b, c, d, g.",
                          "Monte cada segmento com portas: o g, por exemplo, só acende no 2 e no 3, ou seja, quando B1 é 1.",
                          "Ligue cada um no resistor do seu LED."},
                         {"b acende nos quatro números: uma constante 1 resolve.", "a e d acendem nos mesmos números, então a mesma porta serve "
                                                                                   "aos dois.",
                          "f só acende no 0: é uma porta NOR de B1 e B0."},
                         "Seu decodificador desenha números! O decodificador pronto faz isso para os 16 dígitos de 0 a F.", segmentsBoard,
                         {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Nor, PartKind::Constant}, {}, {"B1", "B0"},
                         {"a", "b", "c", "d", "e", "f", "g"}, nullptr);
        l.table = "1111110" "0110000" "1101101" "1111001";
        l.act = 10;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "aleatorio-perguntas";
        l.act = 10;
        l.title = "Apertos e sorteios";
        l.story = "Antes de montar: o botão no contador e o LFSR, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"O contador anda uma vez por borda de subida.", "LFSR de 4 bits: os bits sobem e entra embaixo a porta XNOR de q3 e q2."};
        l.success = "Contagem e sorteio, à mão. Agora é montá-los.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Um botão sem debounce dá 3 pulsos por aperto. Depois de 4 apertos, quanto o contador mostra?", "", 10, "12", "Cada aperto conta 3: 4 × 3 = 12.", "Cada pulso é uma borda."},
            {"Um contador de 4 bits mostra 15 e o botão é apertado mais uma vez. O que ele mostra?", "", 10, "0", "16 não cabe em 4 bits: o contador dá a volta e recomeça do 0.", "Qual é o maior número de 4 bits?"},
            {"No LFSR de 4 bits (XNOR de q3 e q2), o que vem depois de 6 (0110)?", "", 10, "12", "q3 = 0 e q2 = 1 são diferentes: entra 0. Fica 1100, 12.", "Veja q3 e q2 de 0110."},
            {"Por quantos valores esse LFSR passa antes de repetir?", "", 10, "15", "Todos os 16, menos o 1111, que fica preso.", "Um valor de 4 bits fica de fora."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "contador-botao";
        l.act = 10;
        l.title = "Contar apertos";
        l.story = "Um botão vale 1 só enquanto está apertado e volta a 0 quando é solto. Ligado no clock de um contador, cada aperto vira "
                  "um pulso: o visor S conta quantas vezes o Botão foi apertado.";
        l.steps = {"Arraste um contador, da pasta Memória, e mude-o para 4 bits (Bits, no painel).", "Ligue o Botão no Clk do contador e o Q no visor S.",
                   "O contador só conta com En em 1: ligue nele uma constante 1."};
        l.hints = {"A constante fica na pasta Fios.", "Teste clicando no Botão: cada clique soma 1."};
        l.success = "Cada aperto conta uma vez. É assim que um teclado avisa o computador de cada tecla.";
        l.start = buttonBoard;
        l.parts = {PartKind::Constant};
        l.boxes = {"counter"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{{"Botão", 1}}, false, {}}, {{{"Botão", 0}}, false, {{"S", 1}}}, {{{"Botão", 1}}, false, {}},
                      {{{"Botão", 0}}, false, {{"S", 2}}}, {{{"Botão", 1}}, false, {}}, {{{"Botão", 0}}, false, {{"S", 3}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "aleatorio";
        l.act = 10;
        l.title = "Números ao acaso";
        l.story = "Computadores não sorteiam: calculam números que parecem sorteados. Um registrador que desloca os bits para "
                  "cima a cada pulso, recebendo embaixo uma porta XNOR dos dois bits mais altos, passa por 15 valores misturados "
                  "antes de repetir.";
        l.steps = {"Arraste um registrador, mude-o para 4 bits e ligue o clock no Clk e uma constante 1 no En.",
                   "Separe o Q em bits. Uma porta XNOR de q3 e q2 é o bit novo.",
                   "Junte, de baixo para cima, o bit novo, q0, q1 e q2, e ligue no D: os bits sobem uma casa. Ligue o Q no visor S."};
        l.hints = {"No juntador, a primeira entrada é o bit 0, o de baixo.", "A partir de 0, o visor deve mostrar 1, 3, 7, 14, 13, 11, 6..."};
        l.success = "Um gerador pseudoaleatório! O exemplo Gerador aleatório faz o mesmo com 8 bits, e jogos usam geradores assim.";
        l.start = shiftBoard;
        l.parts = {PartKind::Xnor, PartKind::Splitter, PartKind::Joiner, PartKind::Constant};
        l.boxes = {"register"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{}, true, {{"S", 1}}},  {{}, true, {{"S", 3}}},  {{}, true, {{"S", 7}}}, {{}, true, {{"S", 14}}},
                      {{}, true, {{"S", 13}}}, {{}, true, {{"S", 11}}}, {{}, true, {{"S", 6}}}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "ascii-perguntas";
        l.act = 10;
        l.title = "Letras em números";
        l.story = "Antes de ligar o teclado: os códigos ASCII, à mão.";
        l.steps = {"Leia a pergunta e digite a resposta em decimal.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"A é 65, a é 97, 0 é 48, espaço é 32.", "As letras e os algarismos vêm em ordem."};
        l.success = "Letras viraram números. Agora é ligá-las à tela.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Qual é o código da letra c minúscula?", "", 10, "99", "a é 97, b é 98, c é 99.", "Conte a partir de a, que é 97."},
            {"Qual é o código do algarismo 7?", "", 10, "55", "0 é 48: 48 + 7 = 55.", "Comece do 0, que é 48."},
            {"Quanto se soma ao código de uma maiúscula para achar a minúscula?", "", 10, "32", "97 − 65 = 32, e vale para todas as letras.", "Compare A e a."},
            {"Digitando \"oi\" no teclado ligado ao terminal, quantos caracteres o terminal guarda?", "", 10, "2", "Uma borda de Tecla por letra: o e i.", "Cada tecla apertada é um pulso."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "teclado-terminal";
        l.act = 10;
        l.title = "Do teclado à tela";
        l.story = "Cada tecla é um número, o código ASCII: a é 97, o espaço é 32. O Teclado entrega esse código em Código e liga Tecla "
                  "enquanto ela está apertada. Ligue-o num terminal, que guarda cada código e desenha a letra, e mostre no visor S quantas "
                  "letras já foram.";
        l.steps = {"Arraste um terminal, da pasta Entrada e saída.", "Ligue Código no Char e Tecla no Clk: cada tecla que desce é um pulso.",
                   "Ponha uma constante 1 no Write e ligue o N do terminal no visor S."};
        l.hints = {"Para testar, clique no Teclado da placa e digite; Esc solta.", "Sem o Write em 1, o terminal não guarda nada."};
        l.success = "Você ligou um teclado a uma tela! É o caminho de cada letra que você digita num computador.";
        l.start = keyboardBoard;
        l.parts = {PartKind::Constant};
        l.boxes = {"terminal"};
        l.check = LessonCheck::Sequence;
        l.outputs = {"S"};
        l.sequence = {{{{"Teclado", 0x80 | 'o'}}, false, {{"S", 1}}}, {{{"Teclado", 'o'}}, false, {{"S", 1}}},
                      {{{"Teclado", 0x80 | 'i'}}, false, {{"S", 2}}}, {{{"Teclado", 'i'}}, false, {{"S", 2}}}};
        all.push_back(l);
    }

    // Act 12: the 32-bit computer, in MIPS.
    {
        Lesson l;
        l.id = "mips-perguntas";
        l.act = 11;
        l.title = "Registradores e endereços";
        l.story = "Sem montar nada desta vez: os registradores, o syscall e os endereços do MIPS.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"A tabela de registradores e a de serviços estão na aula.", "As palavras ficam de 4 em 4 bytes."};
        l.success = "Você já sabe onde o MIPS guarda as coisas. Agora, o primeiro programa.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quantos registradores tem a CPU MIPS?", "", 10, "32", "Numerados de 0 a 31, cada um com 32 bits.", "Veja a tabela de registradores: o último é o 31."},
            {"Qual é o número do registrador $t0?", "", 10, "8", "$t0 a $t7 são os registradores 8 a 15.", "Procure $t0 na tabela de registradores."},
            {"Que número vai em $v0 para o syscall mostrar um número?", "", 10, "1", "O serviço 1 mostra o número que está em $a0.", "Veja a tabela de serviços."},
            {"Uma palavra está em 0x10010000. Em que endereço está a seguinte? Responda em hexadecimal.", "", 16, "10010004", "Uma palavra tem 4 bytes: 0x10010000 + 4.", "Os endereços contam bytes."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "mips-primeiro";
        l.act = 11;
        l.title = "O primeiro programa MIPS";
        l.story = "No MIPS, o programa pede as coisas ao mundo com syscall: põe o serviço em $v0 e o número em $a0. Faça a saída mostrar 42 e termine.";
        l.steps = {"O editor de programa abre sozinho, com o fim do programa já escrito.", "Antes dele, ponha 42 em $a0 com li, e 1 em $v0.", "Chame syscall; as duas linhas do fim terminam o programa. Monte com Ctrl+Enter."};
        l.hints = {"li $a0, 42 põe 42 em $a0.", "O serviço 1 mostra o número que está em $a0.", "A ordem: li $a0, 42; li $v0, 1; syscall; li $v0, 10; syscall."};
        l.success = "42! O seu primeiro programa num conjunto de instruções de verdade.";
        l.start = mipsStart;
        l.chips = mipsChips;
        l.check = LessonCheck::Program;
        l.mips = true;
        l.mipsRuns = {{0, "42"}};
        l.needs = {{mips::kOpSpecial, mips::kFnSyscall, "syscall", -1}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "mips-vetor";
        l.act = 11;
        l.title = "Somar um vetor";
        l.story = "O .data já tem um vetor de quatro palavras e, em n, quantas são. Percorra o vetor com lw, somando, e mostre a soma: 24.";
        l.steps = {"O começo já está escrito: $t0 aponta o vetor, $t1 conta quantas palavras faltam, $t2 guarda a soma.", "No laço: se $t1 for 0, salte para o fim; senão, leia a palavra em 0($t0), some em $t2, avance $t0 4 bytes, diminua $t1 e volte.", "No fim, mostre $t2 com o serviço 1 e termine."};
        l.hints = {"beq $t1, $zero, fim sai do laço quando acabar.", "addi $t0, $t0, 4 vai para a próxima palavra: os endereços contam bytes.", "j laco volta para o começo do laço. Para mostrar, move $a0, $t2."};
        l.success = "24! Você percorreu a memória de 4 em 4 bytes, como todo programa MIPS faz com um vetor.";
        l.start = mipsArrayStart;
        l.chips = mipsChips;
        l.check = LessonCheck::Program;
        l.mips = true;
        l.mipsRuns = {{0, "24"}};
        l.needs = {{mips::kOpLw, -1, "lw", -1}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "formatos-mips-perguntas";
        l.act = 11;
        l.title = "Campos de 32 bits";
        l.story = "Sem montar nada desta vez: monte e leia as palavras do MIPS.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"R: op 6, rs 5, rt 5, rd 5, shamt 5, funct 6.", "Em hexadecimal, cada dígito são 4 bits."};
        l.success = "Formatos R, I e J: a CPU de 32 bits não sabe nada além disso.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Quantos bits tem o op?", "", 10, "6", "Os 6 bits de cima de toda instrução.", "Veja a tabela de formatos."},
            {"Qual é o funct de add, em decimal?", "", 10, "32", "add é funct 100000, 32.", "Está no exemplo de add $t2, $t0, $t1."},
            {"Escreva add $t2, $t0, $t1 em hexadecimal.", "", 16, "01095020", "000000 01000 01001 01010 00000 100000 = 0x01095020.", "op 0, rs 8, rt 9, rd 10, shamt 0, funct 32. Junte e agrupe de quatro em quatro."},
            {"Qual é o maior número que addi aceita no imediato?", "", 10, "32767", "16 bits com sinal vão de −32768 a 32767.", "addi estende o sinal: metade dos 65536 valores é negativa."},
            {"Com $t0 = 0x5A, quanto vale $t1 depois de srl $t1, $t0, 4? Responda em hexadecimal.", "", 16, "5", "Deslocar 4 casas para a direita desce um dígito hexadecimal: sobra o 5.", "Cada dígito hexadecimal são 4 bits."},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "mips-mascara";
        l.act = 11;
        l.title = "Os dois dígitos hexadecimais";
        l.story = "As chaves dão um número de 8 bits, como 0x5A. Mostre o dígito hexadecimal de baixo e depois o de cima, em decimal, com um espaço entre eles: para 0x5A, 10 5.";
        l.steps = {"Leia as chaves com o serviço 5: o número volta em $v0. Guarde-o em $t0.", "andi com 0xF dá o dígito de baixo; srl de 4 casas, e andi de novo, dá o de cima.", "Mostre o de baixo, um espaço (o serviço 11, com 32 em $a0) e o de cima, e termine."};
        l.hints = {"li $v0, 5 e syscall: as chaves vêm em $v0.", "andi $a0, $t0, 0xF põe o dígito de baixo direto em $a0.", "O espaço é o caractere 32: li $a0, 32, li $v0, 11 e syscall."};
        l.success = "Máscara e deslocamento: é assim que a CPU recorta os campos de uma instrução.";
        l.start = mipsStart;
        l.chips = mipsChips;
        l.check = LessonCheck::Program;
        l.mips = true;
        l.mipsRuns = {{0x5A, "10 5"}, {0x3C, "12 3"}, {0xF1, "1 15"}};
        l.needs = {{mips::kOpAndi, -1, "andi", -1}, {mips::kOpSpecial, mips::kFnSrl, "srl", -1}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "desvios-mips-perguntas";
        l.act = 11;
        l.title = "Para onde vai o PC";
        l.story = "Sem montar nada desta vez: as contas de beq, jal e da pilha.";
        l.steps = {"Leia a pergunta e digite a resposta na base que ela pede.", "Enter (ou Responder) confere. Errou? Tente de novo: a pergunta espera."};
        l.hints = {"beq: PC + 4 + imediato × 4.", "jal guarda em $ra o endereço da instrução seguinte."};
        l.success = "Desvios, saltos e voltas: o PC já não tem segredos.";
        l.start = noBoard;
        l.check = LessonCheck::Answer;
        l.questions = {
            {"Um beq em 0x00400008 tem imediato 2. Se desviar, para que endereço vai? Responda em hexadecimal.", "", 16, "00400014", "0x00400008 + 4 = 0x0040000C, mais 2 × 4 = 8: 0x00400014.", "Comece da instrução seguinte e some 4 por instrução."},
            {"Um jal em 0x00400020 põe que valor em $ra? Responda em hexadecimal.", "", 16, "00400024", "O endereço da instrução seguinte: 0x00400020 + 4.", "Para onde a função deve voltar?"},
            {"Quantos bits de endereço cabem na instrução j?", "", 10, "26", "32 bits menos os 6 do op.", "Veja o formato J."},
            {"Quantos bytes a pilha desce para guardar um registrador?", "", 10, "4", "Um registrador é uma palavra, de 4 bytes: addi $sp, $sp, -4.", "Quantos bytes tem uma palavra?"},
        };
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "mips-soma-ate";
        l.act = 11;
        l.title = "Somar de 1 até N";
        l.story = "Leia N das chaves e mostre 1 + 2 + ... + N: para N = 5, 15; para N = 10, 55. Um desvio faz o laço repetir até o contador passar de N.";
        l.steps = {"Leia N com o serviço 5 e guarde em $t0. Ponha 0 na soma, $t1, e 1 no contador, $t2.", "No laço: se o contador passou de N, saia; senão, some o contador na soma, some 1 no contador e volte.", "Mostre a soma e termine."};
        l.hints = {"bgt $t2, $t0, fim sai do laço quando o contador passar de N.", "Ou conte para baixo: some N na soma, diminua N e volte com bne enquanto N não for 0.", "Com N = 0, a soma é 0: o teste de saída vem antes da primeira soma."};
        l.success = "Um laço em MIPS! O desvio volta, o contador anda, e a CPU repete sem você escrever cada passo.";
        l.start = mipsStart;
        l.chips = mipsChips;
        l.check = LessonCheck::Program;
        l.mips = true;
        l.mipsRuns = {{5, "15"}, {0xA, "55"}, {1, "1"}, {0, "0"}};
        l.needs = {{mips::kOpBeq, -1, "beq ou bne", mips::kOpBne}};
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "mips-quadrado";
        l.act = 11;
        l.title = "Uma função: o quadrado";
        l.story = "Escreva a função quadrado: ela recebe um número em $a0 e devolve em $v0 esse número vezes ele mesmo. Este computador não multiplica: some $a0 a si mesmo $a0 vezes. O programa lê as chaves, chama a função com jal e mostra o resultado.";
        l.steps = {"Em main: leia as chaves (serviço 5), copie $v0 para $a0 e chame jal quadrado.", "Em quadrado: $v0 começa em 0 e um contador em $a0. Enquanto o contador não for 0, some $a0 em $v0 e diminua o contador. Volte com jr $ra.", "De volta em main: copie $v0 para $a0, mostre com o serviço 1 e termine."};
        l.hints = {"A função não chama outra, então não precisa da pilha: $ra continua certo até o jr.", "beq $t0, $zero, pronto antes de somar cuida do quadrado de 0.", "Cuidado: li $v0, 1 apaga a resposta. Copie $v0 para $a0 antes."};
        l.success = "Uma função de verdade: argumento em $a0, resposta em $v0, volta com jr $ra. É o combinado de todo programa MIPS.";
        l.start = mipsStart;
        l.chips = mipsChips;
        l.check = LessonCheck::Program;
        l.mips = true;
        l.mipsRuns = {{7, "49"}, {0xC, "144"}, {0, "0"}, {1, "1"}};
        l.needs = {{mips::kOpJal, -1, "jal", -1}, {mips::kOpSpecial, mips::kFnJr, "jr", -1}};
        all.push_back(l);
    }
    // The gates' act: each gate first, ready from the library, then the same gates made of transistors, then of NANDs.
    static constexpr std::string_view kGatesFirst[] = {"portas-perguntas", "porta-e", "porta-ou", "porta-nao", "porta-nand", "porta-nor",
                                                       "porta-tres-entradas", "expressao", "inversor", "tabelas-transistores",
                                                       "nand-transistores", "nor-transistores"};
    const auto place = [](const Lesson& lesson) {
        const auto found = std::find(std::begin(kGatesFirst), std::end(kGatesFirst), std::string_view(lesson.id));
        return static_cast<int>(found - std::begin(kGatesFirst));
    };
    std::vector<Lesson> gates;
    for (const Lesson& lesson : all) {
        if (lesson.act == 3) gates.push_back(lesson);
    }
    std::erase_if(all, [](const Lesson& lesson) { return lesson.act == 3; });
    std::stable_sort(gates.begin(), gates.end(), [&](const Lesson& a, const Lesson& b) { return place(a) < place(b); });
    const auto after = std::find_if(all.begin(), all.end(), [](const Lesson& lesson) { return lesson.act > 3; });
    all.insert(after, gates.begin(), gates.end());
    return all;
}

// --- Checking -----------------------------------------------------------------------------------------

// A switch by name, or an output by name: a real LED or a logic one.
const Part* labelled(const Board& board, std::string_view label, bool input) {
    for (const Part& part : board.parts()) {
        const bool kind = input ? part.kind == PartKind::Toggle || part.kind == PartKind::Button || part.kind == PartKind::Keyboard
                                : isOutputPart(part.kind) || isLed(part.kind);
        if (kind && part.label == label) return &part;
    }
    return nullptr;
}

using i18n::format;
using i18n::tr;

// "A, B e C", in the chosen language.
std::string listed(const std::vector<std::string>& names) {
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) text += i + 1 == names.size() ? tr(" e ") : ", ";
        text += names[i];
    }
    return text;
}

std::string partList(const std::vector<std::string>& names) { return listed(names); }

// What "A = 1, B = 0" says, with the names as the board shows them.
std::string assignments(const std::vector<std::pair<std::string, std::uint64_t>>& values) {
    std::vector<std::string> each;
    for (const auto& [name, value] : values) each.push_back(std::string(tr(name)) + " = " + std::to_string(value));
    return listed(each);
}

// The sentence for an LED, or another output, showing the wrong thing.
std::string wrongOutput(const std::string& when, const Part& output, std::string_view name, std::uint64_t wanted, std::uint64_t got, bool shows) {
    if (isLed(output.kind)) {
        return format(wanted != 0 ? "{}, o LED {} devia acender, mas fica apagado." : "{}, o LED {} devia ficar apagado, mas acende.", {when, tr(name)});
    }
    return format(shows ? "{}, {} devia mostrar {}, mas mostra {}." : "{}, {} devia ser {}, mas está {}.",
                  {when, tr(name), std::to_string(wanted), std::to_string(got)});
}

std::string missingInput(std::string_view name) { return format("Falta a entrada {}. Ela estava na placa: desfaça para trazer de volta.", {tr(name)}); }
std::string missingOutput(std::string_view name) { return format("Falta a saída {}. Ela estava na placa: desfaça para trazer de volta.", {tr(name)}); }

// A part that burnt, in a sentence, or nothing. A burning LED usually takes its resistor and battery
// along; the LED is what the lesson is about, so it's named first.
std::optional<std::string> burnt(const Board& board, const BoardCircuit& circuit) {
    const auto isBurnt = [&](const Part& part) {
        const std::optional<PartReading> reading = circuit.reading(part.id);
        return reading && reading->burnt;
    };
    for (const Part& part : board.parts()) {
        if (isLed(part.kind) && isBurnt(part)) {
            return std::string("O LED queimou: passou corrente demais. Ponha um resistor em série com ele, ou aumente o que já está lá.");
        }
    }
    for (const Part& part : board.parts()) {
        if (!isBurnt(part)) continue;
        if (isBattery(part.kind)) return std::string("A bateria está em curto: o + está ligado direto no -, sem nada no caminho.");
        return format("{} esquentou demais: passou corrente demais por ele.", {tr(partName(part))});
    }
    return std::nullopt;
}

struct LightState {
    bool any = false;
    bool lit = false;
    double milliamps = 0.0;
};

LightState lights(const Board& board, const BoardCircuit& circuit) {
    LightState state;
    for (const Part& part : board.parts()) {
        if (!isLed(part.kind)) continue;
        state.any = true;
        const std::optional<PartReading> reading = circuit.reading(part.id);
        if (!reading) continue;
        state.milliamps = std::max(state.milliamps, reading->amps * 1000.0);
        if (reading->light >= kLedLitAbove && !reading->burnt) state.lit = true;
    }
    return state;
}

std::string milliamps(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", value);
    std::string out = text;
    if (i18n::language() == i18n::Language::Portuguese) std::replace(out.begin(), out.end(), '.', ',');
    return out + " mA";
}

const char* kNotLit = "O LED ainda não acende. A corrente precisa de um caminho fechado: do + da bateria, pelo LED entrando pela perna do +, "
                      "e de volta até o - da bateria.";

LessonResult checkLight(const Lesson& lesson, const Board& board) {
    BoardCircuit circuit(board);
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Switch) circuit.setToggle(part.id, 1);
    }
    circuit.settle();
    if (const std::optional<std::string> problem = burnt(board, circuit)) return {false, *problem};
    const LightState state = lights(board, circuit);
    const bool battery = std::any_of(board.parts().begin(), board.parts().end(), [](const Part& part) { return isBattery(part.kind); });
    if (!battery) return {false, "Ponha uma bateria na placa: sem ela não há corrente nenhuma."};
    if (!state.any) return {false, "Ponha um LED na placa."};
    if (!state.lit) return {false, kNotLit};
    if (lesson.check == LessonCheck::LightCurrent) {
        if (state.milliamps < lesson.minMilliamps) {
            return {false, format("Passam {} no LED: pouco. O objetivo é entre {} e {}. Diminua o resistor.",
                                  {milliamps(state.milliamps), milliamps(lesson.minMilliamps), milliamps(lesson.maxMilliamps)})};
        }
        if (state.milliamps > lesson.maxMilliamps) {
            return {false, format("Passam {} no LED: demais. O objetivo é entre {} e {}. Aumente o resistor.",
                                  {milliamps(state.milliamps), milliamps(lesson.minMilliamps), milliamps(lesson.maxMilliamps)})};
        }
    }
    return {true, lesson.success};
}

LessonResult checkSwitches(const Lesson& lesson, const Board& board) {
    std::vector<PartId> switches;
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Switch) switches.push_back(part.id);
    }
    if (static_cast<int>(switches.size()) != lesson.switches) {
        return {false, lesson.switches == 1 ? std::string(tr("Esta aula usa uma chave.")) : format("Esta aula usa {} chaves.", {std::to_string(lesson.switches)})};
    }
    BoardCircuit circuit(board);
    const unsigned combinations = 1u << switches.size();
    for (unsigned mask = 0; mask < combinations; ++mask) {
        for (std::size_t i = 0; i < switches.size(); ++i) circuit.setToggle(switches[i], (mask >> i) & 1u);
        circuit.settle();
        if (const std::optional<std::string> problem = burnt(board, circuit)) return {false, *problem};
        const LightState state = lights(board, circuit);
        if (!state.any) return {false, "Ponha um LED na placa."};
        const int closed = std::popcount(mask);
        const bool wanted = lesson.lit[closed] == '1';
        if (state.lit != wanted) {
            const char* when = "";
            if (switches.size() == 1) {
                when = closed == 1 ? "Com a chave fechada" : "Com a chave aberta";
            } else if (closed == 0) {
                when = "Com as chaves abertas";
            } else if (closed == static_cast<int>(switches.size())) {
                when = "Com as chaves fechadas";
            } else {
                when = "Com uma chave fechada e a outra aberta";
            }
            if (wanted && mask + 1 == combinations) return {false, std::string(kNotLit)};
            return {false, format(wanted ? "{}, o LED devia acender, mas fica apagado." : "{}, o LED devia ficar apagado, mas acende.", {tr(when)})};
        }
        if (state.lit && lesson.transistorCarries) {
            bool carried = false;
            for (const Part& part : board.parts()) {
                if (!isTransistor(part.kind)) continue;
                const std::optional<PartReading> reading = circuit.reading(part.id);
                if (reading && reading->amps >= 0.001) carried = true;
            }
            if (!carried) {
                return {false, "O LED acende, mas a corrente dele não passa pelo transistor. Ligue a perna sem o + do LED ao coletor (C) e o emissor (E) ao terra."};
            }
        }
    }
    return {true, lesson.success};
}

LessonResult checkTable(const Lesson& lesson, const Board& board) {
    std::vector<const Part*> inputs;
    std::vector<const Part*> outputs;
    for (const char* name : lesson.inputs) {
        const Part* part = labelled(board, name, true);
        if (part == nullptr) return {false, missingInput(name)};
        inputs.push_back(part);
    }
    for (const char* name : lesson.outputs) {
        const Part* part = labelled(board, name, false);
        if (part == nullptr) return {false, missingOutput(name)};
        outputs.push_back(part);
    }
    BoardCircuit circuit(board);
    if (circuit.hasConflict()) return {false, "Duas saídas estão ligadas no mesmo fio: o fio vermelho mostra onde."};
    if (circuit.hasWidthConflict()) return {false, "Um fio liga pinos de larguras diferentes: o fio vermelho mostra onde."};

    unsigned totalBits = 0;
    for (const Part* part : inputs) totalBits += part->width;
    const std::uint64_t rows = std::uint64_t{1} << totalBits;
    std::vector<std::uint64_t> values(inputs.size());
    for (std::uint64_t row = 0; row < rows; ++row) {
        circuit.reset();
        unsigned remaining = totalBits;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            remaining -= inputs[i]->width;
            values[i] = (row >> remaining) & ((std::uint64_t{1} << inputs[i]->width) - 1);
            circuit.setToggle(inputs[i]->id, values[i]);
        }
        circuit.settle();
        std::vector<std::pair<std::string, std::uint64_t>> set;
        for (std::size_t i = 0; i < inputs.size(); ++i) set.emplace_back(lesson.inputs[i], values[i]);
        const std::string when = format("Com {}", {assignments(set)});
        if (circuit.oscillating()) return {false, format("{}, o circuito não para de mudar: tem um laço sem fim em algum lugar.", {when})};
        if (circuit.electricUnsettled()) return {false, format("{}, um sinal fica no meio do caminho entre 0 e 1. Falta um resistor ou um terra?", {when})};
        if (const std::optional<std::string> problem = burnt(board, circuit)) return {false, when + ": " + *problem};
        for (std::size_t o = 0; o < outputs.size(); ++o) {
            const std::uint64_t wanted =
                lesson.expect != nullptr ? lesson.expect(values, o) : static_cast<std::uint64_t>(lesson.table[row * outputs.size() + o] == '1');
            const std::uint64_t got = outputOf(board, circuit, outputs[o]->id);
            if (got == wanted) continue;
            return {false, wrongOutput(when, *outputs[o], lesson.outputs[o], wanted, got, false)};
        }
    }
    return {true, lesson.success};
}

// A circuit that remembers, taken through its steps from a fresh start: each step sets switches, maybe
// pulses the clock, and then looks at the outputs.
// In a sequence step, this name sets the clock's level instead of a switch.
constexpr std::string_view kClockName = "Clock";

LessonResult checkSequence(const Lesson& lesson, const Board& board) {
    BoardCircuit circuit(board);
    if (circuit.hasConflict()) return {false, "Duas saídas estão ligadas no mesmo fio: o fio vermelho mostra onde."};
    if (circuit.hasWidthConflict()) return {false, "Um fio liga pinos de larguras diferentes: o fio vermelho mostra onde."};
    circuit.reset();
    circuit.settle();
    std::vector<std::pair<std::string, std::uint64_t>> now;  // the switches as the steps left them, to say so
    for (std::size_t s = 0; s < lesson.sequence.size(); ++s) {
        const SequenceStep& step = lesson.sequence[s];
        for (const auto& [name, value] : step.set) {
            if (std::string_view(name) == kClockName) {
                circuit.setClocks(value != 0);  // the clock held at a level, to see what happens between edges
            } else {
                const Part* input = labelled(board, name, true);
                if (input == nullptr) return {false, missingInput(name)};
                circuit.setToggle(input->id, value);
            }
            const auto found = std::find_if(now.begin(), now.end(), [&](const auto& pair) { return pair.first == name; });
            if (found == now.end()) {
                now.emplace_back(name, value);
            } else {
                found->second = value;
            }
        }
        circuit.settle();
        if (step.pulse) {
            circuit.setClocks(true);
            circuit.settle();
            circuit.setClocks(false);
            circuit.settle();
            for (auto& [name, value] : now) {
                if (name == kClockName) value = 0;
            }
        }
        std::string when = now.empty() ? format("No passo {}", {std::to_string(s + 1)}) : format("No passo {}, com {}", {std::to_string(s + 1), assignments(now)});
        if (step.pulse) when += tr(", depois de um pulso do clock");
        if (circuit.oscillating()) return {false, format("{}, o circuito não para de mudar: tem um laço sem fim em algum lugar.", {when})};
        for (const auto& [name, wanted] : step.expect) {
            const Part* output = labelled(board, name, false);
            if (output == nullptr) return {false, missingOutput(name)};
            const std::uint64_t got = outputOf(board, circuit, output->id);
            if (got == wanted) continue;
            return {false, wrongOutput(when, *output, name, wanted, got, true)};
        }
    }
    return {true, lesson.success};
}

std::string numberList(const std::vector<std::uint16_t>& values) {
    std::vector<std::string> each;
    for (const std::uint16_t value : values) each.push_back(std::to_string(value));
    return listed(each);
}

// The operating system at the start of a lesson's program, as words: everything before the label
// "programa", assembled with that label put back at its end so the system's jump to it comes out the same.
std::vector<std::uint16_t> systemWords(const char* program) {
    const std::string_view text(program);
    const std::size_t at = text.find("\nprograma:");
    std::vector<std::uint16_t> words = assembly::assemble(std::string(text.substr(0, at + 1)) + "programa: HLT\n").words;
    if (!words.empty()) words.pop_back();
    return words;
}

// A sentence after "With the switches at 5, ": translated, and starting in lower case.
std::string after(const std::string& when, std::string_view sentence) {
    std::string text(tr(sentence));
    if (when.empty()) return text;
    if (!text.empty() && text[0] >= 'A' && text[0] <= 'Z') text[0] = static_cast<char>(text[0] - 'A' + 'a');
    return when + ", " + text;
}

// The switches of a run, as a sentence's start: in hexadecimal too, when the number is more than a digit.
std::string switchesAt(std::uint32_t switches) {
    std::string number = std::to_string(switches);
    if (switches >= 10) {
        std::string hex;
        for (std::uint32_t rest = switches; rest != 0; rest >>= 4) hex.insert(hex.begin(), "0123456789ABCDEF"[rest & 15u]);
        number += " (0x" + hex + ")";
    }
    return format("Com as chaves em {}", {number});
}

// The 32-bit computer's program, run on the model of asm/mips.hpp with the ROM's words and the RAM's data as the
// board has them, once for each run. The model is what the circuit is built to do, and much faster to run.
LessonResult checkMips(const Lesson& lesson, const Board& board) {
    const Part* rom = nullptr;
    const Part* ram = nullptr;
    for (const Part& part : board.parts()) {
        if (part.kind != PartKind::Box || part.width != kWord32Bits) continue;
        if (part.box == boxFromKey("rom") && rom == nullptr) rom = &part;
        if (part.box == boxFromKey("ram") && ram == nullptr) ram = &part;
    }
    if (rom == nullptr) return {false, "O computador de 32 bits não está inteiro na placa: falta a ROM."};
    mips::Program program;
    if (rom->rom != nullptr) {
        for (const std::uint64_t word : *rom->rom) program.text.push_back(static_cast<std::uint32_t>(word));
    }
    if (ram != nullptr && ram->rom != nullptr) {
        for (const std::uint64_t word : *ram->rom) program.data.push_back(static_cast<std::uint32_t>(word));
    }

    for (const NeededInstruction& needed : lesson.needs) {
        const bool used = std::any_of(program.text.begin(), program.text.end(), [&](std::uint32_t word) {
            const unsigned op = word >> 26;
            if (op == needed.op) return needed.func < 0 || static_cast<int>(word & 63u) == needed.func;
            return needed.alsoOp >= 0 && op == static_cast<unsigned>(needed.alsoOp);
        });
        if (!used) return {false, format("O programa precisa usar {}. Clique em Programar a ROM para mudar o código.", {tr(needed.name)})};
    }

    for (const MipsRun& run : lesson.mipsRuns) {
        mips::Machine machine = mips::boot(program);
        machine.switches = run.switches;
        constexpr std::size_t kMostInstructions = 100000;
        machine.run(kMostInstructions);
        const std::string when = run.switches != 0 ? switchesAt(run.switches) : std::string();
        if (!machine.fault.empty()) return {false, after(when, format("O programa parou: {}.", {machine.fault}))};
        if (!machine.halted) {
            return {false, after(when, "O programa não termina: ele roda para sempre. Confira os desvios e, no fim, termine com li $v0, 10 e syscall.")};
        }
        if (machine.output == run.prints) continue;
        const std::string wanted = "\"" + std::string(run.prints) + "\"";
        if (machine.output.empty()) {
            return {false, after(when, format("A saída não mostrou nada: falta um syscall com o serviço 1. O objetivo é mostrar {}.", {wanted}))};
        }
        return {false, after(when, format("A saída mostrou {}. O objetivo é mostrar {}.", {"\"" + machine.output + "\"", wanted}))};
    }
    return {true, lesson.success};
}

// The program in the computer's ROM, run on the circuit itself until it stops, once for each run.
LessonResult checkProgram(const Lesson& lesson, const Board& board, const ChipBoards* chips) {
    if (chips == nullptr) return {false, "O computador não está inteiro na placa."};
    BoardCircuit circuit(board, chips);
    circuit.reset();
    circuit.settle();
    std::optional<ComputerState> state = readComputer(board, circuit, *chips);
    if (!state) return {false, "O computador não está inteiro na placa: a CPU, a ROM e a RAM precisam estar ligadas."};

    const Part* rom = board.part(state->rom);
    const std::vector<std::uint64_t> none;
    const std::vector<std::uint64_t>& words = rom != nullptr && rom->rom != nullptr ? *rom->rom : none;
    const auto opOf = [](std::uint64_t word) { return static_cast<unsigned>(word >> 12) & 15u; };

    // In machine code, every line with something on it is a .word: a label before it, or a comment after, at most.
    if (lesson.machineCode && rom != nullptr) {
        std::size_t start = 0;
        const std::string& source = rom->source;
        while (start < source.size()) {
            std::size_t end = source.find('\n', start);
            if (end == std::string::npos) end = source.size();
            std::string line = source.substr(start, end - start);
            start = end + 1;
            if (const std::size_t comment = line.find(';'); comment != std::string::npos) line.resize(comment);
            if (const std::size_t colon = line.find(':'); colon != std::string::npos) line.erase(0, colon + 1);
            const std::size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue;
            std::string word = line.substr(first, 5);
            for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (word != ".word") {
                return {false, "Nesta aula o programa vai só em binário: cada linha é um .word com o número da instrução, sem nomes como LI ou OUT."};
            }
        }
    }

    // With an operating system, the system stays as it is, and the program after it asks the system for
    // the switches, the output and the end.
    if (lesson.withSystem != nullptr) {
        const std::vector<std::uint16_t> system = systemWords(lesson.withSystem);
        const bool intact = words.size() >= system.size() && std::equal(system.begin(), system.end(), words.begin());
        if (!intact) {
            return {false, "O sistema operacional mudou. Ele vai do começo da ROM até o rótulo programa, e o seu programa começa ali: desfaça o que "
                           "mudou nele, ou use Recomeçar."};
        }
        for (std::size_t i = system.size(); i < words.size(); ++i) {
            const unsigned op = opOf(words[i]);
            if (op == assembly::kOpOut) {
                return {false, "O seu programa não pode usar OUT: só o sistema mexe na saída. Peça a ele com LI R1, 1 e CALL sistema."};
            }
            if (op == assembly::kOpIn) {
                return {false, "O seu programa não pode usar IN: só o sistema lê as chaves. Peça a ele com LI R1, 2 e CALL sistema."};
            }
            if (op == assembly::kOpHalt) return {false, "O seu programa não pode usar HLT: peça ao sistema para encerrar, com LI R1, 3 e CALL sistema."};
        }
    }

    for (const NeededInstruction& needed : lesson.needs) {
        const bool used = std::any_of(words.begin(), words.end(), [&](std::uint64_t word) {
            const unsigned op = opOf(word);
            if (op == needed.op) return needed.func < 0 || static_cast<int>(word & 7u) == needed.func;
            return needed.alsoOp >= 0 && op == static_cast<unsigned>(needed.alsoOp);
        });
        if (!used) return {false, format("O programa precisa usar {}. Clique em Programar a ROM para mudar o código.", {tr(needed.name)})};
    }

    std::vector<ProgramRun> runs = lesson.runs;
    if (runs.empty()) runs.push_back({0, -1, 0, lesson.shows});
    const Part* switches = labelled(board, "Chaves", true);
    for (const ProgramRun& run : runs) {
        const bool changes = run.changeAt >= 0;
        if ((run.switches != 0 || changes) && switches == nullptr) return {false, missingInput("Chaves")};
        circuit.reset();
        if (switches != nullptr) circuit.setToggle(switches->id, run.switches);
        circuit.settle();
        state = readComputer(board, circuit, *chips);
        if (!state) return {false, "O computador deixou de estar inteiro na placa."};
        const std::string when = changes              ? format("Quando as chaves mudam para {}", {std::to_string(run.later)})
                                 : run.switches != 0 ? format("Com as chaves em {}", {std::to_string(run.switches)})
                                                     : std::string();

        std::vector<std::uint16_t> shown;
        std::uint16_t output = state->output;
        constexpr int kMostInstructions = 3000;
        int count = 0;
        for (; count < kMostInstructions && !state->halted; ++count) {
            if (count == run.changeAt) {
                circuit.setToggle(switches->id, run.later);
                circuit.settle();
            }
            circuit.setClocks(true);
            circuit.settle();
            circuit.setClocks(false);
            circuit.settle();
            state = readComputer(board, circuit, *chips);
            if (!state) return {false, "O computador deixou de estar inteiro na placa."};
            if (state->output != output) {
                output = state->output;
                shown.push_back(output);
            }
        }
        if (changes && state->halted && count <= run.changeAt) {
            return {false, tr("O programa parou antes de alguém mexer nas chaves. Ele precisa esperar: ler as chaves de novo enquanto estiverem em 0.")};
        }
        if (!state->halted) {
            return {false, after(when, lesson.withSystem != nullptr ? "O programa não termina: ele roda para sempre. Confira os desvios e, no fim, peça ao "
                                                                       "sistema para encerrar (serviço 3)."
                                                                     : "O programa não chega no HLT: ele roda para sempre. Confira os desvios e ponha um "
                                                                       "HLT no fim.")};
        }
        const bool ends = shown.size() >= run.shows.size() && std::equal(run.shows.rbegin(), run.shows.rend(), shown.rbegin());
        if (!ends) {
            if (shown.empty()) return {false, after(when, format("A saída não mostrou nada: falta um OUT. O objetivo é mostrar {}.", {numberList(run.shows)}))};
            return {false, after(when, format("A saída mostrou {}. O objetivo é mostrar {}.", {numberList(shown), numberList(run.shows)}))};
        }
        for (const auto& [address, value] : lesson.memory) {
            const std::uint16_t got = address < state->memory.size() ? state->memory[address] : 0;
            if (got != value) {
                return {false, format("No endereço {} da memória devia estar {}, mas está {}.", {std::to_string(address), std::to_string(value), std::to_string(got)})};
            }
        }
    }
    return {true, lesson.success};
}

}  // namespace

const std::vector<LessonAct>& lessonActs() {
    static const std::vector<LessonAct> acts{
        {"Ato 1: Eletricidade", "Bateria, LED, resistor e chaves", true, {}},
        {"Ato 2: Transistores", "A chave que se liga sozinha", true, {}},
        {"Ato 3: Bits", "Zeros e uns: como os fios guardam números", true,
         {{"Ponto flutuante", "Números com vírgula: sinal, expoente e mantissa"}}},
        {"Ato 4: Portas lógicas", "E, OU, NÃO, NAND e XOR", true,
         {{"Simplificar circuitos", "Mapas de Karnaugh: o mesmo circuito com menos portas"}}},
        {"Ato 5: Aritmética", "Somar com portas", true, {}},
        {"Ato 6: Memória", "Circuitos que lembram", true,
         {{"Máquinas de estados", "Circuitos que seguem passos: um semáforo, uma fechadura com senha"}}},
        {"Ato 7: Linguagem de máquina", "A CPU por dentro e as instruções em binário", true,
         {{"Pipeline", "Várias instruções ao mesmo tempo, cada uma numa etapa"},
          {"Medir o desempenho", "Ciclos por instrução, frequência e o tempo de um programa"},
          {"Cache e hierarquia de memória", "Uma memória pequena e rápida perto da CPU"}}},
        {"Ato 8: Programação", "Assembly no computador de 16 bits", true,
         {{"Linguagem de alto nível e compilador", "Do if e do while ao assembly"}}},
        {"Ato 9: Sistema operacional", "O programa que cuida dos outros", true,
         {{"Memória virtual", "Cada programa com os seus próprios endereços"}}},
        {"Ato 10: Mais peças", "Paridade, seleção e contas", true, {}},
        {"Ato 11: Entrada e saída", "Displays, botões e teclado", true,
         {{"Interrupções e DMA", "O dispositivo chama a CPU, e a memória recebe dados sem ela"}}},
        {"Ato 12: O computador de 32 bits", "Registradores, formatos e funções no MIPS", true, {}},
    };
    return acts;
}

const std::vector<Lesson>& lessons() {
    static const std::vector<Lesson> all = makeLessons();
    return all;
}

bool answers(const Question& question, std::string_view typed) {
    const auto number = [&](std::string_view text) -> std::optional<std::uint64_t> {
        std::string kept;
        for (const char c : text) {
            if (c != ' ' && c != '_') kept += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (kept.size() > 2 && kept[0] == '0' && (kept[1] == 'X' || kept[1] == 'B')) kept = kept.substr(2);
        if (kept.empty() || kept.size() > 20) return std::nullopt;
        std::uint64_t value = 0;
        for (const char c : kept) {
            const int digit = std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
            if (digit >= question.base) return std::nullopt;
            value = value * static_cast<std::uint64_t>(question.base) + static_cast<std::uint64_t>(digit);
        }
        return value;
    };
    const std::optional<std::uint64_t> wanted = number(question.answer);
    const std::optional<std::uint64_t> got = number(typed);
    return wanted && got && *wanted == *got;
}

int lessonIndex(std::string_view id) {
    const std::vector<Lesson>& all = lessons();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (id == all[i].id) return static_cast<int>(i);
    }
    return -1;
}

std::string lessonVersion(const Lesson& lesson) {
    const Board board = lesson.start();
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a, over everything that makes up the board
    const auto mix = [&](std::int64_t value) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ull;
    };
    for (const Part& part : board.parts()) {
        mix(static_cast<std::int64_t>(part.kind));
        mix(part.position.x);
        mix(part.position.y);
        mix(part.rotation);
        mix(static_cast<std::int64_t>(part.value));
        mix(part.box);
        mix(part.width);
        for (const char c : part.label) mix(c);
    }
    for (const Wire& wire : board.wires()) {
        mix(wire.from.id);
        mix(wire.from.pin);
        mix(wire.to.id);
        mix(wire.to.pin);
    }
    char text[12];
    std::snprintf(text, sizeof text, "%08llx", static_cast<unsigned long long>(hash & 0xFFFFFFFFull));
    return text;
}

std::vector<PartKind> lessonLibrary(const Lesson& lesson) {
    // Both ways of powering a circuit: the battery, and the source with its ground.
    static constexpr PartKind kBasics[] = {PartKind::BatteryCell, PartKind::Battery, PartKind::Ground, PartKind::Resistor, PartKind::LedBulb,
                                           PartKind::Light};
    std::vector<PartKind> all = lesson.parts;
    for (const PartKind kind : kBasics) {
        if (std::find(all.begin(), all.end(), kind) == all.end()) all.push_back(kind);
    }
    return all;
}

bool lessonAllows(const Lesson& lesson, PartKind kind, BoxId box) {
    if (kind == PartKind::Box) {
        return std::any_of(lesson.boxes.begin(), lesson.boxes.end(), [&](const char* key) { return boxFromKey(key) == box; });
    }
    const std::vector<PartKind> library = lessonLibrary(lesson);
    return std::find(library.begin(), library.end(), kind) != library.end();
}

LessonResult checkLesson(const Lesson& lesson, const Board& board, const ChipBoards* chips) {
    // Only the lesson's parts, and whatever the board started with.
    const Board start = lesson.start();
    std::vector<std::string> extra;
    for (const Part& part : board.parts()) {
        const bool fromStart = std::any_of(start.parts().begin(), start.parts().end(), [&](const Part& s) { return s.kind == part.kind && s.box == part.box; });
        if (fromStart || lessonAllows(lesson, part.kind, part.box) || part.kind == PartKind::Tunnel) continue;
        const std::string name(tr(partName(part)));
        if (std::find(extra.begin(), extra.end(), name) == extra.end()) extra.push_back(name);
    }
    if (!extra.empty()) return {false, format("Esta aula é sem {}: use só as peças da biblioteca da aula.", {partList(extra)})};
    for (const PartKind kind : lesson.required) {
        const bool present = std::any_of(board.parts().begin(), board.parts().end(), [&](const Part& part) { return part.kind == kind; });
        if (!present) return {false, format("Use pelo menos um {}.", {tr(partName(makePart(kind)))})};
    }
    switch (lesson.check) {
        case LessonCheck::LightOn:
        case LessonCheck::LightCurrent: return checkLight(lesson, board);
        case LessonCheck::LightSwitches: return checkSwitches(lesson, board);
        case LessonCheck::Table: return checkTable(lesson, board);
        case LessonCheck::Sequence: return checkSequence(lesson, board);
        case LessonCheck::Program: return lesson.mips ? checkMips(lesson, board) : checkProgram(lesson, board, chips);
        case LessonCheck::Answer: return {false, "Responda as perguntas na tela."};
    }
    return {};
}

}  // namespace bancada::editor
