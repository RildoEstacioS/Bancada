// The X-ray against the wires themselves. Each CPU's own board runs as a circuit of its own, with the ROM and
// the RAM played by this test, and before every clock edge each block's outputs are read where they are:
// the decoder's fields, the two register ports, the immediate, the ALU's result and Z, the value going back
// to the registers, PC+1 or PC+4. They must be what the X-ray writes on the same wires.
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "asm/assembler.hpp"
#include "asm/mips.hpp"
#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/computer16.hpp"
#include "editor/computer32.hpp"
#include "editor/computer32_view.hpp"
#include "editor/computer_view.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

PartId chipPart(const Board& board, ChipId chip) {
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Chip && part.chip == chip) return part.id;
    }
    return 0;
}

PartId boxPart(const Board& board, const char* key) {
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Box && part.box == boxFromKey(key)) return part.id;
    }
    return 0;
}

PartId labelled(const Board& board, PartKind kind, std::string_view label) {
    for (const Part& part : board.parts()) {
        if (part.kind == kind && part.label == label) return part.id;
    }
    return 0;
}

// A CPU's board run on its own: its inputs set by hand, its blocks read pin by pin.
struct Bench {
    const Board& board;
    BoardCircuit circuit;

    Bench(const Board& cpu, const ChipBoards* chips) : board(cpu), circuit(cpu, chips) {}

    std::uint64_t pin(PartId part, std::string_view name) {
        const Part& p = *board.part(part);
        for (unsigned i = 0; i < pinCount(p); ++i) {
            if (name == pinName(p, i)) return circuit.pinValue(part, i);
        }
        std::printf("  no pin %s\n", std::string(name).c_str());
        CHECK(false);
        return 0;
    }
    void set(std::string_view input, std::uint64_t value) {
        circuit.setToggle(labelled(board, PartKind::Toggle, input), value);
        circuit.settle();
    }
    std::uint64_t out(std::string_view output) {
        PartId id = labelled(board, PartKind::Probe, output);
        if (id == 0) id = labelled(board, PartKind::Led, output);
        return circuit.outputValue(id);
    }
    void clock() {
        set("Clk", 1);
        set("Clk", 0);
    }
};

int failures = 0;

void same(const char* what, const std::string& instruction, std::uint64_t wire, std::uint64_t xray) {
    if (wire == xray) return;
    if (++failures <= 12) {
        std::printf("  %s: %s is %llu on the wire, the X-ray says %llu\n", instruction.c_str(), what, static_cast<unsigned long long>(wire),
                    static_cast<unsigned long long>(xray));
    }
    CHECK(false);
}

// --- 16 bits ---

