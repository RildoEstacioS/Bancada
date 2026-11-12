// A 32-bit computer in the style of MIPS, built out of editor parts like the 16-bit one: the same boxes, wires
// and chips a player has. It runs the instructions of asm/mips.hpp, one per clock cycle, the datapath of the
// textbooks: a decoder, a control unit, the register file, the ALU, the program counter, and a small unit for
// syscall, the program's way of asking for things from outside the CPU.
#pragma once

#include <string>
#include <vector>

#include "asm/mips.hpp"
#include "editor/board.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

struct Computer32Project {
    Board board;
    std::vector<ChipData> chips;
    PartId clock = 0;
    PartId reset = 0;
    PartId switches = 0;
    PartId cpu = 0;
    PartId rom = 0;
    PartId ram = 0;
    PartId pcProbe = 0;
    PartId outputProbe = 0;  // the last number or character a syscall printed
    PartId printNumber = 0;  // on for the cycle a syscall prints a number
    PartId printChar = 0;    // on for the cycle a syscall prints a character
    PartId haltLed = 0;
    PartId invalidLed = 0;   // stopped on an instruction this computer doesn't have
    PartId terminal = 0;
    std::vector<PartId> registersPath;  // from the board down to the register file, to read the registers
};

// The bits of a control row: one ROM row per opcode, and one per funct for the R-type instructions.
enum Control32Bit : unsigned {
    kC32RegWrite = 0, kC32MemWrite = 1, kC32AluImm = 2, kC32SignExtend = 3, kC32RegDstLow = 4, kC32WbLow = 6,
    kC32BranchEq = 8, kC32BranchNe = 9, kC32Jump = 10, kC32JumpReg = 11, kC32Syscall = 12, kC32Invalid = 13,
    kC32UseFunct = 14, kC32AluOpLow = 15,  // and the three bits above it
};
inline constexpr unsigned kControl32Bits = 19;

// The ALU's operations, as the control sends them.
enum Alu32Op : unsigned {
    kAlu32And = 0, kAlu32Or = 1, kAlu32Xor = 2, kAlu32Nor = 3, kAlu32Add = 4, kAlu32Sub = 5, kAlu32Slt = 6, kAlu32Sltu = 7,
    kAlu32Sll = 8, kAlu32Srl = 9, kAlu32Sra = 10, kAlu32Lui = 11,
};

inline constexpr const char* kCpu32Name = "CPU 32 bits";
inline constexpr unsigned kWord32Bits = 32;
inline constexpr unsigned kMemory32AddressBits = 10;  // 1024 words, as in mips::kMemoryWords

// The rows of the two control ROMs, for anyone reading them, and for the tests.
std::vector<std::uint64_t> mainControlRows();
std::vector<std::uint64_t> functControlRows();

// The CPU alone, its blocks each a chip of its own, under ids counting from first. The last is the CPU.
std::vector<ChipData> buildCpu32Chips(ChipId first);

// The whole computer, with this program in its ROM and its .data in its RAM. The source is kept in the ROM.
Computer32Project buildComputer32(const mips::Program& program, const std::string& source);

}  // namespace bancada::editor
