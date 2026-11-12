// A 32-bit computer in the style of MIPS, the instruction set so many classes teach, with its assembler and a
// model that runs it one instruction at a time. Every instruction is one 32-bit word:
//
//   R:  op6 rs5 rt5 rd5 shamt5 funct6     op 0: the ALU operations, the shifts, jr and syscall
//   I:  op6 rs5 rt5 imm16                 addi, andi, lw, sw, beq, lui...
//   J:  op6 target26                      j, jal
//
// Addresses count bytes, as in MIPS: the program starts at 0x00400000 and its data at 0x10010000, as in MARS,
// so programs from class work as written. Each memory looks only at an address's low bits (1024 words), which
// puts the data at the start of the RAM and the stack, from 0x7FFFEFFC down, at its end.
//
// Kept simple on purpose: no delay slots (as in MARS by default), no overflow traps, no multiplier, and memory
// is read and written in whole words. An instruction this computer doesn't have stops it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "asm/assembler.hpp"

namespace bancada::mips {

constexpr std::uint32_t kTextBase = 0x00400000u;
constexpr std::uint32_t kDataBase = 0x10010000u;
constexpr std::uint32_t kStackTop = 0x7FFFEFFCu;
constexpr std::uint32_t kGlobalPointer = 0x10008000u;
constexpr std::size_t kMemoryWords = 1024;  // in the ROM and in the RAM: an address's bits 11 to 2 pick the word

enum Opcode : std::uint32_t {
    kOpSpecial = 0, kOpJ = 2, kOpJal = 3, kOpBeq = 4, kOpBne = 5, kOpAddi = 8, kOpAddiu = 9, kOpSlti = 10,
    kOpSltiu = 11, kOpAndi = 12, kOpOri = 13, kOpXori = 14, kOpLui = 15, kOpLw = 35, kOpSw = 43,
};

enum Funct : std::uint32_t {
    kFnSll = 0, kFnSrl = 2, kFnSra = 3, kFnJr = 8, kFnSyscall = 12, kFnAdd = 32, kFnAddu = 33, kFnSub = 34,
    kFnSubu = 35, kFnAnd = 36, kFnOr = 37, kFnXor = 38, kFnNor = 39, kFnSlt = 42, kFnSltu = 43,
};

// The registers' names, as the assembler takes them and the screens show them.
enum Register : int { kZero = 0, kAt = 1, kV0 = 2, kA0 = 4, kSp = 29, kRa = 31 };

// The calls a program makes with syscall, the code in $v0.
enum Call : std::uint32_t { kPrintInt = 1, kReadInt = 5, kExit = 10, kPrintChar = 11 };

struct Program {
    std::vector<std::uint32_t> text;  // the instructions, from kTextBase
    std::vector<int> lines;           // for each instruction, the line it came from, counted from 1
    std::vector<std::uint32_t> data;  // the .data words, from kDataBase
    std::vector<std::pair<std::string, std::uint32_t>> labels;
    std::vector<assembly::Error> errors;
    bool ok() const { return errors.empty(); }
};

// Two passes, so a label can be used before it's defined. Pseudo-instructions (li, la, move, blt...) become
// the real ones, as MARS writes them, using $at. All problems are reported, not just the first.
Program assemble(std::string_view source);

// One word back as text, with the real instruction's name. A branch's target depends on where it sits, so the
// address comes along. A word that means nothing comes back as .word, which assembles to the same word.
std::string disassemble(std::uint32_t word, std::uint32_t address);

// "$t0" for 8, and so on.
std::string_view registerName(unsigned index);

// The computer as the instructions describe it: what the circuit must do, step by step.
struct Machine {
    std::uint32_t pc = kTextBase;
    std::array<std::uint32_t, 32> registers{};
    std::vector<std::uint32_t> rom = std::vector<std::uint32_t>(kMemoryWords, 0);
    std::vector<std::uint32_t> ram = std::vector<std::uint32_t>(kMemoryWords, 0);
    std::uint32_t switches = 0;  // what syscall 5 reads
    std::string output;
    bool halted = false;
    std::string fault;  // why it stopped, when it wasn't syscall 10
    std::size_t steps = 0;

    std::uint32_t& memory(std::uint32_t address) { return ram[(address >> 2) % kMemoryWords]; }
    std::uint32_t fetch() const { return rom[(pc >> 2) % kMemoryWords]; }
    void step();
    void run(std::size_t limit);  // until it halts, or after this many steps
};

// A machine with the program loaded and the stack and global pointers where MARS puts them.
Machine boot(const Program& program);

}  // namespace bancada::mips