void run16(const std::vector<std::uint16_t>& program, std::uint16_t switches, int limit) {
    const std::vector<ChipData> chips = buildCpuChips(1);
    ChipBoards boards;
    for (const ChipData& chip : chips) boards[chip.id] = &chip.board;
    const Board& cpu = chips.back().board;
    Bench b(cpu, &boards);
    const PartId decoder = chipPart(cpu, chips[0].id);
    const PartId control = chipPart(cpu, chips[1].id);
    const PartId counter = chipPart(cpu, chips[2].id);
    const PartId registers = chipPart(cpu, chips[3].id);
    const PartId execute = chipPart(cpu, chips[4].id);
    const PartId back = boxPart(cpu, "mux4_n");
    const std::vector<PartId> file{registers, boxPart(chips[3].board, "regfile")};
    std::vector<std::uint16_t> ram(65536, 0);
    b.set("Chaves", switches);

    for (int step = 0; step < limit; ++step) {
        const auto pc = static_cast<std::uint16_t>(b.pin(counter, "PC"));
        const std::uint16_t word = pc < program.size() ? program[pc] : 0;
        b.set("Instr", word);
        b.set("Dado", ram[static_cast<std::uint16_t>(b.out("End"))]);
        ComputerState state;
        state.pc = pc;
        state.instruction = word;
        for (unsigned r = 0; r < 8; ++r) state.registers[r] = static_cast<std::uint16_t>(b.circuit.memoryWord(file, r));
        state.switches = switches;
        state.output = static_cast<std::uint16_t>(b.out("Saída"));
        state.memoryAtAddress = ram[static_cast<std::uint16_t>(b.out("End"))];
        const InstructionTrace t = traceInstruction(state);
        if (t.halts) return;

        same("PC+1", t.text, b.pin(counter, "Prox"), t.pcPlusOne);
        if (t.readsA) same("port A", t.text, b.pin(registers, "A"), t.valueA);
        if (t.readsB) same("port B", t.text, b.pin(registers, "B"), t.valueB);
        if (t.usesAlu) same("ALU result", t.text, b.pin(execute, "R"), t.aluResult);
        if (t.branches) same("Z", t.text, b.pin(execute, "Z"), t.aluResult == 0 ? 1u : 0u);
        if (t.branches) same("Desvia, from the control", t.text, b.pin(control, "Desvia"), t.branchTaken ? 1u : 0u);
        if (t.writeBack != InstructionTrace::WriteBack::None) same("write-back", t.text, b.pin(back, "Y"), t.writtenValue);
        if (t.usesImmediate) {
            const std::string_view field(t.immediateField);
            const char* pinName = field == "imm6" ? "Imm6" : field == "imm9" ? "Imm9" : field == "imm12" ? "Imm12" : "ImmH";
            same("immediate", t.text, b.pin(decoder, pinName), t.immediate);
        }
        if (t.readsMemory || t.writesMemory) same("address", t.text, b.out("End"), t.address);
        if (t.writesMemory) same("stored value", t.text, b.out("Valor"), t.memoryValue);

        const bool writes = b.out("Grava") != 0;
        const auto address = static_cast<std::uint16_t>(b.out("End"));
        const auto value = static_cast<std::uint16_t>(b.out("Valor"));
        b.clock();
        if (writes) ram[address] = value;
        same("next PC", t.text, b.pin(counter, "PC"), t.nextPc);
    }
}

// --- 32 bits ---

