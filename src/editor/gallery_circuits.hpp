// The circuits of the gallery: each example in every way it's built (the ready part, logic gates,
// transistors, smaller blocks), with what it is and how each version works. gallery.cpp adds the programs.
#pragma once

#include <vector>

#include "editor/gallery.hpp"

namespace bancada::editor {

std::vector<GalleryItem> circuitExamples();

// The groups, in order: the circuits' first, then the programs'.
inline constexpr int kGroupElectronics = 0;
inline constexpr int kGroupGates = 1;
inline constexpr int kGroupMoreGates = 2;
inline constexpr int kGroupOneBit = 3;
inline constexpr int kGroupManyBits = 4;
inline constexpr int kGroupMoreMath = 5;
inline constexpr int kGroupMemory = 6;
inline constexpr int kGroupComputer = 7;
inline constexpr int kGroupSystem = 8;

}  // namespace bancada::editor
