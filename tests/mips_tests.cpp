// The 32-bit MIPS computer's assembler and reference model. The encodings are checked against what MARS gives
// for the same lines, and whole programs from class against the output they must print.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "asm/mips.hpp"
#include "check.hpp"

using namespace bancada;

namespace {

std::vector<std::uint32_t> words(const char* source) {
    const mips::Program program = mips::assemble(source);
    for (const assembly::Error& error : program.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    CHECK(program.ok());
    return program.text;
}

std::string run(const char* source, std::uint32_t switches = 0) {
    const mips::Program program = mips::assemble(source);
    for (const assembly::Error& error : program.errors) std::printf("  line %d: %s\n", error.line, error.message.c_str());
    CHECK(program.ok());
    mips::Machine machine = mips::boot(program);
    machine.switches = switches;
    machine.run(100000);
    CHECK(machine.halted);
    CHECK(machine.fault.empty());
    return machine.output;
}

void same(const std::string& got, const std::string& wanted) {
    if (got != wanted) std::printf("  printed \"%s\", wanted \"%s\"\n", got.c_str(), wanted.c_str());
    CHECK(got == wanted);
}

bool mentions(const mips::Program& program, const char* text) {
    for (const assembly::Error& error : program.errors) {
        if (error.message.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(real_instructions_encode_as_mars_writes_them) {
    const std::vector<std::uint32_t> got = words(
        "  add $t0, $t1, $t2\n"
        "  addi $t0, $zero, 5\n"
        "  lw $t0, 4($sp)\n"
        "  sw $ra, 0($sp)\n"
        "  sll $t0, $t1, 2\n"
        "  jr $ra\n"
        "  syscall\n"
        "  lui $at, 0x1001\n"
        "  slt $t2, $s0, $s1\n"
        "  sra $t0, $t0, 31\n"
        "  andi $t1, $t1, 0xFF\n");
    const std::vector<std::uint32_t> expected = {0x012A4020u, 0x20080005u, 0x8FA80004u, 0xAFBF0000u, 0x00094080u, 0x03E00008u,
                                                 0x0000000Cu, 0x3C011001u, 0x0211502Au, 0x000847C3u, 0x312900FFu};
    CHECK_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < got.size(); ++i) CHECK_EQ(got[i], expected[i]);
}

TEST(branches_and_jumps_count_from_the_next_instruction) {
    const std::vector<std::uint32_t> got = words("here: beq $t0, $t1, here\n  bne $t0, $zero, ahead\n  nop\nahead: j here\n  jal ahead\n");
    CHECK_EQ(got.size(), 5);
    CHECK_EQ(got[0], 0x1109FFFFu);
    CHECK_EQ(got[1], 0x15000001u);
    CHECK_EQ(got[3], 0x08100000u);
    CHECK_EQ(got[4], 0x0C100003u);
}

TEST(pseudo_instructions_become_what_mars_writes) {
    const mips::Program program = mips::assemble(
        ".data\n"
        "x: .word 7\n"
        "y: .word 1, 2, 3\n"
        ".text\n"
        "  li $t0, 5\n"          // addiu
        "  li $t0, 0xFFFF\n"     // ori
        "  li $t0, 0x12345678\n" // lui + ori
        "  la $t1, y\n"          // lui + ori
        "  move $a0, $t2\n"
        "  lw $t2, x\n"          // lui + lw
        "  blt $t0, $t1, end\n"  // slt + bne
        "end: nop\n");
    CHECK(program.ok());
    const std::vector<std::uint32_t> expected = {0x24080005u, 0x3408FFFFu, 0x3C011234u, 0x34285678u, 0x3C011001u, 0x34290004u,
                                                 0x000A2021u, 0x3C011001u, 0x8C2A0000u, 0x0109082Au, 0x14200000u, 0x00000000u};
    CHECK_EQ(program.text.size(), expected.size());
    for (std::size_t i = 0; i < expected.size() && i < program.text.size(); ++i) CHECK_EQ(program.text[i], expected[i]);
    CHECK_EQ(program.data.size(), 4);
    CHECK_EQ(program.data[0], 7u);
    CHECK_EQ(program.data[3], 3u);
}

TEST(mistakes_are_explained) {
    const mips::Program bad = mips::assemble(
        "  add $t0, $t1\n"
        "  addi $t0, $t1, 70000\n"
        "  lw $t0, 0($q1)\n"
        "  beq $t0, $t1, nowhere\n"
        "  LD R1, 0(R2)\n"
        "  mul $t0, $t1, $t2\n"
        "  lb $t0, 0($t1)\n"
        "  frobnicate\n");
    CHECK_EQ(bad.errors.size(), 8);
    CHECK(mentions(bad, "espera 3"));
    CHECK(mentions(bad, "fora da faixa"));
    CHECK(mentions(bad, "$q1"));
    CHECK(mentions(bad, "nowhere"));
    CHECK(mentions(bad, "use lw"));
    CHECK(mentions(bad, "multiplicador"));
    CHECK(mentions(bad, "palavras inteiras"));
    CHECK(mentions(bad, "frobnicate"));
    const mips::Program repeated = mips::assemble("a: nop\na: nop\n");
    CHECK(mentions(repeated, "repetido"));
}

// Every instruction shown as text assembles back to the same word, at the same place.
TEST(disassembly_assembles_back_to_the_same_words) {
    const mips::Program program = mips::assemble(
        "start: add $t0, $t1, $t2\n  addu $s0, $s1, $s2\n  sub $v0, $a0, $a1\n  subu $t3, $t4, $t5\n"
        "  and $t0, $t0, $t1\n  or $t0, $t0, $t1\n  xor $t0, $t0, $t1\n  nor $t0, $t0, $t1\n"
        "  slt $t0, $t1, $t2\n  sltu $t0, $t1, $t2\n  sll $t0, $t1, 4\n  srl $t0, $t1, 4\n  sra $t0, $t1, 4\n"
        "  addi $t0, $t1, -3\n  addiu $sp, $sp, -8\n  slti $t0, $t1, 9\n  sltiu $t0, $t1, 9\n"
        "  andi $t0, $t1, 0xF0\n  ori $t0, $t1, 0x0F\n  xori $t0, $t1, 0xFF\n  lui $t0, 0x8000\n"
        "  lw $t0, -4($sp)\n  sw $t0, 8($gp)\n  beq $t0, $t1, start\n  bne $t0, $t1, start\n"
        "  j start\n  jal start\n  jr $ra\n  syscall\n  nop\n  .word 0xFFFFFFFF\n");
    CHECK(program.ok());
    std::string again;
    for (std::size_t i = 0; i < program.text.size(); ++i) {
        again += "  " + mips::disassemble(program.text[i], mips::kTextBase + static_cast<std::uint32_t>(4 * i)) + "\n";
    }
    const mips::Program back = mips::assemble(again);
    CHECK(back.ok());
    CHECK(back.text == program.text);
}

TEST(the_machine_counts_and_prints) {
    same(run("  li $t0, 1\n  li $t1, 11\n  li $t2, 0\nloop: add $t2, $t2, $t0\n  addi $t0, $t0, 1\n  bne $t0, $t1, loop\n"
                 "  move $a0, $t2\n  li $v0, 1\n  syscall\n  li $a0, '!'\n  li $v0, 11\n  syscall\n  li $v0, 10\n  syscall\n"),
             std::string("55!"));
}

// The kind of program a class gives: add up a vector in memory.
TEST(a_vector_is_summed_from_memory) {
    same(run(".data\n"
                 "vetor: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10\n"
                 "n:     .word 10\n"
                 ".text\n"
                 "main:  la   $t0, vetor      # onde o vetor começa\n"
                 "       lw   $t1, n\n"
                 "       li   $t2, 0          # a soma\n"
                 "       li   $t3, 0          # i\n"
                 "laco:  bge  $t3, $t1, fim\n"
                 "       lw   $t4, 0($t0)\n"
                 "       add  $t2, $t2, $t4\n"
                 "       addi $t0, $t0, 4\n"
                 "       addi $t3, $t3, 1\n"
                 "       j    laco\n"
                 "fim:   move $a0, $t2\n"
                 "       li   $v0, 1\n"
                 "       syscall\n"
                 "       li   $v0, 10\n"
                 "       syscall\n"),
             std::string("55"));
}

// Calls that call themselves keep their state on the stack: fib(10) through jal, jr and the stack pointer.
TEST(recursion_uses_the_stack) {
    same(run("main: li $a0, 10\n  jal fib\n  move $a0, $v0\n  li $v0, 1\n  syscall\n  li $v0, 10\n  syscall\n"
                 "fib: slti $t0, $a0, 2\n  beq $t0, $zero, more\n  move $v0, $a0\n  jr $ra\n"
                 "more: addi $sp, $sp, -12\n  sw $ra, 8($sp)\n  sw $a0, 4($sp)\n"
                 "  addi $a0, $a0, -1\n  jal fib\n  sw $v0, 0($sp)\n"
                 "  lw $a0, 4($sp)\n  addi $a0, $a0, -2\n  jal fib\n"
                 "  lw $t1, 0($sp)\n  add $v0, $v0, $t1\n  lw $ra, 8($sp)\n  addi $sp, $sp, 12\n  jr $ra\n"),
             std::string("55"));
}

TEST(comparisons_and_shifts_follow_the_sign) {
    const mips::Program program = mips::assemble(
        "  li $t0, -1\n  li $t1, 1\n  slt $s0, $t0, $t1\n  sltu $s1, $t0, $t1\n  sra $s2, $t0, 4\n  srl $s3, $t0, 28\n"
        "  nor $s4, $zero, $zero\n  sltiu $s5, $t1, -1\n  li $s6, 0x80000000\n  li $v0, 10\n  syscall\n");
    CHECK(program.ok());
    mips::Machine machine = mips::boot(program);
    machine.run(100);
    CHECK_EQ(machine.registers[16], 1u);
    CHECK_EQ(machine.registers[17], 0u);
    CHECK_EQ(machine.registers[18], 0xFFFFFFFFu);
    CHECK_EQ(machine.registers[19], 15u);
    CHECK_EQ(machine.registers[20], 0xFFFFFFFFu);
    CHECK_EQ(machine.registers[21], 1u);
    CHECK_EQ(machine.registers[22], 0x80000000u);
}

TEST(the_stack_sits_at_the_end_of_the_ram_and_the_data_at_its_start) {
    const mips::Program program = mips::assemble(".data\nv: .word 42\n.text\n  li $t0, 99\n  sw $t0, 0($sp)\n  li $v0, 10\n  syscall\n");
    mips::Machine machine = mips::boot(program);
    machine.run(100);
    CHECK_EQ(machine.ram[0], 42u);
    CHECK_EQ(machine.ram[mips::kMemoryWords - 1], 99u);
    CHECK_EQ(machine.memory(mips::kDataBase), 42u);
}

TEST(reading_the_switches_and_stopping_on_what_it_lacks) {
    same(run("  li $v0, 5\n  syscall\n  move $a0, $v0\n  li $v0, 1\n  syscall\n  li $v0, 10\n  syscall\n", 77), std::string("77"));
    mips::Machine machine = mips::boot(mips::assemble("  li $v0, 4\n  syscall\n"));
    machine.run(10);
    CHECK(machine.halted);
    CHECK(!machine.fault.empty());
    mips::Machine unknown = mips::boot(mips::assemble("  .word 0xFC000000\n"));
    unknown.run(10);
    CHECK(unknown.halted && !unknown.fault.empty());
}

int main() { return check::runAll(); }