void run32(const mips::Program& program, std::uint32_t switches, int limit) {
    const std::vector<ChipData> chips = buildCpu32Chips(1);
    ChipBoards boards;
    for (const ChipData& chip : chips) boards[chip.id] = &chip.board;
    const Board& cpu = chips.back().board;
    Bench b(cpu, &boards);
    const PartId decoder = chipPart(cpu, chips[0].id);
    const PartId control = chipPart(cpu, chips[1].id);
    const PartId execute = chipPart(cpu, chips[3].id);
    const PartId registers = chipPart(cpu, chips[4].id);
    const PartId counter = chipPart(cpu, chips[5].id);
    const PartId system = chipPart(cpu, chips[6].id);
    const PartId back = boxPart(cpu, "mux4_n");
    const std::vector<PartId> file{registers, boxPart(chips[4].board, "regfile")};
    std::vector<std::uint32_t> ram(mips::kMemoryWords, 0);
    for (std::size_t i = 0; i < program.data.size(); ++i) ram[i] = program.data[i];
    b.set("Chaves", switches);
    const auto word = [](std::uint64_t address) { return (address >> 2) % mips::kMemoryWords; };

    for (int step = 0; step < limit; ++step) {
        const auto pc = static_cast<std::uint32_t>(b.pin(counter, "PC"));
        const std::size_t index = word(pc);
        const std::uint32_t instruction = index < program.text.size() ? program.text[index] : 0;
        b.set("Instr", instruction);
        b.set("Dado", ram[word(b.out("End"))]);
        std::array<std::uint32_t, 32> regs{};
        for (unsigned r = 0; r < 32; ++r) regs[r] = static_cast<std::uint32_t>(b.circuit.memoryWord(file, r));
        const Trace32 t = traceInstruction32(instruction, pc, regs, switches, ram[word(b.out("End"))]);
        if (t.halts) return;

        same("op", t.text, b.pin(decoder, "Op"), t.op);
        same("rs", t.text, b.pin(decoder, "Rs"), t.rs);
        same("rt", t.text, b.pin(decoder, "Rt"), t.rt);
        same("rd", t.text, b.pin(decoder, "Rd"), t.rd);
        same("shamt", t.text, b.pin(decoder, "Shamt"), t.shamt);
        same("funct", t.text, b.pin(decoder, "Funct"), t.funct);
        same("imm", t.text, b.pin(decoder, "Imm"), t.imm);
        same("PC+4", t.text, b.pin(counter, "Prox"), t.pc4);
        if (t.readsA) same("port A", t.text, b.pin(registers, "A"), t.valueA);
        if (t.readsB) same("port B", t.text, b.pin(registers, "B"), t.valueB);
        if (t.usesImmediate) same("extended immediate", t.text, b.pin(execute, "ImmExt"), t.extended);
        if (t.usesAlu) same("ALU result", t.text, b.pin(execute, "R"), t.aluResult);
        if (t.branches) same("Z", t.text, b.pin(execute, "Z"), t.zero ? 1u : 0u);
        if (t.branches) same("Desvia, from the control", t.text, b.pin(control, "Desvia"), t.taken ? 1u : 0u);
        if (t.back != Trace32::Back::None) {
            same("destination MUX", t.text, b.pin(control, "RegDest"), t.destination);
            same("write-back", t.text, b.pin(back, "Y"), t.writtenValue);
        }
        if (t.readsMemory || t.writesMemory) same("address", t.text, b.out("End"), t.address);
        if (t.writesMemory) same("stored value", t.text, b.out("Valor"), t.memoryValue);
        same("prints a number", t.text, b.pin(system, "ImprimeNum"), t.printsNumber ? 1u : 0u);
        same("prints a character", t.text, b.pin(system, "ImprimeCar"), t.printsChar ? 1u : 0u);

        const bool writes = b.out("Grava") != 0;
        const auto address = b.out("End");
        const auto value = static_cast<std::uint32_t>(b.out("Valor"));
        b.clock();
        if (writes) ram[word(address)] = value;
        same("next PC", t.text, b.pin(counter, "PC"), t.nextPc);
    }
}

mips::Program mipsProgram(const std::string& source) {
    const mips::Program program = mips::assemble(source);
    for (const assembly::Error& error : program.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    CHECK(program.ok());
    return program;
}

}  // namespace

TEST(the_16_bit_xray_shows_the_values_on_the_wires) {
    failures = 0;
    const assembly::Result program = assembly::assemble(R"(
                LI   R1, 300
                LUI  R2, 3
                ADD  R3, R1, R2
                SUB  R4, R2, R1
                AND  R5, R1, R2
                OR   R5, R1, R2
                XOR  R5, R1, R2
                NOT  R6, R1
                SHL  R6, R1
                SHR  R6, R2
                ADDI R6, R6, -5
                ST   R3, 3(R0)
                LD   R4, 3(R0)
                IN   R5
                OUT  R5
                LI   R1, 5
                LI   R2, 9
                BEQ  R1, R2, um
                BNE  R1, R2, um
        um:     BEQ  R2, R1, dois
                BNE  R2, R1, dois
        dois:   CALL sub
                JMP  fim
        sub:    JR   R7
        fim:    HLT
    )");
    CHECK(program.ok());
    run16(program.words, 0x1234, 80);
    // Random programs of every ALU instruction and memory access, with comparisons both ways round.
    std::mt19937 random(16);
    for (int round = 0; round < 4; ++round) {
        std::string source;
        for (int r = 1; r < 8; ++r) source += "        LI R" + std::to_string(r) + ", " + std::to_string(random() % 512) + "\n";
        static constexpr const char* kOps[] = {"ADD", "SUB", "AND", "OR", "XOR"};
        for (int i = 0; i < 30; ++i) {
            const auto reg = [&] { return "R" + std::to_string(random() % 8); };
            switch (random() % 4) {
                case 0: source += std::string("        ") + kOps[random() % 5] + " " + reg() + ", " + reg() + ", " + reg() + "\n"; break;
                case 1: source += "        ADDI " + reg() + ", " + reg() + ", " + std::to_string(static_cast<int>(random() % 64) - 32) + "\n"; break;
                case 2: {
                    const std::string label = "p" + std::to_string(i);
                    source += "        BEQ " + reg() + ", " + reg() + ", " + label + "\n" + label + ":\n";
                    break;
                }
                default: source += "        ST " + reg() + ", " + std::to_string(random() % 30) + "(R0)\n        LD " + reg() + ", " + std::to_string(random() % 30) + "(R0)\n"; break;
            }
        }
        source += "        HLT\n";
        const assembly::Result random16 = assembly::assemble(source);
        CHECK(random16.ok());
        run16(random16.words, 0, 120);
    }
}

