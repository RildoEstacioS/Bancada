#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "editor/board.hpp"
#include "editor/circuit.hpp"
#include "editor/router.hpp"
#include "editor/session.hpp"
#include "editor/truth_table.hpp"
#include "sim/backend.hpp"

using namespace bancada::editor;
namespace sim = bancada::sim;

namespace {

Drop pinDrop(const Board& board, PartId part, std::uint8_t pin) {
    return {Drop::Kind::Pin, pinPosition(*board.part(part), pin), part, pin};
}

// Connects two pins with an elbow path.
WireId link(Board& board, PartId fromPart, std::uint8_t fromPin, PartId toPart, std::uint8_t toPin) {
    const Drop from = pinDrop(board, fromPart, fromPin);
    const Drop to = pinDrop(board, toPart, toPin);
    return *board.connect(from, to, elbowPath(from.at, to.at));
}

bool straightPath(const std::vector<Point>& path) {
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i].x != path[i + 1].x && path[i].y != path[i + 1].y) return false;
    }
    return path.size() >= 2;
}

// Everything that must hold for any board, after any sequence of edits.
bool boardIsSound(const Board& board) {
    bool sound = true;
    auto sortedById = [](const auto& items) {
        return std::is_sorted(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    };
    sound = sound && sortedById(board.parts()) && sortedById(board.joints()) && sortedById(board.wires());

    for (const Wire& wire : board.wires()) {
        auto exists = [&](const Terminal& t) {
            return t.kind == Terminal::Kind::Pin ? board.part(t.id) != nullptr : board.joint(t.id) != nullptr;
        };
        if (!exists(wire.from) || !exists(wire.to)) return false;
        sound = sound && straightPath(wire.path);
        sound = sound && wire.path.front() == board.terminalPosition(wire.from);
        sound = sound && wire.path.back() == board.terminalPosition(wire.to);
    }
    for (const Joint& joint : board.joints()) {
        const Terminal self{Terminal::Kind::Joint, joint.id, 0};
        const bool used = std::any_of(board.wires().begin(), board.wires().end(),
                                      [&](const Wire& w) { return w.from == self || w.to == self; });
        sound = sound && used;
    }
    for (std::size_t i = 0; i < board.parts().size(); ++i) {
        for (std::size_t j = i + 1; j < board.parts().size(); ++j) {
            sound = sound && !boundsOf(board.parts()[i]).overlaps(boundsOf(board.parts()[j]));
        }
    }
    return sound;
}

// Plays the editor through the same events the window sends.
struct Player {
    Session session;

    void move(int x, int y) { session.mouseMove(static_cast<float>(x), static_cast<float>(y)); }
    void moveTo(Point p) { move(p.x, p.y); }

    void click(Point p) {
        moveTo(p);
        session.leftDown();
        session.leftUp();
    }

    void drag(Point from, Point to) {
        moveTo(from);
        session.leftDown();
        moveTo(to);
        session.leftUp();
    }

    PartId place(PartKind kind, Point topLeft) {
        session.beginPlacing(kind);
        const Rect bounds = boundsOf(kind, {0, 0}, 0);
        click(topLeft + Point{bounds.right / 2, bounds.bottom / 2});
        session.key(Key::Escape);
        return session.board().parts().back().id;
    }

    Point pin(PartId part, unsigned index) const { return pinPosition(*session.board().part(part), index); }

    void wire(Point from, Point to) {
        click(from);
        click(to);
    }

    void clickPart(PartId part) {
        const Rect bounds = boundsOf(*session.board().part(part));
        session.mouseMove(static_cast<float>(bounds.left + bounds.right) / 2.0f,
                          static_cast<float>(bounds.top + bounds.bottom) / 2.0f);
        session.leftDown();
        session.leftUp();
    }
};

}  // namespace

// --- parts -----------------------------------------------------------------------------------------

TEST(parts_cannot_overlap_but_can_touch) {
    Board board;
    CHECK(board.addPart(PartKind::Nand, {0, 0}).has_value());
    CHECK(!board.addPart(PartKind::Nand, {3, 3}).has_value());
    CHECK(board.addPart(PartKind::Nand, {4, 0}).has_value());
}

TEST(four_turns_bring_every_pin_back) {
    Board board;
    const PartId id = *board.addPart(PartKind::Nand, {10, 10});
    const Point before = pinPosition(*board.part(id), 2);
    for (int i = 0; i < 4; ++i) CHECK(board.rotatePart(id));
    CHECK(pinPosition(*board.part(id), 2) == before);
}

TEST(pins_sit_on_the_edge_of_a_part_in_every_rotation) {
    for (std::size_t k = 0; k < kPartKindCount; ++k) {
        const auto kind = static_cast<PartKind>(k);
        const BoxId box = kind == PartKind::Box ? BoxId{1} : kNoBox;
        if (kind == PartKind::Chip) continue;
        for (std::uint8_t rotation = 0; rotation < 4; ++rotation) {
            Board board;
            const PartId id = *board.addPart(kind, {5, 5}, rotation, 0, 1, box);
            const Rect bounds = boundsOf(*board.part(id));
            for (unsigned pin = 0; pin < pinCount(*board.part(id)); ++pin) {
                const Point p = pinPosition(*board.part(id), pin);
                CHECK(bounds.touches(p));
                CHECK(p.x == bounds.left || p.x == bounds.right || p.y == bounds.top || p.y == bounds.bottom);
            }
        }
    }
}

TEST(a_gate_has_one_output_and_it_is_its_last_pin) {
    for (std::size_t k = 0; k < kPartKindCount; ++k) {
        const auto kind = static_cast<PartKind>(k);
        if (!isGate(kind)) continue;
        const Part part = makePart(kind);
        const unsigned pins = pinCount(part);
        for (unsigned pin = 0; pin < pins; ++pin) {
            CHECK(pinRole(part, pin) == (pin + 1 == pins ? PinRole::Output : PinRole::Input));
        }
    }
}

TEST(every_gate_part_follows_its_truth_table) {
    struct Case {
        PartKind kind;
        std::array<int, 4> table;  // out for a=0 b=0, a=1 b=0, a=0 b=1, a=1 b=1
    };
    constexpr std::array<Case, 6> kCases{{{PartKind::And, {0, 0, 0, 1}},
                                          {PartKind::Or, {0, 1, 1, 1}},
                                          {PartKind::Xor, {0, 1, 1, 0}},
                                          {PartKind::Nand, {1, 1, 1, 0}},
                                          {PartKind::Nor, {1, 0, 0, 0}},
                                          {PartKind::Xnor, {1, 0, 0, 1}}}};
    for (const Case& test : kCases) {
        Board board;
        const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
        const PartId b = *board.addPart(PartKind::Toggle, {0, 10});
        const PartId gate = *board.addPart(test.kind, {10, 4});
        const PartId lamp = *board.addPart(PartKind::Led, {20, 5});
        link(board, a, 0, gate, 0);
        link(board, b, 0, gate, 1);
        link(board, gate, 2, lamp, 0);
        BoardCircuit circuit(board);
        for (int i = 0; i < 4; ++i) {
            circuit.setToggle(a, (i & 1) != 0);
            circuit.setToggle(b, (i & 2) != 0);
            circuit.settle();
            CHECK_EQ(circuit.outputOn(lamp), test.table[static_cast<std::size_t>(i)]);
        }
    }
}

TEST(the_not_part_inverts) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId gate = *board.addPart(PartKind::Not, {10, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {20, 0});
    link(board, a, 0, gate, 0);
    link(board, gate, 1, lamp, 0);
    BoardCircuit circuit(board);
    circuit.settle();
    CHECK(circuit.outputOn(lamp));
    circuit.setToggle(a, true);
    circuit.settle();
    CHECK(!circuit.outputOn(lamp));
}

// --- wires: the drawing never decides what connects ---------------------------------------------------

TEST(crossing_wires_stay_apart) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {6, -6});
    const PartId lampA = *board.addPart(PartKind::Led, {20, 0});
    const PartId lampB = *board.addPart(PartKind::Led, {6, 10});

    const Drop aOut = pinDrop(board, a, 0);
    const Drop aIn = pinDrop(board, lampA, 0);
    board.connect(aOut, aIn, {aOut.at, aIn.at});
    const Drop bOut = pinDrop(board, b, 0);
    const Drop bIn = pinDrop(board, lampB, 0);
    board.connect(bOut, bIn, {bOut.at, {bOut.at.x, bIn.at.y}, bIn.at});

    BoardCircuit circuit(board);
    circuit.setToggle(a, true);
    circuit.settle();
    CHECK(circuit.outputOn(lampA));
    CHECK(!circuit.outputOn(lampB));
    circuit.setToggle(a, false);
    circuit.setToggle(b, true);
    circuit.settle();
    CHECK(!circuit.outputOn(lampA));
    CHECK(circuit.outputOn(lampB));
}

TEST(wires_running_on_top_of_each_other_stay_apart) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 10});
    const PartId lampA = *board.addPart(PartKind::Led, {20, 0});
    const PartId lampB = *board.addPart(PartKind::Led, {20, 10});
    const Drop aOut = pinDrop(board, a, 0);
    const Drop aIn = pinDrop(board, lampA, 0);
    const Drop bOut = pinDrop(board, b, 0);
    const Drop bIn = pinDrop(board, lampB, 0);
    board.connect(aOut, aIn, {aOut.at, {8, 1}, {8, 11}, {12, 11}, {12, 1}, aIn.at});
    board.connect(bOut, bIn, {bOut.at, {14, 11}, bIn.at});  // shares part of the other wire's run

    BoardCircuit circuit(board);
    circuit.setToggle(a, true);
    circuit.settle();
    CHECK(circuit.outputOn(lampA));
    CHECK(!circuit.outputOn(lampB));
}

TEST(ending_on_a_wire_joins_it) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lampA = *board.addPart(PartKind::Led, {20, 0});
    const PartId lampB = *board.addPart(PartKind::Led, {10, 10});
    const WireId main = link(board, toggle, 0, lampA, 0);

    const Point middle{11, 1};
    const Drop branch{Drop::Kind::Wire, middle, main, 0};
    const Drop lamp = pinDrop(board, lampB, 0);
    CHECK(board.connect(branch, lamp, elbowPath(middle, lamp.at)).has_value());

    CHECK_EQ(board.joints().size(), 1);
    CHECK_EQ(board.wires().size(), 3);
    CHECK(boardIsSound(board));

    BoardCircuit circuit(board);
    circuit.setToggle(toggle, true);
    circuit.settle();
    CHECK(circuit.outputOn(lampA));
    CHECK(circuit.outputOn(lampB));
}

TEST(a_wire_can_join_itself_at_two_points) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {30, 0});
    const WireId main = link(board, toggle, 0, lamp, 0);
    const Drop first{Drop::Kind::Wire, {8, 1}, main, 0};
    const Drop second{Drop::Kind::Wire, {20, 1}, main, 0};
    CHECK(board.connect(first, second, {{8, 1}, {8, 5}, {20, 5}, {20, 1}}).has_value());
    CHECK(boardIsSound(board));
    CHECK_EQ(board.joints().size(), 2);
}

