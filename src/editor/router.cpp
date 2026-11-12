#include "editor/router.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <queue>

namespace bancada::editor {

namespace {

constexpr int kMargin = 12;            // how far past the two points the search may go
constexpr int kMaxCells = 250000;      // beyond this, a suggestion isn't worth the time

constexpr int kStepCost = 10;
constexpr int kTurnCost = 15;
constexpr int kAlongWireCost = 40;

constexpr std::array<Point, 4> kDirections{Point{1, 0}, Point{0, 1}, Point{-1, 0}, Point{0, -1}};

class Window {
public:
    Window(Point a, Point b)
        : left_(std::min(a.x, b.x) - kMargin), top_(std::min(a.y, b.y) - kMargin),
          width_(std::abs(a.x - b.x) + 2 * kMargin + 1), height_(std::abs(a.y - b.y) + 2 * kMargin + 1) {}

    int width() const { return width_; }
    int height() const { return height_; }
    std::int64_t cells() const { return std::int64_t{width_} * height_; }
    bool inside(Point p) const { return p.x >= left_ && p.y >= top_ && p.x < left_ + width_ && p.y < top_ + height_; }
    std::size_t index(Point p) const {
        return static_cast<std::size_t>(p.y - top_) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(p.x - left_);
    }
    Point at(std::size_t index) const {
        const auto w = static_cast<std::size_t>(width_);
        return {static_cast<int>(index % w) + left_, static_cast<int>(index / w) + top_};
    }
    Rect rect() const { return {left_, top_, left_ + width_, top_ + height_}; }

private:
    int left_;
    int top_;
    int width_;
    int height_;
};

}  // namespace

std::vector<Point> elbowPath(Point from, Point to) {
    std::vector<Point> path{from, Point{to.x, from.y}, to};
    simplifyPath(path);
    return path;
}

std::vector<Point> routePath(const Board& board, Point from, Point to, std::optional<Point> arrivedFrom) {
    if (from == to) return {from, to};
    const Window window(from, to);
    if (window.cells() > kMaxCells) return elbowPath(from, to);
    const auto cells = static_cast<std::size_t>(window.cells());

    std::vector<std::uint8_t> blocked(cells, 0);
    const Rect area = window.rect();
    for (const Part& part : board.parts()) {
        const Rect bounds = boundsOf(part);
        if (!bounds.overlaps(area)) continue;
        for (int y = std::max(bounds.top, area.top); y <= bounds.bottom && y < area.bottom; ++y) {
            for (int x = std::max(bounds.left, area.left); x <= bounds.right && x < area.right; ++x) {
                blocked[window.index({x, y})] = 1;
            }
        }
    }
    blocked[window.index(from)] = 0;
    blocked[window.index(to)] = 0;

    // Bit 0: a wire runs along here horizontally. Bit 1: vertically.
    std::vector<std::uint8_t> used(cells, 0);
    for (const Wire& wire : board.wires()) {
        for (std::size_t i = 0; i + 1 < wire.path.size(); ++i) {
            Point p = wire.path[i];
            const Point end = wire.path[i + 1];
            const Point step{(end.x > p.x) - (end.x < p.x), (end.y > p.y) - (end.y < p.y)};
            const std::uint8_t bit = step.y == 0 ? 1 : 2;
            for (;;) {
                if (window.inside(p)) used[window.index(p)] |= bit;
                if (p == end) break;
                p = p + step;
            }
        }
    }

    struct Entry {
        int estimate;
        std::uint32_t state;
        bool operator>(const Entry& other) const {
            return estimate != other.estimate ? estimate > other.estimate : state > other.state;
        }
    };
    constexpr int kUnreached = INT32_MAX;
    std::vector<int> cost(cells * 4, kUnreached);
    std::vector<std::uint32_t> previous(cells * 4, UINT32_MAX);
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;

    auto heuristic = [&](Point p) { return kStepCost * (std::abs(p.x - to.x) + std::abs(p.y - to.y)); };
    auto stateOf = [](std::size_t cell, std::size_t direction) { return static_cast<std::uint32_t>(cell * 4 + direction); };

    const std::size_t startCell = window.index(from);
    std::size_t firstDirection = 4;  // none in particular
    if (arrivedFrom && !(*arrivedFrom == from)) {
        const Point arrival{(from.x > arrivedFrom->x) - (from.x < arrivedFrom->x),
                            (from.y > arrivedFrom->y) - (from.y < arrivedFrom->y)};
        for (std::size_t direction = 0; direction < 4; ++direction) {
            if (kDirections[direction] == arrival) firstDirection = direction;
        }
    }
    for (std::size_t direction = 0; direction < 4; ++direction) {
        if (firstDirection != 4 && direction != firstDirection) continue;
        cost[stateOf(startCell, direction)] = 0;
        open.push({heuristic(from), stateOf(startCell, direction)});
    }

    std::uint32_t reached = UINT32_MAX;
    const std::size_t goalCell = window.index(to);
    while (!open.empty()) {
        const Entry entry = open.top();
        open.pop();
        const std::size_t cell = entry.state / 4;
        const std::size_t direction = entry.state % 4;
        const int soFar = cost[entry.state];
        if (entry.estimate > soFar + heuristic(window.at(cell))) continue;  // a better way was found since
        if (cell == goalCell) {
            reached = entry.state;
            break;
        }
        const Point here = window.at(cell);
        for (std::size_t next = 0; next < 4; ++next) {
            if ((next + 2) % 4 == direction) continue;  // no turning straight back
            const Point there = here + kDirections[next];
            if (!window.inside(there)) continue;
            const std::size_t thereCell = window.index(there);
            if (blocked[thereCell] != 0) continue;

            const std::uint8_t bit = kDirections[next].y == 0 ? 1 : 2;
            int step = kStepCost;
            if (next != direction) step += kTurnCost;
            if ((used[cell] & bit) != 0 && (used[thereCell] & bit) != 0) step += kAlongWireCost;

            const std::uint32_t state = stateOf(thereCell, next);
            if (soFar + step >= cost[state]) continue;
            cost[state] = soFar + step;
            previous[state] = entry.state;
            open.push({soFar + step + heuristic(there), state});
        }
    }

    if (reached == UINT32_MAX) return elbowPath(from, to);

    std::vector<Point> path;
    for (std::uint32_t state = reached; state != UINT32_MAX; state = previous[state]) path.push_back(window.at(state / 4));
    std::reverse(path.begin(), path.end());
    simplifyPath(path);
    return path;
}

}  // namespace bancada::editor
