// The computer against a reference: an emulator written from the instruction set's definition, sharing
// no code with the circuit. Programs run on both, and every cycle has to agree.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/computer16.hpp"
#include "editor/computer_view.hpp"
#include "editor/session.hpp"
#include "editor/text_edit.hpp"

using namespace bancada;
using namespace bancada::editor;
using namespace bancada::assembly;

namespace {

// What the instruction set says should happen, one instruction at a time.
class Emulator {
public:
    explicit Emulator(const std::vector<std::uint16_t>& program, std::uint16_t switches = 0) : rom_(65536, 0), ram_(65536, 0), switches_(switches) {
        std::copy(program.begin(), program.end(), rom_.begin());
    }

    std::uint16_t pc() const { return pc_; }
    std::uint16_t output() const { return output_; }
    bool halted() const { return halted_; }
    std::uint16_t reg(unsigned index) const { return regs_[index]; }

    void step() {
        const std::uint16_t word = rom_[pc_];
        const unsigned op = word >> 12;
        const unsigned rd = (word >> 9) & 7;
        const unsigned rs = (word >> 6) & 7;
        const unsigned rt = (word >> 3) & 7;
        const unsigned func = word & 7;
        const auto imm6 = static_cast<std::uint16_t>((word & 32) != 0 ? (word & 63) | 0xFFC0 : word & 63);
        const std::uint16_t imm9 = word & 511;
        const std::uint16_t imm12 = word & 4095;
        const auto next = static_cast<std::uint16_t>(pc_ + 1);
        std::uint16_t target = next;
        switch (op) {
            case kOpAlu: write(rd, alu(func, regs_[rs], regs_[rt])); break;
            case kOpAddi: write(rd, static_cast<std::uint16_t>(regs_[rs] + imm6)); break;
            case kOpLi: write(rd, imm9); break;
            case kOpLui: write(rd, static_cast<std::uint16_t>(imm9 << 7)); break;
            case kOpLd: write(rd, ram_[static_cast<std::uint16_t>(regs_[rs] + imm6)]); break;
            case kOpSt: ram_[static_cast<std::uint16_t>(regs_[rs] + imm6)] = regs_[rd]; break;
            case kOpBeq: if (regs_[rs] == regs_[rd]) target = static_cast<std::uint16_t>(next + imm6); break;
            case kOpBne: if (regs_[rs] != regs_[rd]) target = static_cast<std::uint16_t>(next + imm6); break;
            case kOpJmp: target = imm12; break;
            case kOpCall: write(7, next); target = imm12; break;
            case kOpJr: target = regs_[rs]; break;
            case kOpOut: output_ = regs_[rs]; break;
            case kOpIn: write(rd, switches_); break;
            case kOpHalt: halted_ = true; target = pc_; break;
            default: break;
        }
        pc_ = target;
    }

private:
    void write(unsigned reg, std::uint16_t value) {
        if (reg != 0) regs_[reg] = value;
    }

    static std::uint16_t alu(unsigned func, std::uint16_t a, std::uint16_t b) {
        switch (func) {
            case kAnd: return a & b;
            case kOr: return a | b;
            case kXor: return a ^ b;
            case kAdd: return static_cast<std::uint16_t>(a + b);
            case kSub: return static_cast<std::uint16_t>(a - b);
            case kNot: return static_cast<std::uint16_t>(~a);
            case kShl: return static_cast<std::uint16_t>(a << 1);
            default: return a >> 1;
        }
    }

    std::vector<std::uint16_t> rom_;
    std::vector<std::uint16_t> ram_;
    std::array<std::uint16_t, 8> regs_{};
    std::uint16_t pc_ = 0;
    std::uint16_t output_ = 0;
    std::uint16_t switches_ = 0;
    bool halted_ = false;
};

// The computer as a player would have it on the board, driven a clock cycle at a time.
class Machine {
public:
    explicit Machine(const std::vector<std::uint16_t>& program, std::uint16_t switches = 0)
        : project_(buildComputer16(program, "")) {
        for (const ChipData& chip : project_.chips) chips_[chip.id] = &chip.board;
        circuit_ = std::make_unique<BoardCircuit>(project_.board, &chips_);
        circuit_->setToggle(project_.switches, switches);
        circuit_->settle();
    }

    void cycle() {
        circuit_->setClocks(true);
        circuit_->settle();
        circuit_->setClocks(false);
        circuit_->settle();
    }

