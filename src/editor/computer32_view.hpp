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
    std::uint32_t memoryAtAddress = 0;  // the RAM word a lw or sw about to run uses, as it is now
};

std::optional<Computer32State> readComputer32(const Board& board, const BoardCircuit& circuit, const ChipBoards& chips);

// What the instruction about to run will do, in a sentence, with the values it will use.
std::string explainInstruction32(const Computer32State& state);

// Which register the instruction writes, or -1.
int registerWritten32(std::uint32_t instruction);

// Everything the instruction about to run does on the 32-bit datapath, with the value on every wire: what
// the X-ray shows. Worked out from the instruction set; the tests check it against the model.
struct Trace32 {
    enum class Back : std::uint8_t { None, Alu, Memory, Link, Switches };
    enum class Next : std::uint8_t { Plain, Branch, Jump, Register, Stay };

    std::uint32_t pc = 0, pc4 = 0, word = 0;
    std::string text;
    std::string name;   // "add", "lw"...
    char format = 'R';  // R, I or J
    unsigned op = 0, funct = 0, rs = 0, rt = 0, rd = 0, shamt = 0;
    std::uint32_t imm = 0;

    bool readsA = false, readsB = false;
    unsigned registerA = 0, registerB = 0;
    std::uint32_t valueA = 0, valueB = 0;

    bool usesImmediate = false;
    bool signExtends = false;
    std::uint32_t extended = 0;  // the immediate, made 32 bits

    bool usesAlu = false;
    bool aluTakesImmediate = false;
    bool shifts = false;  // sll, srl, sra, lui: the ALU's shifter
    std::uint32_t aluA = 0, aluB = 0, aluResult = 0;
    std::string aluOperation;  // "+", "-", "E", "<"...
    bool zero = false;         // the ALU's Z

    bool readsMemory = false, writesMemory = false;
    std::uint32_t address = 0, memoryValue = 0;

    Back back = Back::None;
    unsigned destination = 0;  // which choice of the write MUX: 0 rt, 1 rd, 2 $ra, 3 $v0
    unsigned written = 0;      // $zero is never written: then back stays None
    std::uint32_t writtenValue = 0;

    bool syscall = false;
    std::uint32_t call = 0;  // $v0
    bool printsNumber = false, printsChar = false;
    std::uint32_t printed = 0;

    Next next = Next::Plain;
    bool branches = false, taken = false;
    std::uint32_t branchTarget = 0, jumpTarget = 0, nextPc = 0;
    bool halts = false, invalid = false;
};

Trace32 traceInstruction32(std::uint32_t word, std::uint32_t pc, const std::array<std::uint32_t, 32>& registers, std::uint32_t switches,
                           std::uint32_t memoryAtAddress);
Trace32 traceInstruction32(const Computer32State& state);

// What happens in one of the six steps, in a sentence or three with the instruction's own values.
std::string explainStage32(const Trace32& trace, int stage);

}  // namespace bancada::editor
