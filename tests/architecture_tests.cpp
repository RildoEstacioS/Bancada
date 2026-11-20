// The "por dentro" plan: every NAND the simulator runs is drawn, nothing is drawn on top of anything, and
// every wire goes from the pin that drives it to every pin that reads it, touching no other wire.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <unordered_map>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/architecture.hpp"
#include "editor/computer16.hpp"
#include "editor/computer32.hpp"
#include "sim/gates.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

// How many NANDs the simulator really has, with each flip-flop counted as the NANDs it's drawn with.
std::size_t simulatorNands(const Board& board, const ChipBoards& chips) {
    const sim::Netlist n = BoardCircuit::tagged(board, &chips);
    sim::Netlist ff;
    sim::gates::dFlipFlop(ff, ff.input("d"), ff.input("c"));
    std::size_t total = 0;
    for (const sim::Node& node : n.nodes()) {
        if (node.op == sim::Op::Nand) total += node.width;
        if (node.op == sim::Op::Dff) total += node.width * ff.nandCount();
    }
    return total;
}

constexpr float kE = 0.001f;

bool within(const ArchRect& inner, const ArchRect& outer) {
    return inner.x >= outer.x - kE && inner.y >= outer.y - kE && inner.x + inner.w <= outer.x + outer.w + kE && inner.y + inner.h <= outer.y + outer.h + kE;
}

bool overlap(const ArchRect& a, const ArchRect& b) {
    return a.x + a.w > b.x + kE && b.x + b.w > a.x + kE && a.y + a.h > b.y + kE && b.y + b.h > a.y + kE;
}

bool vertical(const ArchSegment& s) { return std::abs(s.x0 - s.x1) < kE; }

// A point on a segment, ends included.
bool on(const ArchSegment& s, float x, float y) {
    const float x0 = std::min(s.x0, s.x1), x1 = std::max(s.x0, s.x1);
    const float y0 = std::min(s.y0, s.y1), y1 = std::max(s.y0, s.y1);
    return x >= x0 - kE && x <= x1 + kE && y >= y0 - kE && y <= y1 + kE;
}

long long bucket(float v) { return std::llround(static_cast<double>(v) * 1000.0); }

struct Wires {
    std::size_t unreached = 0;  // pins their wire doesn't reach
    std::size_t shorts = 0;     // wires of two signals that run on each other
    std::size_t touches = 0;    // a wire's end resting on another signal's wire
};

Wires checkWires(const Architecture& plan) {
    Wires result;
    const std::vector<ArchSegment>& segs = plan.segments;
    // Each signal's own pieces: joined where an end of one lies on another.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> bySignal;
    for (std::size_t i = 0; i < segs.size(); ++i) bySignal[segs[i].signal].push_back(i);
    std::vector<std::size_t> parent(segs.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto find = [&](std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (const auto& [signal, list] : bySignal) {
        for (std::size_t a = 0; a < list.size(); ++a) {
            for (std::size_t b = a + 1; b < list.size(); ++b) {
                const ArchSegment& s = segs[list[a]];
                const ArchSegment& t = segs[list[b]];
                if (on(s, t.x0, t.y0) || on(s, t.x1, t.y1) || on(t, s.x0, s.y0) || on(t, s.x1, s.y1)) parent[find(list[a])] = find(list[b]);
            }
        }
    }
    // Every pin of a signal with a driver on the plan must sit on the same piece of wire as the driver.
    std::unordered_map<std::uint32_t, std::vector<const ArchPin*>> pins;
    for (const ArchPin& pin : plan.pins) pins[pin.signal].push_back(&pin);
    for (const auto& [signal, list] : pins) {
        const bool driven = std::any_of(list.begin(), list.end(), [](const ArchPin* p) { return p->drives; });
        if (!driven || list.size() < 2) continue;
        const auto& mine = bySignal[signal];
        std::size_t piece = SIZE_MAX;
        for (const ArchPin* pin : list) {
            std::size_t found = SIZE_MAX;
            for (const std::size_t i : mine) {
                if (on(segs[i], pin->x, pin->y)) {
                    found = find(i);
                    break;
                }
            }
            if (found == SIZE_MAX || (piece != SIZE_MAX && found != piece)) {
                ++result.unreached;
                if (result.unreached <= 5) std::printf("  signal %u: pin at %.2f, %.2f not reached\n", signal, static_cast<double>(pin->x), static_cast<double>(pin->y));
                continue;
            }
            piece = found;
        }
    }
    // Two signals never share a stretch, and no wire ends on another signal's wire.
    std::unordered_map<long long, std::vector<std::size_t>> columns, rows;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (vertical(segs[i])) columns[bucket(segs[i].x0)].push_back(i);
        else rows[bucket(segs[i].y0)].push_back(i);
    }
    const auto overlaps = [&](const std::unordered_map<long long, std::vector<std::size_t>>& lines, bool down) {
        for (const auto& [where, list] : lines) {
            std::vector<std::size_t> sorted = list;
            const auto lo = [&](std::size_t i) { return down ? std::min(segs[i].y0, segs[i].y1) : std::min(segs[i].x0, segs[i].x1); };
            const auto hi = [&](std::size_t i) { return down ? std::max(segs[i].y0, segs[i].y1) : std::max(segs[i].x0, segs[i].x1); };
            std::sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) { return lo(a) < lo(b); });
            for (std::size_t a = 0; a < sorted.size(); ++a) {
                for (std::size_t b = a + 1; b < sorted.size() && lo(sorted[b]) < hi(sorted[a]) - kE; ++b) {
                    if (segs[sorted[a]].signal != segs[sorted[b]].signal) ++result.shorts;
                }
            }
        }
    };
    overlaps(columns, true);
    overlaps(rows, false);
    for (const ArchSegment& s : segs) {
        for (const auto& [x, y] : {std::pair{s.x0, s.y0}, std::pair{s.x1, s.y1}}) {
            const auto check = [&](const std::unordered_map<long long, std::vector<std::size_t>>& lines, long long at) {
                const auto found = lines.find(at);
                if (found == lines.end()) return;
                for (const std::size_t i : found->second) {
                    if (segs[i].signal != s.signal && on(segs[i], x, y)) ++result.touches;
                }
            };
            check(columns, bucket(x));
            check(rows, bucket(y));
        }
    }
    return result;
}