TEST(a_bad_path_is_refused) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {10, 4});
    const Drop from = pinDrop(board, toggle, 0);
    const Drop to = pinDrop(board, lamp, 0);
    CHECK(!board.connect(from, to, {from.at, to.at}).has_value());       // diagonal
    CHECK(!board.connect(from, to, {{0, 0}, to.at}).has_value());        // doesn't start at from
    CHECK(!board.connect(from, from, {from.at, from.at}).has_value());   // to itself
    CHECK(board.wires().empty());
}

TEST(removing_a_branch_makes_the_rest_one_wire_again) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lampA = *board.addPart(PartKind::Led, {20, 0});
    const PartId lampB = *board.addPart(PartKind::Led, {10, 10});
    const WireId main = link(board, toggle, 0, lampA, 0);
    const Drop branchStart{Drop::Kind::Wire, {11, 1}, main, 0};
    const Drop lamp = pinDrop(board, lampB, 0);
    const WireId branch = *board.connect(branchStart, lamp, elbowPath(branchStart.at, lamp.at));

    const std::array gone{branch};
    board.remove({}, gone);
    CHECK_EQ(board.wires().size(), 1);
    CHECK_EQ(board.joints().size(), 0);
    CHECK(boardIsSound(board));
}

TEST(removing_a_part_takes_its_wires) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {20, 0});
    link(board, toggle, 0, lamp, 0);
    const std::array gone{lamp};
    board.remove(gone, {});
    CHECK(board.wires().empty());
    CHECK_EQ(board.parts().size(), 1);
}

TEST(moved_parts_keep_their_wires_straight_and_attached) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {20, 0});
    link(board, toggle, 0, lamp, 0);
    const std::array moving{lamp};
    CHECK(board.moveParts(moving, {3, 7}));
    CHECK(boardIsSound(board));
    CHECK(board.moveParts(moving, {-9, 2}));
    CHECK(boardIsSound(board));
    CHECK(board.rotatePart(lamp));
    CHECK(boardIsSound(board));
}

TEST(moving_both_ends_moves_the_whole_wire) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId lamp = *board.addPart(PartKind::Led, {20, 8});
    const WireId wire = link(board, toggle, 0, lamp, 0);
    const std::vector<Point> before = board.wire(wire)->path;
    const std::array moving{toggle, lamp};
    CHECK(board.moveParts(moving, {5, 5}));
    const std::vector<Point>& after = board.wire(wire)->path;
    CHECK_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) CHECK(after[i] == before[i] + Point{5, 5});
}

TEST(a_move_that_does_not_fit_changes_nothing) {
    Board board;
    const PartId a = *board.addPart(PartKind::Nand, {0, 0});
    board.addPart(PartKind::Nand, {10, 0});
    const std::array moving{a};
    CHECK(!board.moveParts(moving, {8, 0}));
    CHECK(board.part(a)->position == Point{});
}

// --- routing ---------------------------------------------------------------------------------------

TEST(router_goes_straight_on_open_ground) {
    const Board board;
    const std::vector<Point> path = routePath(board, {0, 0}, {20, 0});
    CHECK_EQ(path.size(), 2);
}

TEST(router_goes_around_parts) {
    Board board;
    board.addPart(PartKind::Nand, {8, -2});
    const std::vector<Point> path = routePath(board, {0, 0}, {20, 0});
    CHECK(straightPath(path));
    CHECK(path.front() == Point{0, 0});
    CHECK(path.back() == Point{20, 0});
    const Rect part = boundsOf(board.parts()[0]);
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const int left = std::min(path[i].x, path[i + 1].x);
        const int right = std::max(path[i].x, path[i + 1].x);
        const int top = std::min(path[i].y, path[i + 1].y);
        const int bottom = std::max(path[i].y, path[i + 1].y);
        CHECK(right < part.left || left > part.right || bottom < part.top || top > part.bottom);
    }
}

TEST(router_prefers_not_to_run_along_a_wire) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {-3, -1});
    const PartId b = *board.addPart(PartKind::Led, {20, -1});
    link(board, a, 0, b, 0);
    const std::vector<Point> path = routePath(board, {2, 0}, {18, 0});
    CHECK(path.size() > 2);
}

TEST(router_gives_a_straight_path_for_very_far_points) {
    const Board board;
    const std::vector<Point> path = routePath(board, {0, 0}, {100000, 100000});
    CHECK(straightPath(path));
    CHECK_EQ(path.size(), 3);
}

// --- circuits --------------------------------------------------------------------------------------

TEST(two_outputs_on_one_wire_are_flagged) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 10});
    const WireId wire = link(board, a, 0, b, 0);
    BoardCircuit circuit(board);
    CHECK(circuit.hasConflict());
    CHECK(circuit.wireConflicted(wire));
}

TEST(a_wire_with_no_output_is_floating) {
    Board board;
    const PartId a = *board.addPart(PartKind::Led, {0, 0});
    const PartId b = *board.addPart(PartKind::Led, {10, 0});
    const WireId wire = link(board, a, 0, b, 0);
    const BoardCircuit circuit(board);
    CHECK(circuit.wireFloating(wire));
    CHECK(!circuit.hasConflict());
}

// --- playing the editor ----------------------------------------------------------------------------

// The acceptance test for this stage: an XOR built from NANDs, only with clicks, branching off wires.
TEST(a_player_builds_xor_from_nands) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {2, 4});
    const PartId b = p.place(PartKind::Toggle, {2, 12});
    const PartId n1 = p.place(PartKind::Nand, {10, 8});
    const PartId n2 = p.place(PartKind::Nand, {18, 3});
    const PartId n3 = p.place(PartKind::Nand, {18, 13});
    const PartId n4 = p.place(PartKind::Nand, {27, 8});
    const PartId lamp = p.place(PartKind::Led, {35, 9});
    CHECK_EQ(p.session.board().parts().size(), 7);

    p.wire(p.pin(a, 0), p.pin(n2, 0));
    p.wire(p.pin(b, 0), p.pin(n3, 1));
    p.wire(p.pin(n1, 2), p.pin(n2, 1));
    p.wire(p.pin(n2, 2), p.pin(n4, 0));
    p.wire(p.pin(n3, 2), p.pin(n4, 1));
    p.wire(p.pin(n4, 2), p.pin(lamp, 0));

    // Branches: drag from the middle of a wire to a pin.
    const Board& board = p.session.board();
    auto pointOnWireFrom = [&](Point pin) {
        const Wire* wire = board.wireAt(pin);
        CHECK(wire != nullptr);
        Point first = wire->path[0];
        const Point next = wire->path[1];
        first.x += (next.x > first.x) - (next.x < first.x);
        first.y += (next.y > first.y) - (next.y < first.y);
        return first;
    };
    p.drag(pointOnWireFrom(p.pin(a, 0)), p.pin(n1, 0));
    p.drag(pointOnWireFrom(p.pin(b, 0)), p.pin(n1, 1));
    p.drag(pointOnWireFrom(p.pin(n1, 2)), p.pin(n3, 0));

    CHECK_EQ(p.session.board().joints().size(), 3);
    CHECK(boardIsSound(p.session.board()));
    CHECK(!p.session.circuit().hasConflict());

    bool valueA = false;
    bool valueB = false;
    for (int step = 0; step < 4; ++step) {
        const bool wantA = (step & 1) != 0;
        const bool wantB = (step & 2) != 0;
        if (wantA != valueA) {
            p.clickPart(a);
            valueA = wantA;
        }
        if (wantB != valueB) {
            p.clickPart(b);
            valueB = wantB;
        }
        CHECK_EQ(p.session.toggleOn(a), wantA);
        CHECK_EQ(p.session.toggleOn(b), wantB);
        CHECK_EQ(p.session.circuit().outputOn(lamp), wantA != wantB);
        CHECK(!p.session.circuit().oscillating());
    }
}

TEST(clicks_on_empty_ground_fix_the_wires_path) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {30, 0});
    p.click(p.pin(a, 0));
    p.click({10, 12});  // the wire must pass through here, far from the direct route
    p.click({20, -8});
    p.click(p.pin(lamp, 0));
    CHECK_EQ(p.session.board().wires().size(), 1);
    const Wire& wire = p.session.board().wires()[0];
    CHECK(pathContains(wire.path, {10, 12}));
    CHECK(pathContains(wire.path, {20, -8}));
    CHECK(boardIsSound(p.session.board()));
    CHECK(p.session.mode() == Mode::Idle);
}

TEST(a_clicked_point_survives_a_path_that_turns_back_toward_it) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {20, 30});
    p.click(p.pin(a, 0));
    p.click({10, 12});
    p.click({10, 2});  // straight back up the same line
    p.click(p.pin(lamp, 0));
    const Wire& wire = p.session.board().wires()[0];
    CHECK(pathContains(wire.path, {10, 12}));
    CHECK(pathContains(wire.path, {10, 2}));
    CHECK(boardIsSound(p.session.board()));
}

TEST(a_wire_in_progress_can_be_cancelled_with_nothing_left_behind) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const std::size_t steps = p.session.undoSteps();
    p.click(p.pin(a, 0));
    p.click({10, 10});
    p.session.key(Key::Escape);
    CHECK(p.session.board().wires().empty());
    CHECK(p.session.board().joints().empty());
    CHECK_EQ(p.session.undoSteps(), steps);

    p.click(p.pin(a, 0));
    p.moveTo({8, 8});
    p.session.rightDown();
    CHECK(p.session.mode() == Mode::Idle);
    CHECK(p.session.board().wires().empty());
}

TEST(enter_leaves_a_loose_end_that_can_be_continued) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {30, 0});
    p.click(p.pin(a, 0));
    p.moveTo({12, 6});
    p.session.key(Key::Enter);
    CHECK_EQ(p.session.board().joints().size(), 1);
    p.wire({12, 6}, p.pin(lamp, 0));
    CHECK_EQ(p.session.board().wires().size(), 2);
    p.clickPart(a);
    CHECK(p.session.circuit().outputOn(lamp));
}

TEST(undo_and_redo_walk_every_step) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {20, 0});
    p.wire(p.pin(a, 0), p.pin(lamp, 0));
    CHECK_EQ(p.session.undoSteps(), 3);

    p.session.key(Key::Undo);
    CHECK(p.session.board().wires().empty());
    CHECK_EQ(p.session.board().parts().size(), 2);
    p.session.key(Key::Undo);
    p.session.key(Key::Undo);
    CHECK(p.session.board().parts().empty());
    p.session.key(Key::Undo);  // nothing left: nothing happens
    CHECK_EQ(p.session.redoSteps(), 3);

    p.session.key(Key::Redo);
    p.session.key(Key::Redo);
    p.session.key(Key::Redo);
    CHECK_EQ(p.session.board().wires().size(), 1);
    p.clickPart(a);
    CHECK(p.session.circuit().outputOn(lamp));
}