TEST(the_32_bit_xray_shows_the_values_on_the_wires) {
    failures = 0;
    run32(mipsProgram(".data\nv: .word 5, -3, 100\n.text\n"
                      "  la $t0, v\n  lw $t1, 0($t0)\n  lw $t2, 4($t0)\n  sw $t1, 8($t0)\n  add $t3, $t1, $t2\n  sub $t4, $t1, $t2\n"
                      "  and $t5, $t1, $t2\n  or $t6, $t1, $t2\n  xor $t7, $t1, $t2\n  nor $s0, $t1, $t2\n  slt $s1, $t2, $t1\n  sltu $s2, $t2, $t1\n"
                      "  sll $s3, $t2, 3\n  srl $s4, $t2, 3\n  sra $s5, $t2, 3\n  addi $s6, $t2, -9\n  slti $s7, $t2, 0\n  sltiu $t8, $t2, 5\n"
                      "  andi $t9, $t2, 0xF0\n  ori $v1, $t2, 1\n  xori $a1, $t2, 0xFFFF\n  lui $a2, 0x1234\n"
                      "  beq $t1, $t1, a\n  nop\na: bne $t1, $t2, b\n  nop\nb: beq $t1, $t2, a\n  bne $t1, $t1, a\n"
                      "  jal f\n  li $v0, 5\n  syscall\n  li $a0, 7\n  li $v0, 1\n  syscall\n"
                      "  li $v0, 11\n  syscall\n  j end\nf: jr $ra\nend: li $v0, 10\n  syscall\n"),
          0x2A, 200);
    std::mt19937 random(32);
    static constexpr const char* kThree[] = {"add", "sub", "and", "or", "xor", "nor", "slt", "sltu"};
    for (int round = 0; round < 4; ++round) {
        std::string source;
        const auto reg = [&] { return std::string(mips::registerName(8 + static_cast<unsigned>(random() % 10))); };
        for (int i = 0; i < 6; ++i) source += "  li " + reg() + ", " + std::to_string(static_cast<std::int32_t>(random())) + "\n";
        for (int i = 0; i < 40; ++i) {
            switch (random() % 4) {
                case 0: source += std::string("  ") + kThree[random() % 8] + " " + reg() + ", " + reg() + ", " + reg() + "\n"; break;
                case 1: source += "  addi " + reg() + ", " + reg() + ", " + std::to_string(static_cast<int>(random() % 2000) - 1000) + "\n"; break;
                case 2: source += "  sra " + reg() + ", " + reg() + ", " + std::to_string(random() % 32) + "\n"; break;
                default: source += "  sw " + reg() + ", " + std::to_string(4 * (random() % 16)) + "($gp)\n  lw " + reg() + ", " + std::to_string(4 * (random() % 16)) + "($gp)\n"; break;
            }
        }
        source += "  li $v0, 10\n  syscall\n";
        run32(mipsProgram(source), 0, 200);
    }
}

// --- The X-ray's wires against the circuit's -------------------------------------------------------------
// Every wire the X-ray draws between two blocks is a connection it claims the circuit has: here each one is
// written as the two pins it joins on the CPU's board, and the circuit must have them on the same net.

