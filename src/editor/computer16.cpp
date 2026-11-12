#include "editor/computer16.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "asm/assembler.hpp"
#include "editor/layout.hpp"
#include "i18n/i18n.hpp"

namespace bancada::editor {

namespace {

constexpr std::uint64_t bit(unsigned index) { return std::uint64_t{1} << index; }

std::uint64_t wbSelect(unsigned source) { return static_cast<std::uint64_t>(source) << kWbLow; }
std::uint64_t immSelect(unsigned source) { return static_cast<std::uint64_t>(source) << kImmLow; }
std::uint64_t aluOp(unsigned op) { return static_cast<std::uint64_t>(op) << kAluOpLow; }

// One row per opcode, and one bit per thing the computer can do.
std::vector<std::uint64_t> controlRows() {
    using namespace assembly;
    std::vector<std::uint64_t> rows(16, 0);
    rows[kOpAlu] = bit(kRegWrite) | bit(kUseFunc);
    rows[kOpAddi] = bit(kRegWrite) | bit(kAluImm) | aluOp(kAdd);
    rows[kOpLi] = bit(kRegWrite) | bit(kAluImm) | bit(kAluZero) | immSelect(1) | aluOp(kAdd);
    rows[kOpLui] = bit(kRegWrite) | bit(kAluImm) | bit(kAluZero) | immSelect(2) | aluOp(kAdd);
    rows[kOpLd] = bit(kRegWrite) | bit(kAluImm) | wbSelect(1) | aluOp(kAdd);
    rows[kOpSt] = bit(kMemWrite) | bit(kAluImm) | bit(kReadRd) | aluOp(kAdd);
    rows[kOpBeq] = bit(kBranchEq) | bit(kReadRd) | aluOp(kSub);
    rows[kOpBne] = bit(kBranchNe) | bit(kReadRd) | aluOp(kSub);
    rows[kOpJmp] = bit(kJump);
    rows[kOpCall] = bit(kJump) | bit(kRegWrite) | bit(kLink) | wbSelect(2);
    rows[kOpJr] = bit(kJumpReg);
    rows[kOpOut] = bit(kOutWrite);
    rows[kOpIn] = bit(kRegWrite) | wbSelect(3);
    rows[kOpHalt] = bit(kHalt);
    return rows;
}

// Takes an instruction apart. A five-way split of 16 bits gives 3, 3, 3, 3 and 4, which is exactly how the
// fields are laid out, so most of this is only wires.
ChipData decoderChip(ChipId id) {
    ChipData chip;
    chip.id = id;
    chip.name = "Decodificador";
    Layout l(chip.board);

    const PartId in = l.input("Instr", {0, 3}, 16);
    const PartId split = l.bundle(PartKind::Splitter, {22, 0}, 16, 5);
    l.wire(in, 0u, split, 0u);

    constexpr int kOutputs = 62;
    const auto field = [&](const char* name, unsigned branch, std::uint8_t width) {
        const PartId out = l.output(name, {kOutputs, l.pinY(split, 1 + branch) - 1}, width);
        l.wire(split, 1 + branch, out, 0u);
    };
    field("Func", 0, 3);
    field("Rt", 1, 3);
    field("Rs", 2, 3);
    field("Rd", 3, 3);
    field("Op", 4, 4);

    const PartId signedInput = l.constant(1, {30, 12}, 1);
    const PartId unsignedInput = l.constant(0, {30, 16}, 1);
    const auto immediate = [&](const char* name, int y, std::uint8_t branches, PartId sign) {
        const auto bits = static_cast<std::uint8_t>(3 * branches);
        const PartId join = l.bundle(PartKind::Joiner, {30, y}, bits, branches);
        for (unsigned i = 0; i < branches; ++i) l.wire(split, 1 + i, join, i);
        const PartId extend = l.box("extender", {44, y}, 16, bits);
        l.wire(join, branches, extend, "A");
        l.wire(sign, 0u, extend, "Sinal");
        const PartId out = l.output(name, {kOutputs, l.pinY(extend, l.pin(extend, "Q")) - 1}, 16);
        l.wire(extend, "Q", out, 0u);
        return extend;
    };
    immediate("Imm6", 20, 2, signedInput);
    const PartId nine = immediate("Imm9", 26, 3, unsignedInput);
    immediate("Imm12", 34, 4, unsignedInput);

    // The high part of a number: the nine bits, moved up by seven.
    const PartId seven = l.constant(7, {30, 44}, 3);
    const PartId left = l.constant(0, {30, 48}, 1);
    const PartId up = l.box("shifter", {44, 44}, 16, 3);
    l.wire(nine, "Q", up, "A");
    l.wire(seven, 0u, up, "N");
    l.wire(left, 0u, up, "Dir");
    const PartId out = l.output("ImmH", {kOutputs, l.pinY(up, l.pin(up, "Q")) - 1}, 16);
    l.wire(up, "Q", out, 0u);
    return chip;
}

// The opcode picks a row of this ROM, and every bit of the row is one wire to the rest of the computer.
ChipData controlChip(ChipId id) {
    ChipData chip;
    chip.id = id;
    chip.name = "Controle";
    Layout l(chip.board);

    const PartId op = l.input("Op", {0, 18}, 4);
    Part rom = makePart(PartKind::Box, {14, 16}, 0, 0, 1, boxFromKey("rom"));
    rom.width = kControlBits;
    rom.address = 4;
    rom.rom = std::make_shared<const std::vector<std::uint64_t>>(controlRows());
    const PartId table = l.add(std::move(rom));
    l.wire(op, 0u, table, "A");
    const PartId lines = l.bundle(PartKind::Splitter, {30, 0}, kControlBits, kControlBits);
    l.wire(table, "Q", lines, 0u);

    constexpr int kOutputs = 40;
    const auto single = [&](const char* name, unsigned line) {
        const PartId out = l.output(name, {kOutputs, l.pinY(lines, 1 + line) - 1});
        l.wire(lines, 1 + line, out, 0u);
    };
    const auto field = [&](const char* name, unsigned low, std::uint8_t count) {
        const PartId join = l.bundle(PartKind::Joiner, {kOutputs, l.pinY(lines, 1 + low) - 1}, count, count);
        for (unsigned i = 0; i < count; ++i) l.wire(lines, 1 + low + i, join, i);
        const PartId out = l.output(name, {kOutputs + 8, l.pinY(join, count) - 1}, count);
        l.wire(join, count, out, 0u);
    };
    single("RegEsc", kRegWrite);
    single("MemEsc", kMemWrite);
    single("UsaImm", kAluImm);
    field("Volta", kWbLow, 2);
    single("SeIgual", kBranchEq);
    single("SeDif", kBranchNe);
    single("Salto", kJump);
    single("SaltoR", kJumpReg);
    single("SaiEsc", kOutWrite);
    single("Pare", kHalt);
    single("UsaFunc", kUseFunc);
    field("QualImm", kImmLow, 2);
    single("LeRd", kReadRd);
    single("Liga", kLink);
    single("ZeraA", kAluZero);
    field("OpULA", kAluOpLow, 3);
    return chip;
}

// Where the next instruction comes from: the one after this, a branch, a jump, or a register.
ChipData counterChip(ChipId id) {
    ChipData chip;
    chip.id = id;
    chip.name = "Contador";
    Layout l(chip.board);

    const PartId clock = l.input("Clk", {0, 0});
    const PartId reset = l.input("Rst", {0, 3});
    const PartId halt = l.input("Pare", {0, 6});
    const PartId zero = l.input("Z", {0, 10});
    const PartId onEqual = l.input("SeIgual", {0, 13});
    const PartId onDifferent = l.input("SeDif", {0, 16});
    const PartId jump = l.input("Salto", {0, 19});
    const PartId jumpRegister = l.input("SaltoR", {0, 22});
    const PartId offset = l.input("Imm6", {0, 25}, 16);
    const PartId target = l.input("Imm12", {0, 30}, 16);
    const PartId regValue = l.input("Reg", {0, 35}, 16);

    const PartId go = l.gate(PartKind::Not, {10, 5});
    l.wire(halt, 0u, go, 0u);
    const PartId notZero = l.gate(PartKind::Not, {10, 10});
    l.wire(zero, 0u, notZero, 0u);
    const PartId equalTaken = l.gate(PartKind::And, {18, 11});
    l.wire(onEqual, 0u, equalTaken, 0u);
    l.wire(zero, 0u, equalTaken, 1u);
    const PartId differentTaken = l.gate(PartKind::And, {18, 17});
    l.wire(onDifferent, 0u, differentTaken, 0u);
    l.wire(notZero, 1u, differentTaken, 1u);
    const PartId take = l.gate(PartKind::Or, {26, 14});
    l.wire(equalTaken, 2u, take, 0u);
    l.wire(differentTaken, 2u, take, 1u);

    const PartId pc = l.box("register", {66, 0}, 16);
    l.wire(go, 1u, pc, "En");
    l.wire(reset, 0u, pc, "Rst");
    l.wire(clock, 0u, pc, "Clk");

    const PartId one = l.constant(1, {26, 2}, 16);
    const PartId noCarry = l.constant(0, {26, 7}, 1);
    const PartId increment = l.box("adder_n", {34, 0}, 16);
    l.wire(pc, "Q", increment, "A");
    l.wire(one, 0u, increment, "B");
    l.wire(noCarry, 0u, increment, "Ce");
    const PartId branchTarget = l.box("adder_n", {34, 24}, 16);
    l.wire(increment, "S", branchTarget, "A");
    l.wire(offset, 0u, branchTarget, "B");
    l.wire(noCarry, 0u, branchTarget, "Ce");

    const PartId nextBranch = l.box("mux2_n", {50, 8}, 16);
    l.wire(increment, "S", nextBranch, "A");
    l.wire(branchTarget, "S", nextBranch, "B");
    l.wire(take, 2u, nextBranch, "Sel");
    const PartId nextJump = l.box("mux2_n", {50, 18}, 16);
    l.wire(nextBranch, "S", nextJump, "A");
    l.wire(target, 0u, nextJump, "B");
    l.wire(jump, 0u, nextJump, "Sel");
    const PartId nextPc = l.box("mux2_n", {50, 28}, 16);
    l.wire(nextJump, "S", nextPc, "A");
    l.wire(regValue, 0u, nextPc, "B");
    l.wire(jumpRegister, 0u, nextPc, "Sel");
    l.wire(nextPc, "S", pc, "D");

    const PartId shownPc = l.output("PC", {80, l.pinY(pc, l.pin(pc, "Q")) - 1}, 16);
    const PartId shownNext = l.output("Prox", {80, 12}, 16);
    l.wire(pc, "Q", shownPc, 0u);
    l.wire(increment, "S", shownNext, 0u);
    return chip;
}

// The eight registers, and which of them each instruction looks at.
ChipData registersChip(ChipId id) {
    ChipData chip;
    chip.id = id;
    chip.name = "Registradores";
    Layout l(chip.board);

    const PartId rs = l.input("Rs", {0, 0}, 3);
    const PartId rt = l.input("Rt", {0, 3}, 3);
    const PartId rd = l.input("Rd", {0, 6}, 3);
    const PartId readRd = l.input("LeRd", {0, 10});
    const PartId link = l.input("Liga", {0, 13});
    const PartId write = l.input("Esc", {0, 16});
    const PartId data = l.input("D", {0, 19}, 16);
    const PartId clock = l.input("Clk", {0, 24});

    const PartId returnRegister = l.constant(7, {12, 14}, 3);
    const PartId second = l.box("mux2_n", {22, 2}, 3);
    l.wire(rt, 0u, second, "A");
    l.wire(rd, 0u, second, "B");
    l.wire(readRd, 0u, second, "Sel");
    const PartId written = l.box("mux2_n", {22, 12}, 3);
    l.wire(rd, 0u, written, "A");
    l.wire(returnRegister, 0u, written, "B");
    l.wire(link, 0u, written, "Sel");

    const PartId registers = l.box("regfile", {42, 4}, 16, 3);
    l.wire(rs, 0u, registers, "Ra");
    l.wire(second, "S", registers, "Rb");
    l.wire(written, "S", registers, "Rw");
    l.wire(data, 0u, registers, "D");
    l.wire(write, 0u, registers, "We");
    l.wire(clock, 0u, registers, "Clk");

    const PartId a = l.output("A", {58, l.pinY(registers, l.pin(registers, "A")) - 1}, 16);
    const PartId b = l.output("B", {58, l.pinY(registers, l.pin(registers, "B")) - 1}, 16);
    l.wire(registers, "A", a, 0u);
    l.wire(registers, "B", b, 0u);
    return chip;
}

// The ALU, and the choice of what goes into it.
ChipData executeChip(ChipId id) {
    ChipData chip;
    chip.id = id;
    chip.name = "Execução";
    Layout l(chip.board);

    const PartId a = l.input("A", {0, 0}, 16);
    const PartId b = l.input("B", {0, 5}, 16);
    const PartId imm6 = l.input("Imm6", {0, 10}, 16);
    const PartId imm9 = l.input("Imm9", {0, 15}, 16);
    const PartId immHigh = l.input("ImmH", {0, 20}, 16);
    const PartId whichImm = l.input("QualImm", {0, 25}, 2);
    const PartId useImm = l.input("UsaImm", {0, 28});
    const PartId zeroA = l.input("ZeraA", {0, 31});
    const PartId op = l.input("OpULA", {0, 34}, 3);
    const PartId func = l.input("Func", {0, 37}, 3);
    const PartId useFunc = l.input("UsaFunc", {0, 40});

    const PartId immediate = l.box("mux4_n", {26, 12}, 16);
    l.wire(imm6, 0u, immediate, "D0");
    l.wire(imm9, 0u, immediate, "D1");
    l.wire(immHigh, 0u, immediate, "D2");
    l.wire(whichImm, 0u, immediate, "Sel");

    const PartId nothing = l.constant(0, {34, 4}, 16);
    const PartId first = l.box("mux2_n", {46, 0}, 16);
    l.wire(a, 0u, first, "A");
    l.wire(nothing, 0u, first, "B");
    l.wire(zeroA, 0u, first, "Sel");
    const PartId second = l.box("mux2_n", {46, 10}, 16);
    l.wire(b, 0u, second, "A");
    l.wire(immediate, "Y", second, "B");
    l.wire(useImm, 0u, second, "Sel");
    const PartId operation = l.box("mux2_n", {46, 20}, 3);
    l.wire(op, 0u, operation, "A");
    l.wire(func, 0u, operation, "B");
    l.wire(useFunc, 0u, operation, "Sel");

    const PartId alu = l.box("alu_n", {64, 6}, 16);
    l.wire(first, "S", alu, "A");
    l.wire(second, "S", alu, "B");
    l.wire(operation, "S", alu, "Op");

    const PartId result = l.output("R", {80, l.pinY(alu, l.pin(alu, "R")) - 1}, 16);
    const PartId isZero = l.output("Z", {80, l.pinY(alu, l.pin(alu, "Z")) - 1});
    l.wire(alu, "R", result, 0u);
    l.wire(alu, "Z", isZero, 0u);
    return chip;
}

ChipData cpuChip(ChipId id, const std::vector<ChipData>& parts) {
    ChipData chip;
    chip.id = id;
    chip.name = kCpuName;
    Layout l(chip.board);

    // Two rows between inputs, for the name each one carries above it.
    const PartId clock = l.input("Clk", {0, 0});
    const PartId reset = l.input("Rst", {0, 4});
    const PartId instruction = l.input("Instr", {0, 8}, 16);
    const PartId dataIn = l.input("Dado", {0, 14}, 16);
    const PartId switches = l.input("Chaves", {0, 20}, 16);
    l.nameOutput(clock, "", "Clk");
    l.nameOutput(reset, "", "Rst");
    l.nameOutput(dataIn, "", "Dado", 16);
    l.nameOutput(switches, "", "Chaves", 16);

    const PartId decoder = l.chip(parts[0], {30, 0});
    const PartId control = l.chip(parts[1], {30, 24});
    const PartId registers = l.chip(parts[3], {70, 0});
    const PartId execute = l.chip(parts[4], {70, 20});
    const PartId counter = l.chip(parts[2], {70, 46});
    const PartId writeBack = l.box("mux4_n", {98, 0}, 16);
    const PartId output = l.box("register", {98, 16}, 16);

    l.wire(instruction, 0u, decoder, "Instr");
    l.wire(decoder, "Op", control, "Op");
    l.wire(decoder, "Rs", registers, "Rs");
    l.wire(decoder, "Rt", registers, "Rt");
    l.wire(decoder, "Rd", registers, "Rd");
    l.wire(registers, "A", execute, "A");
    l.wire(registers, "B", execute, "B");
    l.wire(writeBack, "Y", registers, "D");
    l.wire(execute, "R", writeBack, "D0");

    l.nameOutput(decoder, "Func", "Func", 3);
    l.nameOutput(decoder, "Imm6", "Imm6", 16);
    l.nameOutput(decoder, "Imm9", "Imm9", 16);
    l.nameOutput(decoder, "Imm12", "Imm12", 16);
    l.nameOutput(decoder, "ImmH", "ImmH", 16);

    l.nameOutput(control, "RegEsc", "RegEsc");
    l.nameOutput(control, "MemEsc", "MemEsc");
    l.nameOutput(control, "UsaImm", "UsaImm");
    l.nameOutput(control, "Volta", "Volta", 2);
    l.nameOutput(control, "SeIgual", "SeIgual");
    l.nameOutput(control, "SeDif", "SeDif");
    l.nameOutput(control, "Salto", "Salto");
    l.nameOutput(control, "SaltoR", "SaltoR");
    l.nameOutput(control, "SaiEsc", "SaiEsc");
    l.nameOutput(control, "Pare", "Pare");
    l.nameOutput(control, "UsaFunc", "UsaFunc");
    l.nameOutput(control, "QualImm", "QualImm", 2);
    l.nameOutput(control, "LeRd", "LeRd");
    l.nameOutput(control, "Liga", "Liga");
    l.nameOutput(control, "ZeraA", "ZeraA");
    l.nameOutput(control, "OpULA", "OpULA", 3);

    l.nameInput(registers, "LeRd");
    l.nameInput(registers, "Liga");
    l.nameInput(registers, "Esc", "RegEsc");
    l.nameInput(registers, "Clk");

    l.nameInput(execute, "Imm6", 16);
    l.nameInput(execute, "Imm9", 16);
    l.nameInput(execute, "ImmH", 16);
    l.nameInput(execute, "QualImm", 2);
    l.nameInput(execute, "UsaImm");
    l.nameInput(execute, "ZeraA");
    l.nameInput(execute, "OpULA", 3);
    l.nameInput(execute, "Func", 3);
    l.nameInput(execute, "UsaFunc");
    l.nameOutput(execute, "Z", "Z");

    l.nameInput(counter, "Clk");
    l.nameInput(counter, "Rst");
    l.nameInput(counter, "Pare");
    l.nameInput(counter, "Z");
    l.nameInput(counter, "SeIgual");
    l.nameInput(counter, "SeDif");
    l.nameInput(counter, "Salto");
    l.nameInput(counter, "SaltoR");
    l.nameInput(counter, "Imm6", 16);
    l.nameInput(counter, "Imm12", 16);
    l.nameInput(counter, "Reg", "RegA", 16);
    l.nameOutput(counter, "Prox", "Prox", 16);
    l.nameOutput(registers, "A", "RegA", 16);
    l.nameOutput(registers, "B", "RegB", 16);

    l.nameInput(writeBack, "D1", "Dado", 16);
    l.nameInput(writeBack, "D2", "Prox", 16);
    l.nameInput(writeBack, "D3", "Chaves", 16);
    l.nameInput(writeBack, "Sel", "Volta", 2);

    l.nameInput(output, "D", "RegA", 16);
    l.nameInput(output, "En", "SaiEsc");
    l.nameInput(output, "Rst");
    l.nameInput(output, "Clk");

    // The pins the player sees on the CPU, so their names say what they carry.
    const PartId shownPc = l.output("PC", {124, 44}, 16);
    const PartId address = l.output("End", {124, 28}, 16);
    const PartId dataOut = l.output("Valor", {124, 34}, 16);
    const PartId memoryWrite = l.output("Grava", {124, 12});
    const PartId halted = l.output("Parou", {124, 18});
    const PartId shown = l.output("Saída", {124, 22}, 16);
    l.wire(counter, "PC", shownPc, 0u);
    l.wire(execute, "R", address, 0u);
    l.wire(output, "Q", shown, 0u);
    l.nameInput(dataOut, "", "RegB", 16);
    l.nameInput(memoryWrite, "", "MemEsc");
    l.nameInput(halted, "", "Pare");
    return chip;
}

}  // namespace

const char* exampleProgram() {
    return "; Soma os números de 1 a 10 e mostra o total.\n"
           "; Rode o clock: a saída mostra a conta crescendo.\n"
           "\n"
           "        LI   R1, 10      ; contador\n"
           "        LI   R2, 0       ; total\n"
           "laço:   ADD  R2, R2, R1\n"
           "        OUT  R2\n"
           "        ADDI R1, R1, -1\n"
           "        BNE  R1, R0, laço\n"
           "        HLT\n";
}

std::vector<ChipData> buildCpuChips(ChipId first) {
    std::vector<ChipData> chips;
    chips.push_back(decoderChip(first));
    chips.push_back(controlChip(first + 1));
    chips.push_back(counterChip(first + 2));
    chips.push_back(registersChip(first + 3));
    chips.push_back(executeChip(first + 4));
    ChipData cpu = cpuChip(first + 5, chips);
    chips.push_back(std::move(cpu));
    return chips;
}

ComputerProject buildComputer16(const std::vector<std::uint16_t>& program, const std::string& source) {
    ComputerProject project;
    project.chips = buildCpuChips(1);

    // Read from left to right: what goes in, the CPU, the memory it writes, what comes out. Wires that
    // would have to turn back across the board (the program going into the CPU, the data coming back
    // from the RAM) are named instead, so no wire crosses another.
    Board& board = project.board;
    Layout l(board);
    project.clock = board.addPart(PartKind::Clock, {0, 5}).value();
    project.reset = l.input("Rst", {0, 10});
    project.switches = l.input("Chaves", {0, 18}, 16);

    const PartId cpu = l.chip(project.chips.back(), {26, 4});

    Part romPart = makePart(PartKind::Box, {4, 26}, 0, 0, 1, boxFromKey("rom"));
    romPart.width = 16;
    romPart.address = 16;
    romPart.rom = std::make_shared<const std::vector<std::uint64_t>>(program.begin(), program.end());
    // The program as the player reads it: its comments and labels in the chosen language. The words are
    // the same, since only comments and label names differ.
    romPart.source = i18n::tr(source);
    project.rom = l.add(std::move(romPart));
    project.ram = l.box("ram", {56, 10}, 16, 16);
    project.haltLed = l.output("Parou", {78, 6});
    project.outputProbe = l.output("Saída", {78, 8}, 16);
    project.pcProbe = l.output("PC", {78, 17}, 16);

    l.name(project.clock, 0, "Clk", false);
    l.nameInput(cpu, "Clk", "Clk");
    l.wire(project.reset, 0u, cpu, "Rst");
    l.wire(project.switches, 0u, cpu, "Chaves");
    l.nameInput(cpu, "Instr", "Instr", 16);
    l.nameInput(cpu, "Dado", "Dado", 16);

    l.wire(cpu, "Parou", project.haltLed, 0u);
    l.wire(cpu, "Saída", project.outputProbe, 0u);
    l.wire(cpu, "End", project.ram, "A");
    l.wire(cpu, "Valor", project.ram, "D");
    l.nameOutput(cpu, "Grava", "Grava");
    l.nameOutput(cpu, "PC", "PC", 16);

    l.nameInput(project.ram, "We", "Grava");
    l.nameInput(project.ram, "Clk", "Clk");
    l.nameOutput(project.ram, "Q", "Dado", 16);

    l.nameInput(project.rom, "A", "PC", 16);
    l.nameOutput(project.rom, "Q", "Instr", 16);
    l.name(project.pcProbe, 0, "PC", true, 16);
    return project;
}

}  // namespace bancada::editor
