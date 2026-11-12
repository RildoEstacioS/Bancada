// The 32-bit computer seen from outside, as the panel and the debugger show it: the PC, the 32 registers, the
// instruction about to run, the data and the stack. Found on any board with the 32-bit CPU chip and its ROM.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/board.hpp"
#include "editor/circuit.hpp"

namespace bancada::editor {

struct Computer32State {
    PartId cpu = 0;
    PartId rom = 0;
    PartId ram = 0;  // 0 if there's none
    std::uint32_t pc = 0;
    std::uint32_t index = 0;  // the PC as a word of the ROM: bits 11 to 2
    std::uint32_t instruction = 0;
    std::string text;  // the instruction, in assembly
    bool halted = false;
    bool invalid = false;  // stopped on something it can't do
    std::array<std::uint32_t, 32> registers{};
    std::string source;
    std::vector<int> lineOfWord;  // for each ROM word, its line in the source, counted from 1
    std::vector<std::uint32_t> data;   // the first words from 0x10010000
    std::vector<std::uint32_t> stack;  // the words from $sp up
    std::uint32_t switches = 0;
};

std::optional<Computer32State> readComputer32(const Board& board, const BoardCircuit& circuit, const ChipBoards& chips);

// What the instruction about to run will do, in a sentence, with the values it will use.
std::string explainInstruction32(const Computer32State& state);

// Which register the instruction writes, or -1.
int registerWritten32(std::uint32_t instruction);

}  // namespace bancada::editor