TEST(a_new_edit_clears_redo) {
    Player p;
    p.place(PartKind::Toggle, {0, 0});
    p.session.key(Key::Undo);
    CHECK_EQ(p.session.redoSteps(), 1);
    p.place(PartKind::Led, {0, 0});
    CHECK_EQ(p.session.redoSteps(), 0);
}

TEST(a_toggle_flip_is_not_an_edit) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const std::size_t steps = p.session.undoSteps();
    p.clickPart(a);
    CHECK(p.session.toggleOn(a));
    CHECK_EQ(p.session.undoSteps(), steps);
}

TEST(toggles_keep_their_state_across_edits) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {20, 0});
    p.clickPart(a);
    p.wire(p.pin(a, 0), p.pin(lamp, 0));
    CHECK(p.session.circuit().outputOn(lamp));
}

TEST(dragging_moves_a_part_as_one_undo_step) {
    Player p;
    const PartId a = p.place(PartKind::Nand, {0, 0});
    const std::size_t steps = p.session.undoSteps();
    p.session.mouseMove(1.5f, 1.5f);
    p.session.leftDown();
    p.session.mouseMove(4.5f, 3.5f);
    p.session.mouseMove(8.5f, 5.5f);
    CHECK(p.session.mode() == Mode::MovingParts);
    p.session.leftUp();
    CHECK_EQ(p.session.undoSteps(), steps + 1);
    CHECK(p.session.board().part(a)->position == Point{7, 4});  // from grid point 2,2 to 9,6
}

TEST(dragging_a_toggle_does_not_flip_it) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    p.session.mouseMove(1.4f, 0.4f);
    p.session.leftDown();
    p.session.mouseMove(6.4f, 4.4f);
    p.session.leftUp();
    CHECK(!p.session.toggleOn(a));
}

TEST(box_selection_takes_what_is_fully_inside) {
    Player p;
    const PartId a = p.place(PartKind::Nand, {0, 0});
    const PartId b = p.place(PartKind::Nand, {10, 0});
    const PartId c = p.place(PartKind::Nand, {30, 0});
    p.wire(p.pin(a, 2), p.pin(b, 0));
    p.drag({-2, -2}, {16, 6});
    const std::vector<PartId>& selected = p.session.selectedParts();
    CHECK_EQ(selected.size(), 2);
    CHECK(std::find(selected.begin(), selected.end(), c) == selected.end());
    CHECK_EQ(p.session.selectedWires().size(), 1);

    p.session.key(Key::Delete);
    CHECK_EQ(p.session.board().parts().size(), 1);
    CHECK(p.session.board().wires().empty());
    p.session.key(Key::Undo);
    CHECK_EQ(p.session.board().parts().size(), 3);
    CHECK_EQ(p.session.board().wires().size(), 1);
}

TEST(clicking_a_wire_selects_it_for_deletion) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId lamp = p.place(PartKind::Led, {20, 0});
    p.wire(p.pin(a, 0), p.pin(lamp, 0));
    p.click({11, 1});
    CHECK_EQ(p.session.selectedWires().size(), 1);
    p.session.key(Key::Delete);
    CHECK(p.session.board().wires().empty());
    CHECK_EQ(p.session.board().parts().size(), 2);
}

TEST(rotate_key_turns_the_selection_and_the_part_being_placed) {
    Player p;
    p.session.beginPlacing(PartKind::Nand);
    p.session.key(Key::Rotate);
    p.click({10, 10});
    p.session.key(Key::Escape);
    const PartId id = p.session.board().parts()[0].id;
    CHECK_EQ(p.session.board().part(id)->rotation, 1);
    p.clickPart(id);
    p.session.key(Key::Rotate);
    CHECK_EQ(p.session.board().part(id)->rotation, 2);
}

TEST(a_part_cannot_be_placed_on_top_of_another) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    p.session.beginPlacing(PartKind::Nand);
    p.click({2, 2});
    CHECK_EQ(p.session.board().parts().size(), 1);
    CHECK(!p.session.placingFits());
}

TEST(a_gate_looping_on_itself_shows_as_oscillating) {
    Player p;
    const PartId toggle = p.place(PartKind::Toggle, {0, 0});
    const PartId nand = p.place(PartKind::Nand, {10, 0});
    p.wire(p.pin(toggle, 0), p.pin(nand, 0));
    p.click(p.pin(nand, 2));
    p.click({16, 8});
    p.click({8, 8});
    p.click(p.pin(nand, 1));
    CHECK(!p.session.circuit().oscillating());
    p.clickPart(toggle);
    CHECK(p.session.circuit().oscillating());
    p.clickPart(toggle);
    CHECK(!p.session.circuit().oscillating());
}

// --- gates of any size -----------------------------------------------------------------------------

TEST(gates_work_with_every_number_of_inputs) {
    struct Case {
        PartKind kind;
        bool (*expected)(unsigned ones, unsigned inputs);
    };
    const std::array<Case, 6> cases{{
        {PartKind::And, [](unsigned ones, unsigned n) { return ones == n; }},
        {PartKind::Nand, [](unsigned ones, unsigned n) { return ones != n; }},
        {PartKind::Or, [](unsigned ones, unsigned) { return ones > 0; }},
        {PartKind::Nor, [](unsigned ones, unsigned) { return ones == 0; }},
        {PartKind::Xor, [](unsigned ones, unsigned) { return (ones & 1) != 0; }},
        {PartKind::Xnor, [](unsigned ones, unsigned) { return (ones & 1) == 0; }},
    }};
    for (const Case& test : cases) {
        for (unsigned inputs = minInputs(test.kind); inputs <= maxInputs(test.kind); ++inputs) {
            Board board;
            const PartId gate = *board.addPart(test.kind, {20, 0}, 0, static_cast<std::uint8_t>(inputs));
            std::vector<PartId> toggles;
            for (unsigned i = 0; i < inputs; ++i) {
                toggles.push_back(*board.addPart(PartKind::Toggle, {0, static_cast<int>(i) * 4}));
                link(board, toggles[i], 0, gate, static_cast<std::uint8_t>(i));
            }
            const PartId led = *board.addPart(PartKind::Led, {40, 0});
            link(board, gate, static_cast<std::uint8_t>(inputs), led, 0);

            BoardCircuit circuit(board);
            for (unsigned mask = 0; mask < (1u << inputs); ++mask) {
                unsigned ones = 0;
                for (unsigned i = 0; i < inputs; ++i) {
                    const bool on = ((mask >> i) & 1) != 0;
                    ones += on ? 1 : 0;
                    circuit.setToggle(toggles[i], on);
                }
                circuit.settle();
                CHECK_EQ(circuit.outputOn(led), test.expected(ones, inputs));
            }
        }
    }
}

TEST(a_gate_grows_taller_with_more_inputs_and_keeps_its_pins_on_the_grid_spacing) {
    Board board;
    const PartId id = *board.addPart(PartKind::And, {0, 0}, 0, 8);
    const Part& part = *board.part(id);
    CHECK_EQ(boundsOf(part).bottom, 16);
    for (unsigned pin = 0; pin + 1 < part.inputs; ++pin) {
        CHECK_EQ(pinPosition(part, pin + 1).y - pinPosition(part, pin).y, 2);
    }
    CHECK(pinPosition(part, 8) == Point{4, 8});
}

TEST(impossible_shapes_are_refused) {
    Board board;
    CHECK(!board.addPart(PartKind::And, {0, 0}, 0, 1).has_value());
    CHECK(!board.addPart(PartKind::And, {0, 0}, 0, 9).has_value());
    CHECK(!board.addPart(PartKind::Not, {0, 0}, 0, 2).has_value());
    CHECK(!board.addPart(PartKind::And, {0, 0}, 0, 2, 0).has_value());
    CHECK(!board.addPart(PartKind::And, {0, 0}, 0, 2, 4).has_value());
    CHECK(!board.addPart(PartKind::Box, {0, 0}).has_value());
    CHECK(!board.addPart(PartKind::And, {0, 0}, 0, 2, 1, 1).has_value());
    CHECK(!board.addPart(PartKind::Box, {0, 0}, 0, 0, 1, 200).has_value());
    CHECK(board.parts().empty());
}

TEST(size_scales_a_part_and_its_pins_together) {
    Board board;
    const PartId small = *board.addPart(PartKind::And, {0, 0}, 0, 2, 1);
    const PartId big = *board.addPart(PartKind::And, {20, 0}, 0, 2, 3);
    const Rect a = boundsOf(*board.part(small));
    const Rect b = boundsOf(*board.part(big));
    CHECK_EQ(b.right - b.left, 3 * (a.right - a.left));
    CHECK_EQ(b.bottom - b.top, 3 * (a.bottom - a.top));
    CHECK(pinPosition(*board.part(big), 2) == Point{20 + 12, 6});
}

TEST(adding_an_input_keeps_every_wire_on_its_pin) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 8});
    const PartId gate = *board.addPart(PartKind::Or, {12, 2});
    const PartId led = *board.addPart(PartKind::Led, {24, 3});
    link(board, a, 0, gate, 0);
    link(board, b, 0, gate, 1);
    link(board, gate, 2, led, 0);

    CHECK(board.reshapePart(gate, 3, 1));
    CHECK(boardIsSound(board));
    CHECK_EQ(board.wires().size(), 3);
    BoardCircuit circuit(board);
    circuit.setToggle(b, true);
    circuit.settle();
    CHECK(circuit.outputOn(led));  // the output wire followed the output pin to its new number
}

TEST(removing_an_input_removes_only_the_wire_on_it) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 8});
    const PartId c = *board.addPart(PartKind::Toggle, {0, 16});
    const PartId gate = *board.addPart(PartKind::Or, {12, 4}, 0, 3);
    const PartId led = *board.addPart(PartKind::Led, {24, 5});
    link(board, a, 0, gate, 0);
    link(board, b, 0, gate, 1);
    link(board, c, 0, gate, 2);
    link(board, gate, 3, led, 0);

    CHECK(board.reshapePart(gate, 2, 1));
    CHECK_EQ(board.wires().size(), 3);
    CHECK(boardIsSound(board));
    BoardCircuit circuit(board);
    circuit.setToggle(c, true);
    circuit.settle();
    CHECK(!circuit.outputOn(led));  // c isn't connected any more
    circuit.setToggle(a, true);
    circuit.settle();
    CHECK(circuit.outputOn(led));
}

TEST(a_shape_that_does_not_fit_changes_nothing) {
    Board board;
    const PartId gate = *board.addPart(PartKind::And, {0, 0});
    board.addPart(PartKind::Nand, {0, 5});
    CHECK(!board.reshapePart(gate, 8, 1));
    CHECK_EQ(board.part(gate)->inputs, 2);
    CHECK(!board.reshapePart(gate, 2, 3));
    CHECK_EQ(board.part(gate)->scale, 1);
    CHECK(!board.reshapePart(gate, 9, 1));
}

