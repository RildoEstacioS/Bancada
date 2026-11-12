// A running 16-bit computer seen from outside: what the panel shows instead of wires. Found on any board
// that has the CPU chip, its ROM and its RAM, wherever the player put them.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/board.hpp"
#include "editor/circuit.hpp"

namespace bancada::editor {

struct ComputerState {
    PartId cpu = 0;
    PartId rom = 0;
    PartId ram = 0;  // 0 if there's none
    std::uint16_t pc = 0;
    std::uint16_t output = 0;
    bool halted = false;
    std::array<std::uint16_t, 8> registers{};
    std::uint16_t instruction = 0;  // the word at PC: what's about to run
    std::string source;             // the program, if the ROM has it
    std::vector<int> lineOfWord;    // for each ROM word, its line in the source, counted from 1
    std::vector<std::uint16_t> memory;  // the first words of the RAM
    std::uint16_t switches = 0;         // what the switches on the CPU's Chaves pin say
    std::uint16_t memoryAtAddress = 0;  // the RAM word a load or store about to run uses, as it is now
};

std::optional<ComputerState> readComputer(const Board& board, const BoardCircuit& circuit, const ChipBoards& chips);

// Everything the instruction about to run does, with every value it does it with: what the X-ray writes
// on each wire. Worked out from the instruction set; the tests check it against the circuit itself.
struct InstructionTrace {
    enum class WriteBack : std::uint8_t { None, Alu, Memory, Link, Switches };
    enum class Next : std::uint8_t { Plain, Branch, Jump, Register, Stay };

    std::uint16_t pc = 0;
    std::uint16_t pcPlusOne = 0;
    std::uint16_t word = 0;
    std::string text;  // in assembly
    const char* name = "";  // the instruction's name: "ADD", "BNE"...
    unsigned op = 0, rd = 0, rs = 0, rt = 0, func = 0;

    // The fields that become a number: imm6 sign-extended, imm9, imm9 shifted for LUI, or imm12.
    bool usesImmediate = false;
    std::uint16_t immediate = 0;
    const char* immediateField = "";

    // The register file's two read ports.
    bool readsA = false, readsB = false;
    unsigned registerA = 0, registerB = 0;
    std::uint16_t valueA = 0, valueB = 0;

    // The ALU. Its second input is register B or the immediate; LI and LUI add to a zeroed first input.
    bool usesAlu = false;
    bool aluTakesImmediate = false;
    bool aluZeroesA = false;
    std::uint16_t aluA = 0, aluB = 0, aluResult = 0;
    const char* aluOperation = "";  // "+", "−", "E", "OU"...

    bool readsMemory = false, writesMemory = false;
    std::uint16_t address = 0;
    std::uint16_t memoryValue = 0;  // read by a load, or written by a store

    WriteBack writeBack = WriteBack::None;
    unsigned writtenRegister = 0;  // R0 is never written: then writeBack stays None
    std::uint16_t writtenValue = 0;

    bool shows = false;  // OUT
    std::uint16_t shown = 0;

    Next next = Next::Plain;
    bool branches = false;  // BEQ or BNE, taken or not
    bool branchTaken = false;
    std::uint16_t branchTarget = 0;
    std::uint16_t nextPc = 0;
    bool halts = false;
    bool notAnInstruction = false;
};

InstructionTrace traceInstruction(const ComputerState& state);

// The six steps every instruction takes, the way the X-ray shows them.
inline constexpr int kTraceStages = 6;
inline constexpr const char* kTraceStageNames[kTraceStages] = {"Busca", "Decodifica", "Executa", "Memória", "Escreve", "Próximo PC"};

// What happens in one of those steps, in a sentence with the instruction's own values.
std::string explainStage(const InstructionTrace& trace, int stage);

// The parts of the diagram an instruction uses, one bit each.
enum Block : unsigned { kBlockPc, kBlockRom, kBlockDecoder, kBlockControl, kBlockRegisters, kBlockAlu, kBlockRam, kBlockOutput, kBlockSwitches };
enum Path : unsigned {
    kPathPcRom, kPathRomDecoder, kPathDecoderRegisters, kPathRegistersAlu, kPathImmediate, kPathWriteBack, kPathOutput, kPathBranch,
    kPathDecoderControl, kPathAddress, kPathLoad, kPathStore, kPathJump, kPathLink, kPathJumpRegister, kPathSwitches,
};

struct DatapathUse {
    std::uint32_t blocks = 0;
    std::uint32_t paths = 0;
};

DatapathUse datapathUse(std::uint16_t instruction);

// What the instruction about to run will do, in a sentence, with the values it will use.
std::string explainInstruction(const ComputerState& state);

// Which register the instruction writes, or -1.
int registerWritten(std::uint16_t instruction);

}  // namespace bancada::editor
