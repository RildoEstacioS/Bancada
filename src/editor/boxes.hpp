// Ready-made circuits: boxes the player drags onto a board instead of building them by hand. Each one is
// built from NANDs when the board is compiled, or from flip-flops and memory words for the parts that
// hold state, so a box and its hand-made twin behave the same in every case.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sim/netlist.hpp"

namespace bancada::editor {

using BoxId = std::uint8_t;
inline constexpr BoxId kNoBox = 0;

// How many bits a pin of a box has: a number, or one of these for what the player chose.
inline constexpr std::uint8_t kPinWidth = 0;
inline constexpr std::uint8_t kPinAddress = 255;

struct BoxPin {
    const char* name;
    std::uint8_t bits = 1;
};

struct BoxParams {
    unsigned width = 1;
    unsigned address = 0;
    const std::vector<std::uint64_t>* contents = nullptr;
};

struct BoxSpec {
    const char* key;    // the name in saved files: never change one
    const char* name;   // the name on screen
    const char* group;  // the palette folder it goes in
    std::vector<BoxPin> inputs;
    std::vector<BoxPin> outputs;
    void (*build)(sim::Netlist& n, std::span<const sim::NetId> in, std::span<sim::NetId> out, const BoxParams& params);
    unsigned defaultWidth = 1;    // 1 means the box isn't sized: its pins are what they are
    unsigned defaultAddress = 0;  // 0 means it has no address pin
    bool hasContents = false;     // a ROM: what's in it is part of the project
    const char* addressName = "Endereço";  // what the second number means on this box
};

// Ids count from 1, and 0 means "not a box". New boxes go at the end of the list.
std::size_t boxCount();
const BoxSpec& boxSpec(BoxId id);
BoxId boxFromKey(std::string_view key);  // kNoBox if there's no such box

// What a part looks like from outside: its pins, with their names and sizes.
struct PinDef {
    std::string name;
    unsigned bits = 1;
    friend bool operator==(const PinDef&, const PinDef&) = default;
};

struct PartInterface {
    std::string name;
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    friend bool operator==(const PartInterface&, const PartInterface&) = default;
};

using InterfacePtr = std::shared_ptr<const PartInterface>;

InterfacePtr boxInterface(BoxId id, unsigned width, unsigned address);

}  // namespace bancada::editor
