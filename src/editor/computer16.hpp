// A 16-bit computer built entirely out of editor parts: the same boxes, wires and chips a player has, on
// real boards that can be opened and changed. It's a project like any other, and the example in the game.
//
// One instruction takes one clock cycle. The control unit is a ROM: the instruction's opcode picks a row,
// and each bit of the row turns one part of the computer on. The CPU is a board of five chips, the blocks
// of a textbook diagram, so that every board is small enough to read.
#pragma once

#include <string>
#include <vector>

#include "editor/board.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

struct ComputerProject {
    Board board;                  // the computer: clock, CPU chip, ROM, RAM, switches and displays
    std::vector<ChipData> chips;  // the CPU, and the decoder inside it
    PartId clock = 0;
    PartId reset = 0;
    PartId switches = 0;
    PartId rom = 0;
    PartId ram = 0;
    PartId outputProbe = 0;
    PartId pcProbe = 0;
    PartId haltLed = 0;
};

// The bits of a control ROM row, for anyone reading it, and for the tests.
enum ControlBit : unsigned {
    kRegWrite = 0, kMemWrite = 1, kAluImm = 2, kWbLow = 3, kWbHigh = 4, kBranchEq = 5, kBranchNe = 6, kJump = 7,
    kJumpReg = 8, kOutWrite = 9, kHalt = 10, kUseFunc = 11, kImmLow = 12, kImmHigh = 13, kReadRd = 14, kLink = 15,
    kAluZero = 16, kAluOpLow = 17,  // and the two bits above it
};
inline constexpr unsigned kControlBits = 20;
inline constexpr unsigned kWordBits = 16;
inline constexpr const char* kCpuName = "CPU 16 bits";

// The sample program in the example: adds 1 to 10 and shows the total.
const char* exampleProgram();

// The CPU alone, ready to go on any board: its blocks, each a chip of its own to open and read, under ids
// counting from first. The last one is the CPU itself.
inline constexpr ChipId kCpuChipCount = 6;
std::vector<ChipData> buildCpuChips(ChipId first);

// Builds the computer with this program in its ROM. The program's source is kept in the ROM part.
ComputerProject buildComputer16(const std::vector<std::uint16_t>& program, const std::string& source);

}  // namespace bancada::editor