TEST(the_panel_sets_inputs_and_size_while_placing_and_on_a_selected_part) {
    Player p;
    p.session.beginPlacing(PartKind::And);
    CHECK(p.session.inspected().has_value());
    p.session.changeInputs(1);
    p.session.changeScale(1);
    p.click({20, 20});
    p.session.key(Key::Escape);
    const PartId id = p.session.board().parts()[0].id;
    CHECK_EQ(p.session.board().part(id)->inputs, 3);
    CHECK_EQ(p.session.board().part(id)->scale, 2);

    p.clickPart(id);
    CHECK_EQ(p.session.inspected()->part.inputs, 3);
    const std::size_t steps = p.session.undoSteps();
    p.session.changeInputs(1);
    p.session.changeScale(-1);
    CHECK_EQ(p.session.board().part(id)->inputs, 4);
    CHECK_EQ(p.session.board().part(id)->scale, 1);
    CHECK_EQ(p.session.undoSteps(), steps + 2);
    p.session.key(Key::Undo);
    CHECK_EQ(p.session.board().part(id)->scale, 2);
}

TEST(a_not_gate_keeps_its_single_input) {
    Player p;
    const PartId id = p.place(PartKind::Not, {0, 0});
    p.clickPart(id);
    const std::size_t steps = p.session.undoSteps();
    p.session.changeInputs(1);
    CHECK_EQ(p.session.board().part(id)->inputs, 1);
    CHECK_EQ(p.session.undoSteps(), steps);
}

// --- truth table -----------------------------------------------------------------------------------

TEST(the_table_of_a_nand_is_its_truth_table) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 8});
    const PartId gate = *board.addPart(PartKind::Nand, {12, 2});
    const PartId led = *board.addPart(PartKind::Led, {24, 3});
    link(board, a, 0, gate, 0);
    link(board, b, 0, gate, 1);
    link(board, gate, 2, led, 0);

    const TruthTable table = computeTruthTable(board);
    CHECK(table.status == TruthTable::Status::Ready);
    CHECK(table.inputNames == std::vector<std::string>({"A", "B"}));
    CHECK(table.outputNames == std::vector<std::string>({"S"}));
    CHECK(table.rows == std::vector<std::string>({"1", "1", "1", "0"}));
}

TEST(the_table_counts_up_with_the_top_switch_as_the_highest_bit) {
    Board board;
    const PartId top = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId bottom = *board.addPart(PartKind::Toggle, {0, 8});
    const PartId led = *board.addPart(PartKind::Led, {20, 0});
    link(board, top, 0, led, 0);
    (void)bottom;
    const TruthTable table = computeTruthTable(board);
    CHECK(table.labels.at(top) == "A");
    CHECK(table.labels.at(bottom) == "B");
    CHECK(table.rows == std::vector<std::string>({"0", "0", "1", "1"}));
}

TEST(names_follow_the_reading_order_of_the_board) {
    Board board;
    const PartId lower = *board.addPart(PartKind::Toggle, {0, 20});
    const PartId upper = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId outLow = *board.addPart(PartKind::Probe, {20, 20});
    const PartId outHigh = *board.addPart(PartKind::Led, {20, 0});
    link(board, upper, 0, outHigh, 0);
    const TruthTable table = computeTruthTable(board);
    CHECK(table.labels.at(upper) == "A");
    CHECK(table.labels.at(lower) == "B");
    CHECK(table.labels.at(outHigh) == "S1");
    CHECK(table.labels.at(outLow) == "S2");
    CHECK_EQ(table.rows[2].size(), 2);
}

TEST(the_table_has_nothing_to_show_without_inputs_or_outputs) {
    Board board;
    CHECK(computeTruthTable(board).status == TruthTable::Status::NothingToShow);
    board.addPart(PartKind::Toggle, {0, 0});
    CHECK(computeTruthTable(board).status == TruthTable::Status::NothingToShow);
}

TEST(too_many_switches_are_reported_instead_of_tried) {
    Board board;
    for (int i = 0; i < 9; ++i) board.addPart(PartKind::Toggle, {0, i * 3});
    board.addPart(PartKind::Led, {20, 0});
    const TruthTable table = computeTruthTable(board);
    CHECK(table.status == TruthTable::Status::TooManyInputs);
    CHECK(table.rows.empty());
    CHECK_EQ(table.inputNames.size(), 9);
}

TEST(a_row_that_never_settles_is_marked_and_memory_is_flagged) {
    Board board;
    const PartId toggle = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId gate = *board.addPart(PartKind::Nand, {10, 0});
    const PartId led = *board.addPart(PartKind::Led, {24, 1});
    link(board, toggle, 0, gate, 0);
    const Drop out{Drop::Kind::Pin, pinPosition(*board.part(gate), 2), gate, 2};
    const Drop in{Drop::Kind::Pin, pinPosition(*board.part(gate), 1), gate, 1};
    board.connect(out, in, {out.at, {16, 2}, {16, 8}, {8, 8}, {8, 3}, in.at});
    link(board, gate, 2, led, 0);
    const TruthTable table = computeTruthTable(board);
    CHECK(table.hasMemory);
    CHECK(table.rows == std::vector<std::string>({"1", "~"}));
}

TEST(the_table_follows_the_board_as_it_changes) {
    Player p;
    CHECK(p.session.truthTable().status == TruthTable::Status::NothingToShow);
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId led = p.place(PartKind::Led, {20, 0});
    CHECK(p.session.truthTable().rows == std::vector<std::string>({"0", "0"}));
    p.wire(p.pin(a, 0), p.pin(led, 0));
    CHECK(p.session.truthTable().rows == std::vector<std::string>({"0", "1"}));
    p.session.key(Key::Undo);
    CHECK(p.session.truthTable().rows == std::vector<std::string>({"0", "0"}));
}

TEST(the_current_row_follows_the_switches) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId b = p.place(PartKind::Toggle, {0, 8});
    p.place(PartKind::Led, {20, 0});
    CHECK_EQ(p.session.currentRow(), 0);
    p.clickPart(b);
    CHECK_EQ(p.session.currentRow(), 1);
    p.clickPart(a);
    CHECK_EQ(p.session.currentRow(), 3);
}

// --- copy and paste --------------------------------------------------------------------------------

namespace {

void pasteAt(Player& p, Point at) {
    p.session.key(Key::Paste);
    p.moveTo(at);
    p.session.leftDown();
    p.session.leftUp();
}

}  // namespace

TEST(copying_brings_the_wires_between_the_parts_and_not_the_ones_leaving) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId gate = p.place(PartKind::Not, {10, -1});
    const PartId led = p.place(PartKind::Led, {24, 0});
    p.wire(p.pin(a, 0), p.pin(gate, 0));
    p.wire(p.pin(gate, 1), p.pin(led, 0));

    p.drag({-2, -3}, {18, 6});  // the switch and the gate, not the LED
    CHECK_EQ(p.session.selectedParts().size(), 2);
    p.session.key(Key::Copy);
    pasteAt(p, {8, 20});

    CHECK_EQ(p.session.board().parts().size(), 5);
    CHECK_EQ(p.session.board().wires().size(), 3);
    CHECK(boardIsSound(p.session.board()));
    CHECK_EQ(p.session.selectedParts().size(), 2);
    CHECK_EQ(p.session.selectedWires().size(), 1);
}

TEST(a_paste_is_a_separate_circuit) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId led = p.place(PartKind::Led, {12, 0});
    p.wire(p.pin(a, 0), p.pin(led, 0));
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    pasteAt(p, {7, 20});

    const std::vector<Part>& parts = p.session.board().parts();
    CHECK_EQ(parts.size(), 4);
    const PartId copiedSwitch = parts[2].kind == PartKind::Toggle ? parts[2].id : parts[3].id;
    const PartId copiedLed = parts[2].kind == PartKind::Led ? parts[2].id : parts[3].id;
    p.clickPart(copiedSwitch);
    CHECK(p.session.circuit().outputOn(copiedLed));
    CHECK(!p.session.circuit().outputOn(led));
}

TEST(pasting_several_times_and_undoing_each) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    const std::size_t steps = p.session.undoSteps();
    pasteAt(p, {10, 2});
    pasteAt(p, {20, 2});
    pasteAt(p, {30, 2});
    CHECK_EQ(p.session.board().parts().size(), 4);
    CHECK_EQ(p.session.undoSteps(), steps + 3);
    p.session.key(Key::Undo);
    p.session.key(Key::Undo);
    CHECK_EQ(p.session.board().parts().size(), 2);
}

TEST(a_paste_that_does_not_fit_waits_for_a_free_spot) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    p.session.key(Key::Paste);
    p.moveTo({2, 2});
    CHECK(!p.session.pasteFits());
    p.session.leftDown();
    p.session.leftUp();
    CHECK(p.session.mode() == Mode::Pasting);
    CHECK_EQ(p.session.board().sameContent(p.session.board()), true);
    p.moveTo({20, 20});
    CHECK(p.session.pasteFits());
    p.session.leftDown();
    p.session.leftUp();
    CHECK(p.session.mode() == Mode::Idle);
    CHECK_EQ(p.session.board().parts().size(), 2);
}

TEST(escape_cancels_a_paste_with_nothing_left_behind) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    const std::size_t steps = p.session.undoSteps();
    p.session.key(Key::Paste);
    p.moveTo({20, 20});
    CHECK_EQ(p.session.board().parts().size(), 2);  // the preview
    p.session.key(Key::Escape);
    CHECK_EQ(p.session.board().parts().size(), 1);
    CHECK_EQ(p.session.undoSteps(), steps);
}

TEST(cut_removes_and_paste_brings_back) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId led = p.place(PartKind::Led, {12, 0});
    p.wire(p.pin(a, 0), p.pin(led, 0));
    p.session.key(Key::SelectAll);
    p.session.key(Key::Cut);
    CHECK(p.session.board().parts().empty());
    CHECK(p.session.board().wires().empty());
    pasteAt(p, {7, 1});
    CHECK_EQ(p.session.board().parts().size(), 2);
    CHECK_EQ(p.session.board().wires().size(), 1);
}

TEST(with_nothing_selected_copy_keeps_what_was_there_and_paste_of_nothing_does_nothing) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    p.session.key(Key::Paste);
    CHECK(p.session.mode() == Mode::Idle);
    p.session.key(Key::Copy);
    CHECK(!p.session.canPaste());
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    p.session.key(Key::Escape);
    p.session.key(Key::Copy);
    CHECK(p.session.canPaste());
}

TEST(a_copy_keeps_switch_positions_shape_and_box) {
    Player p;
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    p.clickPart(a);
    p.session.beginPlacing(PartKind::Or);
    p.session.changeInputs(2);
    p.session.changeScale(1);
    p.click({20, 20});
    p.session.key(Key::Escape);
    p.session.beginPlacing(PartKind::Box, boxFromKey("mux2"));
    p.click({50, 20});
    p.session.key(Key::Escape);

    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    pasteAt(p, {20, 70});
    const std::vector<Part>& parts = p.session.board().parts();
    CHECK_EQ(parts.size(), 6);
    CHECK(p.session.toggleOn(parts[3].id));
    CHECK_EQ(parts[4].inputs, 4);
    CHECK_EQ(parts[4].scale, 2);
    CHECK_EQ(parts[5].box, boxFromKey("mux2"));
}