namespace {

// One end of a wire: a part of the CPU's board and a pin of it by name; "" for an input's or an output's pin,
// "#n" for a gate's pin n.
struct End {
    PartId part;
    const char* pin;
};

struct Topology {
    const Board& board;
    BoardCircuit circuit;
    int failed = 0;

    Topology(const Board& cpu, const ChipBoards* chips) : board(cpu), circuit(cpu, chips) {}

    std::optional<std::uint32_t> net(End end) {
        const Part* p = board.part(end.part);
        if (p == nullptr) return std::nullopt;
        if (end.pin[0] == '\0') return circuit.netOfPin(end.part, 0);
        if (end.pin[0] == '#') return circuit.netOfPin(end.part, static_cast<unsigned>(end.pin[1] - '0'));  // by index
        for (unsigned i = 0; i < pinCount(*p); ++i) {
            if (std::string_view(end.pin) == pinName(*p, i)) return circuit.netOfPin(end.part, i);
        }
        return std::nullopt;
    }

    // The X-ray's wire, named as the X-ray names it, joins these two pins.
    void wire(const char* xray, End from, End to) {
        const std::optional<std::uint32_t> a = net(from);
        const std::optional<std::uint32_t> b = net(to);
        if (a && b && *a == *b) return;
        std::printf("  the X-ray's wire %s (%s to %s) isn't in the circuit\n", xray, from.pin, to.pin);
        ++failed;
    }
};

PartId part(const Board& board, PartKind kind, std::string_view label) {
    for (const Part& p : board.parts()) {
        if (p.kind == kind && p.label == label) return p.id;
    }
    return 0;
}

// The gate whose output is on this net: the circuit puts one between a control line and its block.
PartId gateFeeding(Topology& t, End end, PartKind kind) {
    const std::optional<std::uint32_t> target = t.net(end);
    for (const Part& p : t.board.parts()) {
        if (p.kind == kind && t.circuit.netOfPin(p.id, p.inputs) == target) return p.id;
    }
    return 0;
}

}  // namespace

