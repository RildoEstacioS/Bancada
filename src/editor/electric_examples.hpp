// The ready-made electric circuits in the library's Exemplos folder.
#pragma once

#include "editor/board.hpp"

namespace bancada::editor {

struct LedExample {
    Board board;
    PartId toggle = 0;  // the switch
    PartId light = 0;
    PartId resistor = 0;
};

// A battery, a switch, a resistor and an LED: the first circuit there is.
LedExample buildLedExample();

struct NandExample {
    Board board;
    PartId a = 0;
    PartId b = 0;
    PartId output = 0;
};

// A NAND made of two transistors and three resistors, with logic switches and an LED around it.
NandExample buildTransistorNand();

struct BlinkerExample {
    Board board;
    PartId left = 0;   // the two LEDs
    PartId right = 0;
};

// Two transistors, two capacitors and four resistors: two LEDs that take turns, with nothing telling them to.
BlinkerExample buildBlinker();

}  // namespace bancada::editor
