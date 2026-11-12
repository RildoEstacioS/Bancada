#include "editor/lessons.hpp"

#include <algorithm>
#include <cctype>
#include <bit>
#include <cstdio>
#include <optional>

#include "asm/assembler.hpp"
#include "editor/boxes.hpp"
#include "editor/computer16.hpp"
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

Board notTransistor() { return transistorGate({"A"}); }
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
        l.success = "Você fez um E com as mãos: uma chave E a outra. Logo os transistores vão fazer isso sozinhos.";
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
        l.id = "inversor";
        l.act = 1;
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
        l.id = "nand-transistores";
        l.act = 1;
        l.title = "NAND com dois transistores";
        l.story = "Agora são dois transistores em fila, um em cima do outro. O atalho para o terra só se abre quando os dois estão ligados: "
                  "A E B. Então o LED só apaga com A e B em 1. Isso é a NAND, a porta que sozinha faz todas as outras.";
        l.steps = {"Cada entrada vai por um resistor até a base de um transistor.",
                   "Ligue o coletor do transistor de cima no ponto entre o resistor e o LED S.",
                   "Ligue o emissor de cima ao coletor de baixo, e o emissor de baixo ao terra."};
        l.hints = {"É o NÃO da aula anterior com mais um transistor embaixo, em série.",
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
        l.act = 1;
        l.title = "NOR com dois transistores";
        l.story = "Lado a lado, em paralelo, cada transistor abre o seu próprio atalho para o terra. Qualquer um ligado já apaga o LED: "
                  "ele só acende quando A e B são 0. Isso é uma NOR.";
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

    // Act 3: logic gates.
    {
        Lesson l;
        l.id = "porta-e";
        l.act = 2;
        l.title = "Acenda um LED com uma porta";
        l.story = "Daqui em diante cada porta é uma caixinha pronta, feita dos transistores que você já montou. A saída de uma porta tem "
                  "força para acender um LED de verdade, com o resistor de sempre. A porta E (AND) liga a saída só quando todas as entradas estão ligadas.";
        l.steps = {"Arraste uma porta AND.", "Ligue A e B nas entradas da porta.",
                   "Ligue a saída da porta na ponta livre do resistor do LED S, e teste clicando em A e B."};
        l.hints = {"As entradas da porta ficam à esquerda e a saída à direita.", "A ponta livre do resistor é a da esquerda: o outro lado já vai para o LED."};
        l.success = "O LED S só acende com A e B ligadas. Por dentro dessa porta há uma NAND e um NÃO, ou seja, três transistores.";
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
        l.id = "nao-com-nand";
        l.act = 2;
        l.title = "NÃO feito de NAND";
        l.story = "A NAND é universal: com ela dá para fazer qualquer porta. Comece pela mais simples. Se as duas entradas de uma NAND "
                  "recebem o mesmo sinal, o que ela faz?";
        l.steps = {"Arraste uma NAND.", "Ligue A nas duas entradas dela.", "Ligue a saída no resistor do LED S."};
        l.hints = {"NAND de A com A: 0 vira 1 e 1 vira 0."};
        l.success = "Uma NAND com as entradas juntas é um NÃO. Guarde esse truque: ele vai aparecer muito.";
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
        l.act = 2;
        l.title = "E feito de NAND";
        l.story = "NAND quer dizer NÃO-E: é um E com a saída invertida. Então, para ter um E, basta inverter a saída de uma NAND de novo.";
        l.steps = {"Ligue A e B numa NAND.", "Faça um NÃO com outra NAND, como na aula anterior.",
                   "Passe a saída da primeira pelo NÃO até o resistor do LED S."};
        l.hints = {"Duas inversões se anulam."};
        l.success = "E feito só de NAND. Cada porta nova é feita das anteriores: é assim que o computador cresce.";
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
        l.act = 2;
        l.title = "OU feito de NAND";
        l.story = "Um desafio: o LED tem que acender quando A OU B estiver ligada, usando só NANDs. A NAND só dá 0 quando as duas entradas são 1... "
                  "e se as entradas chegarem invertidas?";
        l.steps = {"Inverta A com uma NAND, e B com outra.", "Ligue as duas entradas invertidas numa terceira NAND.",
                   "Ligue a saída dela no resistor do LED S."};
        l.hints = {"Pense ao contrário: S só é 0 quando A e B são 0, ou seja, quando NÃO A e NÃO B são 1."};
        l.success = "Três NANDs fazem um OU. Isso tem nome: lei de De Morgan.";
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
        l.act = 2;
        l.title = "OU exclusivo";
        l.story = "O XOR liga quando as entradas são diferentes: uma ou outra, mas não as duas. Ele é o coração da soma: em binário, 1 + 1 dá 0 e vai 1.";
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
        Lesson l = gatesLesson("decodificador", 2, "Decodificador: um fio por número",
                               "Dois bits, A1 e A0, formam um número de 0 a 3. Um decodificador acende só a saída desse número: Y0 com 00, Y3 "
                               "com 11. A CPU usa um destes para escolher qual registrador vai ser escrito.",
                               {"Faça NÃO A1 e NÃO A0 com duas portas NOT.", "Cada saída é uma E: Y0 quer NÃO A1 e NÃO A0; Y3 quer A1 e A0.",
                                "Y1 é 01 (NÃO A1 e A0) e Y2 é 10 (A1 e NÃO A0). Ligue cada E no resistor do seu LED."},
                               {"Cada E reconhece um número só: as quatro nunca acendem juntas.", "Os inversores podem alimentar várias portas."},
                               "Um decodificador de portas, igual à peça pronta Decodificador 2:4.",
                               [] { return namedSignals({"A1", "A0"}, {"Y0", "Y1", "Y2", "Y3"}); },
                               {PartKind::And, PartKind::Not}, {}, {"A1", "A0"}, {"Y0", "Y1", "Y2", "Y3"});
        l.table = "1000" "0100" "0010" "0001";
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("mux4", 2, "Escolher entre quatro",
                               "Um multiplexador 4:1 escolhe uma de quatro entradas, D0 a D3, pelo número que S1 e S0 formam. É ele que, dentro "
                               "da ULA, escolhe qual operação vai para o resultado.",
                               {"Faça NÃO S1 e NÃO S0.", "Para cada D, uma E de três entradas que só deixa passar com o seu número: D2, por "
                                                         "exemplo, com S1 e NÃO S0.",
                                "Junte as quatro E num OU de quatro entradas e ligue no resistor do LED Y."},
                               {"Selecione uma porta e use + no painel para dar mais entradas a ela.", "É o decodificador da aula anterior, com "
                                                                                                       "cada saída abrindo a passagem de um D."},
                               "Um multiplexador de quatro entradas. A ULA do computador tem vários deles.",
                               [] { return namedSignals({"S1", "S0", "D0", "D1", "D2", "D3"}, {"Y"}); },
                               {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"S1", "S0", "D0", "D1", "D2", "D3"}, {"Y"});
        l.expect = fourWayChoice;
        all.push_back(l);
    }

    // Act 4: arithmetic.
    {
        Lesson l;
        l.id = "meio-somador";
        l.act = 3;
        l.title = "Meio somador";
        l.story = "Somar dois bits dá um resultado de dois bits: 0+0=0, 0+1=1, 1+1=10 (dois, em binário). O bit de baixo é a soma S, "
                  "o de cima é o vai-um C.";
        l.steps = {"O LED S acende quando A e B são diferentes: que porta faz isso?", "O LED C acende só quando A e B são 1.",
                   "Ligue as duas portas nos resistores dos LEDs S e C."};
        l.hints = {"S é um XOR, C é um E."};
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
        l.act = 3;
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
        l.act = 3;
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

    // Act 4, more: subtracting, comparing, and the ALU slice.
    {
        Lesson l = gatesLesson("subtrator", 3, "Subtrator completo",
                               "Subtrair um bit é parecido com somar: D é A − B − Ve, e Vs, o empresta-um, liga quando a conta fica abaixo de "
                               "zero e precisa pedir à coluna de cima. Ve é o pedido que chega da coluna de baixo.",
                               {"D é A XOR B XOR Ve: duas XORs, como na soma.", "Vs liga com NÃO A e B, ou com NÃO (A XOR B) e Ve.",
                                "Junte essas duas E num OU e ligue no resistor do LED Vs."},
                               {"Teste 0 − 1: D e Vs acendem, porque 0 − 1 dá 1 pedindo emprestado.", "NÃO (A XOR B) é 1 quando A e B são "
                                                                                                       "iguais."},
                               "Subtrator pronto. Enfileirados, eles subtraem números de qualquer tamanho, como os somadores.",
                               [] { return namedSignals({"A", "B", "Ve"}, {"D", "Vs"}); },
                               {PartKind::Xor, PartKind::And, PartKind::Or, PartKind::Not}, {}, {"A", "B", "Ve"}, {"D", "Vs"});
        l.expect = subtractBit;
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("comparador", 3, "Comparar dois bits",
                               "Um comparador diz se A é maior, igual ou menor que B. É com ele que um programa decide: o BEQ da CPU desvia "
                               "quando dois números são iguais.",
                               {"A>B é A E NÃO B.", "A<B é NÃO A E B.", "A=B é a XNOR de A e B. Ligue cada uma no resistor do seu LED."},
                               {"Só um dos três LEDs pode acender de cada vez.", "A=B também é uma NOR de A>B com A<B: nem maior, nem menor."},
                               "Comparador pronto. Para números de muitos bits, compara-se do bit mais alto para baixo.",
                               [] { return namedSignals({"A", "B"}, {"A>B", "A=B", "A<B"}); },
                               {PartKind::And, PartKind::Not, PartKind::Xnor, PartKind::Nor}, {}, {"A", "B"}, {"A>B", "A=B", "A<B"});
        l.expect = compareBits;
        all.push_back(l);
    }
    {
        Lesson l = gatesLesson("ula-1-bit", 3, "Uma fatia da ULA",
                               "A ULA faz uma de quatro operações com A e B, escolhida por Op1 e Op0: 00 E, 01 OU, 10 XOR, 11 soma. Monte as "
                               "quatro e deixe um multiplexador escolher. Cs é o vai-um, mas só na soma.",
                               {"Faça A E B, A OU B e A XOR B com portas, e A + B + Ce com um somador completo.",
                                "Num MUX 4:1 pronto, ligue as quatro em D0 a D3, Op0 no S0 e Op1 no S1. A saída vai no LED R.",
                                "Cs é o Cs do somador E (Op1 E Op0): o vai-um só sai na soma."},
                               {"O MUX 4:1 e o somador completo estão na biblioteca desta aula.", "A ordem dos D segue os códigos: D0 é 00, a E; "
                                                                                                  "D3 é 11, a soma."},
                               "Uma fatia da ULA! Quatro delas lado a lado são a ULA de 4 bits, e a CPU de 16 bits usa dezesseis.",
                               [] { return namedSignals({"A", "B", "Ce", "Op1", "Op0"}, {"R", "Cs"}); },
                               {PartKind::And, PartKind::Or, PartKind::Xor}, {"full_adder", "mux4"}, {"A", "B", "Ce", "Op1", "Op0"},
                               {"R", "Cs"});
        l.expect = aluOneBit;
        all.push_back(l);
    }

    // Act 5: memory.
    {
        Lesson l;
        l.id = "latch-sr";
        l.act = 4;
        l.title = "Um circuito que lembra";
        l.story = "Até aqui a saída dependia só das entradas de agora. Ligando a saída de cada porta na entrada da outra, o circuito passa a "
                  "lembrar: S liga, R desliga, e com os dois em 0 ele fica como estava. É o começo da memória.";
        l.steps = {"Arraste duas portas NOR. Ligue S numa delas e R na outra.", "A saída de cada NOR vai na entrada livre da outra.",
                   "A saída da NOR do R vai no resistor do LED Q. Teste: S acende, R apaga, e com os dois em 0 o LED fica como estava."};
        l.hints = {"As duas NORs se seguram uma na outra: é isso que guarda o 1 ou o 0.",
                   "Se o LED acende ao contrário, ligue nele a saída da outra NOR."};
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
        l.act = 4;
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
        l.id = "flip-flop";
        l.act = 4;
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
        l.id = "registrador";
        l.act = 4;
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
        l.id = "contador";
        l.act = 4;
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

    // Act 7: programming the computer.
    const auto program = [&](const char* id, const char* title, const char* story, std::vector<const char*> steps, std::vector<const char*> hints,
                             const char* success, std::vector<std::uint16_t> shows, std::vector<NeededInstruction> needs) {
        Lesson l;
        l.id = id;
        l.act = 6;
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
    // Act 6: machine code. The same computer, programmed with the numbers themselves.
    const auto machine = [&](Lesson l, Board (*start)()) {
        l.act = 5;
        l.start = start;
        l.machineCode = true;
        return l;
    };
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
                          {"No Raio-X dá para ver a ULA somando os dois números."}, "512! A ULA somou dois registradores, do jeito que você montou no Ato 4.",
                          {512}, {{assembly::kOpAlu, assembly::kAdd, "ADD", -1}}));
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
    all.push_back(program("sub-rotina", "Sub-rotinas",
                          "Um pedaço de programa que se usa várias vezes vira uma sub-rotina. CALL pula para ela e guarda em R7 o endereço de volta; "
                          "JR R7, no fim dela, volta para logo depois do CALL.",
                          {"Escreva uma sub-rotina dobra: ADD R1, R1, R1 e depois JR R7.", "Comece R1 em 3 e chame CALL dobra três vezes, com OUT R1 depois de cada uma.",
                           "Ponha o HLT antes da sub-rotina, para o programa não cair dentro dela."},
                          {"A saída deve mostrar 6, 12 e 24.", "O rótulo da sub-rotina vai na linha da primeira instrução dela: dobra: ADD R1, R1, R1."},
                          "Sub-rotinas são as funções dos programas. O sistema operacional é feito de muitas delas.", {6, 12, 24},
                          {{assembly::kOpCall, -1, "CALL", -1}, {assembly::kOpJr, -1, "JR", -1}}));

    // Act 8: the operating system. The computer talks to the world, waits for it, and then gets a small
    // system that serves the programs and takes turns between them; the last lesson has the player write
    // the turn-taking itself.
    const auto inAct7 = [&](Lesson l) {
        l.act = 7;
        return l;
    };
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

    // Act 9: more parts, each built from the ones before.
    std::vector<Lesson> extra;
    const auto table = [&](const char* id, const char* title, const char* story, std::vector<const char*> steps, std::vector<const char*> hints,
                           const char* success, Board (*start)(), std::vector<PartKind> parts, std::vector<const char*> boxes,
                           std::vector<const char*> inputs, std::vector<const char*> outputs,
                           std::uint64_t (*expect)(std::span<const std::uint64_t>, std::size_t)) {
        Lesson l;
        l.id = id;
        l.act = 8;
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
    extra.push_back(table("paridade", "Paridade: achar erros",
                          "Quando dados viajam, um bit pode chegar trocado. Um jeito de perceber é a paridade: o LED P acende quando há uma "
                          "quantidade ímpar de 1s entre A, B, C e D. Se um bit mudar no caminho, P muda junto.",
                          {"Ligue A e B numa XOR.", "Ligue o resultado e C em outra XOR, e esse resultado e D numa terceira.",
                           "A última XOR vai no resistor do LED P."},
                          {"Cada XOR inverte o resultado quando o bit que chega é 1.", "A ordem dos bits não importa: a XOR troca de lado sem mudar nada."},
                          "Paridade pronta! Memórias e redes usam um bit assim para avisar que algo chegou errado.", parityBoard,
                          {PartKind::Xor, PartKind::Xnor, PartKind::Not}, {}, {"A", "B", "C", "D"}, {"P"}, oddParity));
    {
        Lesson l = table("multiplexador", "Escolher um sinal",
                         "Um multiplexador escolhe qual entrada vai para a saída: com Sel em 0, S mostra A; com Sel em 1, S mostra B. É como uma "
                         "chave de linha de trem, e a CPU usa dezenas deles para escolher o que vai para cada lugar.",
                         {"Faça A E (NÃO Sel): A só passa com Sel em 0.", "Faça B E Sel: B só passa com Sel em 1.",
                          "Junte os dois num OU e ligue no resistor do LED S."},
                         {"Só uma das duas E deixa passar de cada vez, então o OU entrega a que passou.",
                          "No Raio-X, os blocos que escolhem o caminho dos números são multiplexadores."},
                         "Um multiplexador de portas. A peça pronta MUX 2:1 faz exatamente isso.", muxBoard,
                         {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"A", "B", "Sel"}, {"S"}, nullptr);
        l.table = "00011011";
        extra.push_back(l);
    }
    extra.push_back(table("prioridade", "Quem pede primeiro",
                          "Quando vários aparelhos pedem a atenção da CPU juntos, o mais importante passa na frente. Um codificador de "
                          "prioridade diz, em binário, qual é a entrada mais alta ligada (A1 A0), e V avisa que alguma está.",
                          {"A1 acende com D3 ou D2: um OU.", "A0 acende com D3, ou com D1 quando D2 está apagada.",
                           "V é um OU das quatro entradas. Ligue cada saída no resistor do seu LED."},
                          {"Para D1 só valer sem D2, use uma E com NÃO D2.", "Um OU pode ter mais entradas: selecione-o e use + no painel."},
                          "Prioridade decidida. É assim que um sistema escolhe qual interrupção atender primeiro.", priorityBoard,
                          {PartKind::And, PartKind::Or, PartKind::Not}, {}, {"D0", "D1", "D2", "D3"}, {"A1", "A0", "V"}, priorityOfFour));
    extra.push_back(table("negativo", "Números negativos",
                          "Os computadores guardam −A como NÃO A + 1, o complemento de dois: em 4 bits, −1 é 15 e −3 é 13. A vantagem é que a soma "
                          "comum já funciona com eles. Faça N mostrar −A.",
                          {"Ponha uma porta NÃO e mude para 4 bits (Bits, no painel). Ligue A nela.",
                           "Ligue a saída no A de um somador de N bits com 4 bits.", "Uma constante 1 no Ce soma o 1. Ligue S no visor N."},
                          {"O B do somador fica solto, valendo 0.", "A constante fica na pasta Fios."},
                          "Complemento de dois! A + (−A) dá 0: a mesma soma serve para subtrair.", negativeBoard,
                          {PartKind::Not, PartKind::Constant}, {"adder_n"}, {"A"}, {"N"}, minusOf));
    extra.push_back(table("multiplicar", "Multiplicar em binário",
                          "Em binário, multiplicar bits é uma E: 1 × 1 = 1, o resto dá 0. Para multiplicar A e B de 2 bits, faz-se como no papel: "
                          "os produtos de cada par de bits, somados na casa certa. O visor P mostra o resultado, de 0 a 9.",
                          {"Separe A e B em bits. P0 é a0 E b0.", "P1 é a soma de a1 E b0 com a0 E b1: um meio somador.",
                           "P2 soma a1 E b1 com o vai-um de antes, noutro meio somador; o vai-um dele é P3. Junte os quatro no visor."},
                          {"No separador e no juntador, o bit 0 é o de baixo.", "O multiplicador pronto da biblioteca faz isso para N bits."},
                          "Multiplicação feita de E e somas: é o multiplicador em matriz, igual ao do exemplo Multiplicador.", multiplyBoard,
                          {PartKind::And, PartKind::Splitter, PartKind::Joiner}, {"half_adder"}, {"A", "B"}, {"P"}, productOf));
    for (Lesson& l : extra) all.push_back(std::move(l));

    // Act 10: input and output.
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
                          "f só acende no 0: é uma NOR de B1 e B0."},
                         "Seu decodificador desenha números! O decodificador pronto faz isso para os 16 dígitos de 0 a F.", segmentsBoard,
                         {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Nor, PartKind::Constant}, {}, {"B1", "B0"},
                         {"a", "b", "c", "d", "e", "f", "g"}, nullptr);
        l.table = "1111110" "0110000" "1101101" "1111001";
        l.act = 9;
        all.push_back(l);
    }
    {
        Lesson l;
        l.id = "contador-botao";
        l.act = 9;
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
        l.act = 9;
        l.title = "Números ao acaso";
        l.story = "Computadores não sorteiam: calculam números que parecem sorteados. Um registrador que desloca os bits para cima a cada "
                  "pulso, recebendo embaixo uma XNOR dos dois bits mais altos, passa por 15 valores misturados antes de repetir.";
        l.steps = {"Arraste um registrador, mude-o para 4 bits e ligue o clock no Clk e uma constante 1 no En.",
                   "Separe o Q em bits. Uma XNOR de q3 e q2 é o bit novo.",
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
        l.id = "teclado-terminal";
        l.act = 9;
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
        {"Ato 3: Portas lógicas", "E, OU, NÃO, NAND e XOR", true, {}},
        {"Ato 4: Aritmética", "Somar com portas", true, {}},
        {"Ato 5: Memória", "Circuitos que lembram", true, {}},
        {"Ato 6: Linguagem de máquina", "A CPU por dentro e as instruções em binário", true, {}},
        {"Ato 7: Programação", "Assembly no computador de 16 bits", true, {}},
        {"Ato 8: Sistema operacional", "O programa que cuida dos outros", true, {}},
        {"Ato 9: Mais peças", "Paridade, seleção e contas", true, {}},
        {"Ato 10: Entrada e saída", "Displays, botões e teclado", true, {}},
    };
    return acts;
}

const std::vector<Lesson>& lessons() {
    static const std::vector<Lesson> all = makeLessons();
    return all;
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
        case LessonCheck::Program: return checkProgram(lesson, board, chips);
    }
    return {};
}

}  // namespace bancada::editor