std::size_t overlappingCells(const Architecture& plan) {
    std::vector<const ArchCell*> cells;
    for (const ArchCell& cell : plan.cells) cells.push_back(&cell);
    std::sort(cells.begin(), cells.end(), [](const ArchCell* a, const ArchCell* b) { return a->rect.x < b->rect.x; });
    std::size_t found = 0;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        for (std::size_t j = i + 1; j < cells.size() && cells[j]->rect.x < cells[i]->rect.x + cells[i]->rect.w; ++j) {
            if (overlap(cells[i]->rect, cells[j]->rect)) ++found;
        }
    }
    return found;
}

void checkPlan(const Architecture& plan, std::size_t nands) {
    std::printf("  %zu frames, %zu gates, %zu flip-flops, %zu NANDs, %zu transistors, %zu memory bits, %zu segments, %.0f x %.0f\n",
                plan.frames.size(), plan.counts.gates, plan.counts.flipFlops, plan.counts.nands, plan.counts.transistors, plan.counts.memoryBits,
                plan.segments.size(), static_cast<double>(plan.width), static_cast<double>(plan.height));
    CHECK_EQ(plan.counts.nands, nands);
    const ArchRect whole{0.0f, 0.0f, plan.width, plan.height};
    std::size_t outside = 0;
    for (const ArchCell& cell : plan.cells) outside += within(cell.rect, whole) ? 0 : 1;
    for (const ArchFrame& frame : plan.frames) outside += within(frame.rect, whole) ? 0 : 1;
    CHECK_EQ(outside, std::size_t{0});
    CHECK_EQ(overlappingCells(plan), std::size_t{0});
    std::size_t covered = 0;
    for (std::size_t i = 0; i < plan.frames.size(); ++i) {
        for (std::size_t j = i + 1; j < plan.frames.size(); ++j) {
            if (plan.frames[i].depth == plan.frames[j].depth && overlap(plan.frames[i].rect, plan.frames[j].rect)) ++covered;
        }
    }
    CHECK_EQ(covered, std::size_t{0});
    // Each NAND inside its gate.
    std::size_t stray = 0;
    for (const ArchCell& cell : plan.cells) {
        for (std::uint32_t i = 0; i < cell.nands; ++i) stray += within(plan.nands[cell.firstNand + i].rect, cell.rect) ? 0 : 1;
    }
    CHECK_EQ(stray, std::size_t{0});
    const Wires wires = checkWires(plan);
    std::printf("  wires: %zu pins not reached, %zu shorts, %zu touches\n", wires.unreached, wires.shorts, wires.touches);
    CHECK_EQ(wires.unreached, std::size_t{0});
    CHECK_EQ(wires.shorts, std::size_t{0});
    CHECK_EQ(wires.touches, std::size_t{0});
}

}  // namespace

