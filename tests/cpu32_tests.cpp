// The 32-bit computer's circuit against its instruction set: every program runs on the circuit and on the
// model in asm/mips.cpp, which share no code, and after every clock cycle the PC, all 32 registers and what
// was printed have to agree.
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "asm/mips.hpp"
#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/computer32.hpp"
#include "editor/computer32_view.hpp"
#include "editor/session.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

class Circuit32 {
public:
    explicit Circuit32(const mips::Program& program, std::uint32_t switches = 0) : project_(buildComputer32(program, "")) {
        for (const ChipData& chip : project_.chips) chips_[chip.id] = &chip.board;
        circuit_ = std::make_unique<BoardCircuit>(project_.board, &chips_);
        circuit_->setToggle(project_.switches, switches);
        circuit_->settle();
    }

    void cycle() {
        const bool number = circuit_->outputOn(project_.printNumber);
        const bool character = circuit_->outputOn(project_.printChar);
        circuit_->setClocks(true);
        circuit_->settle();
        circuit_->setClocks(false);
        circuit_->settle();
        const auto printed = static_cast<std::uint32_t>(circuit_->outputValue(project_.outputProbe));
        if (number) output += std::to_string(static_cast<std::int32_t>(printed));
        if (character) output += static_cast<char>(printed & 0xFFu);
    }

    std::uint32_t pc() const { return static_cast<std::uint32_t>(circuit_->outputValue(project_.pcProbe)); }
    bool halted() const { return circuit_->outputOn(project_.haltLed); }
    bool invalid() const { return circuit_->outputOn(project_.invalidLed); }
    std::uint32_t reg(unsigned index) const { return static_cast<std::uint32_t>(circuit_->memoryWord(project_.registersPath, index)); }
    bool healthy() const { return !circuit_->hasWidthConflict() && !circuit_->hasConflict() && !circuit_->oscillating(); }

    std::string output;

private:
    Computer32Project project_;
    ChipBoards chips_;
    std::unique_ptr<BoardCircuit> circuit_;
};

mips::Program assembled(const std::string& source) {
    const mips::Program program = mips::assemble(source);
    for (const assembly::Error& error : program.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    CHECK(program.ok());
    return program;
}

// Runs both until the model stops, comparing after every instruction.
bool runBoth(const mips::Program& program, std::size_t maxCycles, std::uint32_t switches = 0, std::string* printed = nullptr) {
    Circuit32 circuit(program, switches);
    mips::Machine model = mips::boot(program);
    model.switches = switches;
    if (!circuit.healthy()) {
        std::printf("  the circuit has a conflict\n");
        return false;
    }
    for (std::size_t cycle = 0; cycle < maxCycles; ++cycle) {
        if (circuit.pc() != model.pc) {
            std::printf("  cycle %zu: pc 0x%08X, model 0x%08X (%s)\n", cycle, circuit.pc(), model.pc,
                        mips::disassemble(model.fetch(), model.pc).c_str());
            return false;
        }
        for (unsigned r = 1; r < 32; ++r) {
            if (circuit.reg(r) != model.registers[r]) {
                std::printf("  cycle %zu, pc 0x%08X: %s = 0x%08X, model 0x%08X\n", cycle, model.pc, std::string(mips::registerName(r)).c_str(),
                            circuit.reg(r), model.registers[r]);
                return false;
            }
        }
        if (circuit.output != model.output) {
            std::printf("  cycle %zu: printed \"%s\", model \"%s\"\n", cycle, circuit.output.c_str(), model.output.c_str());
            return false;
        }
        if (model.halted) {
            if (!circuit.halted()) std::printf("  the model stopped and the circuit didn't\n");
            if (circuit.invalid() != !model.fault.empty()) std::printf("  the invalid light doesn't match\n");
            if (printed != nullptr) *printed = circuit.output;
            return circuit.halted() && circuit.invalid() == !model.fault.empty();
        }
        circuit.cycle();
        model.step();
    }
    std::printf("  still running after %zu cycles\n", maxCycles);
    return false;
}

}  // namespace

TEST(the_computer_builds_without_conflicts) {
    Circuit32 circuit(assembled("  nop\n"));
    CHECK(circuit.healthy());
    CHECK_EQ(circuit.pc(), mips::kTextBase);
}

