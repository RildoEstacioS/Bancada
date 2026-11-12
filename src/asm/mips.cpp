#include "asm/mips.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <unordered_map>

#include "i18n/i18n.hpp"

namespace bancada::mips {

namespace {

constexpr std::string_view kNames[32] = {
    "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3", "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
    "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7", "$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra",
};

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool isLabelName(std::string_view text) {
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0])) != 0) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) != 0 || c == '_' || c == '.' || u >= 0x80;  // accented letters are letters too
    });
}

// A number as MIPS assemblers write them: 42, -7, 0x2A, 0b101010, or a character in quotes, 'A' or '\n'.
std::optional<std::int64_t> parseNumber(std::string_view whole) {
    std::string kept;
    for (const char c : trim(whole)) {
        if (c != '_') kept += c;
    }
    std::string_view text = kept;
    if (text.size() >= 3 && text.front() == '\'' && text.back() == '\'') {
        const std::string_view inside = text.substr(1, text.size() - 2);
        if (inside.size() == 1) return static_cast<unsigned char>(inside[0]);
        if (inside.size() == 2 && inside[0] == '\\') {
            switch (inside[1]) {
                case 'n': return 10;
                case 't': return 9;
                case '0': return 0;
                case '\\': return '\\';
                case '\'': return '\'';
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        text.remove_prefix(1);
    }
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 34) return std::nullopt;
    std::int64_t value = 0;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        int digit = -1;
        if (std::isdigit(u) != 0) digit = c - '0';
        else if (base == 16 && std::isxdigit(u) != 0) digit = std::tolower(u) - 'a' + 10;
        if (digit < 0 || digit >= base) return std::nullopt;
        value = value * base + digit;
        if (value > 0xFFFFFFFFll) return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<unsigned> parseRegister(std::string_view text) {
    if (text.size() < 2 || text[0] != '$') return std::nullopt;
    const std::string name = lower(text);
    for (unsigned i = 0; i < 32; ++i) {
        if (kNames[i] == name) return i;
    }
    if (name == "$s8") return 30;
    const std::string_view digits = std::string_view(name).substr(1);
    if (digits.size() <= 2 && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        const int number = std::stoi(std::string(digits));
        if (number < 32) return static_cast<unsigned>(number);
    }
    return std::nullopt;
}

std::uint32_t encodeR(unsigned rs, unsigned rt, unsigned rd, unsigned shamt, std::uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct;
}

std::uint32_t encodeI(std::uint32_t op, unsigned rs, unsigned rt, std::uint32_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
}

struct Line {
    int number = 0;
    std::vector<std::string> labels;
    std::string head;  // the instruction or directive, in lower case
    std::vector<std::string> operands;
};

// Cuts a line into labels, its head and its operands. A comment starts at # (or ;) outside quotes.
Line parseLine(std::string_view raw, int number) {
    Line line;
    line.number = number;
    std::string text;
    char quote = 0;
    for (const char c : raw) {
        if (quote == 0 && (c == '#' || c == ';')) break;
        if (c == '\'' || c == '"') quote = quote == 0 ? c : (quote == c ? 0 : quote);
        text += c;
    }
    std::string_view rest = trim(text);
    for (;;) {
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos) break;
        const std::string_view name = trim(rest.substr(0, colon));
        if (name.find_first_of(" \t'\"(,") != std::string_view::npos) break;
        line.labels.emplace_back(name);
        rest = trim(rest.substr(colon + 1));
    }
    if (rest.empty()) return line;
    const std::size_t space = rest.find_first_of(" \t");
    line.head = lower(rest.substr(0, space));
    if (space == std::string_view::npos) return line;
    // Commas or spaces separate the operands; spaces next to parentheses don't: 4 ( $sp ) is 4($sp).
    std::string squeezed;
    const std::string_view operands = trim(rest.substr(space));
    for (std::size_t i = 0; i < operands.size(); ++i) {
        const char c = operands[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            const std::size_t next = operands.find_first_not_of(" \t", i);
            if ((!squeezed.empty() && squeezed.back() == '(') || (next != std::string_view::npos && (operands[next] == '(' || operands[next] == ')'))) continue;
        }
        squeezed += c;
    }
    std::string current;
    quote = 0;
    const auto flush = [&] {
        if (!current.empty()) line.operands.push_back(current);
        current.clear();
    };
    for (const char c : squeezed) {
        if (c == '\'') quote = quote == 0 ? c : 0;
        if (quote == 0 && (c == ',' || std::isspace(static_cast<unsigned char>(c)) != 0)) {
            flush();
            continue;
        }
        current += c;
    }
    flush();
    return line;
}

bool fitsSigned16(std::int64_t value) { return value >= -32768 && value <= 32767; }
bool fitsUnsigned16(std::int64_t value) { return value >= 0 && value <= 65535; }

class Assembler {
public:
    Program run(std::string_view source) {
        int number = 0;
        std::size_t start = 0;
        while (start <= source.size()) {
            const std::size_t end = std::min(source.find('\n', start), source.size());
            ++number;
            lines_.push_back(parseLine(source.substr(start, end - start), number));
            start = end + 1;
        }
        for (pass_ = 1; pass_ <= 2; ++pass_) {
            text_ = kTextBase;
            data_ = kDataBase;
            inData_ = false;
            for (const Line& line : lines_) {
                line_ = line.number;
                for (const std::string& label : line.labels) define(label);
                if (line.head.empty()) continue;
                if (line.head[0] == '.') directive(line);
                else if (inData_) fail(i18n::format("Instrução no .data: escreva .text antes de {}", {line.head}));
                else instruction(line);
            }
        }
        std::stable_sort(program_.errors.begin(), program_.errors.end(), [](const assembly::Error& a, const assembly::Error& b) { return a.line < b.line; });
        for (const auto& [name, address] : labels_) program_.labels.emplace_back(name, address);
        std::sort(program_.labels.begin(), program_.labels.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        return std::move(program_);
    }

private:
    std::vector<Line> lines_;
    Program program_;
    std::unordered_map<std::string, std::uint32_t> labels_;
    int pass_ = 1;
    int line_ = 0;
    std::uint32_t text_ = kTextBase;
    std::uint32_t data_ = kDataBase;
    bool inData_ = false;

    void fail(std::string message) {
        if (pass_ == 2) program_.errors.push_back({line_, std::move(message)});
    }

    void define(const std::string& name) {
        if (pass_ != 1) return;
        if (!isLabelName(name)) {
            program_.errors.push_back({line_, i18n::format("Nome de rótulo inválido: {}", {name})});
        } else if (!labels_.emplace(name, inData_ ? data_ : text_).second) {
            program_.errors.push_back({line_, i18n::format("Rótulo repetido: {}", {name})});
        }
    }

    void emit(std::uint32_t word) {
        if (pass_ == 2) {
            program_.text.push_back(word);
            program_.lines.push_back(line_);
        }
        text_ += 4;
    }

    void emitData(std::uint32_t word) {
        if (pass_ == 2) program_.data.push_back(word);
        data_ += 4;
    }

    bool count(const Line& line, std::size_t wanted) {
        if (line.operands.size() == wanted) return true;
        fail(i18n::format("{} espera {} operando(s)", {line.head, std::to_string(wanted)}));
        return false;
    }

    unsigned reg(const std::string& text) {
        if (const std::optional<unsigned> found = parseRegister(text)) return *found;
        if (!text.empty() && (text[0] == 'R' || text[0] == 'r') && text.size() == 2 && std::isdigit(static_cast<unsigned char>(text[1])) != 0) {
            fail(i18n::format("Registrador inválido: {}. Isso é do computador de 16 bits; no MIPS, use $t0, $s0...", {text}));
        } else {
            fail(i18n::format("Registrador inválido: {} (use $zero, $t0 a $t9, $s0 a $s7, $a0 a $a3, $v0, $v1, $sp, $ra)", {text}));
        }
        return 0;
    }

    bool isRegister(const std::string& text) const { return !text.empty() && text[0] == '$'; }

    // A number, or a label's address. In the first pass the labels aren't all known yet, and count as 0.
    std::int64_t value(const std::string& text) {
        if (const std::optional<std::int64_t> number = parseNumber(text)) return *number;
        if (const auto found = labels_.find(text); found != labels_.end()) return found->second;
        if (pass_ == 2) fail(i18n::format("Valor ou rótulo desconhecido: {}", {text}));
        return 0;
    }

    std::int64_t ranged(const std::string& text, std::int64_t low, std::int64_t high) {
        const std::int64_t found = value(text);
        if (found < low || found > high) {
            fail(i18n::format("{} fora da faixa, de {} a {}", {text, std::to_string(low), std::to_string(high)}));
            return 0;
        }
        return found;
    }

    std::uint32_t word32(const std::string& text) {
        return static_cast<std::uint32_t>(ranged(text, -2147483648ll, 4294967295ll) & 0xFFFFFFFFll);
    }

    void directive(const Line& line) {
        const std::string& d = line.head;
        if (d == ".data") {
            inData_ = true;
        } else if (d == ".text") {
            inData_ = false;
        } else if (d == ".word") {
            if (line.operands.empty()) fail(i18n::tr(".word espera ao menos um valor"));
            for (const std::string& operand : line.operands) {
                // value:count repeats a value, as MARS allows: .word 0:10
                const std::size_t colon = operand.find(':');
                std::uint32_t repeat = 1;
                std::string written = operand;
                if (colon != std::string::npos) {
                    repeat = static_cast<std::uint32_t>(ranged(operand.substr(colon + 1), 1, static_cast<std::int64_t>(kMemoryWords)));
                    written = operand.substr(0, colon);
                }
                const std::uint32_t word = word32(written);
                for (std::uint32_t i = 0; i < repeat; ++i) {
                    if (inData_) emitData(word);
                    else emit(word);
                }
            }
        } else if (d == ".space") {
            if (!count(line, 1)) return;
            const auto bytes = static_cast<std::uint32_t>(ranged(line.operands[0], 0, static_cast<std::int64_t>(kMemoryWords * 4)));
            for (std::uint32_t i = 0; i < (bytes + 3) / 4; ++i) {
                if (inData_) emitData(0);
                else emit(0);
            }
        } else if (d == ".globl" || d == ".global" || d == ".align" || d == ".ent" || d == ".end") {
            // Nothing to do: there's a single file, and everything here is already a whole word.
        } else if (d == ".asciiz" || d == ".ascii" || d == ".byte" || d == ".half") {
            fail(i18n::format("{} ainda não existe neste computador: ele lê e grava palavras inteiras. Use .word", {d}));
        } else {
            fail(i18n::format("Diretiva desconhecida: {}", {d}));
        }
    }

    void branch(std::uint32_t op, unsigned rs, unsigned rt, const std::string& target) {
        const std::int64_t to = value(target);
        const std::int64_t offset = (to - static_cast<std::int64_t>(text_) - 4) / 4;
        const bool known = parseNumber(target).has_value() || labels_.count(target) != 0;  // else value() has told
        if (pass_ == 2 && known && ((to & 3) != 0 || !fitsSigned16(offset))) fail(i18n::format("Desvio longe demais para {}: use j", {target}));
        emit(encodeI(op, rs, rt, static_cast<std::uint32_t>(offset & 0xFFFF)));
    }

    void jump(std::uint32_t op, const std::string& target) {
        const auto to = static_cast<std::uint32_t>(value(target) & 0xFFFFFFFFll);
        const bool known = parseNumber(target).has_value() || labels_.count(target) != 0;
        if (pass_ == 2 && known && (((to ^ (text_ + 4)) & 0xF0000000u) != 0 || (to & 3) != 0)) fail(i18n::format("Endereço de salto inválido: {}", {target}));
        emit((op << 26) | ((to >> 2) & 0x03FFFFFFu));
    }

    // li: one instruction when the value fits in 16 bits, else lui and ori through $at, as MARS does.
    std::size_t loadImmediate(unsigned rd, const std::string& text) {
        const std::optional<std::int64_t> number = parseNumber(text);
        if (!number || *number < -2147483648ll || *number > 4294967295ll) {
            fail(i18n::format("li espera um número de 32 bits: {}", {text}));
            emit(0);
            return 1;
        }
        if (fitsSigned16(*number)) {
            emit(encodeI(kOpAddiu, kZero, rd, static_cast<std::uint32_t>(*number & 0xFFFF)));
            return 1;
        }
        if (fitsUnsigned16(*number)) {
            emit(encodeI(kOpOri, kZero, rd, static_cast<std::uint32_t>(*number)));
            return 1;
        }
        const auto whole = static_cast<std::uint32_t>(*number & 0xFFFFFFFFll);
        emit(encodeI(kOpLui, kZero, kAt, whole >> 16));
        emit(encodeI(kOpOri, kAt, rd, whole & 0xFFFFu));
        return 2;
    }

    // A second operand that's a number is put in $at first, so the branch compares two registers.
    unsigned second(const std::string& text) {
        if (isRegister(text)) return reg(text);
        emit(encodeI(kOpAddiu, kZero, kAt, static_cast<std::uint32_t>(ranged(text, -32768, 32767) & 0xFFFF)));
        return kAt;
    }

    void memoryAccess(std::uint32_t op, const Line& line) {
        if (!count(line, 2)) return;
        const unsigned rt = reg(line.operands[0]);
        const std::string& address = line.operands[1];
        const std::size_t open = address.find('(');
        if (open != std::string::npos) {
            if (address.back() != ')') {
                fail(i18n::format("Endereço inválido: {} (use 4($sp))", {address}));
                emit(0);
                return;
            }
            const unsigned base = reg(address.substr(open + 1, address.size() - open - 2));
            const std::string offset = address.substr(0, open);
            const std::int64_t imm = offset.empty() ? 0 : ranged(offset, -32768, 32767);
            emit(encodeI(op, base, rt, static_cast<std::uint32_t>(imm & 0xFFFF)));
            return;
        }
        // A label (or an address) alone: its high half goes to $at, and the low half is the offset.
        const auto whole = static_cast<std::uint32_t>(value(address) & 0xFFFFFFFFll);
        const std::uint32_t high = ((whole + 0x8000u) >> 16) & 0xFFFFu;
        emit(encodeI(kOpLui, kZero, kAt, high));
        emit(encodeI(op, kAt, rt, whole & 0xFFFFu));
    }

    void instruction(const Line& line) {
        const std::string& m = line.head;
        const std::vector<std::string>& o = line.operands;
        static const std::unordered_map<std::string, std::uint32_t> kThree = {
            {"add", kFnAdd}, {"addu", kFnAddu}, {"sub", kFnSub}, {"subu", kFnSubu}, {"and", kFnAnd}, {"or", kFnOr},
            {"xor", kFnXor}, {"nor", kFnNor}, {"slt", kFnSlt}, {"sltu", kFnSltu}};
        // With a number in place of the third register, the instruction's immediate form: add $t0, $t0, 1.
        static const std::unordered_map<std::string, std::uint32_t> kImmediateOf = {
            {"add", kOpAddi}, {"addu", kOpAddiu}, {"and", kOpAndi}, {"or", kOpOri}, {"xor", kOpXori}, {"slt", kOpSlti}, {"sltu", kOpSltiu}};
        static const std::unordered_map<std::string, std::uint32_t> kImmediate = {
            {"addi", kOpAddi}, {"addiu", kOpAddiu}, {"slti", kOpSlti}, {"sltiu", kOpSltiu}, {"andi", kOpAndi}, {"ori", kOpOri}, {"xori", kOpXori}};
        static const std::unordered_map<std::string, std::uint32_t> kShift = {{"sll", kFnSll}, {"srl", kFnSrl}, {"sra", kFnSra}};

        const auto immediateForm = [&](std::uint32_t op, unsigned rt, unsigned rs, const std::string& text) {
            const bool zeroExtended = op == kOpAndi || op == kOpOri || op == kOpXori;
            const std::int64_t imm = zeroExtended ? ranged(text, 0, 65535) : ranged(text, -32768, 32767);
            emit(encodeI(op, rs, rt, static_cast<std::uint32_t>(imm & 0xFFFF)));
        };

        if (const auto three = kThree.find(m); three != kThree.end()) {
            if (!count(line, 3)) return emit(0);
            const unsigned rd = reg(o[0]);
            const unsigned rs = reg(o[1]);
            if (!isRegister(o[2])) {
                if (m == "sub" || m == "subu") {
                    const std::int64_t imm = ranged(o[2], -32767, 32768);
                    return emit(encodeI(m == "sub" ? kOpAddi : kOpAddiu, rs, rd, static_cast<std::uint32_t>(-imm & 0xFFFF)));
                }
                if (m == "nor") return fail(i18n::tr("nor não tem forma com número")), emit(0);
                return immediateForm(kImmediateOf.at(m), rd, rs, o[2]);
            }
            return emit(encodeR(rs, reg(o[2]), rd, 0, three->second));
        }
        if (const auto immediate = kImmediate.find(m); immediate != kImmediate.end()) {
            if (!count(line, 3)) return emit(0);
            const unsigned rt = reg(o[0]);
            return immediateForm(immediate->second, rt, reg(o[1]), o[2]);
        }
        if (const auto shift = kShift.find(m); shift != kShift.end()) {
            if (!count(line, 3)) return emit(0);
            const unsigned rd = reg(o[0]);
            const unsigned rt = reg(o[1]);
            return emit(encodeR(0, rt, rd, static_cast<unsigned>(ranged(o[2], 0, 31)), shift->second));
        }
        if (m == "lw" || m == "sw") return memoryAccess(m == "lw" ? kOpLw : kOpSw, line);
        if (m == "lui") {
            if (!count(line, 2)) return emit(0);
            const unsigned rt = reg(o[0]);
            return emit(encodeI(kOpLui, 0, rt, static_cast<std::uint32_t>(ranged(o[1], 0, 65535))));
        }
        if (m == "beq" || m == "bne") {
            if (!count(line, 3)) return emit(0);
            const unsigned rs = reg(o[0]);
            const unsigned rt = second(o[1]);
            return branch(m == "beq" ? kOpBeq : kOpBne, rs, rt, o[2]);
        }
        if (m == "j" || m == "jal") {
            if (!count(line, 1)) return emit(0);
            return jump(m == "j" ? kOpJ : kOpJal, o[0]);
        }
        if (m == "jr") {
            if (!count(line, 1)) return emit(0);
            return emit(encodeR(reg(o[0]), 0, 0, 0, kFnJr));
        }
        if (m == "syscall") {
            if (!count(line, 0)) return emit(0);
            return emit(kFnSyscall);
        }
        if (m == "nop") {
            if (!count(line, 0)) return emit(0);
            return emit(0);
        }

        // The pseudo-instructions, written with the real ones.
        if (m == "li") {
            if (!count(line, 2)) return emit(0);
            loadImmediate(reg(o[0]), o[1]);
            return;
        }
        if (m == "la") {
            if (!count(line, 2)) return emit(0), emit(0);
            const unsigned rd = reg(o[0]);
            const auto whole = static_cast<std::uint32_t>(value(o[1]) & 0xFFFFFFFFll);
            emit(encodeI(kOpLui, 0, kAt, whole >> 16));
            return emit(encodeI(kOpOri, kAt, rd, whole & 0xFFFFu));
        }
        if (m == "move") {
            if (!count(line, 2)) return emit(0);
            const unsigned rd = reg(o[0]);
            return emit(encodeR(kZero, reg(o[1]), rd, 0, kFnAddu));
        }
        if (m == "not") {
            if (!count(line, 2)) return emit(0);
            const unsigned rd = reg(o[0]);
            return emit(encodeR(reg(o[1]), kZero, rd, 0, kFnNor));
        }
        if (m == "neg") {
            if (!count(line, 2)) return emit(0);
            const unsigned rd = reg(o[0]);
            return emit(encodeR(kZero, reg(o[1]), rd, 0, kFnSub));
        }
        if (m == "subi") {
            if (!count(line, 3)) return emit(0);
            const unsigned rt = reg(o[0]);
            const unsigned rs = reg(o[1]);
            const std::int64_t imm = ranged(o[2], -32767, 32768);
            return emit(encodeI(kOpAddi, rs, rt, static_cast<std::uint32_t>(-imm & 0xFFFF)));
        }
        if (m == "b") {
            if (!count(line, 1)) return emit(0);
            return branch(kOpBeq, kZero, kZero, o[0]);
        }
        if (m == "beqz" || m == "bnez") {
            if (!count(line, 2)) return emit(0);
            return branch(m == "beqz" ? kOpBeq : kOpBne, reg(o[0]), kZero, o[1]);
        }
        if (m == "blt" || m == "bge" || m == "bgt" || m == "ble") {
            // slt $at, then a branch on $at: blt and bge compare rs < rt, bgt and ble rt < rs.
            if (!count(line, 3)) return emit(0), emit(0);
            const unsigned rs = reg(o[0]);
            const unsigned rt = second(o[1]);
            const bool swapped = m == "bgt" || m == "ble";
            emit(encodeR(swapped ? rt : rs, swapped ? rs : rt, kAt, 0, kFnSlt));
            return branch(m == "blt" || m == "bgt" ? kOpBne : kOpBeq, kAt, kZero, o[2]);
        }

        // Someone used to the 16-bit computer, or to other MIPS instructions, is told what this one has.
        static constexpr std::pair<std::string_view, std::string_view> kElsewhere[] = {
            {"ld", "lw"}, {"st", "sw"}, {"jmp", "j"}, {"call", "jal"}, {"mov", "move"}, {"shl", "sll"}, {"shr", "srl"},
            {"out", "li $v0, 1; syscall"}, {"hlt", "li $v0, 10; syscall"}, {"liw", "li"}, {"in", "li $v0, 5; syscall"}};
        for (const auto& [theirs, ours] : kElsewhere) {
            if (m == theirs) return fail(i18n::format("Instrução desconhecida: {}. No MIPS, use {}", {m, std::string(ours)})), emit(0);
        }
        if (m == "mul" || m == "mult" || m == "multu" || m == "div" || m == "divu" || m == "mflo" || m == "mfhi" || m == "rem") {
            return fail(i18n::format("{} não existe neste computador: ele não tem multiplicador. Some num laço", {m})), emit(0);
        }
        if (m == "lb" || m == "lbu" || m == "sb" || m == "lh" || m == "lhu" || m == "sh") {
            return fail(i18n::format("{} ainda não existe neste computador: ele lê e grava palavras inteiras. Use lw e sw", {m})), emit(0);
        }
        fail(i18n::format("Instrução desconhecida: {}", {m}));
        emit(0);
    }
};

std::int32_t signedOf(std::uint32_t word) { return static_cast<std::int32_t>(word); }

}  // namespace

std::string_view registerName(unsigned index) { return kNames[index % 32]; }

Program assemble(std::string_view source) { return Assembler().run(source); }

std::string disassemble(std::uint32_t word, std::uint32_t address) {
    const std::uint32_t op = word >> 26;
    const unsigned rs = (word >> 21) & 31u;
    const unsigned rt = (word >> 16) & 31u;
    const unsigned rd = (word >> 11) & 31u;
    const unsigned shamt = (word >> 6) & 31u;
    const std::uint32_t funct = word & 63u;
    const std::uint32_t imm = word & 0xFFFFu;
    const int simm = static_cast<std::int16_t>(static_cast<std::uint16_t>(imm));
    const auto name = [](unsigned index) { return std::string(registerName(index)); };

    char out[64];
    const auto format = [&](const auto&... values) {
        std::snprintf(out, sizeof out, values...);
        return std::string(out);
    };
    const auto raw = [&] { return format(".word 0x%08X", static_cast<unsigned>(word)); };

    if (op == kOpSpecial) {
        if (word == 0) return "nop";
        switch (funct) {
            case kFnSll:
            case kFnSrl:
            case kFnSra:
                if (rs != 0) return raw();
                return format("%s %s, %s, %u", funct == kFnSll ? "sll" : funct == kFnSrl ? "srl" : "sra", name(rd).c_str(), name(rt).c_str(), shamt);
            case kFnJr:
                if (rt != 0 || rd != 0 || shamt != 0) return raw();
                return "jr " + name(rs);
            case kFnSyscall: return word == kFnSyscall ? std::string("syscall") : raw();
            default: break;
        }
        static const std::unordered_map<std::uint32_t, const char*> kThree = {
            {kFnAdd, "add"}, {kFnAddu, "addu"}, {kFnSub, "sub"}, {kFnSubu, "subu"}, {kFnAnd, "and"},
            {kFnOr, "or"},   {kFnXor, "xor"},   {kFnNor, "nor"}, {kFnSlt, "slt"},   {kFnSltu, "sltu"}};
        const auto found = kThree.find(funct);
        if (found == kThree.end() || shamt != 0) return raw();
        return format("%s %s, %s, %s", found->second, name(rd).c_str(), name(rs).c_str(), name(rt).c_str());
    }
    switch (op) {
        case kOpJ:
        case kOpJal: {
            const std::uint32_t target = ((address + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
            return format("%s 0x%08X", op == kOpJ ? "j" : "jal", static_cast<unsigned>(target));
        }
        case kOpBeq:
        case kOpBne: {
            const std::uint32_t target = address + 4 + static_cast<std::uint32_t>(simm * 4);
            return format("%s %s, %s, 0x%08X", op == kOpBeq ? "beq" : "bne", name(rs).c_str(), name(rt).c_str(), static_cast<unsigned>(target));
        }
        case kOpAddi: return format("addi %s, %s, %d", name(rt).c_str(), name(rs).c_str(), simm);
        case kOpAddiu: return format("addiu %s, %s, %d", name(rt).c_str(), name(rs).c_str(), simm);
        case kOpSlti: return format("slti %s, %s, %d", name(rt).c_str(), name(rs).c_str(), simm);
        case kOpSltiu: return format("sltiu %s, %s, %d", name(rt).c_str(), name(rs).c_str(), simm);
        case kOpAndi: return format("andi %s, %s, 0x%04X", name(rt).c_str(), name(rs).c_str(), static_cast<unsigned>(imm));
        case kOpOri: return format("ori %s, %s, 0x%04X", name(rt).c_str(), name(rs).c_str(), static_cast<unsigned>(imm));
        case kOpXori: return format("xori %s, %s, 0x%04X", name(rt).c_str(), name(rs).c_str(), static_cast<unsigned>(imm));
        case kOpLui:
            if (rs != 0) return raw();
            return format("lui %s, 0x%04X", name(rt).c_str(), static_cast<unsigned>(imm));
        case kOpLw: return format("lw %s, %d(%s)", name(rt).c_str(), simm, name(rs).c_str());
        case kOpSw: return format("sw %s, %d(%s)", name(rt).c_str(), simm, name(rs).c_str());
        default: return raw();
    }
}

void Machine::step() {
    if (halted) return;
    const std::uint32_t word = fetch();
    const std::uint32_t op = word >> 26;
    const unsigned rs = (word >> 21) & 31u;
    const unsigned rt = (word >> 16) & 31u;
    const unsigned rd = (word >> 11) & 31u;
    const unsigned shamt = (word >> 6) & 31u;
    const std::uint32_t funct = word & 63u;
    const std::uint32_t zeroExtended = word & 0xFFFFu;
    const std::uint32_t signExtended = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(static_cast<std::uint16_t>(zeroExtended))));
    const std::uint32_t a = registers[rs];
    const std::uint32_t b = registers[rt];
    std::uint32_t next = pc + 4;
    const auto stop = [&](std::string why) {
        halted = true;
        fault = std::move(why);
        next = pc;
    };

    if (op == kOpSpecial) {
        switch (funct) {
            case kFnSll: registers[rd] = b << shamt; break;
            case kFnSrl: registers[rd] = b >> shamt; break;
            case kFnSra: registers[rd] = static_cast<std::uint32_t>(signedOf(b) >> shamt); break;
            case kFnJr: next = a; break;
            case kFnSyscall:
                switch (registers[kV0]) {
                    case kPrintInt: output += std::to_string(signedOf(registers[kA0])); break;
                    case kPrintChar: output += static_cast<char>(registers[kA0] & 0xFFu); break;
                    case kReadInt: registers[kV0] = switches; break;
                    case kExit:
                        halted = true;
                        next = pc;
                        break;
                    default: stop(i18n::format("syscall {} não existe neste computador", {std::to_string(registers[kV0])})); break;
                }
                break;
            case kFnAdd:
            case kFnAddu: registers[rd] = a + b; break;
            case kFnSub:
            case kFnSubu: registers[rd] = a - b; break;
            case kFnAnd: registers[rd] = a & b; break;
            case kFnOr: registers[rd] = a | b; break;
            case kFnXor: registers[rd] = a ^ b; break;
            case kFnNor: registers[rd] = ~(a | b); break;
            case kFnSlt: registers[rd] = signedOf(a) < signedOf(b) ? 1u : 0u; break;
            case kFnSltu: registers[rd] = a < b ? 1u : 0u; break;
            default: stop(i18n::tr("Instrução que este computador não tem")); break;
        }
    } else {
        switch (op) {
            case kOpJ: next = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2); break;
            case kOpJal:
                registers[kRa] = pc + 4;
                next = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
                break;
            case kOpBeq:
                if (a == b) next = pc + 4 + (signExtended << 2);
                break;
            case kOpBne:
                if (a != b) next = pc + 4 + (signExtended << 2);
                break;
            case kOpAddi:
            case kOpAddiu: registers[rt] = a + signExtended; break;
            case kOpSlti: registers[rt] = signedOf(a) < signedOf(signExtended) ? 1u : 0u; break;
            case kOpSltiu: registers[rt] = a < signExtended ? 1u : 0u; break;
            case kOpAndi: registers[rt] = a & zeroExtended; break;
            case kOpOri: registers[rt] = a | zeroExtended; break;
            case kOpXori: registers[rt] = a ^ zeroExtended; break;
            case kOpLui: registers[rt] = zeroExtended << 16; break;
            case kOpLw: registers[rt] = memory(a + signExtended); break;
            case kOpSw: memory(a + signExtended) = b; break;
            default: stop(i18n::tr("Instrução que este computador não tem")); break;
        }
    }
    registers[kZero] = 0;
    pc = next;
    ++steps;
}

void Machine::run(std::size_t limit) {
    for (std::size_t i = 0; i < limit && !halted; ++i) step();
}

Machine boot(const Program& program) {
    Machine machine;
    for (std::size_t i = 0; i < program.text.size() && i < kMemoryWords; ++i) machine.rom[i] = program.text[i];
    for (std::size_t i = 0; i < program.data.size() && i < kMemoryWords; ++i) machine.ram[i] = program.data[i];
    machine.registers[kSp] = kStackTop;
    machine.registers[28] = kGlobalPointer;
    return machine;
}

}  // namespace bancada::mips