TEST(the_16_bit_plan_draws_every_nand_and_wire) {
    const ComputerProject project = buildComputer16({}, "");
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    checkPlan(architecture16(), simulatorNands(project.board, chips));
}

TEST(the_32_bit_plan_draws_every_nand_and_wire) {
    const Computer32Project project = buildComputer32(mips::Program{}, "");
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    checkPlan(architecture32(), simulatorNands(project.board, chips));
}

// The plan is drawn from the tagged netlist and the circuit runs the plain one: they must be the same nets,
// numbered the same, for the plan to show the running circuit's values.
TEST(the_plan_and_the_running_circuit_number_their_nets_alike) {
    const auto same = [](const Board& board, const ChipBoards& chips) {
        const sim::Netlist tagged = BoardCircuit::tagged(board, &chips);
        const sim::Netlist plain = BoardCircuit::plain(board, &chips);
        if (tagged.nodes().size() != plain.nodes().size()) return false;
        for (std::size_t i = 0; i < plain.nodes().size(); ++i) {
            const sim::Node& a = tagged.nodes()[i];
            const sim::Node& b = plain.nodes()[i];
            if (a.op != b.op || a.width != b.width || a.lo != b.lo || a.value != b.value || a.args != b.args) return false;
        }
        return tagged.memories().size() == plain.memories().size();
    };
    const ComputerProject project16 = buildComputer16({}, "");
    ChipBoards chips16;
    for (const ChipData& chip : project16.chips) chips16[chip.id] = &chip.board;
    CHECK(same(project16.board, chips16));
    const Computer32Project project32 = buildComputer32(mips::Program{}, "");
    ChipBoards chips32;
    for (const ChipData& chip : project32.chips) chips32[chip.id] = &chip.board;
    CHECK(same(project32.board, chips32));
}

// Shown live, every NAND of the plan must read as a NAND: its output is 0 only when both its inputs are 1.
// That holds only if every signal is the right net of the running circuit, flip-flops' insides included.
TEST(the_plan_shows_the_running_circuit) {
    const char* source = "LI R1, 5\nlaço: ADDI R1, R1, -1\nOUT R1\nBNE R1, R0, laço\nHLT\n";
    const ComputerProject project = buildComputer16(assembly::assemble(source).words, source);
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    const Architecture plan = buildArchitecture(project.board, &chips, "teste");
    BoardCircuit circuit(project.board, &chips);
    circuit.reset();
    circuit.settle();
    const auto at = [](float x, float y) { return (static_cast<std::int64_t>(std::lround(x * 100.0f)) << 32) ^ std::lround(y * 100.0f); };
    std::size_t checked = 0, wrong = 0;
    for (int half = 0; half < 14; ++half) {
        circuit.setClocks(half % 2 == 1);
        circuit.settle();
        const std::vector<std::uint8_t> values = archValues(plan, circuit);
        std::unordered_map<std::int64_t, int> pinValue;
        for (const ArchPin& pin : plan.pins) pinValue[at(pin.x, pin.y)] = values[pin.signal];
        for (const ArchTie& tie : plan.ties) pinValue.emplace(at(tie.x, tie.y), tie.value);
        for (const ArchNand& nand : plan.nands) {
            const auto a = pinValue.find(at(nand.rect.x, nand.rect.y + 0.25f));
            const auto b = pinValue.find(at(nand.rect.x, nand.rect.y + 0.75f));
            const auto q = pinValue.find(at(nand.rect.x + nand.rect.w, nand.rect.y + 0.5f));
            if (a == pinValue.end() || b == pinValue.end() || q == pinValue.end()) continue;
            ++checked;
            if (q->second != ((a->second != 0 && b->second != 0) ? 0 : 1)) ++wrong;
        }
    }
    std::printf("  %zu NANDs checked, %zu wrong\n", checked, wrong);
    CHECK(checked > 1000);
    CHECK_EQ(wrong, std::size_t{0});
    // And every part of the board on top has a place in the plan.
    CHECK(plan.places.size() >= 3);
}

TEST(tags_cost_nothing_when_off) {
    sim::Netlist n;
    sim::gates::xorGate(n, n.input("a"), n.input("b"));
    CHECK(n.tags().empty());
    CHECK_EQ(n.tagOf(2), sim::kNoTag);
}

int main() { return check::runAll(); }
