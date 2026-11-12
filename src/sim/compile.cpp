#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim/program.hpp"

namespace bancada::sim {

namespace {

constexpr std::uint32_t kNone = UINT32_MAX;

bool isCombinational(Op op) { return op == Op::Nand || op == Op::Slice || op == Op::Concat || op == Op::MemRead; }

// Follows wires back to the net that actually drives them. Wires with no driver, or joined only to
// each other, end up at themselves: they get a slot of their own and read as 0.
std::vector<NetId> resolveWires(const Netlist& netlist, std::vector<Diagnostic>& diagnostics) {
    const std::vector<Node>& nodes = netlist.nodes();
    std::vector<NetId> root(nodes.size());
    std::vector<std::uint8_t> state(nodes.size(), 0);  // 0 new, 1 on the current path, 2 done
    std::vector<NetId> path;

    for (NetId start = 0; start < nodes.size(); ++start) {
        if (state[start] == 2) continue;
        path.clear();
        NetId at = start;
        NetId end = start;
        for (;;) {
            if (state[at] == 2) {
                end = root[at];
                break;
            }
            if (state[at] == 1) {
                diagnostics.push_back({Diagnostic::Kind::WireLoop, at});
                end = at;
                break;
            }
            const Node& node = nodes[at];
            state[at] = 1;
            path.push_back(at);
            if (node.op != Op::Wire || node.args.empty()) {
                end = at;
                break;
            }
            at = node.args[0];
        }
        for (NetId net : path) {
            root[net] = end;
            state[net] = 2;
        }
    }

    for (NetId net = 0; net < nodes.size(); ++net) {
        const Node& node = nodes[net];
        if (node.op != Op::Wire) continue;
        if (node.args.empty()) diagnostics.push_back({Diagnostic::Kind::Undriven, net});
        if (node.args.size() > 1) diagnostics.push_back({Diagnostic::Kind::MultipleDrivers, net});
    }
    return root;
}

// Groups the logic into components (a gate alone, or gates feeding each other) with Tarjan's
// algorithm, written with its own stack: a long chain of gates must not overflow the call stack.
// `order` holds the components in dependency order, each one's members sorted by net, so the same
// netlist always simulates the same way, races included.
struct Components {
    std::vector<std::vector<NetId>> order;
    std::vector<std::uint8_t> selfLoop;
};

Components findComponents(const std::vector<std::vector<NetId>>& users, const std::vector<std::uint8_t>& isLogic) {
    const std::size_t n = users.size();
    std::vector<std::uint32_t> index(n, kNone), low(n, 0);
    std::vector<std::uint8_t> onStack(n, 0);
    std::vector<NetId> stack;
    struct Frame {
        NetId net;
        std::uint32_t nextUser;
    };
    std::vector<Frame> frames;
    std::uint32_t counter = 0;
    Components out;

    for (NetId start = 0; start < n; ++start) {
        if (!isLogic[start] || index[start] != kNone) continue;
        frames.push_back({start, 0});
        while (!frames.empty()) {
            Frame& frame = frames.back();
            const NetId v = frame.net;
            if (frame.nextUser == 0 && index[v] == kNone) {
                index[v] = low[v] = counter++;
                stack.push_back(v);
                onStack[v] = 1;
            }
            bool descended = false;
            while (frame.nextUser < users[v].size()) {
                const NetId w = users[v][frame.nextUser++];
                if (index[w] == kNone) {
                    frames.push_back({w, 0});
                    descended = true;
                    break;
                }
                if (onStack[w]) low[v] = std::min(low[v], index[w]);
            }
            if (descended) continue;

            if (low[v] == index[v]) {
                std::vector<NetId> members;
                for (;;) {
                    const NetId w = stack.back();
                    stack.pop_back();
                    onStack[w] = 0;
                    members.push_back(w);
                    if (w == v) break;
                }
                std::sort(members.begin(), members.end());
                const std::vector<NetId>& usersOfV = users[v];
                const bool self =
                    members.size() == 1 && std::find(usersOfV.begin(), usersOfV.end(), v) != usersOfV.end();
                out.order.push_back(std::move(members));
                out.selfLoop.push_back(self ? 1 : 0);
            }
            frames.pop_back();
            if (!frames.empty()) {
                const NetId parent = frames.back().net;
                low[parent] = std::min(low[parent], low[v]);
            }
        }
    }

    // Tarjan finishes a component only after everything that depends on it.
    std::reverse(out.order.begin(), out.order.end());
    std::reverse(out.selfLoop.begin(), out.selfLoop.end());
    return out;
}

}  // namespace

Program compile(const Netlist& netlist) {
    const std::vector<Node>& nodes = netlist.nodes();
    Program program;
    const std::vector<NetId> root = resolveWires(netlist, program.diagnostics);

    // One slot for each net that isn't merely a wire onto another.
    std::vector<Slot> slotOfRoot(nodes.size(), kNone);
    Slot nextSlot = 0;
    for (NetId net = 0; net < nodes.size(); ++net) {
        if (root[net] == net) slotOfRoot[net] = nextSlot++;
    }
    program.slotCount = nextSlot;
    program.slotOfNet.resize(nodes.size());
    program.isInput.resize(nodes.size());
    program.netWidth.resize(nodes.size());
    for (NetId net = 0; net < nodes.size(); ++net) {
        program.slotOfNet[net] = slotOfRoot[root[net]];
        program.isInput[net] = nodes[net].op == Op::Input ? 1 : 0;
        program.netWidth[net] = nodes[net].width;
    }

    // Who uses each net, seen through wires, counting only logic.
    std::vector<std::vector<NetId>> users(nodes.size());
    std::vector<std::uint8_t> isLogic(nodes.size(), 0);
    for (NetId net = 0; net < nodes.size(); ++net) {
        const Node& node = nodes[net];
        if (root[net] != net || !isCombinational(node.op)) continue;
        isLogic[net] = 1;
        for (NetId arg : node.args) {
            const NetId from = root[arg];
            if (isCombinational(nodes[from].op)) users[from].push_back(net);
        }
    }

    const Components components = findComponents(users, isLogic);

    auto emit = [&](NetId net) {
        const Node& node = nodes[net];
        Instr instr;
        instr.op = node.op;
        instr.lo = node.lo;
        instr.dst = slotOfRoot[net];
        instr.mask = widthMask(node.width);
        if (node.op == Op::Concat) {
            instr.a = static_cast<Slot>(program.concatParts.size());
            instr.b = static_cast<Slot>(node.args.size());
            unsigned shift = 0;
            for (NetId arg : node.args) {
                program.concatParts.push_back({slotOfRoot[root[arg]], static_cast<std::uint8_t>(shift)});
                shift += nodes[arg].width;
            }
        } else if (node.op == Op::MemRead) {
            instr.a = slotOfRoot[root[node.args[0]]];
            instr.b = static_cast<Slot>(node.value);
        } else {
            instr.a = slotOfRoot[root[node.args[0]]];
            instr.b = node.op == Op::Nand ? slotOfRoot[root[node.args[1]]] : 0;
        }
        program.instrs.push_back(instr);
    };

    program.instrs.reserve(nodes.size());
    for (std::size_t i = 0; i < components.order.size(); ++i) {
        const std::vector<NetId>& members = components.order[i];
        const bool loop = members.size() > 1 || components.selfLoop[i] != 0;
        const auto first = static_cast<std::uint32_t>(program.instrs.size());
        for (NetId net : members) emit(net);
        const auto count = static_cast<std::uint32_t>(members.size());
        if (!loop && !program.blocks.empty() && !program.blocks.back().loop) {
            program.blocks.back().count += count;
        } else {
            program.blocks.push_back({first, count, loop});
        }
    }

    for (NetId net = 0; net < nodes.size(); ++net) {
        const Node& node = nodes[net];
        if (root[net] != net) continue;
        if (node.op == Op::Dff) {
            program.dffs.push_back(
                {slotOfRoot[net], slotOfRoot[root[node.args[0]]], slotOfRoot[root[node.args[1]]], node.value});
        } else if (node.op == Op::Const) {
            program.constants.emplace_back(slotOfRoot[net], node.value);
        }
    }
    program.memories = netlist.memories();
    for (const MemoryPort& port : netlist.memoryPorts()) {
        const auto slot = [&](NetId net) { return slotOfRoot[root[net]]; };
        program.ports.push_back({port.memory, slot(port.address), slot(port.data), slot(port.enable), slot(port.clock)});
    }
    return program;
}

}  // namespace bancada::sim