TEST(every_wire_of_the_16_bit_xray_is_in_the_circuit) {
    const std::vector<ChipData> chips = buildCpuChips(1);
    ChipBoards boards;
    for (const ChipData& chip : chips) boards[chip.id] = &chip.board;
    const Board& cpu = chips.back().board;
    Topology t(cpu, &boards);
    const PartId decoder = chipPart(cpu, chips[0].id), control = chipPart(cpu, chips[1].id), counter = chipPart(cpu, chips[2].id);
    const PartId registers = chipPart(cpu, chips[3].id), execute = chipPart(cpu, chips[4].id);
    const PartId back = boxPart(cpu, "mux4_n"), output = boxPart(cpu, "register");
    const auto in = [&](const char* name) { return End{part(cpu, PartKind::Toggle, name), ""}; };
    const auto out = [&](const char* name) {
        const PartId probe = part(cpu, PartKind::Probe, name);
        return End{probe != 0 ? probe : part(cpu, PartKind::Led, name), ""};
    };

    t.wire("PC para a ROM", {counter, "PC"}, out("PC"));
    t.wire("ROM para o Decodificador", in("Instr"), {decoder, "Instr"});
    t.wire("op para o Controle", {decoder, "Op"}, {control, "Op"});
    t.wire("rs para lê A", {decoder, "Rs"}, {registers, "Rs"});
    t.wire("rt para lê B", {decoder, "Rt"}, {registers, "Rt"});
    t.wire("rd para escreve", {decoder, "Rd"}, {registers, "Rd"});
    t.wire("imm6 para o Extensor", {decoder, "Imm6"}, {execute, "Imm6"});
    t.wire("imm9 para o Extensor", {decoder, "Imm9"}, {execute, "Imm9"});
    t.wire("imm9 × 128 para o Extensor", {decoder, "ImmH"}, {execute, "ImmH"});
    t.wire("A para a ULA", {registers, "A"}, {execute, "A"});
    t.wire("B para o MUX B", {registers, "B"}, {execute, "B"});
    t.wire("Z para o Controle", {execute, "Z"}, {control, "Z"});
    t.wire("Controle para o Próximo PC: desvia", {control, "Desvia"}, {counter, "Desvia"});
    t.wire("Controle para o Próximo PC: salta", {control, "Salto"}, {counter, "Salto"});
    t.wire("Controle para o Próximo PC: salta p/ reg", {control, "SaltoR"}, {counter, "SaltoR"});
    t.wire("imm6 para o Alvo do desvio", {decoder, "Imm6"}, {counter, "Imm6"});
    t.wire("imm12 para o Próximo PC", {decoder, "Imm12"}, {counter, "Imm12"});
    t.wire("A para o Próximo PC (JR)", {registers, "A"}, {counter, "Reg"});
    t.wire("A para a Saída", {registers, "A"}, {output, "D"});
    t.wire("SaiEsc para a Saída", {control, "SaiEsc"}, {output, "En"});
    t.wire("ULA para o endereço da RAM", {execute, "R"}, out("End"));
    t.wire("B para a RAM", {registers, "B"}, out("Valor"));
    t.wire("MemEsc para a RAM", {control, "MemEsc"}, out("Grava"));
    t.wire("RAM para o MUX de volta", in("Dado"), {back, "D1"});
    t.wire("ULA para o MUX de volta", {execute, "R"}, {back, "D0"});
    t.wire("PC + 1 para o MUX de volta", {counter, "Prox"}, {back, "D2"});
    t.wire("Chaves para o MUX de volta", in("Chaves"), {back, "D3"});
    t.wire("Volta para o MUX de volta", {control, "Volta"}, {back, "Sel"});
    t.wire("MUX de volta para os Registradores", {back, "Y"}, {registers, "D"});
    t.wire("RegEsc para os Registradores", {control, "RegEsc"}, {registers, "Esc"});
    t.wire("UsaImm para o MUX B", {control, "UsaImm"}, {execute, "UsaImm"});
    t.wire("ZeraA para o MUX A", {control, "ZeraA"}, {execute, "ZeraA"});
    t.wire("OpULA para a ULA", {control, "OpULA"}, {execute, "OpULA"});
    t.wire("QualImm para o Extensor", {control, "QualImm"}, {execute, "QualImm"});
    CHECK_EQ(t.failed, 0);
}

