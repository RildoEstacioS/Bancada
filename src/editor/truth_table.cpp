#include "editor/truth_table.hpp"

#include <algorithm>
#include <utility>

namespace bancada::editor {

namespace {

std::string hexOf(std::uint64_t value, unsigned digits) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text(digits, '0');
    for (unsigned i = 0; i < digits; ++i) {
        text[digits - 1 - i] = kDigits[value & 15];
        value >>= 4;
    }
    return text;
}

}  // namespace

std::uint64_t outputOf(const Board& board, const BoardCircuit& circuit, PartId output) {
    const Part* part = board.part(output);
    if (part != nullptr && isLed(part->kind)) {
        const std::optional<PartReading> reading = circuit.reading(output);
        return reading && reading->light >= kLedLitAbove && !reading->burnt ? 1 : 0;
    }
    return circuit.outputValue(output);
}

TruthTable computeTruthTable(const Board& board, const ChipBoards* chips) {
    TruthTable table;
    table.inputs = inputsInOrder(board);
    table.outputs = outputsInOrder(board);

    for (std::size_t i = 0; i < table.inputs.size(); ++i) {
        const Part& part = *board.part(table.inputs[i]);
        table.inputNames.push_back(part.label.empty() ? defaultInputName(i) : part.label);
        table.inputBits.push_back(part.width);
        table.totalInputBits += part.width;
        table.labels[part.id] = table.inputNames.back();
    }
    for (std::size_t i = 0; i < table.outputs.size(); ++i) {
        const Part& part = *board.part(table.outputs[i]);
        table.outputNames.push_back(part.label.empty() ? defaultOutputName(i, table.outputs.size()) : part.label);
        table.outputBits.push_back(part.width);
        table.labels[part.id] = table.outputNames.back();
    }
    // Real LEDs come after: lit is 1. Only one the player named gets its name drawn on the board.
    const std::vector<PartId> lights = lightsInOrder(board);
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const Part& part = *board.part(lights[i]);
        table.outputs.push_back(part.id);
        table.outputNames.push_back(!part.label.empty() ? part.label : lights.size() == 1 ? "LED" : "LED" + std::to_string(i + 1));
        table.outputBits.push_back(1);
        if (!part.label.empty()) table.labels[part.id] = part.label;
    }

    if (table.inputs.empty() || table.outputs.empty()) return table;
    if (table.totalInputBits > kMaxTableInputs) {
        table.status = TruthTable::Status::TooManyInputs;
        return table;
    }
    if (board.parts().size() > kMaxTableParts) {
        table.status = TruthTable::Status::BoardTooBig;
        return table;
    }

    BoardCircuit circuit(board, chips);
    table.hasMemory = circuit.hasMemory();
    const std::size_t rows = std::size_t{1} << table.totalInputBits;
    table.rows.reserve(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        circuit.reset();
        unsigned remaining = static_cast<unsigned>(table.totalInputBits);
        for (std::size_t i = 0; i < table.inputs.size(); ++i) {
            remaining -= table.inputBits[i];
            circuit.setToggle(table.inputs[i], row >> remaining);
        }
        circuit.settle();
        std::string values;
        for (std::size_t i = 0; i < table.outputs.size(); ++i) {
            const unsigned digits = hexDigits(table.outputBits[i]);
            values += circuit.oscillating() ? std::string(digits, '~') : hexOf(outputOf(board, circuit, table.outputs[i]), digits);
        }
        table.rows.push_back(std::move(values));
    }
    table.status = TruthTable::Status::Ready;
    return table;
}

}  // namespace bancada::editor