TEST(a_copy_keeps_joints_between_copied_parts_and_cuts_the_rest) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Led, {20, 0});
    const PartId c = *board.addPart(PartKind::Led, {20, 10});
    const PartId far = *board.addPart(PartKind::Led, {10, 20});
    link(board, a, 0, b, 0);
    const Drop middle{Drop::Kind::Wire, {10, 1}, board.wires()[0].id, 0};
    const Drop toC{Drop::Kind::Pin, pinPosition(*board.part(c), 0), c, 0};
    board.connect(middle, toC, elbowPath(middle.at, toC.at));
    const Drop onBranch = board.dropAt({10, 5});
    const Drop toFar{Drop::Kind::Pin, pinPosition(*board.part(far), 0), far, 0};
    board.connect(onBranch, toFar, elbowPath(onBranch.at, toFar.at));

    const std::array keep{a, b, c};
    const Board copy = board.fragment(keep, {});
    CHECK_EQ(copy.parts().size(), 3);
    CHECK(boardIsSound(copy));
    BoardCircuit circuit(copy);
    circuit.setToggle(a, true);
    circuit.settle();
    CHECK(circuit.outputOn(b));
    CHECK(circuit.outputOn(c));

    const std::array alone{a};
    const Board single = board.fragment(alone, {});
    CHECK_EQ(single.parts().size(), 1);
    CHECK(single.wires().empty());
    CHECK(single.joints().empty());
}

// --- boxes -----------------------------------------------------------------------------------------

namespace {

// Tries every combination of a box's inputs against what it should do. Bit i of the input number is
// input i, and bit j of the result is output j.
template <typename Expected>
void checkBox(const char* key, Expected expected) {
    const BoxId id = boxFromKey(key);
    CHECK(id != kNoBox);
    if (id == kNoBox) return;
    const BoxSpec& spec = boxSpec(id);
    sim::Netlist n;
    std::vector<sim::NetId> in;
    for (std::size_t i = 0; i < spec.inputs.size(); ++i) in.push_back(n.input("in"));
    std::vector<sim::NetId> out(spec.outputs.size());
    spec.build(n, in, out, BoxParams{});
    auto simulator = sim::makeEventSimulator(sim::compile(n));
    for (unsigned mask = 0; mask < (1u << spec.inputs.size()); ++mask) {
        for (std::size_t i = 0; i < in.size(); ++i) simulator->set(in[i], (mask >> i) & 1);
        CHECK(!simulator->settle().oscillating);
        unsigned result = 0;
        for (std::size_t j = 0; j < out.size(); ++j) result |= static_cast<unsigned>(simulator->get(out[j])) << j;
        const unsigned wanted = expected(mask);
        if (result != wanted) std::printf("  %s: input %u gave %u, expected %u\n", key, mask, result, wanted);
        CHECK_EQ(result, wanted);
    }
}

unsigned bit(unsigned value, unsigned index) { return (value >> index) & 1; }

// Drives a clocked box by hand. Inputs are by position, and the outputs are read after each step.
struct BoxBench {
    explicit BoxBench(const char* key) {
        const BoxSpec& spec = boxSpec(boxFromKey(key));
        for (std::size_t i = 0; i < spec.inputs.size(); ++i) in.push_back(net.input("in"));
        out.resize(spec.outputs.size());
        spec.build(net, in, out, BoxParams{});
        simulator = sim::makeEventSimulator(sim::compile(net));
        simulator->settle();
    }
    void set(std::size_t input, unsigned value) {
        simulator->set(in[input], value);
        simulator->settle();
    }
    void pulse(std::size_t clock) {
        set(clock, 1);
        set(clock, 0);
    }
    unsigned q() const { return static_cast<unsigned>(simulator->get(out[0])); }
    unsigned notQ() const { return static_cast<unsigned>(simulator->get(out[1])); }

    sim::Netlist net;
    std::vector<sim::NetId> in;
    std::vector<sim::NetId> out;
    std::unique_ptr<sim::Backend> simulator;
};

}  // namespace

TEST(every_box_has_a_unique_key_and_pins) {
    for (BoxId id = 1; id <= boxCount(); ++id) {
        const BoxSpec& spec = boxSpec(id);
        CHECK_EQ(boxFromKey(spec.key), id);
        CHECK(!spec.inputs.empty());
        CHECK(!spec.outputs.empty() || std::string_view(spec.key) == "led_matrix");  // a screen only shows
        const Part part = makePart(PartKind::Box, {}, 0, 0, 1, id);
        CHECK_EQ(pinCount(part), spec.inputs.size() + spec.outputs.size());
        const Rect bounds = boundsOf(part);
        for (unsigned pin = 0; pin < pinCount(part); ++pin) {
            const Point p = pinPosition(part, pin);
            CHECK(p.x == (pin < spec.inputs.size() ? bounds.left : bounds.right));
            CHECK(p.y > bounds.top);
            CHECK(p.y < bounds.bottom);
        }
    }
    CHECK_EQ(boxFromKey("no such box"), kNoBox);
}

TEST(arithmetic_boxes_add_and_compare) {
    checkBox("half_adder", [](unsigned m) { return bit(m, 0) + bit(m, 1); });
    checkBox("full_adder", [](unsigned m) { return bit(m, 0) + bit(m, 1) + bit(m, 2); });
    checkBox("adder4", [](unsigned m) { return (m & 15) + ((m >> 4) & 15) + bit(m, 8); });
    checkBox("full_subtractor", [](unsigned m) {
        const int result = static_cast<int>(bit(m, 0)) - static_cast<int>(bit(m, 1)) - static_cast<int>(bit(m, 2));
        return static_cast<unsigned>(result & 1) | (result < 0 ? 2u : 0u);
    });
    checkBox("comparator", [](unsigned m) {
        const unsigned a = bit(m, 0);
        const unsigned b = bit(m, 1);
        return (a > b ? 1u : 0u) | (a == b ? 2u : 0u) | (a < b ? 4u : 0u);
    });
}

TEST(selection_boxes_select) {
    checkBox("mux2", [](unsigned m) { return bit(m, bit(m, 2)); });
    checkBox("mux4", [](unsigned m) { return bit(m, (m >> 4) & 3); });
    checkBox("demux2", [](unsigned m) { return bit(m, 0) << bit(m, 1); });
    checkBox("decoder2", [](unsigned m) { return 1u << (m & 3); });
    checkBox("encoder4", [](unsigned m) { return (bit(m, 1) | bit(m, 3)) | ((bit(m, 2) | bit(m, 3)) << 1); });
}

TEST(alu_boxes_do_all_four_operations) {
    const auto compute = [](unsigned a, unsigned b, unsigned carry, unsigned op, unsigned width) {
        const unsigned mask = (1u << width) - 1;
        switch (op) {
            case 0: return a & b;
            case 1: return a | b;
            case 2: return a ^ b;
            default: return ((a + b + carry) & mask) | (((a + b + carry) >> width) << width);
        }
    };
    checkBox("alu1", [&](unsigned m) { return compute(bit(m, 0), bit(m, 1), bit(m, 2), (m >> 3) & 3, 1); });
    checkBox("alu4", [&](unsigned m) { return compute(m & 15, (m >> 4) & 15, 0, (m >> 8) & 3, 4); });
}

TEST(the_sr_latch_box_sets_and_resets_on_ones) {
    BoxBench b("sr_latch");
    b.set(0, 1);
    b.set(0, 0);
    CHECK_EQ(b.q(), 1);
    CHECK_EQ(b.notQ(), 0);
    b.set(1, 1);
    b.set(1, 0);
    CHECK_EQ(b.q(), 0);
    CHECK_EQ(b.notQ(), 1);
}

TEST(the_d_latch_box_follows_only_while_enabled) {
    BoxBench b("d_latch");
    b.set(1, 1);
    b.set(0, 1);
    CHECK_EQ(b.q(), 1);
    b.set(1, 0);
    b.set(0, 0);
    CHECK_EQ(b.q(), 1);
}

TEST(the_d_flip_flop_box_stores_on_the_rising_edge) {
    BoxBench b("d_flip_flop");
    b.pulse(1);
    CHECK_EQ(b.q(), 0);
    b.set(0, 1);
    CHECK_EQ(b.q(), 0);
    b.pulse(1);
    CHECK_EQ(b.q(), 1);
    CHECK_EQ(b.notQ(), 0);
}

TEST(the_t_flip_flop_box_toggles_only_with_t_on) {
    BoxBench b("t_flip_flop");
    const unsigned start = b.q();
    b.pulse(1);
    CHECK_EQ(b.q(), start);
    b.set(0, 1);
    b.pulse(1);
    CHECK_EQ(b.q(), 1 - start);
    b.pulse(1);
    CHECK_EQ(b.q(), start);
}

TEST(the_jk_flip_flop_box_sets_resets_holds_and_toggles) {
    BoxBench b("jk_flip_flop");
    b.set(0, 1);
    b.pulse(2);
    CHECK_EQ(b.q(), 1);
    b.set(0, 0);
    b.pulse(2);
    CHECK_EQ(b.q(), 1);
    b.set(1, 1);
    b.pulse(2);
    CHECK_EQ(b.q(), 0);
    b.set(0, 1);
    b.pulse(2);
    CHECK_EQ(b.q(), 1);
    b.pulse(2);
    CHECK_EQ(b.q(), 0);
}

TEST(a_box_on_a_board_works_like_any_part_and_shows_in_the_table) {
    Board board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId b = *board.addPart(PartKind::Toggle, {0, 8});
    const PartId box = *board.addPart(PartKind::Box, {12, 0}, 0, 0, 1, boxFromKey("half_adder"));
    const PartId sum = *board.addPart(PartKind::Led, {30, 0});
    const PartId carry = *board.addPart(PartKind::Probe, {30, 8});
    link(board, a, 0, box, 0);
    link(board, b, 0, box, 1);
    link(board, box, 2, sum, 0);
    link(board, box, 3, carry, 0);
    CHECK(boardIsSound(board));
    const TruthTable table = computeTruthTable(board);
    CHECK(table.status == TruthTable::Status::Ready);
    CHECK(table.rows == std::vector<std::string>({"00", "10", "10", "01"}));
}

TEST(boxes_turn_and_grow_like_any_part) {
    Board board;
    const PartId box = *board.addPart(PartKind::Box, {0, 0}, 0, 0, 1, boxFromKey("adder4"));
    for (int i = 0; i < 4; ++i) CHECK(board.rotatePart(box));
    CHECK(board.reshapePart(box, 0, 2));
    const Part& part = *board.part(box);
    const Rect bounds = boundsOf(part);
    CHECK(bounds.touches(pinPosition(part, pinCount(part) - 1)));
}

// --- buses -----------------------------------------------------------------------------------------

