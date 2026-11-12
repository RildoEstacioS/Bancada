#include "sim/netlist.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bancada::sim {

namespace {

void checkWidth(unsigned width) {
    if (width == 0 || width > kMaxWidth) throw std::invalid_argument("net width must be 1 to 64 bits");
}

}  // namespace

NetId Netlist::add(Node node) {
    nodes_.push_back(std::move(node));
    return static_cast<NetId>(nodes_.size() - 1);
}

void Netlist::check(NetId net) const {
    if (net >= nodes_.size()) throw std::invalid_argument("unknown net");
}

unsigned Netlist::width(NetId net) const {
    check(net);
    return nodes_[net].width;
}

NetId Netlist::input(std::string name, unsigned width) {
    checkWidth(width);
    Node node;
    node.op = Op::Input;
    node.width = static_cast<std::uint8_t>(width);
    node.name = std::move(name);
    return add(std::move(node));
}

NetId Netlist::constant(std::uint64_t value, unsigned width) {
    checkWidth(width);
    Node node;
    node.op = Op::Const;
    node.width = static_cast<std::uint8_t>(width);
    node.value = value & widthMask(width);
    return add(std::move(node));
}

NetId Netlist::wire(unsigned width, std::string name) {
    checkWidth(width);
    Node node;
    node.op = Op::Wire;
    node.width = static_cast<std::uint8_t>(width);
    node.name = std::move(name);
    return add(std::move(node));
}

void Netlist::drive(NetId wire, NetId source) {
    check(wire);
    check(source);
    Node& node = nodes_[wire];
    if (node.op != Op::Wire) throw std::invalid_argument("only a wire can be driven");
    if (node.width != nodes_[source].width) throw std::invalid_argument("wire and driver have different widths");
    node.args.push_back(source);  // a second driver is reported by compile()
}

NetId Netlist::nand(NetId a, NetId b) {
    check(a);
    check(b);
    if (nodes_[a].width != nodes_[b].width) throw std::invalid_argument("nand inputs have different widths");
    Node node;
    node.op = Op::Nand;
    node.width = nodes_[a].width;
    node.args = {a, b};
    return add(std::move(node));
}

NetId Netlist::slice(NetId a, unsigned lo, unsigned width) {
    check(a);
    checkWidth(width);
    if (lo + width > nodes_[a].width) throw std::invalid_argument("slice goes past the end of its bus");
    Node node;
    node.op = Op::Slice;
    node.width = static_cast<std::uint8_t>(width);
    node.lo = static_cast<std::uint8_t>(lo);
    node.args = {a};
    return add(std::move(node));
}

NetId Netlist::concat(std::span<const NetId> lowBitsFirst) {
    if (lowBitsFirst.empty()) throw std::invalid_argument("concat needs at least one net");
    unsigned total = 0;
    for (NetId net : lowBitsFirst) {
        check(net);
        total += nodes_[net].width;
    }
    checkWidth(total);
    Node node;
    node.op = Op::Concat;
    node.width = static_cast<std::uint8_t>(total);
    node.args.assign(lowBitsFirst.begin(), lowBitsFirst.end());
    return add(std::move(node));
}

NetId Netlist::dff(NetId d, NetId clock, std::uint64_t resetValue) {
    check(d);
    check(clock);
    if (nodes_[clock].width != 1) throw std::invalid_argument("a clock is a single wire");
    Node node;
    node.op = Op::Dff;
    node.width = nodes_[d].width;
    node.value = resetValue & widthMask(nodes_[d].width);
    node.args = {d, clock};
    return add(std::move(node));
}

std::uint32_t Netlist::memory(unsigned addressBits, unsigned dataBits, std::vector<std::uint64_t> initial) {
    if (addressBits == 0 || addressBits > kMaxAddressBits) throw std::invalid_argument("memory address must be 1 to 24 bits");
    checkWidth(dataBits);
    if (initial.size() > (std::size_t{1} << addressBits)) throw std::invalid_argument("more initial words than the memory holds");
    for (std::uint64_t& word : initial) word &= widthMask(dataBits);
    memories_.push_back({addressBits, dataBits, std::move(initial)});
    return static_cast<std::uint32_t>(memories_.size() - 1);
}

NetId Netlist::memoryRead(std::uint32_t memory, NetId address) {
    check(address);
    if (memory >= memories_.size()) throw std::invalid_argument("unknown memory");
    if (nodes_[address].width != memories_[memory].addressBits) throw std::invalid_argument("wrong address width");
    Node node;
    node.op = Op::MemRead;
    node.width = static_cast<std::uint8_t>(memories_[memory].dataBits);
    node.value = memory;
    node.args = {address};
    return add(std::move(node));
}

void Netlist::memoryWrite(std::uint32_t memory, NetId address, NetId data, NetId enable, NetId clock) {
    check(address);
    check(data);
    check(enable);
    check(clock);
    if (memory >= memories_.size()) throw std::invalid_argument("unknown memory");
    const Memory& target = memories_[memory];
    if (nodes_[address].width != target.addressBits || nodes_[data].width != target.dataBits) {
        throw std::invalid_argument("memory port has the wrong widths");
    }
    if (nodes_[enable].width != 1 || nodes_[clock].width != 1) throw std::invalid_argument("enable and clock are single wires");
    ports_.push_back({memory, address, data, enable, clock});
}

void Netlist::name(NetId net, std::string name) {
    check(net);
    nodes_[net].name = std::move(name);
}

std::size_t Netlist::nandCount() const {
    return static_cast<std::size_t>(
        std::count_if(nodes_.begin(), nodes_.end(), [](const Node& n) { return n.op == Op::Nand; }));
}

}  // namespace bancada::sim
