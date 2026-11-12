// Everything above NAND, built only from NAND: the same circuits the lessons will have players build.
// All of these work on buses of any width, bit by bit, unless they say otherwise.
#pragma once

#include <array>
#include <vector>

#include "sim/netlist.hpp"

namespace bancada::sim::gates {

inline NetId notGate(Netlist& n, NetId a) { return n.nand(a, a); }
inline NetId andGate(Netlist& n, NetId a, NetId b) { return notGate(n, n.nand(a, b)); }
inline NetId orGate(Netlist& n, NetId a, NetId b) { return n.nand(notGate(n, a), notGate(n, b)); }
inline NetId norGate(Netlist& n, NetId a, NetId b) { return notGate(n, orGate(n, a, b)); }

inline NetId xorGate(Netlist& n, NetId a, NetId b) {
    const NetId shared = n.nand(a, b);
    return n.nand(n.nand(a, shared), n.nand(b, shared));
}

inline NetId xnorGate(Netlist& n, NetId a, NetId b) { return notGate(n, xorGate(n, a, b)); }

// out = a while sel is 0, b while sel is 1. All three are single wires.
inline NetId mux(Netlist& n, NetId a, NetId b, NetId sel) {
    return n.nand(n.nand(a, notGate(n, sel)), n.nand(b, sel));
}

// One wire copied onto every bit of a bus.
inline NetId fan(Netlist& n, NetId bit, unsigned width) {
    if (width == 1) return bit;
    const std::vector<NetId> copies(width, bit);
    return n.concat(copies);
}

// A whole bus through a mux: a while sel is 0, b while it's 1.
inline NetId muxBus(Netlist& n, NetId a, NetId b, NetId sel) {
    const unsigned width = n.width(a);
    const NetId wide = fan(n, sel, width);
    return n.nand(n.nand(a, notGate(n, wide)), n.nand(b, wide));
}

struct Sum {
    NetId sum;
    NetId carry;
};

inline Sum fullAdder(Netlist& n, NetId a, NetId b, NetId carryIn) {
    const NetId partial = xorGate(n, a, b);
    return {xorGate(n, partial, carryIn), n.nand(n.nand(a, b), n.nand(partial, carryIn))};
}

// Adds two buses of the same width, one full adder per bit.
inline Sum rippleAdder(Netlist& n, NetId a, NetId b, NetId carryIn) {
    const unsigned width = n.width(a);
    std::vector<NetId> bits;
    bits.reserve(width);
    NetId carry = carryIn;
    for (unsigned i = 0; i < width; ++i) {
        const Sum bit = fullAdder(n, n.slice(a, i, 1), n.slice(b, i, 1), carry);
        bits.push_back(bit.sum);
        carry = bit.carry;
    }
    return {n.concat(bits), carry};
}

struct Latch {
    NetId q;
    NetId notQ;
};

// Two NANDs feeding each other. notSet and notReset rest at 1; a 0 on either one acts.
inline Latch srLatch(Netlist& n, NetId notSet, NetId notReset) {
    const NetId q = n.wire(1, "q");
    const NetId notQ = n.wire(1, "notQ");
    n.drive(q, n.nand(notSet, notQ));
    n.drive(notQ, n.nand(notReset, q));
    return {q, notQ};
}

// Follows d while enable is 1, holds while it is 0.
inline Latch dLatch(Netlist& n, NetId d, NetId enable) {
    return srLatch(n, n.nand(d, enable), n.nand(notGate(n, d), enable));
}

// Two latches back to back: the first is open while the clock is low, the second while it's high, so
// q changes only on the rising edge.
inline Latch dFlipFlop(Netlist& n, NetId d, NetId clock) {
    const Latch first = dLatch(n, d, notGate(n, clock));
    return dLatch(n, first.q, clock);
}

}  // namespace bancada::sim::gates