namespace {

Part sized(PartKind kind, Point at, std::uint8_t bits) {
    Part part = makePart(kind, at);
    part.width = bits;
    return part;
}

}  // namespace

TEST(a_gate_works_bit_by_bit_on_a_bus) {
    Board board;
    const PartId a = *board.addPart(sized(PartKind::Toggle, {0, 0}, 8));
    const PartId b = *board.addPart(sized(PartKind::Toggle, {0, 10}, 8));
    const PartId constant = [&] {
        Part part = sized(PartKind::Constant, {0, 30}, 8);
        part.value = 0x0F;
        return *board.addPart(part);
    }();
    Part gatePart = makePart(PartKind::And, {30, 0}, 0, 3);
    gatePart.width = 8;
    const PartId gate = *board.addPart(gatePart);
    const PartId out = *board.addPart(sized(PartKind::Probe, {50, 0}, 8));
    link(board, a, 0, gate, 0);
    link(board, b, 0, gate, 1);
    link(board, constant, 0, gate, 2);
    link(board, gate, 3, out, 0);

    BoardCircuit circuit(board);
    CHECK(!circuit.hasWidthConflict());
    circuit.setToggle(a, 0xFA);
    circuit.setToggle(b, 0x3C);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(out), 0xFAu & 0x3Cu & 0x0Fu);
    CHECK_EQ(circuit.wireBits(board.wires().back().id), 8);
}

TEST(a_wire_between_different_widths_is_flagged_and_reads_nothing) {
    Board board;
    const PartId a = *board.addPart(sized(PartKind::Toggle, {0, 0}, 8));
    const PartId out = *board.addPart(sized(PartKind::Probe, {30, 0}, 4));
    const WireId wire = link(board, a, 0, out, 0);
    BoardCircuit circuit(board);
    CHECK(circuit.hasWidthConflict());
    CHECK(circuit.wireConflicted(wire));
    circuit.setToggle(a, 0xFF);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(out), 0);
}

TEST(branches_add_up_to_the_bus_with_the_extra_bits_last) {
    for (unsigned width = 2; width <= 64; ++width) {
        for (unsigned fan = 2; fan <= std::min(width, 16u); ++fan) {
            Part part = makePart(PartKind::Splitter, {}, 0, static_cast<std::uint8_t>(fan));
            part.width = static_cast<std::uint8_t>(width);
            unsigned total = 0;
            for (unsigned branch = 0; branch < fan; ++branch) {
                total += branchBits(part, branch);
                if (branch > 0) CHECK(branchBits(part, branch) >= branchBits(part, branch - 1));
            }
            CHECK_EQ(total, width);
        }
    }
}

TEST(a_splitter_and_a_joiner_take_a_bus_apart_and_back) {
    Board board;
    const PartId a = *board.addPart(sized(PartKind::Toggle, {0, 0}, 8));
    Part splitterPart = makePart(PartKind::Splitter, {30, 0}, 0, 3);
    splitterPart.width = 8;
    const PartId split = *board.addPart(splitterPart);
    Part joinerPart = makePart(PartKind::Joiner, {50, 0}, 0, 3);
    joinerPart.width = 8;
    const PartId join = *board.addPart(joinerPart);
    const PartId whole = *board.addPart(sized(PartKind::Probe, {70, 0}, 8));
    const PartId low = *board.addPart(sized(PartKind::Probe, {70, 12}, 2));
    link(board, a, 0, split, 0);
    for (std::uint8_t branch = 0; branch < 3; ++branch) link(board, split, static_cast<std::uint8_t>(1 + branch), join, branch);
    link(board, join, 3, whole, 0);
    link(board, split, 1, low, 0);

    BoardCircuit circuit(board);
    CHECK(!circuit.hasWidthConflict());
    circuit.setToggle(a, 0b10110110);
    circuit.settle();
    CHECK_EQ(circuit.outputValue(whole), 0b10110110);
    CHECK_EQ(circuit.outputValue(low), 0b10);
}

TEST(clicking_a_many_bit_switch_flips_the_bit_under_the_cursor) {
    Player p;
    p.session.beginPlacing(PartKind::Toggle);
    for (int i = 0; i < 7; ++i) p.session.changeBits(1);
    p.click({10, 10});
    p.session.key(Key::Escape);
    const PartId id = p.session.board().parts()[0].id;
    const Part part = *p.session.board().part(id);
    CHECK_EQ(part.width, 8);
    const auto x = static_cast<float>(part.position.x);
    const auto y = static_cast<float>(part.position.y);

    p.session.mouseMove(x + 1.3f, y + 1.3f);  // the first lamp: the highest bit
    p.session.leftDown();
    p.session.leftUp();
    CHECK_EQ(p.session.toggleValue(id), 0x80);
    p.session.mouseMove(x + 15.4f, y + 1.4f);  // the last lamp: bit 0
    p.session.leftDown();
    p.session.leftUp();
    CHECK_EQ(p.session.toggleValue(id), 0x81);
}

TEST(the_bit_under_the_cursor_is_right_in_every_rotation) {
    for (std::uint8_t rotation = 0; rotation < 4; ++rotation) {
        Part part = makePart(PartKind::Toggle, {10, 20}, rotation);
        part.width = 8;
        const Rect bounds = boundsOf(part);
        std::vector<int> found;
        for (float offset = 0.5f; offset < 17.0f; offset += 0.5f) {
            const bool sideways = (rotation & 1) != 0;
            const float x = sideways ? static_cast<float>(bounds.left) + 1.0f : static_cast<float>(bounds.left) + offset;
            const float y = sideways ? static_cast<float>(bounds.top) + offset : static_cast<float>(bounds.top) + 1.0f;
            const int bit = toggleBitAt(part, x, y);
            if (bit >= 0 && std::find(found.begin(), found.end(), bit) == found.end()) found.push_back(bit);
        }
        CHECK_EQ(found.size(), 8);
    }
}

TEST(the_table_shows_many_bit_values_in_hex) {
    Board board;
    const PartId a = *board.addPart(sized(PartKind::Toggle, {0, 0}, 2));
    Part constantPart = sized(PartKind::Constant, {0, 10}, 2);
    constantPart.value = 1;
    const PartId one = *board.addPart(constantPart);
    Part adder = makePart(PartKind::Box, {12, 0}, 0, 0, 1, boxFromKey("adder_n"));
    adder.width = 2;
    const PartId box = *board.addPart(adder);
    const PartId out = *board.addPart(sized(PartKind::Probe, {30, 0}, 2));
    link(board, a, 0, box, 0);
    link(board, one, 0, box, 1);
    link(board, box, 3, out, 0);
    const TruthTable table = computeTruthTable(board);
    CHECK(table.status == TruthTable::Status::Ready);
    CHECK_EQ(table.totalInputBits, 2);
    CHECK(table.rows == std::vector<std::string>({"1", "2", "3", "0"}));
}

TEST(switch_bits_are_what_limit_the_table) {
    Board board;
    board.addPart(sized(PartKind::Toggle, {0, 0}, 9));
    board.addPart(PartKind::Led, {30, 0});
    CHECK(computeTruthTable(board).status == TruthTable::Status::TooManyInputs);
}

TEST(hovering_tells_a_wires_size_and_value) {
    Player p;
    Board board;
    const PartId a = *board.addPart(sized(PartKind::Toggle, {0, 0}, 4));
    const PartId out = *board.addPart(sized(PartKind::Probe, {20, 0}, 4));
    link(board, a, 0, out, 0);
    const std::array<std::pair<PartId, std::uint64_t>, 1> values{{{a, 5}}};
    p.session.adopt(board);
    p.session.setToggleValues(values);
    p.moveTo({12, 1});
    const std::optional<HoverInfo> info = p.session.hoverInfo();
    CHECK(info.has_value());
    CHECK_EQ(info->bits, 4);
    CHECK_EQ(info->value, 5);
}

TEST(changing_bits_keeps_wires_and_a_size_that_does_not_fit_changes_nothing) {
    Player p;
    const PartId a = p.place(PartKind::And, {0, 0});
    p.clickPart(a);
    p.session.changeBits(3);
    CHECK_EQ(p.session.board().part(a)->width, 4);
    Part tooMany = *p.session.board().part(a);
    tooMany.width = 65;
    Board board = p.session.board();
    CHECK(!board.reconfigurePart(a, tooMany));
}

// --- boxes of any size -----------------------------------------------------------------------------

namespace {

struct SizedBench {
    SizedBench(const char* key, unsigned width, unsigned address = 0, const std::vector<std::uint64_t>* contents = nullptr) {
        const BoxId id = boxFromKey(key);
        const InterfacePtr pins = boxInterface(id, width, address);
        for (const PinDef& pin : pins->inputs) in.push_back(net.input(pin.name, pin.bits));
        out.resize(pins->outputs.size());
        boxSpec(id).build(net, in, out, BoxParams{width, address, contents});
        simulator = sim::makeEventSimulator(sim::compile(net));
        simulator->settle();
    }
    void set(std::size_t input, std::uint64_t value) {
        simulator->set(in[input], value);
        simulator->settle();
    }
    void pulse(std::size_t clock) {
        set(clock, 1);
        set(clock, 0);
    }
    std::uint64_t get(std::size_t output) const { return simulator->get(out[output]); }

    sim::Netlist net;
    std::vector<sim::NetId> in;
    std::vector<sim::NetId> out;
    std::unique_ptr<sim::Backend> simulator;
};

}  // namespace

TEST(sized_adders_and_subtractors_match_real_arithmetic) {
    std::mt19937_64 random(5);
    for (unsigned width : {2u, 3u, 8u, 16u, 32u, 63u}) {
        const std::uint64_t mask = sim::widthMask(width);
        SizedBench add("adder_n", width);
        SizedBench subtract("sub_n", width);
        for (int i = 0; i < 300; ++i) {
            const std::uint64_t a = random() & mask;
            const std::uint64_t b = random() & mask;
            const std::uint64_t c = random() & 1;
            add.set(0, a);
            add.set(1, b);
            add.set(2, c);
            const std::uint64_t sum = a + b + c;  // widths stop at 63 bits, so this can't overflow
            CHECK_EQ(add.get(0), sum & mask);
            CHECK_EQ(add.get(1), sum >> width);
            subtract.set(0, a);
            subtract.set(1, b);
            subtract.set(2, c);
            CHECK_EQ(subtract.get(0), (a - b - c) & mask);
            CHECK_EQ(subtract.get(1), a < b + c ? 1u : 0u);
        }
    }
}

TEST(the_sized_comparator_orders_every_pair) {
    SizedBench box("cmp_n", 4);
    for (unsigned a = 0; a < 16; ++a) {
        for (unsigned b = 0; b < 16; ++b) {
            box.set(0, a);
            box.set(1, b);
            CHECK_EQ(box.get(0), a > b ? 1u : 0u);
            CHECK_EQ(box.get(1), a == b ? 1u : 0u);
            CHECK_EQ(box.get(2), a < b ? 1u : 0u);
        }
    }
}

