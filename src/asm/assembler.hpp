// The game's own 16-bit instruction set, and its assembler. Every instruction is one 16-bit word:
//
//   R:  op4 rd3 rs3 rt3 func3      ALU operations: op 0, and func picks one
//   I6: op4 rd3 rs3 imm6           ADDI, LD, ST, BEQ, BNE
//   I9: op4 rd3 imm9               LI, LUI
//   J:  op4 imm12                  JMP, CALL
//
// R0 always reads 0. A CALL leaves the return address in R7. Code and data live in separate memories.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bancada::assembly {

enum Op : std::uint16_t {
    kOpAlu = 0, kOpAddi = 1, kOpLi = 2, kOpLui = 3, kOpLd = 4, kOpSt = 5, kOpBeq = 6, kOpBne = 7,
    kOpJmp = 8, kOpCall = 9, kOpJr = 10, kOpOut = 11, kOpIn = 12, kOpHalt = 15,
};

enum Func : std::uint16_t { kAnd = 0, kOr = 1, kXor = 2, kAdd = 3, kSub = 4, kNot = 5, kShl = 6, kShr = 7 };

struct Error {
    int line = 0;  // counted from 1
    std::string message;
};

struct Result {
    std::vector<std::uint16_t> words;
    std::vector<int> lines;  // for each word, the line it came from, counted from 1
    std::vector<Error> errors;
    bool ok() const { return errors.empty(); }
};

// Two passes, so a label can be used before it's defined. All problems are reported, not just the first.
Result assemble(std::string_view source);

// One word back as text, for a ROM whose program has no source. The address is where the word sits, since
// a branch's target depends on it. A word that means nothing comes back as .word, so that assembling the
// text always gives the same word.
std::string disassemble(std::uint16_t word, std::size_t address);

}  // namespace bancada::assembly