TEST(each_alu_instruction_agrees_with_the_model) {
    CHECK(runBoth(assembled(
                      "  li $t0, 0x12345678\n  li $t1, -77\n  li $t2, 5\n"
                      "  add $s0, $t0, $t1\n  addu $s1, $t0, $t2\n  sub $s2, $t1, $t0\n  subu $s3, $t2, $t1\n"
                      "  and $s4, $t0, $t1\n  or $s5, $t0, $t1\n  xor $s6, $t0, $t1\n  nor $s7, $t0, $t1\n"
                      "  slt $t3, $t1, $t2\n  slt $t4, $t2, $t1\n  sltu $t5, $t1, $t2\n  sltu $t6, $t2, $t1\n"
                      "  sll $t7, $t0, 7\n  srl $t8, $t1, 3\n  sra $t9, $t1, 3\n  sra $v1, $t0, 31\n"
                      "  addi $a1, $t1, -100\n  addiu $a2, $t0, 32767\n  slti $a3, $t1, -76\n  sltiu $k0, $t2, -1\n"
                      "  andi $k1, $t1, 0xF0F0\n  ori $fp, $t1, 0x0F0F\n  xori $ra, $t1, 0xFFFF\n  lui $s0, 0xBEEF\n"
                      "  li $v0, 10\n  syscall\n"),
                  100));
}

TEST(memory_branches_jumps_and_calls_agree) {
    CHECK(runBoth(assembled(".data\nv: .word 5, -3, 100\n.text\n"
                            "  la $t0, v\n  lw $t1, 0($t0)\n  lw $t2, 4($t0)\n  lw $t3, v\n"
                            "  sw $t2, 8($t0)\n  lw $t4, 8($t0)\n  sw $t1, -4($sp)\n  lw $t5, -4($sp)\n"
                            "  beq $t1, $t3, same\n  li $s0, 1\nsame: bne $t1, $t3, off\n  li $s1, 2\n"
                            "off: blt $t2, $zero, neg\n  li $s3, 4\nneg: jal f\n  j end\n  li $s2, 3\nf: addi $v1, $ra, 0\n  jr $ra\n"
                            "end: li $v0, 10\n  syscall\n"),
                  100));
}

TEST(class_programs_print_what_they_should) {
    std::string printed;
    CHECK(runBoth(assembled(".data\n"
                            "vetor: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10\n"
                            "n:     .word 10\n"
                            ".text\n"
                            "main:  la   $t0, vetor\n  lw $t1, n\n  li $t2, 0\n  li $t3, 0\n"
                            "laco:  bge  $t3, $t1, fim\n  lw $t4, 0($t0)\n  add $t2, $t2, $t4\n  addi $t0, $t0, 4\n  addi $t3, $t3, 1\n  j laco\n"
                            "fim:   move $a0, $t2\n  li $v0, 1\n  syscall\n  li $a0, '!'\n  li $v0, 11\n  syscall\n  li $v0, 10\n  syscall\n"),
                  1000, 0, &printed));
    CHECK(printed == "55!");
    // fib(10) through the stack, which starts where MARS puts it.
    CHECK(runBoth(assembled("main: li $a0, 10\n  jal fib\n  move $a0, $v0\n  li $v0, 1\n  syscall\n  li $v0, 10\n  syscall\n"
                            "fib: slti $t0, $a0, 2\n  beq $t0, $zero, more\n  move $v0, $a0\n  jr $ra\n"
                            "more: addi $sp, $sp, -12\n  sw $ra, 8($sp)\n  sw $a0, 4($sp)\n"
                            "  addi $a0, $a0, -1\n  jal fib\n  sw $v0, 0($sp)\n"
                            "  lw $a0, 4($sp)\n  addi $a0, $a0, -2\n  jal fib\n"
                            "  lw $t1, 0($sp)\n  add $v0, $v0, $t1\n  lw $ra, 8($sp)\n  addi $sp, $sp, 12\n  jr $ra\n"),
                  5000, 0, &printed));
    CHECK(printed == "55");
}

TEST(syscalls_read_the_switches_and_stop_on_what_they_lack) {
    std::string printed;
    CHECK(runBoth(assembled("  li $v0, 5\n  syscall\n  move $a0, $v0\n  li $v0, 1\n  syscall\n  li $v0, 10\n  syscall\n"), 50, 0xFFFFFF85u, &printed));
    CHECK(printed == "-123");
    CHECK(runBoth(assembled("  li $v0, 4\n  syscall\n"), 50));
    CHECK(runBoth(assembled("  .word 0xFC000000\n"), 50));
    CHECK(runBoth(assembled("  .word 0x0000003F\n"), 50));
}