TEST(sized_muxes_pass_the_chosen_bus) {
    SizedBench two("mux2_n", 8);
    two.set(0, 0xAA);
    two.set(1, 0x55);
    CHECK_EQ(two.get(0), 0xAA);
    two.set(2, 1);
    CHECK_EQ(two.get(0), 0x55);

    SizedBench four("mux4_n", 16);
    for (unsigned i = 0; i < 4; ++i) four.set(i, 0x1000u + i);
    for (unsigned select = 0; select < 4; ++select) {
        four.set(4, select);
        CHECK_EQ(four.get(0), 0x1000u + select);
    }
}

TEST(the_sized_alu_does_every_operation) {
    std::mt19937_64 random(7);
    constexpr unsigned kWidth = 16;
    const std::uint64_t mask = sim::widthMask(kWidth);
    SizedBench alu("alu_n", kWidth);
    for (int i = 0; i < 2000; ++i) {
        const std::uint64_t a = random() & mask;
        const std::uint64_t b = random() & mask;
        const unsigned op = static_cast<unsigned>(random() & 7);
        alu.set(0, a);
        alu.set(1, b);
        alu.set(2, op);
        std::uint64_t expected = 0;
        std::uint64_t carry = 0;
        switch (op) {
            case 0: expected = a & b; break;
            case 1: expected = a | b; break;
            case 2: expected = a ^ b; break;
            case 3: expected = (a + b) & mask; carry = (a + b) >> kWidth; break;
            case 4: expected = (a - b) & mask; carry = a < b ? 1 : 0; break;
            case 5: expected = ~a & mask; break;
            case 6: expected = (a << 1) & mask; break;
            default: expected = a >> 1; break;
        }
        CHECK_EQ(alu.get(0), expected);
        CHECK_EQ(alu.get(1), carry);
        CHECK_EQ(alu.get(2), expected == 0 ? 1u : 0u);
    }
}

TEST(a_register_stores_only_when_enabled_and_clears_on_reset) {
    SizedBench box("register", 8);
    box.set(0, 0x5A);
    box.pulse(3);
    CHECK_EQ(box.get(0), 0);  // enable is off
    box.set(1, 1);
    box.pulse(3);
    CHECK_EQ(box.get(0), 0x5A);
    box.set(1, 0);
    box.set(0, 0x11);
    box.pulse(3);
    CHECK_EQ(box.get(0), 0x5A);
    box.set(2, 1);
    box.pulse(3);
    CHECK_EQ(box.get(0), 0);
}

TEST(a_counter_counts_wraps_and_says_when_it_is_at_the_top) {
    SizedBench box("counter", 4);
    box.set(0, 1);
    for (unsigned i = 1; i <= 15; ++i) {
        box.pulse(2);
        CHECK_EQ(box.get(0), i);
    }
    CHECK_EQ(box.get(1), 1);
    box.pulse(2);
    CHECK_EQ(box.get(0), 0);
    CHECK_EQ(box.get(1), 0);
    box.set(0, 0);
    box.pulse(2);
    CHECK_EQ(box.get(0), 0);
    box.set(0, 1);
    box.pulse(2);
    box.set(1, 1);
    box.pulse(2);
    CHECK_EQ(box.get(0), 0);
}

TEST(a_shift_register_shifts_bits_in) {
    SizedBench box("shift_register", 4);
    box.set(1, 1);
    for (unsigned bit : {1u, 0u, 1u, 1u}) {
        box.set(0, bit);
        box.pulse(2);
    }
    CHECK_EQ(box.get(0), 0b1011);
}

TEST(ram_keeps_what_it_is_given_at_each_address) {
    SizedBench ram("ram", 8, 4);
    ram.set(2, 1);
    for (unsigned address = 0; address < 16; ++address) {
        ram.set(0, address);
        ram.set(1, address * 3);
        ram.pulse(3);
    }
    ram.set(2, 0);
    for (unsigned address = 0; address < 16; ++address) {
        ram.set(0, address);
        CHECK_EQ(ram.get(0), address * 3);
    }
}

TEST(rom_gives_back_its_contents_and_zero_past_them) {
    const std::vector<std::uint64_t> words{7, 8, 9};
    SizedBench rom("rom", 8, 4, &words);
    rom.set(0, 1);
    CHECK_EQ(rom.get(0), 8);
    rom.set(0, 2);
    CHECK_EQ(rom.get(0), 9);
    rom.set(0, 3);
    CHECK_EQ(rom.get(0), 0);
}

TEST(a_rom_on_the_board_is_filled_through_the_session) {
    Player p;
    p.session.beginPlacing(PartKind::Box, boxFromKey("rom"));
    p.click({20, 20});
    p.session.key(Key::Escape);
    const PartId id = p.session.board().parts()[0].id;
    p.clickPart(id);
    p.session.setRomWords({1, 2, 0x1FF});
    const Part& part = *p.session.board().part(id);
    CHECK(part.rom != nullptr);
    CHECK_EQ((*part.rom)[2], 0xFF);  // cut to the ROM's 8 bits
    p.session.changeAddress(-7);
    CHECK_EQ(p.session.board().part(id)->address, 1);
    CHECK_EQ(p.session.board().part(id)->rom->size(), 2);  // a smaller ROM keeps what fits
}

// --- clock -----------------------------------------------------------------------------------------

namespace {

struct CounterRig {
    PartId clock = 0;
    PartId run = 0;
    PartId counter = 0;
    PartId out = 0;
    Board board;
};

CounterRig counterRig() {
    CounterRig rig;
    Board& board = rig.board;
    rig.clock = *board.addPart(PartKind::Clock, {0, 0});
    rig.run = *board.addPart(PartKind::Toggle, {0, 8});
    rig.counter = *board.addPart(PartKind::Box, {12, 0}, 0, 0, 1, boxFromKey("counter"));
    rig.out = *board.addPart(sized(PartKind::Probe, {40, 0}, 8));
    link(board, rig.run, 0, rig.counter, 0);
    link(board, rig.clock, 0, rig.counter, 2);
    link(board, rig.counter, 3, rig.out, 0);
    return rig;
}

}  // namespace

TEST(two_half_steps_of_the_clock_are_one_tick) {
    const CounterRig rig = counterRig();
    Session session;
    const std::array on{rig.run};
    session.adopt(rig.board, on);
    CHECK(session.hasClock());
    session.stepClock();
    CHECK_EQ(session.circuit().outputValue(rig.out), 1);
    session.stepClock();
    CHECK_EQ(session.circuit().outputValue(rig.out), 1);
    session.stepClock();
    CHECK_EQ(session.circuit().outputValue(rig.out), 2);
}

TEST(a_running_clock_ticks_at_its_speed) {
    const CounterRig rig = counterRig();
    Session session;
    const std::array on{rig.run};
    session.adopt(rig.board, on);
    while (session.speed() != 8) session.changeSpeed(session.speed() > 8 || session.speed() == 0 ? -1 : 1);
    session.advance(1.0);
    CHECK_EQ(session.circuit().outputValue(rig.out), 0);  // not running yet
    session.setRunning(true);
    for (int i = 0; i < 10; ++i) session.advance(0.1);
    CHECK_EQ(session.circuit().outputValue(rig.out), 8);
}

TEST(a_clock_cannot_run_on_a_board_without_one) {
    Session session;
    session.setRunning(true);
    CHECK(!session.running());
}

TEST(reset_clears_what_the_circuit_remembers_and_keeps_the_switches) {
    const CounterRig rig = counterRig();
    Session session;
    const std::array on{rig.run};
    session.adopt(rig.board, on);
    session.stepClock();
    session.stepClock();
    session.stepClock();
    CHECK(session.circuit().outputValue(rig.out) != 0);
    session.resetSimulation();
    CHECK_EQ(session.circuit().outputValue(rig.out), 0);
    CHECK(session.toggleOn(rig.run));
    session.stepClock();
    CHECK_EQ(session.circuit().outputValue(rig.out), 1);
}

TEST(with_no_clock_a_counter_is_memory_in_the_table) {
    const CounterRig rig = counterRig();
    const TruthTable table = computeTruthTable(rig.board);
    CHECK(table.hasMemory);
}

TEST(the_fastest_speed_runs_without_stopping_the_frame_for_long) {
    const CounterRig rig = counterRig();
    Session session;
    const std::array on{rig.run};
    session.adopt(rig.board, on);
    while (session.speed() != 0) session.changeSpeed(1);
    session.setRunning(true);
    const auto start = std::chrono::steady_clock::now();
    session.advance(1.0 / 60.0);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(took < 0.1);
    CHECK(session.measuredSpeed() > 0.0);
}

// --- chips -----------------------------------------------------------------------------------------

namespace {

// Puts a two-input AND inside a new chip of the session, and comes back to the main board.
ChipId addAndChip(Player& p) {
    const ChipId id = p.session.createChip("meu and");
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId b = p.place(PartKind::Toggle, {0, 8});
    const PartId gate = p.place(PartKind::And, {10, 2});
    const PartId out = p.place(PartKind::Led, {20, 3});
    p.wire(p.pin(a, 0), p.pin(gate, 0));
    p.wire(p.pin(b, 0), p.pin(gate, 1));
    p.wire(p.pin(gate, 2), p.pin(out, 0));
    p.session.openCircuit(0);
    return id;
}

PartId placeChip(Player& p, ChipId chip, Point centre) {
    p.session.beginPlacingChip(chip);
    p.click(centre);
    p.session.key(Key::Escape);
    return p.session.board().parts().back().id;
}

}  // namespace

TEST(a_chip_works_like_the_circuit_inside_it) {
    Player p;
    const ChipId chip = addAndChip(p);
    const PartId box = placeChip(p, chip, {20, 10});
    const Part part = *p.session.board().part(box);
    CHECK_EQ(inputCount(part), 2);
    CHECK_EQ(outputCount(part), 1);
    CHECK(std::string(partName(part)) == "meu and");

    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId b = p.place(PartKind::Toggle, {0, 8});
    const PartId out = p.place(PartKind::Led, {40, 8});
    p.wire(p.pin(a, 0), p.pin(box, 0));
    p.wire(p.pin(b, 0), p.pin(box, 1));
    p.wire(p.pin(box, 2), p.pin(out, 0));

    CHECK(p.session.truthTable().rows == std::vector<std::string>({"0", "0", "0", "1"}));
    p.clickPart(a);
    p.clickPart(b);
    CHECK(p.session.circuit().outputOn(out));
}

TEST(chips_go_inside_chips) {
    Player p;
    const ChipId inner = addAndChip(p);
    const ChipId outer = p.session.createChip("dois");
    const PartId first = placeChip(p, inner, {12, 4});
    const PartId a = p.place(PartKind::Toggle, {-10, 0});
    const PartId b = p.place(PartKind::Toggle, {-10, 8});
    const PartId out = p.place(PartKind::Probe, {30, 2});
    p.wire(p.pin(a, 0), p.pin(first, 0));
    p.wire(p.pin(b, 0), p.pin(first, 1));
    p.wire(p.pin(first, 2), p.pin(out, 0));
    p.session.openCircuit(0);

    const PartId box = placeChip(p, outer, {20, 10});
    const PartId x = p.place(PartKind::Toggle, {0, 0});
    const PartId y = p.place(PartKind::Toggle, {0, 8});
    const PartId led = p.place(PartKind::Led, {40, 8});
    p.wire(p.pin(x, 0), p.pin(box, 0));
    p.wire(p.pin(y, 0), p.pin(box, 1));
    p.wire(p.pin(box, 2), p.pin(led, 0));
    CHECK(p.session.truthTable().rows == std::vector<std::string>({"0", "0", "0", "1"}));
}