    std::uint64_t pc() const { return circuit_->outputValue(project_.pcProbe); }
    std::uint64_t output() const { return circuit_->outputValue(project_.outputProbe); }
    bool halted() const { return circuit_->outputOn(project_.haltLed); }
    bool healthy() const { return !circuit_->hasWidthConflict() && !circuit_->hasConflict() && !circuit_->oscillating(); }

private:
    ComputerProject project_;
    ChipBoards chips_;
    std::unique_ptr<BoardCircuit> circuit_;
};

std::vector<std::uint16_t> assembled(const char* source) {
    const Result result = assemble(source);
    for (const Error& error : result.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    CHECK(result.ok());
    return result.words;
}

// Runs both until the program stops, comparing after every instruction.
bool runBoth(const std::vector<std::uint16_t>& program, int maxCycles, std::uint64_t seed = 0) {
    Machine machine(program, 0x1234);
    Emulator emulator(program, 0x1234);
    for (int cycle = 0; cycle < maxCycles && !emulator.halted(); ++cycle) {
        if (machine.pc() != emulator.pc() || machine.output() != emulator.output()) {
            std::printf("  seed %llu, cycle %d: pc %llu vs %u, output %llu vs %u\n", static_cast<unsigned long long>(seed), cycle,
                        static_cast<unsigned long long>(machine.pc()), emulator.pc(),
                        static_cast<unsigned long long>(machine.output()), emulator.output());
            return false;
        }
        emulator.step();
        machine.cycle();
    }
    return machine.pc() == emulator.pc() && machine.output() == emulator.output() && machine.healthy();
}

std::uint64_t finalOutput(const char* source, int maxCycles = 5000) {
    const std::vector<std::uint16_t> program = assembled(source);
    Machine machine(program);
    for (int cycle = 0; cycle < maxCycles && !machine.halted(); ++cycle) machine.cycle();
    CHECK(machine.halted());
    return machine.output();
}

}  // namespace

// --- the assembler ---------------------------------------------------------------------------------

TEST(instructions_encode_as_defined) {
    const std::vector<std::uint16_t> words = assembled(R"(
        ADD R1, R2, R3
        SUB R7, R0, R1
        ADDI R1, R2, -1
        LI R3, 511
        LUI R4, 1
        LD R1, 4(R2)
        ST R1, (R2)
        JMP 100
        CALL 4095
        JR R5
        RET
        OUT R6
        IN R1
        HLT
    )");
    const std::vector<std::uint16_t> expected{
        0x0000 | (1 << 9) | (2 << 6) | (3 << 3) | kAdd,
        0x0000 | (7 << 9) | (0 << 6) | (1 << 3) | kSub,
        0x1000 | (1 << 9) | (2 << 6) | 63,
        0x2000 | (3 << 9) | 511,
        0x3000 | (4 << 9) | 1,
        0x4000 | (1 << 9) | (2 << 6) | 4,
        0x5000 | (1 << 9) | (2 << 6),
        0x8000 | 100,
        0x9000 | 4095,
        0xA000 | (5 << 6),
        0xA000 | (7 << 6),
        0xB000 | (6 << 6),
        0xC000 | (1 << 9),
        0xF000,
    };
    CHECK(words == expected);
}

TEST(labels_work_before_and_after_they_are_defined) {
    const std::vector<std::uint16_t> words = assembled(R"(
        inicio: JMP fim
                NOP
        fim:    BNE R0, R1, inicio
                BEQ R0, R1, depois
                NOP
        depois: HLT
    )");
    CHECK_EQ(words[0], 0x8000 | 2);
    CHECK_EQ(words[2] & 63, 64 - 3);  // back to 0 from the next instruction, 3
    CHECK_EQ(words[3] & 63, 1);
}

TEST(numbers_can_be_written_in_any_base_and_case_does_not_matter) {
    const std::vector<std::uint16_t> words = assembled("li r1, 0x10\nLI R2, 0b101\nLi R3, 7 ; comentario\n.word 0xBEEF\n.word -1");
    CHECK_EQ(words[0] & 511, 16);
    CHECK_EQ(words[1] & 511, 5);
    CHECK_EQ(words[2] & 511, 7);
    CHECK_EQ(words[3], 0xBEEF);
    CHECK_EQ(words[4], 0xFFFF);
}

TEST(mistakes_are_reported_with_their_line) {
    const Result result = assemble(R"(NOP
        FOO R1
        ADD R1, R2
        ADD R1, R2, R9
        LI R1, 512
        ADDI R1, R1, 40
        JMP nowhere
        x: NOP
        x: NOP
        1bad: NOP
        LD R1, R2
        BEQ R0, R0, 1000
    )");
    CHECK(!result.ok());
    std::vector<int> lines;
    for (const Error& error : result.errors) lines.push_back(error.line);
    for (int line : {2, 3, 4, 5, 6, 7, 9, 10, 11, 12}) CHECK(std::find(lines.begin(), lines.end(), line) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 1) == lines.end());
}

TEST(an_empty_program_is_fine) {
    CHECK(assemble("").ok());
    CHECK(assemble("  ; nada aqui\n\n").words.empty());
}

TEST(any_sixteen_bit_constant_can_be_loaded) {
    for (int value : {0, 1, 31, 32, 127, 128, 1000, 0x1234, 0xFFFF}) {
        const std::string source = "LIW R1, " + std::to_string(value) + "\nOUT R1\nHLT\n";
        CHECK_EQ(finalOutput(source.c_str()), value);
    }
}

// --- the computer ----------------------------------------------------------------------------------

TEST(the_circuit_has_no_wiring_faults) {
    const Machine machine({});
    CHECK(machine.healthy());
}

TEST(the_example_program_adds_one_to_ten) { CHECK_EQ(finalOutput(exampleProgram()), 55); }

