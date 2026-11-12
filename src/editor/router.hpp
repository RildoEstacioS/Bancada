// Suggests paths for wires. It only ever suggests: the player's clicks decide where a wire goes, and
// whatever it returns has no effect on what the wire connects to.
#pragma once

#include <optional>
#include <vector>

#include "editor/board.hpp"

namespace bancada::editor {

// One corner at most, leaving horizontally.
std::vector<Point> elbowPath(Point from, Point to);

// A short path with few turns that goes around parts and avoids running along other wires. Crossing a
// wire is free. Falls back to elbowPath if the two points are very far apart or there's no way through.
// With arrivedFrom, the path won't double straight back toward that point: when the player fixes a
// corner by clicking, the next stretch has to leave it as a corner, or the click would be lost.
std::vector<Point> routePath(const Board& board, Point from, Point to, std::optional<Point> arrivedFrom = std::nullopt);

}  // namespace bancada::editor