TEST(a_chip_cannot_end_up_inside_itself) {
    Player p;
    const ChipId inner = addAndChip(p);
    const ChipId outer = p.session.createChip("fora");
    CHECK(p.session.canPlaceChip(inner));
    CHECK(!p.session.canPlaceChip(outer));  // itself
    placeChip(p, inner, {10, 10});
    p.session.openCircuit(inner);
    CHECK(!p.session.canPlaceChip(outer));  // the one that holds this one
    p.session.beginPlacingChip(outer);
    CHECK(p.session.mode() == Mode::Idle);
    CHECK(!p.session.canPlaceChip(0));      // the main board isn't a chip
}

TEST(pasting_cannot_put_a_chip_inside_itself_either) {
    Player p;
    const ChipId chip = addAndChip(p);
    placeChip(p, chip, {20, 10});
    p.session.key(Key::SelectAll);
    p.session.key(Key::Copy);
    p.session.openCircuit(chip);
    p.session.key(Key::Paste);
    CHECK(p.session.mode() == Mode::Idle);
}

TEST(editing_a_chip_updates_every_use_and_keeps_the_wires_that_still_fit) {
    Player p;
    const ChipId chip = addAndChip(p);
    const PartId box = placeChip(p, chip, {20, 10});
    const PartId a = p.place(PartKind::Toggle, {0, 0});
    const PartId b = p.place(PartKind::Toggle, {0, 8});
    const PartId led = p.place(PartKind::Led, {40, 8});
    p.wire(p.pin(a, 0), p.pin(box, 0));
    p.wire(p.pin(b, 0), p.pin(box, 1));
    p.wire(p.pin(box, 2), p.pin(led, 0));
    CHECK_EQ(p.session.board().wires().size(), 3);

    p.session.openCircuit(chip);
    p.place(PartKind::Toggle, {0, 30});  // a third input
    p.session.openCircuit(0);
    const Part* part = p.session.board().part(box);
    CHECK_EQ(inputCount(*part), 3);
    CHECK_EQ(p.session.board().wires().size(), 3);
    CHECK(boardIsSound(p.session.board()));

    p.session.openCircuit(chip);
    p.session.key(Key::Undo);
    p.session.key(Key::Undo);  // takes away part of the circuit, and with it a pin
    p.session.openCircuit(0);
    CHECK(boardIsSound(p.session.board()));
}

TEST(each_circuit_keeps_its_own_undo_and_board) {
    Player p;
    p.place(PartKind::Nand, {0, 0});
    const ChipId chip = p.session.createChip("x");
    CHECK(p.session.board().parts().empty());
    CHECK_EQ(p.session.undoSteps(), 0);
    p.place(PartKind::Not, {0, 0});
    p.session.openCircuit(0);
    CHECK_EQ(p.session.board().parts().size(), 1);
    CHECK_EQ(p.session.undoSteps(), 1);
    p.session.openCircuit(chip);
    CHECK(p.session.board().parts()[0].kind == PartKind::Not);
}

TEST(a_clock_inside_a_chip_runs) {
    Player p;
    const ChipId chip = p.session.createChip("relogio");
    const PartId clock = p.place(PartKind::Clock, {0, 0});
    const PartId out = p.place(PartKind::Led, {10, 0});
    p.wire(p.pin(clock, 0), p.pin(out, 0));
    p.session.openCircuit(0);
    const PartId box = placeChip(p, chip, {20, 10});
    const PartId led = p.place(PartKind::Led, {40, 8});
    p.wire(p.pin(box, 0), p.pin(led, 0));
    CHECK(p.session.hasClock());
    CHECK(!p.session.circuit().outputOn(led));
    p.session.stepClock();
    CHECK(p.session.circuit().outputOn(led));
}

TEST(a_many_bit_pin_on_a_chip_carries_the_whole_bus) {
    Player p;
    const ChipId chip = p.session.createChip("copia");
    Board inner;
    const PartId in = *inner.addPart(sized(PartKind::Toggle, {0, 0}, 8));
    const PartId out = *inner.addPart(sized(PartKind::Probe, {30, 0}, 8));
    link(inner, in, 0, out, 0);
    p.session.adoptProjectChips({ChipData{chip, "copia", inner, {}}});
    p.session.adopt(Board{});

    const PartId box = placeChip(p, chip, {20, 10});
    CHECK_EQ(pinBits(*p.session.board().part(box), 0), 8);
    const PartId a = p.place(PartKind::Led, {40, 8});
    (void)a;
    Board board = p.session.board();
    const PartId source = *board.addPart(sized(PartKind::Toggle, {0, 0}, 8));
    const PartId probe = *board.addPart(sized(PartKind::Probe, {50, 20}, 8));
    link(board, source, 0, box, 0);
    link(board, box, 1, probe, 0);
    const std::array<std::pair<PartId, std::uint64_t>, 1> values{{{source, 0xC3}}};
    const std::vector<ChipData> chips = p.session.projectChips();
    p.session.adoptProjectChips(chips);
    p.session.adopt(board);
    p.session.setToggleValues(values);
    CHECK_EQ(p.session.circuit().outputValue(probe), 0xC3);
}

// --- fuzz ------------------------------------------------------------------------------------------

TEST(random_board_edits_keep_the_board_sound) {
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        std::mt19937 random(seed);
        auto below = [&](std::uint32_t n) { return static_cast<int>(random() % n); };
        auto spot = [&] { return Point{below(60), below(40)}; };
        Board board;
        for (int step = 0; step < 120; ++step) {
            switch (below(8)) {
                case 0:
                case 1: {
                    const auto kind = static_cast<PartKind>(below(kPartKindCount));
                    board.addPart(kind, spot(), static_cast<std::uint8_t>(below(4)), static_cast<std::uint8_t>(below(10)),
                                  static_cast<std::uint8_t>(below(4)),
                                  kind == PartKind::Box ? static_cast<BoxId>(1 + below(static_cast<std::uint32_t>(boxCount()))) : kNoBox);
                    break;
                }
                case 7:
                    if (!board.parts().empty()) {
                        board.reshapePart(board.parts()[static_cast<std::size_t>(below(static_cast<std::uint32_t>(board.parts().size())))].id,
                                          static_cast<std::uint8_t>(below(10)), static_cast<std::uint8_t>(below(4)));
                    }
                    break;
                case 2:
                case 3: {
                    const Drop from = board.dropAt(spot());
                    Point end = spot();
                    if (!board.parts().empty() && below(2) == 0) {
                        const Part& part = board.parts()[static_cast<std::size_t>(below(static_cast<std::uint32_t>(board.parts().size())))];
                        end = pinPosition(part, 0);
                    }
                    const Drop to = board.dropAt(end);
                    board.connect(from, to, routePath(board, from.at, to.at));
                    break;
                }
                case 4:
                    if (!board.parts().empty()) {
                        const std::array moving{board.parts()[static_cast<std::size_t>(below(static_cast<std::uint32_t>(board.parts().size())))].id};
                        board.moveParts(moving, {below(13) - 6, below(13) - 6});
                    }
                    break;
                case 5:
                    if (!board.parts().empty()) {
                        board.rotatePart(board.parts()[static_cast<std::size_t>(below(static_cast<std::uint32_t>(board.parts().size())))].id);
                    }
                    break;
                default:
                    if (!board.wires().empty()) {
                        const std::array gone{board.wires()[static_cast<std::size_t>(below(static_cast<std::uint32_t>(board.wires().size())))].id};
                        board.remove({}, gone);
                    }
                    break;
            }
            if (!boardIsSound(board)) {
                std::printf("  board broke: seed %u, step %d\n", seed, step);
                CHECK(false);
                return;
            }
        }
        const BoardCircuit circuit(board);  // whatever was built must still compile and run
        (void)circuit;
    }
}

TEST(random_play_then_undoing_everything_leaves_an_empty_board) {
    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        std::mt19937 random(seed);
        auto below = [&](std::uint32_t n) { return static_cast<int>(random() % n); };
        Player p;
        for (int step = 0; step < 400; ++step) {
            p.session.mouseMove(static_cast<float>(below(500)) / 10.0f, static_cast<float>(below(300)) / 10.0f, below(5) == 0);
            switch (below(27)) {
                case 14: p.session.key(Key::Copy); break;
                case 15: p.session.key(Key::Cut); break;
                case 16: p.session.key(Key::Paste); break;
                case 17: p.session.changeInputs(below(2) == 0 ? 1 : -1); break;
                case 18: p.session.changeScale(below(2) == 0 ? 1 : -1); break;
                case 19:
                    p.session.beginPlacing(PartKind::Box, static_cast<BoxId>(1 + below(static_cast<std::uint32_t>(boxCount()))));
                    break;
                case 20: p.session.changeBits(below(2) == 0 ? 1 : -1); break;
                case 21: p.session.changeAddress(below(2) == 0 ? 1 : -1); break;
                case 22: p.session.stepClock(); break;
                case 23: p.session.resetSimulation(); break;
                case 24: if (below(10) == 0) p.session.createChip("x"); break;
                case 25: p.session.openCircuit(static_cast<ChipId>(below(4))); break;
                case 26: p.session.beginPlacingChip(static_cast<ChipId>(below(4))); break;
                case 0: p.session.leftDown(below(4) == 0); break;
                case 1: case 2: p.session.leftUp(); break;
                case 3: p.session.rightDown(); break;
                case 4: p.session.key(Key::Escape); break;
                case 5: p.session.key(Key::Delete); break;
                case 6: p.session.key(Key::Enter); break;
                case 7: p.session.key(Key::Rotate); break;
                case 8: p.session.key(Key::Undo); break;
                case 9: p.session.key(Key::Redo); break;
                case 10: p.session.key(Key::SelectAll); break;
                case 11: p.session.beginPlacing(static_cast<PartKind>(below(kPartKindCount))); break;
                default: break;
            }
            if (!boardIsSound(p.session.board())) {
                std::printf("  board broke: seed %u, step %d\n", seed, step);
                CHECK(false);
                return;
            }
        }
        p.session.key(Key::Escape);
        p.session.openCircuit(0);
        for (std::size_t i = 0; i < 200; ++i) p.session.key(Key::Undo);
        // At most 100 steps are kept, so only a short session can be fully undone.
        if (p.session.undoSteps() == 0 && p.session.redoSteps() < 100) CHECK(p.session.board().parts().empty());
    }
}

int main() { return check::runAll(); }
