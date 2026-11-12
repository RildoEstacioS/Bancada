// The truth table of whatever is on the board: every switch combination, tried on a real copy of the
// circuit. Switches become the columns A, B, C... and LEDs and outputs become S, or S1, S2...
// Top to bottom and then left to right, the way a person reads a schematic, unless the player named them.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/board.hpp"
#include "editor/circuit.hpp"

namespace bancada::editor {

struct TruthTable {
    enum class Status : std::uint8_t {
        Ready,
        NothingToShow,   // no switches, or no outputs
        TooManyInputs,   // more switch bits than a table can sensibly show
        BoardTooBig,     // skipped, to keep edits on huge boards quick
    };

    Status status = Status::NothingToShow;
    std::vector<PartId> inputs;
    std::vector<PartId> outputs;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::vector<unsigned> inputBits;
    std::vector<unsigned> outputBits;
    std::size_t totalInputBits = 0;

    // rows[i] holds every output in hex digits, one after another: hexDigits(bits) of them each, or '~'
    // for a signal that never settles. Reading row number i as a binary number gives the switches, with
    // the first switch in its highest bits, so the table counts up as it should.
    std::vector<std::string> rows;

    bool hasMemory = false;  // something remembers: each row is what happens from a fresh start
    std::unordered_map<PartId, std::string> labels;
};

inline constexpr std::size_t kMaxTableInputs = 8;
inline constexpr std::size_t kMaxTableParts = 20000;

TruthTable computeTruthTable(const Board& board, const ChipBoards* chips = nullptr);

// What an output shows: its value, or for a real LED 1 when it's lit and 0 when it's dark or burnt.
std::uint64_t outputOf(const Board& board, const BoardCircuit& circuit, PartId output);

}  // namespace bancada::editor