TEST(the_computer_multiplies_by_repeated_addition) {
    CHECK_EQ(finalOutput(R"(
                LI R1, 7
                LI R2, 6
                LI R3, 0
        laco:   ADD R3, R3, R1
                ADDI R2, R2, -1
                BNE R2, R0, laco
                OUT R3
                HLT
    )"), 42);
}

TEST(the_computer_runs_fibonacci) {
    CHECK_EQ(finalOutput(R"(
                LI R1, 0
                LI R2, 1
                LI R4, 20
        laco:   ADD R3, R1, R2
                MOV R1, R2
                MOV R2, R3
                ADDI R4, R4, -1
                BNE R4, R0, laco
                OUT R1
                HLT
    )"), 6765);
}

TEST(memory_keeps_a_table_written_in_a_loop) {
    CHECK_EQ(finalOutput(R"(
                LI R1, 0        ; endereco
                LI R2, 10       ; quantos
        grava:  ST R1, 0(R1)
                ADDI R1, R1, 1
                BNE R1, R2, grava
                LI R1, 0
                LI R3, 0
        soma:   LD R4, 0(R1)
                ADD R3, R3, R4
                ADDI R1, R1, 1
                BNE R1, R2, soma
                OUT R3
                HLT
    )"), 45);
}

TEST(subroutines_call_and_return) {
    CHECK_EQ(finalOutput(R"(
                LI R1, 5
                CALL dobra
                CALL dobra
                OUT R1
                HLT
        dobra:  ADD R1, R1, R1
                RET
    )"), 20);
}

TEST(a_call_inside_a_call_works_with_a_stack_in_memory) {
    CHECK_EQ(finalOutput(R"(
                LI R6, 100          ; a pilha
                LI R1, 3
                CALL fora
                OUT R1
                HLT
        fora:   ADDI R6, R6, -1
                ST R7, 0(R6)
                CALL dentro
                ADD R1, R1, R1
                LD R7, 0(R6)
                ADDI R6, R6, 1
                RET
        dentro: ADDI R1, R1, 4
                RET
    )"), 14);
}

TEST(register_zero_stays_zero) {
    CHECK_EQ(finalOutput("LI R0, 99\nADDI R0, R0, 5\nOUT R0\nHLT\n"), 0);
}

TEST(the_switches_reach_the_program) {
    const std::vector<std::uint16_t> program = assembled("IN R1\nOUT R1\nHLT\n");
    Machine machine(program, 0xBEEF);
    for (int i = 0; i < 5; ++i) machine.cycle();
    CHECK_EQ(machine.output(), 0xBEEF);
}

TEST(a_halted_computer_stays_where_it_stopped) {
    const std::vector<std::uint16_t> program = assembled("LI R1, 1\nHLT\nLI R1, 2\nOUT R1\n");
    Machine machine(program);
    for (int i = 0; i < 20; ++i) machine.cycle();
    CHECK(machine.halted());
    CHECK_EQ(machine.pc(), 1);
    CHECK_EQ(machine.output(), 0);
}

TEST(every_instruction_agrees_with_the_reference_on_hand_written_programs) {
    CHECK(runBoth(assembled(exampleProgram()), 200));
    CHECK(runBoth(assembled(R"(
                LIW R1, 0xF0F0
                LIW R2, 0x3333
                AND R3, R1, R2
                OUT R3
                OR R3, R1, R2
                OUT R3
                XOR R3, R1, R2
                OUT R3
                SUB R3, R1, R2
                OUT R3
                SUB R3, R2, R1
                OUT R3
                NOT R3, R1
                OUT R3
                SHL R3, R1
                OUT R3
                SHR R3, R1
                OUT R3
                IN R5
                OUT R5
                HLT
    )"), 200));
}

// Random programs: every kind of instruction, on random registers, with branches that only go forward so
// that every program ends. At the end, all the registers are shown, so nothing can go wrong unseen.
TEST(random_programs_agree_with_the_reference_on_every_cycle) {
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        std::mt19937 random(static_cast<std::uint32_t>(seed));
        const auto below = [&](std::uint32_t n) { return static_cast<int>(random() % n); };
        const auto r = [&] { return std::string("R") + std::to_string(below(8)); };

        std::string source;
        constexpr int kLength = 120;
        for (int line = 0; line < kLength; ++line) {
            const std::string label = "L" + std::to_string(line);
            source += label + ": ";
            const std::string forward = "L" + std::to_string(std::min(kLength, line + 1 + below(20)));
            switch (below(19)) {
                case 0: source += "ADD " + r() + "," + r() + "," + r(); break;
                case 1: source += "SUB " + r() + "," + r() + "," + r(); break;
                case 2: source += "AND " + r() + "," + r() + "," + r(); break;
                case 3: source += "OR " + r() + "," + r() + "," + r(); break;
                case 4: source += "XOR " + r() + "," + r() + "," + r(); break;
                case 5: source += "NOT " + r() + "," + r(); break;
                case 6: source += "SHL " + r() + "," + r(); break;
                case 7: source += "SHR " + r() + "," + r(); break;
                case 8: source += "ADDI " + r() + "," + r() + "," + std::to_string(below(64) - 32); break;
                case 9: source += "LI " + r() + "," + std::to_string(below(512)); break;
                case 10: source += "LUI " + r() + "," + std::to_string(below(512)); break;
                case 11: source += "ST " + r() + "," + std::to_string(below(64) - 32) + "(" + r() + ")"; break;
                case 12: source += "LD " + r() + "," + std::to_string(below(64) - 32) + "(" + r() + ")"; break;
                case 13: source += "BEQ " + r() + "," + r() + "," + forward; break;
                case 14: source += "BNE " + r() + "," + r() + "," + forward; break;
                case 15: source += "OUT " + r(); break;
                case 16: source += "IN " + r(); break;
                case 17: source += "CALL " + forward; break;
                default: source += "JMP " + forward; break;
            }
            source += "\n";
        }
        source += "L" + std::to_string(kLength) + ": NOP\n";
        for (int reg = 0; reg < 8; ++reg) source += "OUT R" + std::to_string(reg) + "\n";
        source += "HLT\n";

        const Result result = assemble(source);
        CHECK(result.ok());
        if (!result.ok()) {
            std::printf("  seed %llu: line %d: %s\n", static_cast<unsigned long long>(seed), result.errors[0].line, result.errors[0].message.c_str());
            return;
        }
        if (!runBoth(result.words, 400, seed)) {
            CHECK(false);
            return;
        }
    }
}

// Now anything goes: branches back, returns and jumps to wherever a register points. These may never end,
// so what's compared is every cycle of the first few hundred, with outputs all over the place.
TEST(wild_programs_agree_with_the_reference_on_every_cycle) {
    for (std::uint64_t seed = 1000; seed <= 1040; ++seed) {
        std::mt19937 random(static_cast<std::uint32_t>(seed));
        const auto below = [&](std::uint32_t n) { return static_cast<int>(random() % n); };
        const auto r = [&] { return std::string("R") + std::to_string(below(8)); };

        std::string source;
        constexpr int kLength = 60;
        for (int line = 0; line < kLength; ++line) {
            const int low = std::max(0, line - 15);
            const std::string nearby = "L" + std::to_string(low + below(static_cast<std::uint32_t>(std::min(kLength, line + 16) - low)));
            source += "L" + std::to_string(line) + ": ";
            switch (below(10)) {
                case 0: source += "ADD " + r() + "," + r() + "," + r(); break;
                case 1: source += "ADDI " + r() + "," + r() + "," + std::to_string(below(64) - 32); break;
                case 2: source += "LI " + r() + "," + std::to_string(below(kLength)); break;
                case 3: source += "BNE " + r() + "," + r() + "," + nearby; break;
                case 4: source += "BEQ " + r() + "," + r() + "," + nearby; break;
                case 5: source += "CALL " + nearby; break;
                case 6: source += "RET"; break;
                case 7: source += "JR " + r(); break;
                default: source += "OUT " + r(); break;
            }
            source += "\n";
        }
        const Result result = assemble(source);
        CHECK(result.ok());
        if (!result.ok() || !runBoth(result.words, 300, seed)) {
            CHECK(false);
            return;
        }
    }
}

// --- the two boxes the decoder needed --------------------------------------------------------------

namespace {

// A box between switches and a probe, with each input set from the list.
std::uint64_t boxResult(const char* key, std::uint8_t width, std::uint8_t address, const std::vector<std::uint64_t>& values) {
    Board board;
    Part box = makePart(PartKind::Box, {30, 0}, 0, 0, 1, boxFromKey(key));
    box.width = width;
    box.address = address;
    const PartId boxId = *board.addPart(box);
    const Part placed = *board.part(boxId);  // a copy: adding parts moves them
    std::vector<PartId> toggles;
    for (unsigned i = 0; i < values.size(); ++i) {
        Part toggle = makePart(PartKind::Toggle, {0, static_cast<int>(i) * 6});
        toggle.width = static_cast<std::uint8_t>(pinBits(placed, i));
        toggles.push_back(*board.addPart(toggle));
        const Point from = pinPosition(*board.part(toggles.back()), 0);
        const Point to = pinPosition(placed, i);
        board.connect({Drop::Kind::Pin, from, toggles.back(), 0}, {Drop::Kind::Pin, to, boxId, static_cast<std::uint8_t>(i)}, {from, {20, from.y}, {20, to.y}, to});
    }
    Part probe = makePart(PartKind::Probe, {50, 0});
    probe.width = width;
    const PartId probeId = *board.addPart(probe);
    const Point out = pinPosition(placed, static_cast<unsigned>(values.size()));
    const Point in = pinPosition(*board.part(probeId), 0);
    board.connect({Drop::Kind::Pin, out, boxId, static_cast<std::uint8_t>(values.size())}, {Drop::Kind::Pin, in, probeId, 0}, {out, {45, out.y}, {45, in.y}, in});

    ChipBoards none;
    BoardCircuit circuit(board, &none);
    for (std::size_t i = 0; i < values.size(); ++i) circuit.setToggle(toggles[i], values[i]);
    circuit.settle();
    CHECK(!circuit.hasWidthConflict());
    return circuit.outputValue(probeId);
}

}  // namespace

TEST(the_extender_keeps_a_negative_number_negative) {
    CHECK_EQ(boxResult("extender", 16, 6, {0b100000, 1}), 0xFFE0);
    CHECK_EQ(boxResult("extender", 16, 6, {0b011111, 1}), 0b011111);
    CHECK_EQ(boxResult("extender", 16, 6, {0b100000, 0}), 0b100000);
    CHECK_EQ(boxResult("extender", 8, 12, {0xABC, 1}), 0xBC);
    CHECK_EQ(boxResult("extender", 8, 8, {0xBC, 1}), 0xBC);
}

TEST(the_shifter_moves_both_ways_and_fills_with_zeros) {
    for (std::uint64_t amount = 0; amount < 16; ++amount) {
        constexpr std::uint64_t kValue = 0xB5C3;
        CHECK_EQ(boxResult("shifter", 16, 4, {kValue, amount, 0}), (kValue << amount) & 0xFFFF);
        CHECK_EQ(boxResult("shifter", 16, 4, {kValue, amount, 1}), kValue >> amount);
    }
    CHECK_EQ(boxResult("shifter", 8, 5, {0xFF, 31, 0}), 0);
    CHECK_EQ(boxResult("shifter", 8, 5, {0xFF, 31, 1}), 0);
    CHECK_EQ(boxResult("shifter", 8, 5, {0x0F, 8, 0}), 0);
    CHECK_EQ(boxResult("shifter", 8, 5, {0x0F, 9, 0}), 0);
}

// --- the panel -------------------------------------------------------------------------------------

TEST(every_word_remembers_the_line_it_came_from) {
    // 1000 is 7 times 128 plus 104, and 104 takes four ADDI of at most 31: LUI and four words more.
    const Result result = assemble("; comentário\n\nLI R1, 1\nLIW R2, 1000\nfim: HLT\n");
    CHECK(result.lines == std::vector<int>({3, 4, 4, 4, 4, 4, 5}));
}

TEST(the_panel_reads_the_running_computer_as_the_reference_says) {
    const std::vector<std::uint16_t> program = assembled(R"(
                LI R1, 10
                LI R2, 0
        laço:   ADD R2, R2, R1
                OUT R2
                ST R2, 3(R0)
                ADDI R1, R1, -1
                BNE R1, R0, laço
                HLT
    )");
    ComputerProject computer = buildComputer16(program, "");
    ChipBoards chips;
    for (const ChipData& chip : computer.chips) chips[chip.id] = &chip.board;
    BoardCircuit circuit(computer.board, &chips);
    Emulator emulator(program);
    for (int cycle = 0; cycle < 200 && !emulator.halted(); ++cycle) {
        const std::optional<ComputerState> state = readComputer(computer.board, circuit, chips);
        CHECK(state.has_value());
        if (!state) return;
        CHECK_EQ(state->pc, emulator.pc());
        CHECK_EQ(state->output, emulator.output());
        for (unsigned r = 0; r < 8; ++r) CHECK_EQ(state->registers[r], emulator.reg(r));
        CHECK_EQ(state->instruction, program[emulator.pc()]);
        emulator.step();
        circuit.setClocks(true);
        circuit.settle();
        circuit.setClocks(false);
        circuit.settle();
    }
    const std::optional<ComputerState> state = readComputer(computer.board, circuit, chips);
    CHECK(emulator.halted());
    CHECK(state->halted);
    CHECK_EQ(state->memory[3], 55);
    CHECK(state->ram == computer.ram);
}

// The X-ray writes a value on every wire. Each instruction's trace must say what the circuit then does:
// the register it writes and with what, the memory it stores, the output and where the PC goes next.
TEST(the_xray_trace_agrees_with_the_circuit_on_every_instruction) {
    const std::vector<std::uint16_t> program = assembled(R"(
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
                ADDI R0, R1, 7
                IN   R5
                OUT  R5
                BEQ  R4, R3, um
                LI   R1, 1
        um:     BNE  R4, R3, um
                BNE  R1, R3, dois
                LI   R1, 2
        dois:   CALL sub
                JMP  fim
        sub:    JR   R7
        fim:    HLT
    )");
    ComputerProject computer = buildComputer16(program, "");
    ChipBoards chips;
    for (const ChipData& chip : computer.chips) chips[chip.id] = &chip.board;
    BoardCircuit circuit(computer.board, &chips);
    circuit.setToggle(computer.switches, 0x1234);
    circuit.settle();
    const std::array<PartId, 1> ram{computer.ram};

    int instructions = 0;
    std::uint32_t seen = 0;  // which opcodes ran
    for (; instructions < 60; ++instructions) {
        const std::optional<ComputerState> before = readComputer(computer.board, circuit, chips);
        CHECK(before.has_value());
        if (!before || before->halted) break;
        const InstructionTrace trace = traceInstruction(*before);
        seen |= 1u << trace.op;
        circuit.setClocks(true);
        circuit.settle();
        circuit.setClocks(false);
        circuit.settle();
        const std::optional<ComputerState> after = readComputer(computer.board, circuit, chips);
        if (!after) break;

        CHECK_EQ(after->pc, trace.nextPc);
        // Stopping shows as soon as the PC reaches HLT, so it's a fact about the next instruction.
        CHECK_EQ(after->halted, traceInstruction(*after).halts);
        for (unsigned r = 0; r < 8; ++r) {
            const bool written = trace.writeBack != InstructionTrace::WriteBack::None && r == trace.writtenRegister;
            if (after->registers[r] != (written ? trace.writtenValue : before->registers[r])) {
                std::printf("  %s: R%u is %u, the trace said %u\n", trace.text.c_str(), r, after->registers[r],
                            written ? trace.writtenValue : before->registers[r]);
            }
            CHECK_EQ(after->registers[r], written ? trace.writtenValue : before->registers[r]);
        }
        CHECK_EQ(after->output, trace.shows ? trace.shown : before->output);
        if (trace.writesMemory) CHECK_EQ(circuit.memoryWord(ram, trace.address), trace.memoryValue);
    }
    CHECK(instructions > 20);
    // The CPU says it stopped as soon as the PC reaches HLT, and the X-ray says so too: it ends there.
    const std::optional<ComputerState> end = readComputer(computer.board, circuit, chips);
    CHECK(end && end->halted);
    CHECK(end && traceInstruction(*end).halts);
    // Every other kind of instruction ran at least once.
    for (const unsigned op : {kOpAlu, kOpAddi, kOpLi, kOpLui, kOpLd, kOpSt, kOpBeq, kOpBne, kOpJmp, kOpCall, kOpJr, kOpOut, kOpIn}) {
        if ((seen >> op & 1u) == 0) std::printf("  opcode %u never ran\n", op);
        CHECK((seen >> op & 1u) != 0);
    }
}

TEST(no_cpu_means_no_panel) {
    const Board board;
    const ChipBoards chips;
    BoardCircuit circuit(board, &chips);
    CHECK(!readComputer(board, circuit, chips).has_value());
}

TEST(each_instruction_is_explained_with_its_values) {
    ComputerState state;
    state.registers = {0, 10, 5, 0, 0, 0, 0, 42};
    state.pc = 4;
    const auto said = [&](const char* source) {
        state.instruction = assembled(source)[0];
        return explainInstruction(state);
    };
    CHECK(said("ADD R3, R1, R2") == "Soma R1 (10) e R2 (5) e guarda em R3.");
    CHECK(said("ADDI R1, R1, -1") == "Soma -1 a R1 (10) e guarda em R1.");
    CHECK(said("LI R4, 7") == "Coloca 7 em R4.");
    CHECK(said("OUT R2") == "Mostra R2 (5) na saída.");
    CHECK(said("BNE R1, R0, 0").find("Vai pular") != std::string::npos);
    CHECK(said("BEQ R1, R0, 0").find("Não vai pular") != std::string::npos);
    CHECK(said("RET") == "Volta para 42, o endereço guardado em R7.");
    CHECK(said("HLT") == "Para o computador.");
    state.halted = true;
    CHECK(said("NOP").find("parou") != std::string::npos);
}

TEST(the_diagram_lights_what_each_instruction_uses) {
    const auto uses = [](const char* source, unsigned path) { return (datapathUse(assembled(source)[0]).paths >> path & 1u) != 0; };
    CHECK(uses("ADD R1, R2, R3", kPathWriteBack));
    CHECK(!uses("ADD R1, R2, R3", kPathImmediate));
    CHECK(uses("LD R1, 2(R2)", kPathLoad));
    CHECK(uses("ST R1, 2(R2)", kPathStore));
    CHECK(!uses("ST R1, 2(R2)", kPathWriteBack));
    CHECK(uses("CALL 9", kPathLink));
    CHECK(uses("OUT R1", kPathOutput));
    CHECK(uses("HLT", kPathPcRom));
    CHECK_EQ(registerWritten(assembled("CALL 9")[0]), 7);
    CHECK_EQ(registerWritten(assembled("ST R1, 0(R2)")[0]), -1);
    CHECK_EQ(registerWritten(assembled("LI R0, 3")[0]), -1);
}

// --- labels ----------------------------------------------------------------------------------------

namespace {

struct LabelRig {
    Board board;
    PartId input = 0;
    PartId output = 0;
};

LabelRig labelledWire(const char* sent, const char* received, std::uint8_t sentBits = 1, std::uint8_t receivedBits = 1) {
    LabelRig rig;
    const auto pinDrop = [&](PartId id) { return Drop{Drop::Kind::Pin, pinPosition(*rig.board.part(id), 0), id, 0}; };
    const auto link = [&](PartId a, PartId b) {
        const Drop from = pinDrop(a);
        const Drop to = pinDrop(b);
        rig.board.connect(from, to, {from.at, {from.at.x, to.at.y}, to.at});
    };
    Part toggle = makePart(PartKind::Toggle, {0, 0});
    toggle.width = sentBits;
    rig.input = *rig.board.addPart(toggle);
    Part first = makePart(PartKind::Tunnel, {24, 0}, 0);
    first.label = sent;
    first.width = sentBits;
    Part second = makePart(PartKind::Tunnel, {60, 20}, 2);
    second.label = received;
    second.width = receivedBits;
    Part probe = makePart(PartKind::Probe, {90, 20});
    probe.width = receivedBits;
    const PartId a = *rig.board.addPart(first);
    const PartId b = *rig.board.addPart(second);
    rig.output = *rig.board.addPart(probe);
    (void)a;
    (void)b;
    const Part& firstPlaced = *rig.board.part(a);
    const Drop start = pinDrop(rig.input);
    const Drop end{Drop::Kind::Pin, pinPosition(firstPlaced, 0), a, 0};
    rig.board.connect(start, end, {start.at, {start.at.x + 2, start.at.y}, {start.at.x + 2, end.at.y}, end.at});
    link(b, rig.output);
    return rig;
}

}  // namespace

TEST(two_labels_with_the_same_name_are_one_wire) {
    LabelRig rig = labelledWire("Dados", "Dados");
    BoardCircuit circuit(rig.board);
    circuit.setToggle(rig.input, 1);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(rig.output), 1);
    circuit.setToggle(rig.input, 0);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(rig.output), 0);
}

TEST(a_different_name_is_a_different_wire) {
    LabelRig rig = labelledWire("Dados", "dados");
    BoardCircuit circuit(rig.board);
    circuit.setToggle(rig.input, 1);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(rig.output), 0);
}

TEST(labels_carry_whole_buses) {
    LabelRig rig = labelledWire("Bus", "Bus", 8, 8);
    BoardCircuit circuit(rig.board);
    circuit.setToggle(rig.input, 0xA5);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(rig.output), 0xA5);
    CHECK(!circuit.hasWidthConflict());
}

TEST(labels_of_different_widths_are_reported_like_any_wire) {
    LabelRig rig = labelledWire("Bus", "Bus", 8, 4);
    BoardCircuit circuit(rig.board);
    circuit.setToggle(rig.input, 0xA5);
    circuit.settle();
    CHECK(circuit.hasWidthConflict());
}

// --- reading a ROM back ----------------------------------------------------------------------------

TEST(every_possible_word_disassembles_to_something_that_assembles_back) {
    for (unsigned word = 0; word < 65536; ++word) {
        const std::string text = disassemble(static_cast<std::uint16_t>(word), 0);
        const Result result = assemble(text);
        if (!result.ok() || result.words.size() != 1 || result.words[0] != word) {
            std::printf("  word %04X became \"%s\"\n", word, text.c_str());
            CHECK(false);
            return;
        }
    }
}

TEST(a_whole_program_disassembles_and_assembles_back_with_its_branches) {
    const std::vector<std::uint16_t> words = assembled(exampleProgram());
    std::string text;
    for (std::size_t i = 0; i < words.size(); ++i) text += disassemble(words[i], i) + "\n";
    CHECK(assembled(text.c_str()) == words);
    CHECK(text.find("BNE R1, R0, 2") != std::string::npos);
}

// # starts a comment too; MIPS instructions and registers are named, and pointed to what this computer has.
TEST(hash_comments_work_and_mips_gets_pointed_to_ours) {
    const assembly::Result commented = assembly::assemble("        LI R1, 5   # five\n# a whole line\n        HLT\n");
    CHECK(commented.ok());
    CHECK_EQ(commented.words.size(), 2);
    const assembly::Result mips = assembly::assemble("        lw $t0, 0($t1)\n        syscall\n        bge R1, R2, x\n");
    CHECK(!mips.ok());
    bool load = false, call = false, branch = false;
    for (const assembly::Error& error : mips.errors) {
        load = load || error.message.find("LD") != std::string::npos;
        call = call || error.message.find("OUT") != std::string::npos;
        branch = branch || error.message.find("BEQ e BNE") != std::string::npos;
    }
    CHECK(load && call && branch);
    const assembly::Result dollar = assembly::assemble("        ADD $t0, R1, R2\n");
    CHECK(!dollar.ok() && dollar.errors.front().message.find("MIPS") != std::string::npos);
}

TEST(accented_labels_work_because_people_will_write_them) {
    CHECK_EQ(assembled("la\xC3\xA7o: NOP\nJMP la\xC3\xA7o\n")[1], 0x8000);
}

// --- editing a program -----------------------------------------------------------------------------

TEST(typing_and_erasing_across_lines) {
    TextEdit text;
    text.insert("ab");
    text.newline();
    text.insert("cd");
    CHECK(text.text() == "ab\ncd");
    CHECK_EQ(text.lineCount(), 2);
    text.moveLeft();
    text.moveLeft();
    text.backspace();
    CHECK(text.text() == "abcd");
    CHECK_EQ(text.cursor().column, 2);
    text.moveToLineEnd();
    text.deleteForward();
    CHECK(text.text() == "abcd");
    text.backspace();
    CHECK(text.text() == "abc");
    text.moveToStart();
    text.backspace();
    CHECK(text.text() == "abc");
}

TEST(a_new_line_keeps_the_indentation) {
    TextEdit text;
    text.insert("        LI R1, 1");
    text.newline();
    text.insert("OUT R1");
    CHECK(text.line(1) == "        OUT R1");
}

TEST(a_selection_is_replaced_by_what_comes_next) {
    TextEdit text("um\ndois\ntres");
    text.moveRight();
    text.moveVertically(2, true);
    CHECK(text.selectedText() == "m\ndois\nt");
    text.insert("X");
    CHECK(text.text() == "uXres");
    CHECK(!text.hasSelection());

    text.selectAll();
    CHECK(text.selectedText() == "uXres");
    text.deleteSelection();
    CHECK(text.text().empty());
}

TEST(arrows_collapse_a_selection_to_its_near_edge) {
    TextEdit text("abcdef");
    text.moveRight();
    text.moveRight(true);
    text.moveRight(true);
    text.moveLeft();
    CHECK_EQ(text.cursor().column, 1);
    text.moveRight(true);
    text.moveRight(true);
    text.moveRight();
    CHECK_EQ(text.cursor().column, 3);
}

TEST(going_through_a_short_line_keeps_the_column) {
    TextEdit text("abcdef\nx\nabcdef");
    text.moveToLineEnd();
    text.moveVertically(1);
    CHECK_EQ(text.cursor().column, 1);
    text.moveVertically(1);
    CHECK_EQ(text.cursor().column, 6);
    text.moveVertically(-99);
    CHECK_EQ(text.cursor().line, 0);
    text.moveVertically(99);
    CHECK_EQ(text.cursor().line, 2);
}

TEST(home_goes_to_the_first_word_then_to_the_very_start) {
    TextEdit text("    LI R1, 1");
    text.moveToLineEnd();
    text.moveToLineStart();
    CHECK_EQ(text.cursor().column, 4);
    text.moveToLineStart();
    CHECK_EQ(text.cursor().column, 0);
}

TEST(an_accent_is_one_character) {
    TextEdit text;
    text.insert("la\xC3\xA7o");
    CHECK_EQ(text.displayColumn(text.cursor()), 4);
    text.moveLeft();
    text.moveLeft();
    CHECK_EQ(text.cursor().column, 2);
    CHECK_EQ(text.displayColumn(text.cursor()), 2);
    text.moveRight();
    CHECK_EQ(text.cursor().column, 4);
    text.backspace();
    CHECK(text.text() == "lao");
    text.place(0, 2);
    text.insert("\xC3\xA7");
    text.place(0, 3);
    CHECK_EQ(text.cursor().column, 4);
}

TEST(a_click_past_the_end_lands_on_the_end) {
    TextEdit text("abc\nde");
    text.place(50, 50);
    CHECK_EQ(text.cursor().line, 1);
    CHECK_EQ(text.cursor().column, 2);
    text.place(-3, -3);
    CHECK_EQ(text.cursor().line, 0);
    CHECK_EQ(text.cursor().column, 0);
}

TEST(undo_takes_back_a_whole_word_and_redo_brings_it_back) {
    TextEdit text;
    for (const char* letter : {"a", "b", "c"}) text.insert(letter);
    text.newline();
    for (const char* letter : {"d", "e"}) text.insert(letter);
    text.undo();
    CHECK(text.text() == "abc\n");
    text.undo();
    CHECK(text.text() == "abc");
    text.undo();
    CHECK(text.text().empty());
    CHECK(!text.undo());
    text.redo();
    text.redo();
    text.redo();
    CHECK(text.text() == "abc\nde");
    CHECK(!text.redo());
    text.undo();
    text.insert("z");
    CHECK(!text.redo());
}

TEST(moving_the_cursor_ends_a_word_for_undo) {
    TextEdit text;
    text.insert("a");
    text.moveLeft();
    text.moveRight();
    text.insert("b");
    text.undo();
    CHECK(text.text() == "a");
}

TEST(pasted_text_is_made_safe) {
    TextEdit text;
    text.insert("a\r\nb\tc\x01\x7F");
    CHECK(text.text() == "a\nb    c");

    // Everything here is something JSON would refuse, which would make the project impossible to save.
    TextEdit broken;
    broken.insert("x\xC3");
    CHECK(broken.text() == "x?");
    for (const char* bad : {"\xFF", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82"}) {
        TextEdit one;
        one.insert(bad);
        CHECK(one.text().find('?') != std::string::npos);
        for (const char c : one.text()) CHECK((static_cast<unsigned char>(c) & 0x80) == 0);
    }
    TextEdit good;
    good.insert("\xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(good.text() == "\xE2\x82\xAC \xF0\x9F\x98\x80");
}

TEST(a_text_too_big_is_refused_whole) {
    TextEdit text("abc");
    CHECK(!text.insert(std::string(TextEdit::kMaxBytes, 'x')));
    CHECK(text.text() == "abc");
    text.selectAll();
    CHECK(text.insert(std::string(TextEdit::kMaxBytes, 'y')));
}

TEST(a_big_paste_takes_no_time_at_all) {
    std::string many;
    for (int i = 0; i < 60000; ++i) many += "ADD R1, R2, R3\n";
    TextEdit text;
    text.insert(many);
    CHECK_EQ(text.lineCount(), 60001);
    CHECK(text.undo());
    CHECK(text.text().empty());
}

// --- through the session, as the game does it ------------------------------------------------------

TEST(a_program_assembled_into_the_rom_runs_and_can_be_undone) {
    ComputerProject computer = buildComputer16(assembled(exampleProgram()), exampleProgram());
    Session session;
    session.adoptProjectChips(computer.chips);
    session.adopt(computer.board);

    const auto run = [&] {
        for (int i = 0; i < 400 && !session.circuit().outputOn(computer.haltLed); ++i) session.stepClock();
        return session.circuit().outputValue(computer.outputProbe);
    };
    CHECK_EQ(run(), 55);

    const Rect bounds = boundsOf(*session.board().part(computer.rom));
    session.mouseMove(static_cast<float>(bounds.left + 2), static_cast<float>(bounds.top + 2));
    session.leftDown();
    session.leftUp();
    CHECK(session.inspected().has_value());

    const char* program = "LI R1, 77\nOUT R1\nHLT\n";
    const std::vector<std::uint16_t> words = assembled(program);
    session.setRomWords({words.begin(), words.end()}, program);
    CHECK(session.board().part(computer.rom)->source == program);
    CHECK_EQ(run(), 77);

    session.key(Key::Undo);
    CHECK(session.board().part(computer.rom)->source == exampleProgram());
    CHECK_EQ(run(), 55);
}

TEST(typing_a_rom_by_hand_drops_the_source_that_no_longer_matches_it) {
    ComputerProject computer = buildComputer16(assembled(exampleProgram()), exampleProgram());
    Session session;
    session.adoptProjectChips(computer.chips);
    session.adopt(computer.board);
    const Rect bounds = boundsOf(*session.board().part(computer.rom));
    session.mouseMove(static_cast<float>(bounds.left + 2), static_cast<float>(bounds.top + 2));
    session.leftDown();
    session.leftUp();
    session.setRomWords({0xF000});
    CHECK(session.board().part(computer.rom)->source.empty());
}

TEST(the_cpu_chip_can_go_on_any_board) {
    Session session;
    const ChipId first = session.nextChipId();
    session.addChips(buildCpuChips(first));
    CHECK_EQ(session.circuits().size(), 1 + kCpuChipCount);
    CHECK(session.dirty());
    session.beginPlacingChip(first + kCpuChipCount - 1);
    CHECK(session.mode() == Mode::Placing);
    session.mouseMove(10, 10);
    session.leftDown();
    session.leftUp();
    session.key(Key::Escape);
    CHECK_EQ(session.board().parts().size(), 1);
    const Part& cpu = session.board().parts()[0];
    CHECK_EQ(pinBits(cpu, 2), 16);
    CHECK(!session.circuit().hasWidthConflict());
}

TEST(the_truth_table_gives_up_on_the_computer_without_any_fuss) {
    ComputerProject computer = buildComputer16(assembled(exampleProgram()), exampleProgram());
    Session session;
    session.adoptProjectChips(computer.chips);
    session.adopt(computer.board);
    CHECK(session.truthTable().rows.empty());
}

int main() { return check::runAll(); }
