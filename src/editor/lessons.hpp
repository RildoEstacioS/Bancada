// The lessons: from lighting an LED to a computer, one small step at a time. Each lesson starts from a
// board, limits the library to the parts it's about, and checks the player's board by itself: it lights
// the LED, closes every switch combination, or tries every row of a truth table, the way a person would.
// Everything here is data and plain functions, so the tests can solve every lesson.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/board.hpp"
#include "editor/circuit.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

// One step of a circuit that remembers: switches set by name, maybe a clock pulse, and what the named
// outputs must show afterwards.
struct SequenceStep {
    std::vector<std::pair<const char*, std::uint64_t>> set;
    bool pulse = false;
    std::vector<std::pair<const char*, std::uint64_t>> expect;
};

// One run of a program lesson, from a fresh start: the switches when the computer starts, maybe a change
// to them after some instructions (for a program that has to wait for it), and what the output must end
// up showing.
struct ProgramRun {
    std::uint16_t switches = 0;
    int changeAt = -1;  // after this many instructions the switches change to `later`; -1: they never do
    std::uint16_t later = 0;
    std::vector<std::uint16_t> shows;
};

struct LessonAct {
    const char* title;
    const char* line;
    bool ready;  // false: announced, with its lessons still to come
    std::vector<const char*> planned;  // the titles still to come
};

enum class LessonCheck : std::uint8_t {
    LightOn,         // an LED lights and nothing burns
    LightCurrent,    // ... with a current in a range
    LightSwitches,   // the LED lights exactly when the switches say
    Table,           // named inputs and outputs follow a truth table
    Sequence,        // a circuit that remembers goes through the steps
    Program,         // the computer's program shows the outputs, stores the memory and uses the instructions
    Answer,          // no circuit: questions answered with a number, one after the other
};

// A question of an Answer lesson: maybe a row of bits shown as lights, and the number that answers it,
// typed in this base. Spaces in the bits only group them, four by four, the way they are read.
struct Question {
    const char* prompt;
    const char* bits;  // '0' and '1', or "" for none
    int base;          // 2, 10 or 16: how the answer is typed
    const char* answer;
    const char* explain;  // why, shown once it's answered right
    const char* hint;     // what Dica shows for this question: the way to it, without the answer
};

// Whether what the player typed answers the question: the same number, however many zeros it starts with,
// with or without 0x or 0b in front, in capitals or not.
bool answers(const Question& question, std::string_view typed);

// What an instruction a program lesson asks for is: an opcode, and for the ALU ones the function too.
struct NeededInstruction {
    unsigned op;
    int func;      // -1: any
    const char* name;
    int alsoOp;    // another opcode that will do instead, or -1
};

struct Lesson {
    const char* id;  // for the saved progress: never change one
    int act;
    const char* title;
    const char* story;
    std::vector<const char*> steps;
    std::vector<const char*> hints;
    const char* success;

    Board (*start)();
    std::vector<PartKind> parts;      // the library in this lesson, besides what the board starts with
    std::vector<const char*> boxes;   // ready-made boxes, by key
    std::vector<PartKind> required;   // at least one of each must be on the board

    LessonCheck check = LessonCheck::LightOn;
    double minMilliamps = 0.0;  // LightCurrent
    double maxMilliamps = 0.0;
    int switches = 0;           // LightSwitches: how many, and the LED for each number of closed switches...
    const char* lit = "";       // ...as '0' and '1', for 0, 1, 2... switches closed
    bool transistorCarries = false;  // LightSwitches: the LED's current must go through a transistor

    // Table: switches by name and outputs by name, and what each output shows. Either a string with a
    // '0' or '1' per output for each row, rows counted up with the first input in the highest bits, or
    // a function for tables too big to write.
    std::vector<const char*> inputs;
    std::vector<const char*> outputs;
    const char* table = "";
    std::uint64_t (*expect)(std::span<const std::uint64_t> inputs, std::size_t output) = nullptr;

    // Sequence: the steps, from a fresh start.
    std::vector<SequenceStep> sequence;

    // Program: the lesson's board is the computer, whose chips these are. The program must make the output
    // show these values in this order and stop, leave these words in memory, and use these instructions.
    std::vector<ChipData> (*chips)() = nullptr;
    std::vector<std::uint16_t> shows;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> memory;  // address, value
    std::vector<NeededInstruction> needs;
    std::vector<ProgramRun> runs;  // with none, one run with the switches at 0, which must show `shows`

    // Program, with an operating system: the program the lesson starts with. Everything in it before the
    // label "programa" is the system, which must stay as it is, and the player's program after it may not
    // use IN, OUT or HLT: it asks the system instead.
    const char* withSystem = nullptr;

    // Program, in machine code: the ROM's program may only be .word lines, the instructions written as
    // numbers by hand, with no names like LI or OUT.
    bool machineCode = false;

    // Answer: the questions, in order.
    std::vector<Question> questions;
};

const std::vector<LessonAct>& lessonActs();
const std::vector<Lesson>& lessons();
int lessonIndex(std::string_view id);  // -1 if there's none

struct LessonResult {
    bool passed = false;
    std::string message;  // what's missing or wrong, in a sentence; the lesson's success line when it passes
};

LessonResult checkLesson(const Lesson& lesson, const Board& board, const ChipBoards* chips = nullptr);

// A short code that changes whenever the lesson's first board changes. The player's progress is kept
// under it, so that a lesson rebuilt in a new version of the game starts fresh instead of reopening a
// board made for the old one.
std::string lessonVersion(const Lesson& lesson);

// The parts the library offers in a lesson: the lesson's own, and in every lesson the basics to light
// something up: both ways of powering it, the battery and the source with its ground, a resistor, and an
// LED both ways too.
std::vector<PartKind> lessonLibrary(const Lesson& lesson);

// Whether the library offers this part in this lesson.
bool lessonAllows(const Lesson& lesson, PartKind kind, BoxId box);

}  // namespace bancada::editor
