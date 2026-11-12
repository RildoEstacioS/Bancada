// What a netlist compiles into: flat, ordered, and free of pointers, so every backend (the interpreter
// now, a JIT later) runs the same thing. Nets may share a slot once wires are resolved.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "sim/netlist.hpp"

namespace bancada::sim {

using Slot = std::uint32_t;

struct Instr {
    Op op = Op::Nand;              // Nand, Slice, Concat or MemRead
    std::uint8_t lo = 0;           // Slice: first bit
    Slot dst = 0;
    Slot a = 0;                    // Concat: index into Program::concatParts. MemRead: the address
    Slot b = 0;                    // Concat: how many parts. MemRead: which memory
    std::uint64_t mask = 0;
};

struct ConcatPart {
    Slot slot = 0;
    std::uint8_t shift = 0;
};

struct DffInstr {
    Slot q = 0;
    Slot d = 0;
    Slot clock = 0;
    std::uint64_t resetValue = 0;
};

struct PortInstr {
    std::uint32_t memory = 0;
    Slot address = 0;
    Slot data = 0;
    Slot enable = 0;
    Slot clock = 0;
};

// A run of instructions. A plain block is in dependency order and needs one pass; a loop block is a
// group of gates feeding each other, repeated until it stops changing.
struct Block {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    bool loop = false;
};

struct Diagnostic {
    enum class Kind : std::uint8_t {
        Undriven,          // a wire with nothing driving it: it reads as 0
        MultipleDrivers,   // two outputs on one wire: the first one wins
        WireLoop,          // wires joined only to each other: they read as 0
    };
    Kind kind = Kind::Undriven;
    NetId net = 0;
};

struct Program {
    std::vector<Instr> instrs;
    std::vector<ConcatPart> concatParts;
    std::vector<Block> blocks;
    std::vector<DffInstr> dffs;
    std::vector<Memory> memories;
    std::vector<PortInstr> ports;

    std::uint32_t slotCount = 0;
    std::vector<Slot> slotOfNet;          // every net's slot
    std::vector<std::uint8_t> isInput;    // by net
    std::vector<std::uint8_t> netWidth;   // by net
    std::vector<std::pair<Slot, std::uint64_t>> constants;

    std::vector<Diagnostic> diagnostics;
};

Program compile(const Netlist& netlist);

}  // namespace bancada::sim
