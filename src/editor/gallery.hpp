// The gallery: ready-made circuits and programs to open, look at and change, from an LED to programs
// running on the 16-bit computer. Each is built from the same parts the player has; the tests build every
// one, check that the circuits do what they say, and run every program to its end.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "editor/board.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

struct GalleryProject {
    Board board;
    std::vector<ChipData> chips;
    std::vector<std::pair<PartId, std::uint64_t>> toggles;  // switches that start on, and with what
    std::string note;                                       // said when it opens
    bool computer = false;                                  // opens on the panel
};

// A program for the computer examples: what it does, its source, and what the switches start at.
struct ComputerProgram {
    const char* id;
    const char* title;
    const char* line;
    const char* source;
    std::uint16_t switches = 0;
    bool halts = true;
};

const std::vector<ComputerProgram>& computerPrograms();

// One way to build an example: as the ready part from the library, out of logic gates, out of
// transistors, out of smaller blocks. The player switches between them above the board.
struct GalleryVersion {
    const char* name;  // on its button
    GalleryProject (*build)();
    const char* how;   // how this version does it
};

struct GalleryItem {
    const char* id;
    int group;  // index into galleryGroups()
    const char* title;
    const char* line;
    std::vector<GalleryVersion> versions;  // a circuit, each way it's built; none for a program
    int program = -1;                      // or a program, by index into computerPrograms()
    const char* about = "";                // what it is, what it does and what it's for
    const char* tryIt = "";                // what to do to see it work
};

const std::vector<const char*>& galleryGroups();
const std::vector<GalleryItem>& galleryItems();
// The example in one of its versions; a version past the last is the first.
GalleryProject buildGalleryItem(const GalleryItem& item, std::size_t version = 0);

// How a part is made, from the gallery: the example whose first version is that part alone (a gate, or a
// ready box), and the version after it, which builds the same thing from smaller parts (a box from gates, a
// gate from transistors). No item if the gallery doesn't show this part.
struct PartInside {
    const GalleryItem* item = nullptr;
    std::size_t version = 0;
};
PartInside galleryInsideOf(const Part& part);

}  // namespace bancada::editor
