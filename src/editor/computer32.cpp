#include "editor/computer32.hpp"

#include <memory>
#include <string>
#include <utility>

#include "editor/layout.hpp"
#include "i18n/i18n.hpp"

namespace bancada::editor {

namespace {

constexpr std::uint64_t bit(unsigned index) { return std::uint64_t{1} << index; }
std::uint64_t regDst(unsigned which) { return static_cast<std::uint64_t>(which) << kC32RegDstLow; }
std::uint64_t writeBack(unsigned which) { return static_cast<std::uint64_t>(which) << kC32WbLow; }
std::uint64_t aluOp(unsigned op) { return static_cast<std::uint64_t>(op) << kC32AluOpLow; }

// Where a register write goes, and what is written.
enum : unsigned { kDstRt = 0, kDstRd = 1, kDstRa = 2, kDstV0 = 3 };
enum : unsigned { kBackAlu = 0, kBackMemory = 1, kBackNext = 2, kBackSwitches = 3 };

// A gate as wide as a bus: every bit of it its own gate.
PartId busGate(Layout& l, PartKind kind, Point at, std::uint8_t width, std::uint8_t inputs = 2) {
    Part part = makePart(kind, at, 0, kind == PartKind::Not ? 0 : inputs);
    part.width = width;
    return l.add(std::move(part));
}

PartId romBox(Layout& l, Point at, std::uint8_t width, std::uint8_t address, std::vector<std::uint64_t> rows) {
    Part rom = makePart(PartKind::Box, at, 0, 0, 1, boxFromKey("rom"));
    rom.width = width;
    rom.address = address;
    rom.rom = std::make_shared<const std::vector<std::uint64_t>>(std::move(rows));
    return l.add(std::move(rom));
}

ChipData newChip(ChipId id, const char* name) {
    ChipData chip;
    chip.id = id;
    chip.name = name;
    return chip;
}

// Takes an instruction apart, with wires only. The top half splits into rt, rs and op (5, 5 and 6 bits), just
// as a splitter shares 16 bits among three; the bottom half is the immediate, and bit by bit it gives funct,
// shamt and rd.
ChipData decoderChip(ChipId id) {
    ChipData chip = newChip(id, "Decodificador");
    Layout l(chip.board);
    const PartId in = l.input("Instr", {0, 30}, 32);
    const PartId halves = l.bundle(PartKind::Splitter, {22, 30}, 32, 2);
    l.wire(in, 0u, halves, 0u);
    const PartId low = l.bundle(PartKind::Splitter, {32, 0}, 16, 16);
    const PartId high = l.bundle(PartKind::Splitter, {32, 40}, 16, 3);
    l.wire(halves, 1u, low, 0u);
    l.wire(halves, 2u, high, 0u);

    constexpr int kOutputs = 58;
    const auto joined = [&](const char* name, unsigned first, std::uint8_t count) {
        const PartId join = l.bundle(PartKind::Joiner, {44, l.pinY(low, 1 + first) - 1}, count, count);
        for (unsigned i = 0; i < count; ++i) l.wire(low, 1 + first + i, join, i);
        const PartId out = l.output(name, {kOutputs, l.pinY(join, count) - 1}, count);
        l.wire(join, count, out, 0u);
    };
    joined("Funct", 0, 6);
    joined("Shamt", 6, 5);
    joined("Rd", 11, 5);
    const auto direct = [&](const char* name, unsigned branch, std::uint8_t width) {
        const PartId out = l.output(name, {kOutputs, l.pinY(high, 1 + branch) - 1}, width);
        l.wire(high, 1 + branch, out, 0u);
    };
    direct("Rt", 0, 5);
    direct("Rs", 1, 5);
    direct("Op", 2, 6);
    const PartId imm = l.output("Imm", {kOutputs, 52}, 16);
    l.wire(halves, 1u, imm, 0u);
    return chip;
}

// Two ROMs, as in the textbooks' main control and ALU control: the opcode picks a row of the first; when that
// row says the instruction is an R type (opcode 0), the row the funct picks in the second is used instead.
ChipData controlChip(ChipId id) {
    ChipData chip = newChip(id, "Controle");
    Layout l(chip.board);
    const PartId op = l.input("Op", {0, 1}, 6);
    const PartId funct = l.input("Funct", {0, 21}, 6);
    const PartId main = romBox(l, {14, 0}, kControl32Bits, 6, mainControlRows());
    const PartId byFunct = romBox(l, {14, 20}, kControl32Bits, 6, functControlRows());
    l.wire(op, 0u, main, "A");
    l.wire(funct, 0u, byFunct, "A");

    const PartId which = l.box("bit_select", {32, 8}, kControl32Bits, 5);
    const PartId useFunct = l.constant(kC32UseFunct, {16, 10}, 5);
    l.wire(main, "Q", which, "D");
    l.wire(useFunct, 0u, which, "Sel");
    const PartId chosen = l.box("mux2_n", {48, 14}, kControl32Bits);
    l.wire(main, "Q", chosen, "A");
    l.wire(byFunct, "Q", chosen, "B");
    l.wire(which, "S", chosen, "Sel");

    const PartId lines = l.bundle(PartKind::Splitter, {64, 0}, kControl32Bits, kControl32Bits);
    l.wire(chosen, "S", lines, 0u);
    constexpr int kOutputs = 74;
    const auto single = [&](const char* name, unsigned line) {
        const PartId out = l.output(name, {kOutputs, l.pinY(lines, 1 + line) - 1});
        l.wire(lines, 1 + line, out, 0u);
    };
    const auto field = [&](const char* name, unsigned first, std::uint8_t count) {
        const PartId join = l.bundle(PartKind::Joiner, {kOutputs, l.pinY(lines, 1 + first) - 1}, count, count);
        for (unsigned i = 0; i < count; ++i) l.wire(lines, 1 + first + i, join, i);
        const PartId out = l.output(name, {kOutputs + 8, l.pinY(join, count) - 1}, count);
        l.wire(join, count, out, 0u);
    };
    single("RegEsc", kC32RegWrite);
    single("MemEsc", kC32MemWrite);
    single("UsaImm", kC32AluImm);
    single("SinalImm", kC32SignExtend);
    field("RegDest", kC32RegDstLow, 2);
    field("Volta", kC32WbLow, 2);
    single("SeIgual", kC32BranchEq);
    single("SeDif", kC32BranchNe);
    single("Salto", kC32Jump);
    single("SaltoR", kC32JumpReg);
    single("Sistema", kC32Syscall);
    single("Inválida", kC32Invalid);
    field("OpULA", kC32AluOpLow, 4);
    return chip;
}

// The ALU, out of parts a player has: the gates for the logic, one adder for adding and subtracting (B turned
// over and a carry in, which is two's complement), the sign bits for slt, and shifters for the shifts.
ChipData aluChip(ChipId id) {
    ChipData chip = newChip(id, "ULA 32 bits");
    Layout l(chip.board);
    const PartId a = l.input("A", {0, 0}, 32);
    const PartId b = l.input("B", {0, 30}, 32);
    const PartId op = l.input("Op", {0, 70}, 4);
    const PartId shamt = l.input("Shamt", {0, 90}, 5);

    // Subtracting (sub, slt, sltu: operations 5, 6 and 7) turns B over and carries a 1 in.
    const PartId opBits = l.bundle(PartKind::Splitter, {16, 66}, 4, 4);
    l.wire(op, 0u, opBits, 0u);
    const PartId lowAny = l.gate(PartKind::Or, {26, 64});
    l.wire(opBits, 1u, lowAny, 0u);
    l.wire(opBits, 2u, lowAny, 1u);
    const PartId notTop = l.gate(PartKind::Not, {26, 72});
    l.wire(opBits, 4u, notTop, 0u);
    Part three = makePart(PartKind::And, {36, 66}, 0, 3);
    const PartId subtracting = l.add(std::move(three));
    l.wire(lowAny, 2u, subtracting, 0u);
    l.wire(opBits, 3u, subtracting, 1u);
    l.wire(notTop, 1u, subtracting, 2u);
    const PartId everyBit = l.box("extender", {46, 66}, 32, 1);
    const PartId on = l.constant(1, {38, 76}, 1);
    l.wire(subtracting, 3u, everyBit, "A");
    l.wire(on, 0u, everyBit, "Sinal");
    const PartId turned = busGate(l, PartKind::Xor, {30, 32}, 32);
    l.wire(b, 0u, turned, 0u);
    l.wire(everyBit, "Q", turned, 1u);
    const PartId adder = l.box("adder_n", {46, 30}, 32);
    l.wire(a, 0u, adder, "A");
    l.wire(turned, 2u, adder, "B");
    l.wire(subtracting, 3u, adder, "Ce");

    const PartId andGate = busGate(l, PartKind::And, {46, 0}, 32);
    const PartId orGate = busGate(l, PartKind::Or, {46, 6}, 32);
    const PartId xorGate = busGate(l, PartKind::Xor, {46, 12}, 32);
    const PartId norGate = busGate(l, PartKind::Nor, {46, 18}, 32);
    for (const PartId gate : {andGate, orGate, xorGate, norGate}) {
        l.wire(a, 0u, gate, 0u);
        l.wire(b, 0u, gate, 1u);
    }

    // slt: when the signs differ, A is the smaller if it's the negative one; when they're the same, the
    // difference can't overflow, and its sign says. sltu: there was a borrow, no carry out.
    const PartId top = l.constant(31, {58, 44}, 5);
    const PartId signA = l.box("bit_select", {70, 40}, 32, 5);
    const PartId signB = l.box("bit_select", {70, 48}, 32, 5);
    const PartId signDiff = l.box("bit_select", {70, 56}, 32, 5);
    l.wire(a, 0u, signA, "D");
    l.wire(b, 0u, signB, "D");
    l.wire(adder, "S", signDiff, "D");
    for (const PartId sign : {signA, signB, signDiff}) l.wire(top, 0u, sign, "Sel");
    const PartId differ = l.gate(PartKind::Xor, {86, 42});
    l.wire(signA, "S", differ, 0u);
    l.wire(signB, "S", differ, 1u);
    const PartId less = l.box("mux2", {96, 50});
    l.wire(signDiff, "S", less, "A");
    l.wire(signA, "S", less, "B");
    l.wire(differ, 2u, less, "Sel");
    const PartId borrow = l.gate(PartKind::Not, {86, 34});
    l.wire(adder, "Cs", borrow, 0u);
    const PartId off = l.constant(0, {98, 62}, 1);
    const PartId lessWord = l.box("extender", {110, 50}, 32, 1);
    const PartId borrowWord = l.box("extender", {110, 34}, 32, 1);
    l.wire(less, "S", lessWord, "A");
    l.wire(borrow, 1u, borrowWord, "A");
    l.wire(off, 0u, lessWord, "Sinal");
    l.wire(off, 0u, borrowWord, "Sinal");

    // The shifts move B: by shamt, or by 16 for lui. sra is srl of B turned over, turned back, when B is
    // negative, so the new bits copy the sign.
    const PartId left = l.constant(0, {14, 104}, 1);
    const PartId right = l.constant(1, {14, 110}, 1);
    const PartId sixteen = l.constant(16, {14, 116}, 5);
    const PartId sll = l.box("shifter", {46, 80}, 32, 5);
    const PartId srl = l.box("shifter", {46, 90}, 32, 5);
    const PartId notB = busGate(l, PartKind::Not, {30, 100}, 32);
    const PartId srlNot = l.box("shifter", {46, 100}, 32, 5);
    const PartId lui = l.box("shifter", {46, 112}, 32, 5);
    l.wire(b, 0u, notB, 0u);
    l.wire(b, 0u, sll, "A");
    l.wire(b, 0u, srl, "A");
    l.wire(notB, 1u, srlNot, "A");
    l.wire(b, 0u, lui, "A");
    l.wire(shamt, 0u, sll, "N");
    l.wire(shamt, 0u, srl, "N");
    l.wire(shamt, 0u, srlNot, "N");
    l.wire(sixteen, 0u, lui, "N");
    l.wire(left, 0u, sll, "Dir");
    l.wire(right, 0u, srl, "Dir");
    l.wire(right, 0u, srlNot, "Dir");
    l.wire(left, 0u, lui, "Dir");
    const PartId backOver = busGate(l, PartKind::Not, {62, 102}, 32);
    l.wire(srlNot, "Q", backOver, 0u);
    const PartId sra = l.box("mux2_n", {74, 92}, 32);
    l.wire(srl, "Q", sra, "A");
    l.wire(backOver, 1u, sra, "B");
    l.wire(signB, "S", sra, "Sel");

    // Four groups of four, picked by the two halves of Op: logic, arithmetic, shifts, and nothing.
    const PartId opHalves = l.bundle(PartKind::Splitter, {16, 80}, 4, 2);
    l.wire(op, 0u, opHalves, 0u);
    const PartId logic = l.box("mux4_n", {126, 0}, 32);
    l.wire(andGate, 2u, logic, "D0");
    l.wire(orGate, 2u, logic, "D1");
    l.wire(xorGate, 2u, logic, "D2");
    l.wire(norGate, 2u, logic, "D3");
    const PartId arithmetic = l.box("mux4_n", {126, 30}, 32);
    l.wire(adder, "S", arithmetic, "D0");
    l.wire(adder, "S", arithmetic, "D1");
    l.wire(lessWord, "Q", arithmetic, "D2");
    l.wire(borrowWord, "Q", arithmetic, "D3");
    const PartId shifts = l.box("mux4_n", {126, 84}, 32);
    l.wire(sll, "Q", shifts, "D0");
    l.wire(srl, "Q", shifts, "D1");
    l.wire(sra, "S", shifts, "D2");
    l.wire(lui, "Q", shifts, "D3");
    for (const PartId group : {logic, arithmetic, shifts}) l.nameInput(group, "Sel", "OpBaixo", 2);
    l.name(opHalves, 1u, "OpBaixo", false, 2);
    const PartId nothing = l.constant(0, {130, 66}, 32);
    const PartId result = l.box("mux4_n", {150, 30}, 32);
    l.wire(logic, "Y", result, "D0");
    l.wire(arithmetic, "Y", result, "D1");
    l.wire(shifts, "Y", result, "D2");
    l.wire(nothing, 0u, result, "D3");
    l.wire(opHalves, 2u, result, "Sel");

    const PartId zero = l.constant(0, {150, 50}, 32);
    const PartId compare = l.box("cmp_n", {168, 44}, 32);
    l.wire(result, "Y", compare, "A");
    l.wire(zero, 0u, compare, "B");
    const PartId r = l.output("R", {186, l.pinY(result, l.pin(result, "Y")) - 1}, 32);
    l.wire(result, "Y", r, 0u);
    const PartId z = l.output("Z", {186, l.pinY(compare, l.pin(compare, "A=B")) - 1});
    l.wire(compare, "A=B", z, 0u);
    return chip;
}

// What goes into the ALU: register B, or the immediate, its 16 bits made 32 with or without the sign.
ChipData executeChip(ChipId id, const ChipData& alu) {
    ChipData chip = newChip(id, "Execução");
    Layout l(chip.board);
    const PartId a = l.input("A", {0, 0}, 32);
    const PartId b = l.input("B", {0, 12}, 32);
    const PartId imm = l.input("Imm", {0, 24}, 16);
    const PartId sign = l.input("SinalImm", {0, 34});
    const PartId useImm = l.input("UsaImm", {0, 38});
    const PartId op = l.input("OpULA", {0, 42}, 4);
    const PartId shamt = l.input("Shamt", {0, 46}, 5);

    const PartId extend = l.box("extender", {24, 24}, 32, 16);
    l.wire(imm, 0u, extend, "A");
    l.wire(sign, 0u, extend, "Sinal");
    const PartId second = l.box("mux2_n", {44, 12}, 32);
    l.wire(b, 0u, second, "A");
    l.wire(extend, "Q", second, "B");
    l.wire(useImm, 0u, second, "Sel");
    const PartId unit = l.chip(alu, {64, 0});
    l.wire(a, 0u, unit, "A");
    l.wire(second, "S", unit, "B");
    l.wire(op, 0u, unit, "Op");
    l.wire(shamt, 0u, unit, "Shamt");

    const PartId r = l.output("R", {86, l.pinY(unit, l.pin(unit, "R")) - 1}, 32);
    const PartId z = l.output("Z", {86, l.pinY(unit, l.pin(unit, "Z")) - 1});
    const PartId extended = l.output("ImmExt", {86, 30}, 32);
    l.wire(unit, "R", r, 0u);
    l.wire(unit, "Z", z, 0u);
    l.wire(extend, "Q", extended, 0u);
    return chip;
}

// The 32 registers, and which of them each instruction reads and writes. A syscall reads $v0 and $a0.
ChipData registersChip(ChipId id) {
    ChipData chip = newChip(id, "Registradores");
    Layout l(chip.board);
    const PartId rs = l.input("Rs", {0, 0}, 5);
    const PartId rt = l.input("Rt", {0, 4}, 5);
    const PartId rd = l.input("Rd", {0, 8}, 5);
    const PartId dst = l.input("RegDest", {0, 12}, 2);
    const PartId syscall = l.input("Sistema", {0, 16});
    const PartId write = l.input("Esc", {0, 20});
    const PartId data = l.input("D", {0, 24}, 32);
    const PartId clock = l.input("Clk", {0, 34});

    const PartId v0 = l.constant(mips::kV0, {14, 40}, 5);
    const PartId a0 = l.constant(mips::kA0, {14, 44}, 5);
    const PartId ra = l.constant(mips::kRa, {14, 48}, 5);
    const PartId first = l.box("mux2_n", {28, 0}, 5);
    l.wire(rs, 0u, first, "A");
    l.wire(v0, 0u, first, "B");
    l.wire(syscall, 0u, first, "Sel");
    const PartId second = l.box("mux2_n", {28, 8}, 5);
    l.wire(rt, 0u, second, "A");
    l.wire(a0, 0u, second, "B");
    l.wire(syscall, 0u, second, "Sel");
    const PartId written = l.box("mux4_n", {28, 18}, 5);
    l.wire(rt, 0u, written, "D0");
    l.wire(rd, 0u, written, "D1");
    l.wire(ra, 0u, written, "D2");
    l.wire(v0, 0u, written, "D3");
    l.wire(dst, 0u, written, "Sel");

    Part file = makePart(PartKind::Box, {48, 6}, 0, 0, 1, boxFromKey("regfile"));
    file.width = 32;
    file.address = 5;
    // $gp and $sp start where MARS puts them, so programs from class find their stack.
    std::vector<std::uint64_t> start(32, 0);
    start[28] = mips::kGlobalPointer;
    start[mips::kSp] = mips::kStackTop;
    file.rom = std::make_shared<const std::vector<std::uint64_t>>(std::move(start));
    const PartId registers = l.add(std::move(file));
    l.wire(first, "S", registers, "Ra");
    l.wire(second, "S", registers, "Rb");
    l.wire(written, "Y", registers, "Rw");
    l.wire(data, 0u, registers, "D");
    l.wire(write, 0u, registers, "We");
    l.wire(clock, 0u, registers, "Clk");
    const PartId outA = l.output("A", {66, l.pinY(registers, l.pin(registers, "A")) - 1}, 32);
    const PartId outB = l.output("B", {66, l.pinY(registers, l.pin(registers, "B")) - 1}, 32);
    l.wire(registers, "A", outA, 0u);
    l.wire(registers, "B", outB, 0u);
    return chip;
}

// Where the next instruction comes from: PC+4, a branch (PC+4 plus the offset times four), a jump (the
// target times four, in the same 256 MB as PC+4), or a register (jr). The register keeps the address turned
// over where the program starts, 0x00400000: so the PC comes on, and comes back from a reset, right there.
ChipData counterChip(ChipId id) {
    ChipData chip = newChip(id, "Contador");
    Layout l(chip.board);
    const PartId clock = l.input("Clk", {0, 0});
    const PartId reset = l.input("Rst", {0, 4});
    const PartId halt = l.input("Pare", {0, 8});
    const PartId zero = l.input("Z", {0, 12});
    const PartId onEqual = l.input("SeIgual", {0, 16});
    const PartId onDifferent = l.input("SeDif", {0, 20});
    const PartId jump = l.input("Salto", {0, 24});
    const PartId jumpRegister = l.input("SaltoR", {0, 28});
    const PartId offset = l.input("ImmExt", {0, 34}, 32);
    const PartId instruction = l.input("Instr", {0, 46}, 32);
    const PartId regValue = l.input("Reg", {0, 58}, 32);

    const PartId go = l.gate(PartKind::Not, {22, 6});
    l.wire(halt, 0u, go, 0u);
    const PartId notZero = l.gate(PartKind::Not, {22, 12});
    l.wire(zero, 0u, notZero, 0u);
    const PartId equalTaken = l.gate(PartKind::And, {30, 14});
    l.wire(onEqual, 0u, equalTaken, 0u);
    l.wire(zero, 0u, equalTaken, 1u);
    const PartId differentTaken = l.gate(PartKind::And, {30, 20});
    l.wire(onDifferent, 0u, differentTaken, 0u);
    l.wire(notZero, 1u, differentTaken, 1u);
    const PartId take = l.gate(PartKind::Or, {38, 17});
    l.wire(equalTaken, 2u, take, 0u);
    l.wire(differentTaken, 2u, take, 1u);

    const PartId start = l.constant(mips::kTextBase, {84, 4}, 32);
    const PartId pc = l.box("register", {112, 0}, 32);
    const PartId shown = busGate(l, PartKind::Xor, {140, 2}, 32);
    l.wire(pc, "Q", shown, 0u);
    l.name(shown, 1u, "Início", true, 32);
    l.nameOutput(start, "", "Início", 32);
    l.wire(go, 1u, pc, "En");
    l.wire(reset, 0u, pc, "Rst");
    l.wire(clock, 0u, pc, "Clk");

    const PartId four = l.constant(4, {38, 0}, 32);
    const PartId noCarry = l.constant(0, {40, 28}, 1);
    const PartId plusFour = l.box("adder_n", {54, 0}, 32);
    l.nameInput(plusFour, "A", "PC", 32);
    l.wire(four, 0u, plusFour, "B");
    l.wire(noCarry, 0u, plusFour, "Ce");

    const PartId two = l.constant(2, {20, 40}, 2);
    const PartId leftward = l.constant(0, {20, 44}, 1);
    const PartId times4 = l.box("shifter", {38, 34}, 32, 2);
    l.wire(offset, 0u, times4, "A");
    l.wire(two, 0u, times4, "N");
    l.wire(leftward, 0u, times4, "Dir");
    const PartId branchTarget = l.box("adder_n", {54, 30}, 32);
    l.wire(plusFour, "S", branchTarget, "A");
    l.wire(times4, "Q", branchTarget, "B");
    l.wire(noCarry, 0u, branchTarget, "Ce");

    const PartId targetTimes4 = l.box("shifter", {38, 48}, 32, 2);
    l.wire(instruction, 0u, targetTimes4, "A");
    l.wire(two, 0u, targetTimes4, "N");
    l.wire(leftward, 0u, targetTimes4, "Dir");
    const PartId lowMask = l.constant(0x0FFFFFFFu, {38, 62}, 32);
    const PartId highMask = l.constant(0xF0000000u, {38, 66}, 32);
    const PartId targetBits = busGate(l, PartKind::And, {54, 52}, 32);
    l.wire(targetTimes4, "Q", targetBits, 0u);
    l.wire(lowMask, 0u, targetBits, 1u);
    const PartId region = busGate(l, PartKind::And, {54, 60}, 32);
    l.wire(plusFour, "S", region, 0u);
    l.wire(highMask, 0u, region, 1u);
    const PartId jumpTarget = busGate(l, PartKind::Or, {64, 54}, 32);
    l.wire(targetBits, 2u, jumpTarget, 0u);
    l.wire(region, 2u, jumpTarget, 1u);

    const PartId nextBranch = l.box("mux2_n", {74, 12}, 32);
    l.wire(plusFour, "S", nextBranch, "A");
    l.wire(branchTarget, "S", nextBranch, "B");
    l.wire(take, 2u, nextBranch, "Sel");
    const PartId nextJump = l.box("mux2_n", {74, 26}, 32);
    l.wire(nextBranch, "S", nextJump, "A");
    l.wire(jumpTarget, 2u, nextJump, "B");
    l.wire(jump, 0u, nextJump, "Sel");
    const PartId nextPc = l.box("mux2_n", {74, 40}, 32);
    l.wire(nextJump, "S", nextPc, "A");
    l.wire(regValue, 0u, nextPc, "B");
    l.wire(jumpRegister, 0u, nextPc, "Sel");
    const PartId stored = busGate(l, PartKind::Xor, {98, 12}, 32);
    l.wire(nextPc, "S", stored, 0u);
    l.wire(start, 0u, stored, 1u);
    l.wire(stored, 2u, pc, "D");

    const PartId outPc = l.output("PC", {160, 3}, 32);
    l.wire(shown, 2u, outPc, 0u);
    l.name(shown, 2u, "PC", false, 32);
    const PartId outNext = l.output("Prox", {160, 12}, 32);
    l.wire(plusFour, "S", outNext, 0u);
    return chip;
}

// What a syscall asks for, by the code in $v0: 1 shows a number, 11 a character, 5 reads the switches into
// $v0, 10 stops. Any other code stops the computer too, since it can't do it.
ChipData systemChip(ChipId id) {
    ChipData chip = newChip(id, "Sistema");
    Layout l(chip.board);
    const PartId call = l.input("Sistema", {0, 0});
    const PartId code = l.input("V0", {0, 20}, 32);
    const auto is = [&](std::uint64_t value, int y) {
        const PartId constant = l.constant(value, {20, y + 4}, 32);
        const PartId compare = l.box("cmp_n", {34, y}, 32);
        l.wire(code, 0u, compare, "A");
        l.wire(constant, 0u, compare, "B");
        return compare;
    };
    const PartId number = is(mips::kPrintInt, 0);
    const PartId read = is(mips::kReadInt, 10);
    const PartId exit = is(mips::kExit, 20);
    const PartId character = is(mips::kPrintChar, 30);
    const auto when = [&](PartId compare, int y) {
        const PartId gate = l.gate(PartKind::And, {54, y});
        l.wire(call, 0u, gate, 0u);
        l.wire(compare, "A=B", gate, 1u);
        return gate;
    };
    const PartId printNumber = when(number, 0);
    const PartId readSwitches = when(read, 10);
    const PartId stop = when(exit, 20);
    const PartId printChar = when(character, 30);
    Part known = makePart(PartKind::Or, {54, 40}, 0, 4);
    const PartId anyKnown = l.add(std::move(known));
    l.wire(number, "A=B", anyKnown, 0u);
    l.wire(read, "A=B", anyKnown, 1u);
    l.wire(exit, "A=B", anyKnown, 2u);
    l.wire(character, "A=B", anyKnown, 3u);
    const PartId unknown = l.gate(PartKind::Not, {62, 42});
    l.wire(anyKnown, 4u, unknown, 0u);
    const PartId unknownCall = l.gate(PartKind::And, {70, 40});
    l.wire(call, 0u, unknownCall, 0u);
    l.wire(unknown, 1u, unknownCall, 1u);
    const PartId halt = l.gate(PartKind::Or, {80, 22});
    l.wire(stop, 2u, halt, 0u);
    l.wire(unknownCall, 2u, halt, 1u);

    const auto out = [&](const char* name, PartId gate, unsigned pin, int y) {
        const PartId led = l.output(name, {92, y});
        l.wire(gate, pin, led, 0u);
    };
    out("ImprimeNum", printNumber, 2u, 1);
    out("LeChaves", readSwitches, 2u, 11);
    out("Pare", halt, 2u, 23);
    out("ImprimeCar", printChar, 2u, 31);
    out("Desconhecida", unknownCall, 2u, 41);
    return chip;
}

ChipData cpuChip(ChipId id, const std::vector<ChipData>& parts) {
    ChipData chip = newChip(id, kCpu32Name);
    Layout l(chip.board);
    const PartId clock = l.input("Clk", {0, 0});
    const PartId reset = l.input("Rst", {0, 4});
    const PartId instruction = l.input("Instr", {0, 10}, 32);
    const PartId dataIn = l.input("Dado", {0, 22}, 32);
    const PartId switches = l.input("Chaves", {0, 34}, 32);
    l.nameOutput(clock, "", "Clk");
    l.nameOutput(reset, "", "Rst");
    l.nameOutput(instruction, "", "Instr", 32);
    l.nameOutput(dataIn, "", "Dado", 32);
    l.nameOutput(switches, "", "Chaves", 32);

    const PartId decoder = l.chip(parts[0], {34, 0});
    const PartId control = l.chip(parts[1], {34, 30});
    const PartId registers = l.chip(parts[4], {80, 0});
    const PartId execute = l.chip(parts[3], {80, 30});
    const PartId counter = l.chip(parts[5], {80, 62});
    const PartId system = l.chip(parts[6], {126, 62});
    const PartId back = l.box("mux4_n", {126, 0}, 32);

    l.nameInput(decoder, "Instr", "Instr", 32);
    l.wire(decoder, "Op", control, "Op");
    l.nameOutput(decoder, "Funct", "Funct", 6);
    l.nameInput(control, "Funct", "Funct", 6);
    l.wire(decoder, "Rs", registers, "Rs");
    l.wire(decoder, "Rt", registers, "Rt");
    l.wire(decoder, "Rd", registers, "Rd");
    l.nameOutput(decoder, "Shamt", "Shamt", 5);
    l.nameOutput(decoder, "Imm", "Imm", 16);

    l.nameOutput(control, "RegEsc", "RegEsc");
    l.nameOutput(control, "MemEsc", "MemEsc");
    l.nameOutput(control, "UsaImm", "UsaImm");
    l.nameOutput(control, "SinalImm", "SinalImm");
    l.nameOutput(control, "RegDest", "RegDest", 2);
    l.nameOutput(control, "Volta", "Volta", 2);
    l.nameOutput(control, "SeIgual", "SeIgual");
    l.nameOutput(control, "SeDif", "SeDif");
    l.nameOutput(control, "Salto", "Salto");
    l.nameOutput(control, "SaltoR", "SaltoR");
    l.nameOutput(control, "Sistema", "Sistema");
    l.nameOutput(control, "Inválida", "Inválida");
    l.nameOutput(control, "OpULA", "OpULA", 4);

    l.nameInput(registers, "RegDest", "RegDest", 2);
    l.nameInput(registers, "Sistema", "Sistema");
    l.nameInput(registers, "Esc", "Escreve");
    l.nameInput(registers, "D", "Volta32", 32);
    l.nameInput(registers, "Clk", "Clk");
    l.wire(registers, "A", execute, "A");
    l.wire(registers, "B", execute, "B");
    l.nameOutput(registers, "A", "RegA", 32);
    l.nameOutput(registers, "B", "RegB", 32);

    l.nameInput(execute, "Imm", "Imm", 16);
    l.nameInput(execute, "SinalImm", "SinalImm");
    l.nameInput(execute, "UsaImm", "UsaImm");
    l.nameInput(execute, "OpULA", "OpULA", 4);
    l.nameInput(execute, "Shamt", "Shamt", 5);
    l.nameOutput(execute, "R", "Resultado", 32);
    l.nameOutput(execute, "Z", "Z");
    l.nameOutput(execute, "ImmExt", "ImmExt", 32);

    l.nameInput(counter, "Clk", "Clk");
    l.nameInput(counter, "Rst", "Rst");
    l.nameInput(counter, "Pare", "Parou");
    l.nameInput(counter, "Z", "Z");
    l.nameInput(counter, "SeIgual", "SeIgual");
    l.nameInput(counter, "SeDif", "SeDif");
    l.nameInput(counter, "Salto", "Salto");
    l.nameInput(counter, "SaltoR", "SaltoR");
    l.nameInput(counter, "ImmExt", "ImmExt", 32);
    l.nameInput(counter, "Instr", "Instr", 32);
    l.nameInput(counter, "Reg", "RegA", 32);
    l.nameOutput(counter, "PC", "PC", 32);
    l.nameOutput(counter, "Prox", "Prox", 32);

    l.nameInput(system, "Sistema", "Sistema");
    l.nameInput(system, "V0", "RegA", 32);
    l.nameOutput(system, "LeChaves", "LeChaves");
    l.nameOutput(system, "Pare", "PareSis");
    l.nameOutput(system, "ImprimeNum", "ImprimeNum");
    l.nameOutput(system, "ImprimeCar", "ImprimeCar");
    l.nameOutput(system, "Desconhecida", "Desconhecida");

    l.nameInput(back, "D0", "Resultado", 32);
    l.nameInput(back, "D1", "Dado", 32);
    l.nameInput(back, "D2", "Prox", 32);
    l.nameInput(back, "D3", "Chaves", 32);
    l.nameInput(back, "Sel", "Volta", 2);
    l.nameOutput(back, "Y", "Volta32", 32);

    // A register is written when the instruction says so, or when syscall 5 brings the switches into $v0.
    const PartId writes = l.gate(PartKind::Or, {166, 4});
    l.name(writes, 0u, "RegEsc", true);
    l.name(writes, 1u, "LeChaves", true);
    l.name(writes, 2u, "Escreve", false);
    // It stops on syscall 10, or on what it can't do: an instruction it doesn't have, a syscall it doesn't know.
    const PartId cannot = l.gate(PartKind::Or, {166, 14});
    l.name(cannot, 0u, "Inválida", true);
    l.name(cannot, 1u, "Desconhecida", true);
    l.name(cannot, 2u, "NãoSabe", false);
    const PartId stops = l.gate(PartKind::Or, {166, 24});
    l.name(stops, 0u, "PareSis", true);
    l.name(stops, 1u, "NãoSabe", true);
    l.name(stops, 2u, "Parou", false);
    // What a syscall printed stays here, for the screens to show: $a0, read through the B side.
    const PartId printing = l.gate(PartKind::Or, {166, 34});
    l.name(printing, 0u, "ImprimeNum", true);
    l.name(printing, 1u, "ImprimeCar", true);
    const PartId printed = l.box("register", {184, 34}, 32);
    l.wire(printing, 2u, printed, "En");
    l.nameInput(printed, "D", "RegB", 32);
    l.nameInput(printed, "Rst", "Rst");
    l.nameInput(printed, "Clk", "Clk");

    // The pins the computer sees on the CPU.
    constexpr int kPins = 214;
    const PartId pc = l.output("PC", {kPins, 0}, 32);
    const PartId address = l.output("End", {kPins, 6}, 32);
    const PartId value = l.output("Valor", {kPins, 12}, 32);
    const PartId memoryWrite = l.output("Grava", {kPins, 18});
    const PartId halted = l.output("Parou", {kPins, 22});
    const PartId invalid = l.output("Inválida", {kPins, 26});
    const PartId number = l.output("ImprimeNum", {kPins, 30});
    const PartId character = l.output("ImprimeCar", {kPins, 34});
    const PartId shown = l.output("Saída", {kPins, 40}, 32);
    l.nameInput(pc, "", "PC", 32);
    l.nameInput(address, "", "Resultado", 32);
    l.nameInput(value, "", "RegB", 32);
    l.nameInput(memoryWrite, "", "MemEsc");
    l.nameInput(halted, "", "Parou");
    l.nameInput(invalid, "", "NãoSabe");
    l.nameInput(number, "", "ImprimeNum");
    l.nameInput(character, "", "ImprimeCar");
    l.wire(printed, "Q", shown, 0u);
    return chip;
}

}  // namespace

std::vector<std::uint64_t> mainControlRows() {
    using namespace mips;
    std::vector<std::uint64_t> rows(64, bit(kC32Invalid));
    const std::uint64_t immediate = bit(kC32RegWrite) | bit(kC32AluImm) | regDst(kDstRt);
    rows[kOpSpecial] = bit(kC32UseFunct);
    rows[kOpJ] = bit(kC32Jump);
    rows[kOpJal] = bit(kC32Jump) | bit(kC32RegWrite) | regDst(kDstRa) | writeBack(kBackNext);
    rows[kOpBeq] = bit(kC32BranchEq) | bit(kC32SignExtend) | aluOp(kAlu32Sub);
    rows[kOpBne] = bit(kC32BranchNe) | bit(kC32SignExtend) | aluOp(kAlu32Sub);
    rows[kOpAddi] = immediate | bit(kC32SignExtend) | aluOp(kAlu32Add);
    rows[kOpAddiu] = immediate | bit(kC32SignExtend) | aluOp(kAlu32Add);
    rows[kOpSlti] = immediate | bit(kC32SignExtend) | aluOp(kAlu32Slt);
    rows[kOpSltiu] = immediate | bit(kC32SignExtend) | aluOp(kAlu32Sltu);
    rows[kOpAndi] = immediate | aluOp(kAlu32And);
    rows[kOpOri] = immediate | aluOp(kAlu32Or);
    rows[kOpXori] = immediate | aluOp(kAlu32Xor);
    rows[kOpLui] = immediate | aluOp(kAlu32Lui);
    rows[kOpLw] = immediate | bit(kC32SignExtend) | writeBack(kBackMemory) | aluOp(kAlu32Add);
    rows[kOpSw] = bit(kC32MemWrite) | bit(kC32AluImm) | bit(kC32SignExtend) | aluOp(kAlu32Add);
    return rows;
}

std::vector<std::uint64_t> functControlRows() {
    using namespace mips;
    std::vector<std::uint64_t> rows(64, bit(kC32Invalid));
    const auto r = [](unsigned op) { return bit(kC32RegWrite) | regDst(kDstRd) | aluOp(op); };
    rows[kFnSll] = r(kAlu32Sll);
    rows[kFnSrl] = r(kAlu32Srl);
    rows[kFnSra] = r(kAlu32Sra);
    rows[kFnJr] = bit(kC32JumpReg);
    rows[kFnSyscall] = bit(kC32Syscall) | regDst(kDstV0) | writeBack(kBackSwitches);
    rows[kFnAdd] = r(kAlu32Add);
    rows[kFnAddu] = r(kAlu32Add);
    rows[kFnSub] = r(kAlu32Sub);
    rows[kFnSubu] = r(kAlu32Sub);
    rows[kFnAnd] = r(kAlu32And);
    rows[kFnOr] = r(kAlu32Or);
    rows[kFnXor] = r(kAlu32Xor);
    rows[kFnNor] = r(kAlu32Nor);
    rows[kFnSlt] = r(kAlu32Slt);
    rows[kFnSltu] = r(kAlu32Sltu);
    return rows;
}

std::vector<ChipData> buildCpu32Chips(ChipId first) {
    std::vector<ChipData> chips;
    chips.push_back(decoderChip(first));
    chips.push_back(controlChip(first + 1));
    chips.push_back(aluChip(first + 2));
    chips.push_back(executeChip(first + 3, chips[2]));
    chips.push_back(registersChip(first + 4));
    chips.push_back(counterChip(first + 5));
    chips.push_back(systemChip(first + 6));
    ChipData cpu = cpuChip(first + 7, chips);
    chips.push_back(std::move(cpu));
    return chips;
}

namespace {

// Addresses count bytes, and memories count words: bits 11 to 2 of an address are the word, only wires.
ChipData wordChip(ChipId id) {
    ChipData chip = newChip(id, "Palavra");
    Layout l(chip.board);
    const PartId address = l.input("End", {0, 14}, 32);
    const PartId pairs = l.bundle(PartKind::Splitter, {22, 0}, 32, 16);
    l.wire(address, 0u, pairs, 0u);
    const PartId word = l.bundle(PartKind::Joiner, {34, l.pinY(pairs, 2) - 1}, kMemory32AddressBits, 5);
    for (unsigned i = 0; i < 5; ++i) l.wire(pairs, 2 + i, word, i);
    const PartId out = l.output("Palavra", {44, l.pinY(word, 5) - 1}, kMemory32AddressBits);
    l.wire(word, 5u, out, 0u);
    return chip;
}

PartId partWithChip(const Board& board, ChipId chip) {
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Chip && part.chip == chip) return part.id;
    }
    return 0;
}

}  // namespace

