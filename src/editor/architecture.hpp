// A computer drawn whole, every gate in its place, as a real schematic: the "por dentro" plan.
//
// Everything is a box with pins: a NAND, a logic gate (the NANDs it's made of), a full adder, a register,
// the ALU, the CPU, the computer. Inside each box its parts stand in columns, in the order the signals go
// through them, and every wire runs from the pin that drives it to every pin that reads it: across to a
// channel between the columns, down the channel on a track of its own, and across into the pin. A wire that
// skips columns goes over the top on a lane of its own. No wire ever runs through a part.
//
// It's made from the netlist the simulator runs, so what it shows is what runs. The three pictures, logic
// gates, NANDs and transistors, are the same plan: a gate is drawn over the very spot its NANDs fill, with
// its pins where the NANDs' wires come in and go out.
//
// Lengths are in units: a NAND is one unit square.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "editor/circuit.hpp"

namespace bancada::editor {

struct ArchRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

enum class ArchGate : std::uint8_t { Not, And, Or, Nand, Nor, Xor, Xnor };
enum class ArchFrameKind : std::uint8_t { Computer, Chip, Box, Detail };

// A stretch of wire, straight across or straight down. Stretches with the same signal are the same wire.
struct ArchSegment {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    std::uint32_t signal = 0;
};

struct ArchPoint {
    float x = 0.0f, y = 0.0f;
};

// What a frame or a gate draws of the wires: its own, in a range of Architecture::segments and ::dots.
struct ArchWires {
    std::uint32_t firstSegment = 0, segments = 0;
    std::uint32_t firstDot = 0, dots = 0;
};

struct ArchFrame {
    ArchRect rect;
    std::string label;
    ArchFrameKind kind = ArchFrameKind::Box;
    int depth = 0;   // 0 for the computer
    ArchWires wires;  // the wires between the parts right inside it
};

// One logic gate: its pins, and the NANDs inside it with the wires between them.
struct ArchCell {
    ArchRect rect;
    ArchGate gate = ArchGate::Nand;
    std::vector<float> inputs;  // the y of each input pin, on the left edge
    float output = 0.0f;        // the y of the output pin, on the right edge
    std::uint32_t firstNand = 0, nands = 0;
    ArchWires wires;  // between its NANDs: drawn only when the NANDs are
};

// A NAND: inputs at a quarter and three quarters of its height on the left, the output halfway on the right.
struct ArchNand {
    ArchRect rect;
};

// A memory: a grid of one-bit cells, in banks side by side, with its pins down the sides.
struct ArchMemory {
    ArchRect rect;   // the whole part, pins and all
    ArchRect grid;   // where the bits are
    std::string label;
    unsigned words = 0, bits = 0;
    unsigned banks = 1;
    bool rom = false;
};
inline constexpr float kArchMemoryCell = 0.125f;  // the side of one bit of memory

// A pin held at 0 or 1.
struct ArchTie {
    float x = 0.0f, y = 0.0f;
    std::uint8_t value = 0;
};

// Where a signal comes into the computer from outside: the clock, the switches...
struct ArchPort {
    float x = 0.0f, y = 0.0f;
    std::string label;
};

struct ArchCounts {
    std::size_t gates = 0;
    std::size_t flipFlops = 0;
    std::size_t nands = 0;
    std::size_t transistors = 0;
    std::size_t memoryBits = 0;
};

// A pin of a NAND or a memory, with the signal on it: for checking that every wire arrives where it should.
struct ArchPin {
    float x = 0.0f, y = 0.0f;
    std::uint32_t signal = 0;
    bool drives = false;
};

// How to know what a signal of the plan carries while the circuit runs. Most are a bit of a net of the
// running circuit; the wires inside a flip-flop, which runs as one piece, follow from the ones around it.
enum class ArchRule : std::uint8_t { Net, Not, Nand, Latched };
struct ArchSignal {
    ArchRule rule = ArchRule::Net;
    std::uint32_t net = 0;  // Net: the net, and the bit of it
    unsigned bit = 0;
    std::uint32_t a = 0, b = 0, c = 0;  // Not: a. Nand: a and b. Latched: b while the clock a is 1, c while it's 0
};

// Where a part of the board the plan was drawn from ended up, by the cell it sits on in that board.
struct ArchPlace {
    int x = 0, y = 0;
    ArchRect rect;
};

struct Architecture {
    float width = 0.0f, height = 0.0f;
    std::vector<ArchFrame> frames;  // a frame comes before the frames inside it
    std::vector<ArchCell> cells;
    std::vector<ArchNand> nands;
    std::vector<ArchMemory> memories;
    std::vector<ArchSegment> segments;
    std::vector<ArchPoint> dots;  // where a wire branches
    std::vector<ArchTie> ties;
    std::vector<ArchPort> ports;
    std::vector<ArchPin> pins;
    std::vector<ArchSignal> signals;  // by the number in a segment or a pin
    std::vector<ArchPlace> places;    // the parts of the board on top, each where it was drawn
    ArchCounts counts;
};

// Each signal's value now, 0 or 1, from a running circuit of the same board and chips the plan was drawn from.
std::vector<std::uint8_t> archValues(const Architecture& plan, const BoardCircuit& circuit);

// The plan of a board and every chip inside it.
Architecture buildArchitecture(const Board& board, const ChipBoards* chips, const std::string& title);

// The example computers, whole.
Architecture architecture16();
Architecture architecture32();

}  // namespace bancada::editor
