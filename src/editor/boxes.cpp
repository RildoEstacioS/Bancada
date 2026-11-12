#include "editor/boxes.hpp"

#include <array>
#include <map>
#include <tuple>
#include <utility>

#include "sim/gates.hpp"

namespace bancada::editor {

namespace {

using sim::NetId;
using sim::Netlist;
namespace g = sim::gates;
using In = std::span<const NetId>;
using Out = std::span<NetId>;
using Params = const BoxParams&;

NetId mux4(Netlist& n, NetId d0, NetId d1, NetId d2, NetId d3, NetId s0, NetId s1) {
    return g::mux(n, g::mux(n, d0, d1, s0), g::mux(n, d2, d3, s0), s1);
}

NetId mux4Bus(Netlist& n, NetId d0, NetId d1, NetId d2, NetId d3, NetId s0, NetId s1) {
    return g::muxBus(n, g::muxBus(n, d0, d1, s0), g::muxBus(n, d2, d3, s0), s1);
}

struct AluBit {
    NetId result;
    NetId carry;
};

// Op1 Op0: 00 AND, 01 OR, 10 XOR, 11 ADD.
AluBit aluBit(Netlist& n, NetId a, NetId b, NetId carryIn, NetId op0, NetId op1) {
    const g::Sum sum = g::fullAdder(n, a, b, carryIn);
    const NetId result = mux4(n, g::andGate(n, a, b), g::orGate(n, a, b), g::xorGate(n, a, b), sum.sum, op0, op1);
    const NetId adding = g::andGate(n, op0, op1);
    return {result, g::andGate(n, sum.carry, adding)};
}

// a - b - borrowIn, and a 1 on the borrow wire if it went below zero.
g::Sum subtract(Netlist& n, NetId a, NetId b, NetId borrowIn) {
    const g::Sum sum = g::rippleAdder(n, a, g::notGate(n, b), g::notGate(n, borrowIn));
    return {sum.sum, g::notGate(n, sum.carry)};
}

// 1 if every bit of a bus is 1.
NetId allOnes(Netlist& n, NetId bus) {
    const unsigned width = n.width(bus);
    NetId result = n.slice(bus, 0, 1);
    for (unsigned i = 1; i < width; ++i) result = g::andGate(n, result, n.slice(bus, i, 1));
    return result;
}

void halfAdder(Netlist& n, In in, Out out, Params) {
    out[0] = g::xorGate(n, in[0], in[1]);
    out[1] = g::andGate(n, in[0], in[1]);
}

void fullAdder(Netlist& n, In in, Out out, Params) {
    const g::Sum sum = g::fullAdder(n, in[0], in[1], in[2]);
    out[0] = sum.sum;
    out[1] = sum.carry;
}

void adder4(Netlist& n, In in, Out out, Params) {
    NetId carry = in[8];
    for (std::size_t i = 0; i < 4; ++i) {
        const g::Sum sum = g::fullAdder(n, in[i], in[i + 4], carry);
        out[i] = sum.sum;
        carry = sum.carry;
    }
    out[4] = carry;
}

void fullSubtractor(Netlist& n, In in, Out out, Params) {
    const NetId a = in[0];
    const NetId b = in[1];
    const NetId borrowIn = in[2];
    const NetId difference = g::xorGate(n, a, b);
    out[0] = g::xorGate(n, difference, borrowIn);
    const NetId notA = g::notGate(n, a);
    const NetId notDifference = g::notGate(n, difference);
    out[1] = g::orGate(n, g::andGate(n, notA, b), g::andGate(n, notDifference, borrowIn));
}

void comparator(Netlist& n, In in, Out out, Params) {
    out[0] = g::andGate(n, in[0], g::notGate(n, in[1]));
    out[1] = g::xnorGate(n, in[0], in[1]);
    out[2] = g::andGate(n, g::notGate(n, in[0]), in[1]);
}

void mux2(Netlist& n, In in, Out out, Params) { out[0] = g::mux(n, in[0], in[1], in[2]); }

// --- Phase two: the parts every simulator has -----------------------------------------------------------

NetId concatBits(Netlist& n, const std::vector<NetId>& lowFirst) { return n.concat(std::span<const NetId>(lowFirst)); }

// A bus wired to a bus as wide, through two inverters: the signal is the same, made strong again.
void buffer(Netlist& n, In in, Out out, Params) { out[0] = g::notGate(n, g::notGate(n, in[0])); }

NetId xorOfBits(Netlist& n, NetId bus) {
    NetId result = n.slice(bus, 0, 1);
    for (unsigned i = 1; i < n.width(bus); ++i) result = g::xorGate(n, result, n.slice(bus, i, 1));
    return result;
}

// 1 when the number of 1s is odd; the even one says 1 when it's even, so the bits and it together are odd.
void parityOdd(Netlist& n, In in, Out out, Params) { out[0] = xorOfBits(n, in[0]); }
void parityEven(Netlist& n, In in, Out out, Params) { out[0] = g::notGate(n, xorOfBits(n, in[0])); }

NetId orOfBits(Netlist& n, NetId bus) {
    NetId result = n.slice(bus, 0, 1);
    for (unsigned i = 1; i < n.width(bus); ++i) result = g::orGate(n, result, n.slice(bus, i, 1));
    return result;
}

// The bit of D numbered by Sel: each bit has an AND that opens only when Sel is its number.
void bitSelect(Netlist& n, In in, Out out, Params params) {
    NetId result = n.constant(0);
    const unsigned address = params.address;
    for (unsigned i = 0; i < params.width && (address >= 32 || i < (1u << address)); ++i) {
        const NetId here = allOnes(n, g::xnorGate(n, in[1], n.constant(i, address)));
        result = g::orGate(n, result, g::andGate(n, n.slice(in[0], i, 1), here));
    }
    out[0] = result;
}

// The number of the highest input that's on, and V when any is.
void priority4(Netlist& n, In in, Out out, Params) {
    out[0] = g::orGate(n, in[3], g::andGate(n, g::notGate(n, in[2]), in[1]));
    out[1] = g::orGate(n, in[3], in[2]);
    out[2] = g::orGate(n, g::orGate(n, in[0], in[1]), g::orGate(n, in[2], in[3]));
}

// Shift and add, as on paper: for each bit of B that's 1, A shifted to that bit's place is added in.
void multiplier(Netlist& n, In in, Out out, Params params) {
    const unsigned w = params.width;
    NetId total = n.constant(0, 2 * w);
    for (unsigned i = 0; i < w; ++i) {
        const NetId row = g::andGate(n, in[0], g::fan(n, n.slice(in[1], i, 1), w));
        std::vector<NetId> parts;
        if (i > 0) parts.push_back(n.constant(0, i));
        parts.push_back(row);
        parts.push_back(n.constant(0, w - i));
        total = g::rippleAdder(n, total, concatBits(n, parts), n.constant(0)).sum;
    }
    out[0] = n.slice(total, 0, w);
    out[1] = n.slice(total, w, w);
}

// Long division, one bit of A at a time from the top: the remainder takes the next bit, and B comes off
// it whenever it fits, which makes that bit of the quotient a 1.
void divider(Netlist& n, In in, Out out, Params params) {
    const unsigned w = params.width;
    NetId remainder = n.constant(0, w + 1);
    const std::vector<NetId> wideB{in[1], n.constant(0)};
    const NetId divisor = concatBits(n, wideB);
    std::vector<NetId> quotient(w);
    for (unsigned k = 0; k < w; ++k) {
        const unsigned i = w - 1 - k;
        const std::vector<NetId> shifted{n.slice(in[0], i, 1), n.slice(remainder, 0, w)};
        const NetId tried = concatBits(n, shifted);
        const g::Sum difference = subtract(n, tried, divisor, n.constant(0));
        quotient[i] = g::notGate(n, difference.carry);
        remainder = g::muxBus(n, difference.sum, tried, difference.carry);
    }
    out[0] = concatBits(n, quotient);
    out[1] = n.slice(remainder, 0, w);
}

// Minus A in two's complement: every bit inverted, plus one.
void negator(Netlist& n, In in, Out out, Params params) {
    out[0] = g::rippleAdder(n, g::notGate(n, in[0]), n.constant(0, params.width), n.constant(1)).sum;
}

// The segments a to g of each hexadecimal digit, a in the highest of the seven bits.
constexpr std::uint8_t kSegmentsOf[16] = {0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011, 0b1011011, 0b1011111, 0b1110000,
                                          0b1111111, 0b1111011, 0b1110111, 0b0011111, 0b1001110, 0b0111101, 0b1001111, 0b1000111};

// One segment, from the four bits of the digit: a tree of multiplexers over its sixteen answers.
NetId segmentOf(Netlist& n, NetId digit, unsigned segment) {
    std::vector<NetId> level;
    for (unsigned value = 0; value < 16; ++value) level.push_back(n.constant((kSegmentsOf[value] >> (6 - segment)) & 1u));
    for (unsigned bit = 0; bit < 4; ++bit) {
        std::vector<NetId> next;
        for (std::size_t i = 0; i < level.size(); i += 2) next.push_back(g::mux(n, level[i], level[i + 1], n.slice(digit, bit, 1)));
        level = std::move(next);
    }
    return level[0];
}

void decoder7(Netlist& n, In in, Out out, Params) {
    for (unsigned segment = 0; segment < 7; ++segment) out[segment] = segmentOf(n, in[0], segment);
}

// Numbers that look random: eight flip-flops shifting left on each clock, the new bit an XNOR of
// four of them (a linear feedback shift register). It goes through 255 values before repeating, and
// starts from 0, which an XNOR register can leave. Rst takes it back to 0.
void randomBox(Netlist& n, In in, Out out, Params) {
    const NetId next = n.wire(8);
    const NetId q = n.dff(next, in[0]);
    const auto bit = [&](unsigned i) { return n.slice(q, i, 1); };
    const NetId feedback = g::xnorGate(n, g::xnorGate(n, bit(7), bit(5)), g::xnorGate(n, bit(4), bit(3)));
    const std::vector<NetId> shifted{feedback, n.slice(q, 0, 7)};
    n.drive(next, g::muxBus(n, n.concat(std::span<const NetId>(shifted)), n.constant(0, 8), in[1]));
    out[0] = q;
}

// Eight rows of LEDs, a bus each: the board draws every bit of a row as a dot. Nothing to compute.
void ledMatrix(Netlist&, In, Out, Params) {}

// A screen of text: on each clock pulse with Write on, the character goes into the next of 64 places of a
// memory, and a counter moves on. Clear takes the counter back to the start. The board draws the
// characters up to the counter, and N says how many there are.
void terminal(Netlist& n, In in, Out out, Params) {
    const std::uint32_t memory = n.memory(6, 7);
    const NetId next = n.wire(6);
    const NetId count = n.dff(next, in[2]);
    n.memoryWrite(memory, count, in[0], in[1], in[2]);
    const NetId moved = g::muxBus(n, count, g::rippleAdder(n, count, n.constant(0, 6), n.constant(1)).sum, in[1]);
    n.drive(next, g::muxBus(n, moved, n.constant(0, 6), in[3]));
    out[0] = count;
}

// How many bits are 1: each bit added to a counter in turn.
void bitCount(Netlist& n, In in, Out out, Params params) {
    NetId total = n.constant(0, 7);
    for (unsigned i = 0; i < params.width; ++i) {
        const std::vector<NetId> one{n.slice(in[0], i, 1), n.constant(0, 6)};
        total = g::rippleAdder(n, total, concatBits(n, one), n.constant(0)).sum;
    }
    out[0] = total;
}

// The number of the lowest bit that's 1, and V when there is one. Going down from the top, each bit
// that's on takes over, so the lowest one wins.
void bitFind(Netlist& n, In in, Out out, Params params) {
    NetId index = n.constant(0, 6);
    for (unsigned k = 0; k < params.width; ++k) {
        const unsigned i = params.width - 1 - k;
        index = g::muxBus(n, index, n.constant(i, 6), n.slice(in[0], i, 1));
    }
    out[0] = index;
    out[1] = orOfBits(n, in[0]);
}

void mux4Box(Netlist& n, In in, Out out, Params) { out[0] = mux4(n, in[0], in[1], in[2], in[3], in[4], in[5]); }

void demux2(Netlist& n, In in, Out out, Params) {
    out[0] = g::andGate(n, in[0], g::notGate(n, in[1]));
    out[1] = g::andGate(n, in[0], in[1]);
}

void decoder2(Netlist& n, In in, Out out, Params) {
    const NetId notA0 = g::notGate(n, in[0]);
    const NetId notA1 = g::notGate(n, in[1]);
    out[0] = g::andGate(n, notA0, notA1);
    out[1] = g::andGate(n, in[0], notA1);
    out[2] = g::andGate(n, notA0, in[1]);
    out[3] = g::andGate(n, in[0], in[1]);
}

void encoder4(Netlist& n, In in, Out out, Params) {
    out[0] = g::orGate(n, in[1], in[3]);
    out[1] = g::orGate(n, in[2], in[3]);
}

// Set and reset are active on 1 here, the way a player thinks of them. Inside, the two crossed NANDs
// want them inverted.
void srLatch(Netlist& n, In in, Out out, Params) {
    const g::Latch latch = g::srLatch(n, g::notGate(n, in[0]), g::notGate(n, in[1]));
    out[0] = latch.q;
    out[1] = latch.notQ;
}

void dLatch(Netlist& n, In in, Out out, Params) {
    const g::Latch latch = g::dLatch(n, in[0], in[1]);
    out[0] = latch.q;
    out[1] = latch.notQ;
}

void dFlipFlop(Netlist& n, In in, Out out, Params) {
    const g::Latch ff = g::dFlipFlop(n, in[0], in[1]);
    out[0] = ff.q;
    out[1] = ff.notQ;
}

void tFlipFlop(Netlist& n, In in, Out out, Params) {
    const NetId d = n.wire();
    const g::Latch ff = g::dFlipFlop(n, d, in[1]);
    n.drive(d, g::xorGate(n, in[0], ff.q));
    out[0] = ff.q;
    out[1] = ff.notQ;
}

void jkFlipFlop(Netlist& n, In in, Out out, Params) {
    const NetId d = n.wire();
    const g::Latch ff = g::dFlipFlop(n, d, in[2]);
    n.drive(d, g::orGate(n, g::andGate(n, in[0], ff.notQ), g::andGate(n, g::notGate(n, in[1]), ff.q)));
    out[0] = ff.q;
    out[1] = ff.notQ;
}

void alu1(Netlist& n, In in, Out out, Params) {
    const AluBit bit = aluBit(n, in[0], in[1], in[2], in[3], in[4]);
    out[0] = bit.result;
    out[1] = bit.carry;
}

void alu4(Netlist& n, In in, Out out, Params) {
    NetId carry = n.constant(0);
    for (std::size_t i = 0; i < 4; ++i) {
        const AluBit bit = aluBit(n, in[i], in[i + 4], carry, in[8], in[9]);
        out[i] = bit.result;
        carry = bit.carry;
    }
    out[4] = carry;
}

void adderN(Netlist& n, In in, Out out, Params) {
    const g::Sum sum = g::rippleAdder(n, in[0], in[1], in[2]);
    out[0] = sum.sum;
    out[1] = sum.carry;
}

void subtractorN(Netlist& n, In in, Out out, Params) {
    const g::Sum difference = subtract(n, in[0], in[1], in[2]);
    out[0] = difference.sum;
    out[1] = difference.carry;
}

void comparatorN(Netlist& n, In in, Out out, Params) {
    const NetId below = subtract(n, in[0], in[1], n.constant(0)).carry;
    const NetId equal = allOnes(n, g::xnorGate(n, in[0], in[1]));
    out[0] = g::norGate(n, below, equal);
    out[1] = equal;
    out[2] = below;
}

void mux2N(Netlist& n, In in, Out out, Params) { out[0] = g::muxBus(n, in[0], in[1], in[2]); }

void mux4N(Netlist& n, In in, Out out, Params) {
    out[0] = mux4Bus(n, in[0], in[1], in[2], in[3], n.slice(in[4], 0, 1), n.slice(in[4], 1, 1));
}

// Op: 0 AND, 1 OR, 2 XOR, 3 ADD, 4 SUB, 5 NOT A, 6 shift A left, 7 shift A right.
void aluN(Netlist& n, In in, Out out, Params params) {
    const NetId a = in[0];
    const NetId b = in[1];
    const NetId op0 = n.slice(in[2], 0, 1);
    const NetId op1 = n.slice(in[2], 1, 1);
    const NetId op2 = n.slice(in[2], 2, 1);
    const unsigned width = params.width;

    const NetId zeroBit = n.constant(0);
    const g::Sum add = g::rippleAdder(n, a, b, zeroBit);
    const g::Sum sub = subtract(n, a, b, zeroBit);

    NetId left = n.constant(0, width);
    NetId right = n.constant(0, width);
    if (width > 1) {
        const std::array<NetId, 2> shiftedLeft{zeroBit, n.slice(a, 0, width - 1)};
        left = n.concat(shiftedLeft);
        const std::array<NetId, 2> shiftedRight{n.slice(a, 1, width - 1), zeroBit};
        right = n.concat(shiftedRight);
    }

    const NetId logic = mux4Bus(n, g::andGate(n, a, b), g::orGate(n, a, b), g::xorGate(n, a, b), add.sum, op0, op1);
    const NetId other = mux4Bus(n, sub.sum, g::notGate(n, a), left, right, op0, op1);
    const NetId result = g::muxBus(n, logic, other, op2);
    out[0] = result;

    const NetId adding = g::andGate(n, g::andGate(n, op0, op1), g::notGate(n, op2));
    const NetId subtracting = g::andGate(n, g::norGate(n, op0, op1), op2);
    out[1] = g::orGate(n, g::andGate(n, adding, add.carry), g::andGate(n, subtracting, sub.carry));
    out[2] = allOnes(n, g::notGate(n, result));
}

void registerN(Netlist& n, In in, Out out, Params params) {
    const NetId next = n.wire(params.width);
    const NetId q = n.dff(next, in[3]);
    const NetId held = g::muxBus(n, q, in[0], in[1]);
    n.drive(next, g::muxBus(n, held, n.constant(0, params.width), in[2]));
    out[0] = q;
}

void counterN(Netlist& n, In in, Out out, Params params) {
    const NetId next = n.wire(params.width);
    const NetId q = n.dff(next, in[2]);
    const g::Sum plusOne = g::rippleAdder(n, q, n.constant(0, params.width), in[0]);
    n.drive(next, g::muxBus(n, plusOne.sum, n.constant(0, params.width), in[1]));
    out[0] = q;
    out[1] = g::andGate(n, plusOne.carry, in[0]);
}

void shiftRegisterN(Netlist& n, In in, Out out, Params params) {
    const NetId next = n.wire(params.width);
    const NetId q = n.dff(next, in[2]);
    NetId moved = in[0];
    if (params.width > 1) {
        const std::array<NetId, 2> shifted{in[0], n.slice(q, 0, params.width - 1)};
        moved = n.concat(shifted);
    }
    n.drive(next, g::muxBus(n, q, moved, in[1]));
    out[0] = q;
}

void ram(Netlist& n, In in, Out out, Params params) {
    const std::uint32_t memory = n.memory(params.address, params.width);
    n.memoryWrite(memory, in[0], in[1], in[2], in[3]);
    out[0] = n.memoryRead(memory, in[0]);
}

// Two registers read at once and one written on the clock, with register 0 always reading 0.
void registerFile(Netlist& n, In in, Out out, Params params) {
    const std::uint32_t memory = n.memory(params.address, params.width);
    NetId notZero = n.slice(in[2], 0, 1);
    for (unsigned i = 1; i < params.address; ++i) notZero = g::orGate(n, notZero, n.slice(in[2], i, 1));
    n.memoryWrite(memory, in[2], in[3], g::andGate(n, in[4], notZero), in[5]);
    out[0] = n.memoryRead(memory, in[0]);
    out[1] = n.memoryRead(memory, in[1]);
}

// A short bus into a long one. With Sinal on, the top bit is repeated, which keeps a negative number
// negative; off, the new bits are zero. A long bus into a short one just loses its top.
void extender(Netlist& n, In in, Out out, Params params) {
    const unsigned bits = params.address;
    if (bits >= params.width) {
        out[0] = n.slice(in[0], 0, params.width);
        return;
    }
    const NetId top = g::andGate(n, n.slice(in[0], bits - 1, 1), in[1]);
    const std::array<NetId, 2> parts{in[0], g::fan(n, top, params.width - bits)};
    out[0] = n.concat(parts);
}

// Moves a bus by N places, one stage for each bit of N: by 1, by 2, by 4 and so on. Dir 0 is left, towards
// the high bits, and whatever comes in is zero.
void shifter(Netlist& n, In in, Out out, Params params) {
    const unsigned width = params.width;
    NetId value = in[0];
    for (unsigned stage = 0; stage < params.address; ++stage) {
        const std::uint64_t amount = std::uint64_t{1} << stage;
        NetId moved = n.constant(0, width);
        if (amount < width) {
            const auto kept = width - static_cast<unsigned>(amount);
            const NetId zeros = n.constant(0, static_cast<unsigned>(amount));
            const std::array<NetId, 2> left{zeros, n.slice(value, 0, kept)};
            const std::array<NetId, 2> right{n.slice(value, static_cast<unsigned>(amount), kept), zeros};
            moved = g::muxBus(n, n.concat(left), n.concat(right), in[2]);
        }
        value = g::muxBus(n, value, moved, n.slice(in[1], stage, 1));
    }
    out[0] = value;
}

void rom(Netlist& n, In in, Out out, Params params) {
    std::vector<std::uint64_t> contents;
    if (params.contents != nullptr) contents = *params.contents;
    const std::uint32_t memory = n.memory(params.address, params.width, std::move(contents));
    out[0] = n.memoryRead(memory, in[0]);
}

const std::vector<BoxSpec>& allBoxes() {
    static const std::vector<BoxSpec> boxes{
        {"half_adder", "Meio somador", "Aritmética", {{"A"}, {"B"}}, {{"S"}, {"C"}}, halfAdder},
        {"full_adder", "Somador", "Aritmética", {{"A"}, {"B"}, {"Ce"}}, {{"S"}, {"Cs"}}, fullAdder},
        {"adder_n", "Somador N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}, {"Ce"}}, {{"S", kPinWidth}, {"Cs"}}, adderN, 8},
        {"sub_n", "Subtrator N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}, {"Ve"}}, {{"D", kPinWidth}, {"Vs"}}, subtractorN, 8},
        {"cmp_n", "Comparador N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}}, {{"A>B"}, {"A=B"}, {"A<B"}}, comparatorN, 8},
        {"adder4", "Somador 4 bits", "Aritmética",
         {{"A0"}, {"A1"}, {"A2"}, {"A3"}, {"B0"}, {"B1"}, {"B2"}, {"B3"}, {"Ce"}}, {{"S0"}, {"S1"}, {"S2"}, {"S3"}, {"Cs"}}, adder4},
        {"full_subtractor", "Subtrator", "Aritmética", {{"A"}, {"B"}, {"Ve"}}, {{"D"}, {"Vs"}}, fullSubtractor},
        {"comparator", "Comparador", "Aritmética", {{"A"}, {"B"}}, {{"A>B"}, {"A=B"}, {"A<B"}}, comparator},
        {"alu_n", "ULA N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}, {"Op", 3}}, {{"R", kPinWidth}, {"Cs"}, {"Z"}}, aluN, 8},
        {"alu1", "ULA 1 bit", "Aritmética", {{"A"}, {"B"}, {"Ce"}, {"Op0"}, {"Op1"}}, {{"R"}, {"Cs"}}, alu1},
        {"alu4", "ULA 4 bits", "Aritmética",
         {{"A0"}, {"A1"}, {"A2"}, {"A3"}, {"B0"}, {"B1"}, {"B2"}, {"B3"}, {"Op0"}, {"Op1"}}, {{"R0"}, {"R1"}, {"R2"}, {"R3"}, {"Cs"}}, alu4},
        {"mux2_n", "MUX 2:1 N bits", "Seleção", {{"A", kPinWidth}, {"B", kPinWidth}, {"Sel"}}, {{"S", kPinWidth}}, mux2N, 8},
        {"mux4_n", "MUX 4:1 N bits", "Seleção",
         {{"D0", kPinWidth}, {"D1", kPinWidth}, {"D2", kPinWidth}, {"D3", kPinWidth}, {"Sel", 2}}, {{"Y", kPinWidth}}, mux4N, 8},
        {"mux2", "MUX 2:1", "Seleção", {{"A"}, {"B"}, {"Sel"}}, {{"S"}}, mux2},
        {"mux4", "MUX 4:1", "Seleção", {{"D0"}, {"D1"}, {"D2"}, {"D3"}, {"S0"}, {"S1"}}, {{"Y"}}, mux4Box},
        {"demux2", "DEMUX 1:2", "Seleção", {{"E"}, {"Sel"}}, {{"Y0"}, {"Y1"}}, demux2},
        {"decoder2", "Decodificador 2:4", "Seleção", {{"A0"}, {"A1"}}, {{"Y0"}, {"Y1"}, {"Y2"}, {"Y3"}}, decoder2},
        {"encoder4", "Codificador 4:2", "Seleção", {{"D0"}, {"D1"}, {"D2"}, {"D3"}}, {{"A0"}, {"A1"}}, encoder4},
        {"register", "Registrador", "Memória", {{"D", kPinWidth}, {"En"}, {"Rst"}, {"Clk"}}, {{"Q", kPinWidth}}, registerN, 8},
        {"counter", "Contador", "Memória", {{"En"}, {"Rst"}, {"Clk"}}, {{"Q", kPinWidth}, {"Max"}}, counterN, 8},
        {"shift_register", "Reg. deslocamento", "Memória", {{"D"}, {"En"}, {"Clk"}}, {{"Q", kPinWidth}}, shiftRegisterN, 8},
        {"ram", "RAM", "Memória", {{"A", kPinAddress}, {"D", kPinWidth}, {"We"}, {"Clk"}}, {{"Q", kPinWidth}}, ram, 8, 8},
        {"rom", "ROM", "Memória", {{"A", kPinAddress}}, {{"Q", kPinWidth}}, rom, 8, 8, true},
        {"regfile", "Banco de registradores", "Memória",
         {{"Ra", kPinAddress}, {"Rb", kPinAddress}, {"Rw", kPinAddress}, {"D", kPinWidth}, {"We"}, {"Clk"}},
         {{"A", kPinWidth}, {"B", kPinWidth}}, registerFile, 16, 3},
        {"extender", "Extensor de bits", "Seleção", {{"A", kPinAddress}, {"Sinal"}}, {{"Q", kPinWidth}}, extender, 16, 8, false, "Entrada"},
        {"shifter", "Deslocador", "Seleção", {{"A", kPinWidth}, {"N", kPinAddress}, {"Dir"}}, {{"Q", kPinWidth}}, shifter, 8, 3, false, "Casas"},
        {"sr_latch", "Latch SR", "Memória", {{"S"}, {"R"}}, {{"Q"}, {"!Q"}}, srLatch},
        {"d_latch", "Latch D", "Memória", {{"D"}, {"En"}}, {{"Q"}, {"!Q"}}, dLatch},
        {"d_flip_flop", "Flip-flop D", "Memória", {{"D"}, {"Clk"}}, {{"Q"}, {"!Q"}}, dFlipFlop},
        {"t_flip_flop", "Flip-flop T", "Memória", {{"T"}, {"Clk"}}, {{"Q"}, {"!Q"}}, tFlipFlop},
        {"jk_flip_flop", "Flip-flop JK", "Memória", {{"J"}, {"K"}, {"Clk"}}, {{"Q"}, {"!Q"}}, jkFlipFlop},
        // Phase two.
        {"buffer", "Buffer", "Portas", {{"A"}}, {{"S"}}, buffer},
        {"parity_odd", "Paridade ímpar", "Portas", {{"D", kPinWidth}}, {{"S"}}, parityOdd, 4},
        {"parity_even", "Paridade par", "Portas", {{"D", kPinWidth}}, {{"S"}}, parityEven, 4},
        {"bit_select", "Seletor de bits", "Seleção", {{"D", kPinWidth}, {"Sel", kPinAddress}}, {{"S"}}, bitSelect, 8, 3, false, "Seleção"},
        {"priority4", "Codificador de prioridade", "Seleção", {{"D0"}, {"D1"}, {"D2"}, {"D3"}}, {{"A0"}, {"A1"}, {"V"}}, priority4},
        {"multiplier", "Multiplicador N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}}, {{"Baixo", kPinWidth}, {"Alto", kPinWidth}}, multiplier, 4},
        {"divider", "Divisor N bits", "Aritmética", {{"A", kPinWidth}, {"B", kPinWidth}}, {{"Q", kPinWidth}, {"R", kPinWidth}}, divider, 4},
        {"negator", "Negador N bits", "Aritmética", {{"A", kPinWidth}}, {{"N", kPinWidth}}, negator, 4},
        {"bit_count", "Contador de bits", "Aritmética", {{"D", kPinWidth}}, {{"C", 7}}, bitCount, 8},
        {"bit_find", "Localizador de bit", "Aritmética", {{"D", kPinWidth}}, {{"I", 6}, {"V"}}, bitFind, 8},
        {"decoder7", "Para 7 segmentos", "Seleção", {{"N", 4}}, {{"a"}, {"b"}, {"c"}, {"d"}, {"e"}, {"f"}, {"g"}}, decoder7},
        {"random", "Gerador aleatório", "Memória", {{"Clk"}, {"Rst"}}, {{"Q", 8}}, randomBox},
        {"led_matrix", "Matriz de LEDs", "Entrada e saída",
         {{"L0", kPinWidth}, {"L1", kPinWidth}, {"L2", kPinWidth}, {"L3", kPinWidth}, {"L4", kPinWidth}, {"L5", kPinWidth}, {"L6", kPinWidth}, {"L7", kPinWidth}},
         {}, ledMatrix, 8},
        {"terminal", "Terminal", "Entrada e saída", {{"Char", 7}, {"Write"}, {"Clk"}, {"Clear"}}, {{"N", 6}}, terminal},
    };
    return boxes;
}

std::vector<PinDef> resolvePins(const std::vector<BoxPin>& pins, unsigned width, unsigned address) {
    std::vector<PinDef> out;
    out.reserve(pins.size());
    for (const BoxPin& pin : pins) {
        const unsigned bits = pin.bits == kPinWidth ? width : pin.bits == kPinAddress ? address : pin.bits;
        out.push_back({pin.name, bits});
    }
    return out;
}

}  // namespace

std::size_t boxCount() { return allBoxes().size(); }

const BoxSpec& boxSpec(BoxId id) { return allBoxes()[static_cast<std::size_t>(id) - 1]; }

BoxId boxFromKey(std::string_view key) {
    const std::vector<BoxSpec>& boxes = allBoxes();
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (key == boxes[i].key) return static_cast<BoxId>(i + 1);
    }
    return kNoBox;
}

InterfacePtr boxInterface(BoxId id, unsigned width, unsigned address) {
    static std::map<std::tuple<BoxId, unsigned, unsigned>, InterfacePtr> cache;
    const auto key = std::make_tuple(id, width, address);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    const BoxSpec& spec = boxSpec(id);
    auto made = std::make_shared<PartInterface>();
    made->name = spec.name;
    made->inputs = resolvePins(spec.inputs, width, address);
    made->outputs = resolvePins(spec.outputs, width, address);
    cache[key] = made;
    return made;
}

}  // namespace bancada::editor
