// A flat circuit: what the editor's boards become before they are simulated.
//
// Every net is the output of one node. NAND is the only logic primitive on purpose: everything else
// is built from it, the way the player builds it. Dff stands for the memory parts, and it is the one
// node whose output does not depend on its inputs within a settle, which is what breaks clocked loops.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bancada::sim {

using NetId = std::uint32_t;

inline constexpr unsigned kMaxWidth = 64;

constexpr std::uint64_t widthMask(unsigned width) {
    return width >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
}

enum class Op : std::uint8_t {
    Input,   // set from outside
    Const,   // fixed value
    Wire,    // same value as args[0]; left undriven until drive() is called, for feedback
    Nand,    // ~(args[0] & args[1]), bit by bit
    Slice,   // args[0] >> lo, width bits
    Concat,  // args joined, args[0] in the low bits
    Dff,     // on a rising edge of args[1], stores args[0]
    MemRead, // the word of memory number `value` at address args[0]: changes as soon as the address does
};

inline constexpr unsigned kMaxAddressBits = 24;

// Words of memory, read at once and written on a clock edge. Like Dff, it stands for a part the player
// could build from gates, and stays a single node because a real memory has millions of bits.
struct Memory {
    unsigned addressBits = 1;
    unsigned dataBits = 8;
    std::vector<std::uint64_t> initial;  // the first words after reset; the rest are 0
};

// On a rising edge of clock, if enable is 1, stores data at address.
struct MemoryPort {
    std::uint32_t memory = 0;
    NetId address = 0;
    NetId data = 0;
    NetId enable = 0;
    NetId clock = 0;
};

// Where each node came from, for drawing a circuit's insides: the chips and boxes it's in, and the gate it
// belongs to. Kept only when asked for: the simulator never needs them, and then they cost nothing.
enum class TagKind : std::uint8_t {
    Chip,    // a chip on a board
    Box,     // a ready-made box on a board
    Detail,  // a known piece inside a box, like a full adder
    Gate,    // one logic gate: its NANDs, together
};
inline constexpr std::uint32_t kNoTag = 0xFFFFFFFFu;

struct Tag {
    TagKind kind = TagKind::Chip;
    std::uint32_t parent = kNoTag;
    std::string label;  // a gate's kind ("AND", "XOR"...), or the name of a chip or box
    int x = 0, y = 0;   // where the part that made it sits on its board: the order of the board, kept
};

struct Node {
    Op op = Op::Wire;
    std::uint8_t width = 1;
    std::uint8_t lo = 0;
    std::uint64_t value = 0;  // Const: the value. Dff: the value after reset.
    std::vector<NetId> args;
    std::string name;
};

// Throws std::invalid_argument for mistakes the editor must never let through (a bad net id or
// mismatched widths). Things a half-built circuit really can be, like an undriven wire or two
// drivers on one wire, are not errors here: compile() reports them as diagnostics.
class Netlist {
public:
    NetId input(std::string name, unsigned width = 1);
    NetId constant(std::uint64_t value, unsigned width = 1);
    NetId wire(unsigned width = 1, std::string name = {});
    void drive(NetId wire, NetId source);

    NetId nand(NetId a, NetId b);
    NetId slice(NetId a, unsigned lo, unsigned width);
    NetId concat(std::span<const NetId> lowBitsFirst);
    NetId dff(NetId d, NetId clock, std::uint64_t resetValue = 0);

    std::uint32_t memory(unsigned addressBits, unsigned dataBits, std::vector<std::uint64_t> initial = {});
    NetId memoryRead(std::uint32_t memory, NetId address);
    void memoryWrite(std::uint32_t memory, NetId address, NetId data, NetId enable, NetId clock);

    void name(NetId net, std::string name);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Memory>& memories() const { return memories_; }
    const std::vector<MemoryPort>& memoryPorts() const { return ports_; }
    unsigned width(NetId net) const;

    // How many NAND nodes, the number that decides simulation speed.
    std::size_t nandCount() const;

    // Tags: off until keepTags(). openTag() makes a new tag, under the current one, the current one, and
    // returns the one closeTag() goes back to. A gate inside a gate gets no tag of its own: the outer gate
    // is the one a person sees, its NANDs all together.
    void keepTags() { tagging_ = true; }
    bool tagging() const { return tagging_; }
    std::uint32_t openTag(TagKind kind, std::string_view label, int x = 0, int y = 0);
    void closeTag(std::uint32_t previous) { current_ = previous; }
    const std::vector<Tag>& tags() const { return tags_; }
    std::uint32_t tagOf(NetId net) const { return net < nodeTags_.size() ? nodeTags_[net] : kNoTag; }
    std::uint32_t memoryTag(std::uint32_t memory) const { return memory < memoryTags_.size() ? memoryTags_[memory] : kNoTag; }

private:
    NetId add(Node node);
    void check(NetId net) const;

    std::vector<Node> nodes_;
    std::vector<Memory> memories_;
    std::vector<MemoryPort> ports_;
    bool tagging_ = false;
    std::vector<Tag> tags_;
    std::vector<std::uint32_t> nodeTags_;    // each node's innermost tag, while tagging
    std::vector<std::uint32_t> memoryTags_;  // the tag each memory was made in
    std::uint32_t current_ = kNoTag;
};

// A tag open for as long as this lives.
class TagScope {
public:
    TagScope(Netlist& n, TagKind kind, std::string_view label, int x = 0, int y = 0) : n_(n), previous_(n.openTag(kind, label, x, y)) {}
    ~TagScope() { n_.closeTag(previous_); }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;

private:
    Netlist& n_;
    std::uint32_t previous_;
};

}  // namespace bancada::sim