// Random instructions, with random registers, against the model, to reach what the programs above don't.
TEST(random_programs_agree) {
    std::mt19937 random(2026);
    static constexpr const char* kThree[] = {"add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"};
    static constexpr const char* kImmediate[] = {"addi", "addiu", "slti", "sltiu"};
    static constexpr const char* kLogicImmediate[] = {"andi", "ori", "xori"};
    static constexpr const char* kShifts[] = {"sll", "srl", "sra"};
    for (int round = 0; round < 6; ++round) {
        std::string source;
        const auto reg = [&] { return std::string(mips::registerName(8 + static_cast<unsigned>(random() % 18))); };
        for (int i = 0; i < 8; ++i) source += "  li " + reg() + ", " + std::to_string(static_cast<std::int32_t>(random())) + "\n";
        for (int i = 0; i < 60; ++i) {
            switch (random() % 5) {
                case 0: source += std::string("  ") + kThree[random() % 10] + " " + reg() + ", " + reg() + ", " + reg() + "\n"; break;
                case 1: source += std::string("  ") + kImmediate[random() % 4] + " " + reg() + ", " + reg() + ", " + std::to_string(static_cast<int>(random() % 65536) - 32768) + "\n"; break;
                case 2: source += std::string("  ") + kLogicImmediate[random() % 3] + " " + reg() + ", " + reg() + ", " + std::to_string(random() % 65536) + "\n"; break;
                case 3: source += std::string("  ") + kShifts[random() % 3] + " " + reg() + ", " + reg() + ", " + std::to_string(random() % 32) + "\n"; break;
                default: source += "  lui " + reg() + ", " + std::to_string(random() % 65536) + "\n"; break;
            }
        }
        source += "  li $v0, 10\n  syscall\n";
        CHECK(runBoth(assembled(source), 200));
    }
}

// What the X-ray writes on its wires, against what the model does: the next PC, the register written and
// its value, the word stored, and the stop.
bool traceAgrees(const mips::Program& program, std::size_t steps) {
    mips::Machine model = mips::boot(program);
    model.switches = 0x2A;
    for (std::size_t i = 0; i < steps && !model.halted; ++i) {
        const std::uint32_t word = model.fetch();
        const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word & 0xFFFFu)));
        const std::uint32_t memory = model.memory(model.registers[(word >> 21) & 31u] + offset);
        const Trace32 trace = traceInstruction32(word, model.pc, model.registers, model.switches, memory);
        const std::array<std::uint32_t, 32> before = model.registers;
        model.step();
        bool ok = model.pc == trace.nextPc && model.halted == trace.halts && (!model.fault.empty()) == trace.invalid;
        for (unsigned r = 1; r < 32; ++r) {
            const bool written = trace.back != Trace32::Back::None && trace.written == r;
            ok = ok && (written ? model.registers[r] == trace.writtenValue : model.registers[r] == before[r]);
        }
        if (trace.writesMemory) ok = ok && model.memory(trace.address) == trace.memoryValue;
        if (!ok) {
            std::printf("  step %zu: %s, model pc 0x%08X, trace says 0x%08X\n", i, trace.text.c_str(), model.pc, trace.nextPc);
            return false;
        }
        for (int stage = 0; stage < 6; ++stage) {
            if (explainStage32(trace, stage).empty()) return false;
        }
    }
    return true;
}

TEST(the_xray_trace_agrees_with_the_model) {
    CHECK(traceAgrees(assembled(".data\nv: .word 5, -3\n.text\n"
                                "  la $t0, v\n  lw $t1, 0($t0)\n  lw $t2, 4($t0)\n  sw $t1, 4($t0)\n  add $t3, $t1, $t2\n  sub $t4, $t1, $t2\n"
                                "  and $t5, $t1, $t2\n  or $t6, $t1, $t2\n  xor $t7, $t1, $t2\n  nor $s0, $t1, $t2\n  slt $s1, $t2, $t1\n  sltu $s2, $t2, $t1\n"
                                "  sll $s3, $t2, 3\n  srl $s4, $t2, 3\n  sra $s5, $t2, 3\n  addi $s6, $t2, -9\n  slti $s7, $t2, 0\n  sltiu $t8, $t2, 5\n"
                                "  andi $t9, $t2, 0xF0\n  ori $v1, $t2, 1\n  xori $a1, $t2, 0xFFFF\n  lui $a2, 0x1234\n"
                                "  beq $t1, $t1, a\n  nop\na: bne $t1, $t1, a\n  jal f\n  li $v0, 5\n  syscall\n  li $a0, 7\n  li $v0, 1\n  syscall\n"
                                "  li $v0, 11\n  syscall\n  j end\nf: jr $ra\nend: li $v0, 10\n  syscall\n"),
                      200));
    CHECK(traceAgrees(assembled("  li $v0, 4\n  syscall\n"), 10));
    CHECK(traceAgrees(assembled("  .word 0xFC000000\n"), 10));
    CHECK(traceAgrees(assembled("  .word 0x0000003F\n"), 10));
}

TEST(the_control_rows_cover_every_instruction) {
    const std::vector<std::uint64_t> main = mainControlRows();
    const std::vector<std::uint64_t> byFunct = functControlRows();
    CHECK_EQ(main.size(), 64);
    CHECK_EQ(byFunct.size(), 64);
    CHECK((main[mips::kOpSpecial] >> kC32UseFunct & 1u) == 1u);
    CHECK((main[1] >> kC32Invalid & 1u) == 1u);
    CHECK((byFunct[mips::kFnJr] >> kC32RegWrite & 1u) == 0u);
}

int main() { return check::runAll(); }
