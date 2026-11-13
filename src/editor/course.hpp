// The course around the lessons: each act is a row of topics, and each topic is a reading, the theory,
// followed by the exercises that put it to work. The exercises are the lessons of lessons.hpp; a topic
// only names them. An act opens once every exercise of the one before it is done.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/board.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

// A circuit drawn inside a reading: a board, live, with these switches closed.
struct TheoryFigure {
    Board board;
    std::vector<PartId> closed;  // switches closed and inputs on
    std::vector<std::pair<PartId, std::uint64_t>> values;  // inputs of several bits, and the number each is set to
    std::vector<ChipData> chips;
};

enum class TheoryBlockKind : std::uint8_t {
    Heading,  // a section's title
    Text,     // a paragraph
    Figure,   // a circuit, with the text as its caption
    Table,    // rows split by '\n', cells by '|'; the first row is the header
    Tip,      // a paragraph set apart: a rule worth remembering
    Word,     // an instruction's 16 bits, field by field in colour, with the text as its caption
    Code,     // a few lines of assembly, coloured the way the editor colours them
};

struct TheoryBlock {
    TheoryBlockKind kind;
    const char* text;
    TheoryFigure (*figure)() = nullptr;
    int height = 0;          // a figure's height on screen, in pixels
    std::uint16_t word = 0;  // a Word block's instruction
};

struct Topic {
    const char* id;  // for the saved progress: never change one
    int act;
    const char* title;
    const char* line;  // what it covers, in a few words
    std::vector<TheoryBlock> blocks;
    std::vector<const char*> exercises;  // lesson ids, in order
};

const std::vector<Topic>& topics();

// Circuits from every act, for the start screen to show faded behind its menu.
const std::vector<TheoryFigure (*)()>& backdropFigures();
int topicIndex(std::string_view id);  // -1 if there's none

// The topic a lesson is an exercise of, or -1 when its act has no readings yet.
int topicOfLesson(int lesson);

// What's saved when a reading has been read, next to the finished lessons.
std::string readingId(const Topic& topic);

// The lessons of an act, in order, by index.
std::vector<int> actLessons(int act);

// Open when it's the first act, when every lesson of the act before is done, or when the player already
// finished something in it (progress from before the acts had locks).
bool actOpen(int act, std::span<const std::string> finished);

// How many lessons of the act are done.
int actDone(int act, std::span<const std::string> finished);

}  // namespace bancada::editor