TEST(every_wire_of_the_32_bit_xray_is_in_the_circuit) {
    const std::vector<ChipData> chips = buildCpu32Chips(1);
    ChipBoards boards;
    for (const ChipData& chip : chips) boards[chip.id] = &chip.board;
    const Board& cpu = chips.back().board;
    Topology t(cpu, &boards);
    const PartId decoder = chipPart(cpu, chips[0].id), control = chipPart(cpu, chips[1].id), execute = chipPart(cpu, chips[3].id);
    const PartId registers = chipPart(cpu, chips[4].id), counter = chipPart(cpu, chips[5].id), system = chipPart(cpu, chips[6].id);
    const PartId back = boxPart(cpu, "mux4_n"), printed = boxPart(cpu, "register");
    const auto in = [&](const char* name) { return End{part(cpu, PartKind::Toggle, name), ""}; };
    const auto out = [&](const char* name) {
        const PartId probe = part(cpu, PartKind::Probe, name);
        return End{probe != 0 ? probe : part(cpu, PartKind::Led, name), ""};
    };

    t.wire("PC para a ROM", {counter, "PC"}, out("PC"));
    t.wire("ROM para o Decodificador", in("Instr"), {decoder, "Instr"});
    t.wire("op para o Controle", {decoder, "Op"}, {control, "Op"});
    t.wire("funct para o Controle", {decoder, "Funct"}, {control, "Funct"});
    t.wire("rs para lê A", {decoder, "Rs"}, {registers, "Rs"});
    t.wire("rt para lê B", {decoder, "Rt"}, {registers, "Rt"});
    t.wire("rd para o MUX de destino", {decoder, "Rd"}, {registers, "Rd"});
    t.wire("imm para o Extensor", {decoder, "Imm"}, {execute, "Imm"});
    t.wire("shamt para a ULA", {decoder, "Shamt"}, {execute, "Shamt"});
    t.wire("alvo para o Alvo do salto", in("Instr"), {counter, "Instr"});
    t.wire("A para a ULA", {registers, "A"}, {execute, "A"});
    t.wire("B para o MUX B", {registers, "B"}, {execute, "B"});
    t.wire("Extensor × 4 para o Alvo do desvio", {execute, "ImmExt"}, {counter, "ImmExt"});
    t.wire("Z para o Controle", {execute, "Z"}, {control, "Z"});
    t.wire("Controle para o Próximo PC: desvia", {control, "Desvia"}, {counter, "Desvia"});
    t.wire("Controle para o Próximo PC: salta", {control, "Salto"}, {counter, "Salto"});
    t.wire("Controle para o Próximo PC: salta p/ reg", {control, "SaltoR"}, {counter, "SaltoR"});
    t.wire("A para o Próximo PC (jr)", {registers, "A"}, {counter, "Reg"});
    t.wire("ULA para o endereço da RAM", {execute, "R"}, out("End"));
    t.wire("B para a RAM", {registers, "B"}, out("Valor"));
    t.wire("MemEsc para a RAM", {control, "MemEsc"}, out("Grava"));
    t.wire("RAM para o MUX de volta", in("Dado"), {back, "D1"});
    t.wire("ULA para o MUX de volta", {execute, "R"}, {back, "D0"});
    t.wire("PC + 4 para o MUX de volta", {counter, "Prox"}, {back, "D2"});
    t.wire("Chaves para o MUX de volta", in("Chaves"), {back, "D3"});
    t.wire("Volta para o MUX de volta", {control, "Volta"}, {back, "Sel"});
    t.wire("MUX de volta para os Registradores", {back, "Y"}, {registers, "D"});
    t.wire("OpULA para a ULA", {control, "OpULA"}, {execute, "OpULA"});
    t.wire("UsaImm para o MUX B", {control, "UsaImm"}, {execute, "UsaImm"});
    t.wire("SinalImm para o Extensor", {control, "SinalImm"}, {execute, "SinalImm"});
    t.wire("RegDest para o MUX de destino", {control, "RegDest"}, {registers, "RegDest"});
    t.wire("Sistema para a unidade Sistema", {control, "Sistema"}, {system, "Sistema"});
    t.wire("Sistema para os Registradores ($v0, $a0)", {control, "Sistema"}, {registers, "Sistema"});
    t.wire("$v0 para a unidade Sistema", {registers, "A"}, {system, "V0"});
    t.wire("$a0 para o Console", {registers, "B"}, {printed, "D"});
    // RegEsc reaches the registers through an OR with LeChaves: syscall 5 writes $v0 too.
    const PartId writes = gateFeeding(t, {registers, "Esc"}, PartKind::Or);
    CHECK(writes != 0);
    if (writes != 0) {
        t.wire("RegEsc para os Registradores", {control, "RegEsc"}, {writes, "#0"});
        t.wire("LeChaves para os Registradores", {system, "LeChaves"}, {writes, "#1"});
    }
    CHECK_EQ(t.failed, 0);
}

TEST(a_wire_the_circuit_lacks_is_caught) {
    const std::vector<ChipData> chips = buildCpu32Chips(1);
    ChipBoards boards;
    for (const ChipData& chip : chips) boards[chip.id] = &chip.board;
    const Board& cpu = chips.back().board;
    Topology t(cpu, &boards);
    const PartId decoder = chipPart(cpu, chips[0].id), registers = chipPart(cpu, chips[4].id);
    std::printf("  (two wrong wires on purpose:)\n");
    t.wire("rs para lê B", {decoder, "Rs"}, {registers, "Rt"});
    t.wire("um pino que não existe", {decoder, "Nada"}, {registers, "Rt"});
    CHECK_EQ(t.failed, 2);
}

int main() { return check::runAll(); }