Computer32Project buildComputer32(const mips::Program& program, const std::string& source) {
    Computer32Project project;
    project.chips = buildCpu32Chips(1);
    project.chips.push_back(wordChip(static_cast<ChipId>(project.chips.size() + 1)));
    const ChipData& cpuData = project.chips[project.chips.size() - 2];
    const ChipData& word = project.chips.back();

    Board& board = project.board;
    Layout l(board);
    project.clock = board.addPart(PartKind::Clock, {0, 5}).value();
    project.reset = l.input("Rst", {0, 10});
    project.switches = l.input("Chaves", {0, 18}, 32);
    project.cpu = l.chip(cpuData, {30, 4});
    l.name(project.clock, 0, "Clk", false);
    l.nameInput(project.cpu, "Clk", "Clk");
    l.wire(project.reset, 0u, project.cpu, "Rst");
    l.wire(project.switches, 0u, project.cpu, "Chaves");
    l.nameInput(project.cpu, "Instr", "Instr", 32);
    l.nameInput(project.cpu, "Dado", "Dado", 32);

    Part romPart = makePart(PartKind::Box, {26, 34}, 0, 0, 1, boxFromKey("rom"));
    romPart.width = kWord32Bits;
    romPart.address = kMemory32AddressBits;
    romPart.rom = std::make_shared<const std::vector<std::uint64_t>>(program.text.begin(), program.text.end());
    romPart.source = i18n::tr(source);
    project.rom = l.add(std::move(romPart));
    const PartId romWord = l.chip(word, {8, 34});
    l.nameInput(romWord, "End", "PC", 32);
    l.wire(romWord, "Palavra", project.rom, "A");
    l.nameOutput(project.rom, "Q", "Instr", 32);

    Part ramPart = makePart(PartKind::Box, {92, 44}, 0, 0, 1, boxFromKey("ram"));
    ramPart.width = kWord32Bits;
    ramPart.address = kMemory32AddressBits;
    ramPart.rom = std::make_shared<const std::vector<std::uint64_t>>(program.data.begin(), program.data.end());
    project.ram = l.add(std::move(ramPart));
    const PartId ramWord = l.chip(word, {62, 44});
    l.nameInput(ramWord, "End", "End", 32);
    l.wire(ramWord, "Palavra", project.ram, "A");
    l.nameOutput(project.cpu, "End", "End", 32);
    l.nameInput(project.ram, "D", "Valor", 32);
    l.nameOutput(project.cpu, "Valor", "Valor", 32);
    l.nameInput(project.ram, "We", "Grava");
    l.nameOutput(project.cpu, "Grava", "Grava");
    l.nameInput(project.ram, "Clk", "Clk");
    l.nameOutput(project.ram, "Q", "Dado", 32);

    project.pcProbe = l.output("PC", {100, 4}, 32);
    project.haltLed = l.output("Parou", {100, 8});
    project.invalidLed = l.output("Inválida", {100, 11});
    project.printNumber = l.output("ImprimeNum", {100, 14});
    project.printChar = l.output("ImprimeCar", {100, 17});
    project.outputProbe = l.output("Saída", {100, 21}, 32);
    l.nameOutput(project.cpu, "PC", "PC", 32);
    l.name(project.pcProbe, 0, "PC", true, 32);
    l.wire(project.cpu, "Parou", project.haltLed, 0u);
    l.wire(project.cpu, "Inválida", project.invalidLed, 0u);
    l.wire(project.cpu, "ImprimeNum", project.printNumber, 0u);
    l.wire(project.cpu, "ImprimeCar", project.printChar, 0u);
    l.wire(project.cpu, "Saída", project.outputProbe, 0u);

    const ChipData& registersData = project.chips[4];
    const PartId registersPart = partWithChip(cpuData.board, registersData.id);
    PartId file = 0;
    for (const Part& part : registersData.board.parts()) {
        if (part.kind == PartKind::Box && part.box == boxFromKey("regfile")) file = part.id;
    }
    project.registersPath = {project.cpu, registersPart, file};
    return project;
}

}  // namespace bancada::editor
