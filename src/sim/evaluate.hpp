// One instruction's result. Shared by the backends, so that they can only differ in what they choose
// to evaluate, never in what an instruction means.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim/program.hpp"

namespace bancada::sim {

using MemoryWords = std::vector<std::uint64_t>;

inline std::uint64_t evaluate(const Instr& instr, const std::uint64_t* values, const ConcatPart* parts, const MemoryWords* memories) {
    switch (instr.op) {
        case Op::Nand:
            return ~(values[instr.a] & values[instr.b]) & instr.mask;
        case Op::Slice:
            return (values[instr.a] >> instr.lo) & instr.mask;
        case Op::MemRead:
            return memories[instr.b][values[instr.a]];
        default: {  // Op::Concat
            std::uint64_t value = 0;
            for (Slot i = 0; i < instr.b; ++i) {
                const ConcatPart& part = parts[instr.a + i];
                value |= values[part.slot] << part.shift;
            }
            return value;
        }
    }
}

// A memory as it is after reset.
inline void resetMemory(MemoryWords& words, const Memory& memory) {
    words.assign(std::size_t{1} << memory.addressBits, 0);
    std::copy(memory.initial.begin(), memory.initial.end(), words.begin());
}

// Every pass of a loop of gates uses values from earlier in the same pass, so a long loop settles in
// few passes. One that hasn't settled after this many never will, for any size.
constexpr std::uint64_t loopPassLimit(std::uint32_t instructionsInLoop) {
    return 64 + std::uint64_t{4} * instructionsInLoop;
}

// How many times clock edges may set off further clock edges in one settle. A ripple counter needs
// one round per bit, so this leaves room for any real circuit.
inline constexpr int kMaxEdgeRounds = 4096;

}  // namespace bancada::sim
