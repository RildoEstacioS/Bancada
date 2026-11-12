#include "asm/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>

#include "i18n/i18n.hpp"

namespace bancada::assembly {

namespace {

constexpr int kImm6Min = -32;
constexpr int kImm6Max = 31;
constexpr int kImm9Max = 511;
constexpr int kImm12Max = 4095;

std::string upper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
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
        return std::isalnum(u) != 0 || c == '_' || u >= 0x80;  // accented letters are letters too
    });
}

std::optional<int> parseNumber(std::string_view whole) {
    // An underscore only separates digits, to read a long number by its fields: 0b0010_001_000000111.
    std::string kept;
    for (const char c : trim(whole)) {
        if (c != '_') kept += c;
    }
    std::string_view text = kept;
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
    if (text.empty() || text.size() > 16) return std::nullopt;
    int value = 0;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        int digit = -1;
        if (std::isdigit(u) != 0) digit = c - '0';
        if (std::isalpha(u) != 0) digit = std::toupper(u) - 'A' + 10;
        if (digit < 0 || digit >= base) return std::nullopt;
        value = value * base + digit;
        if (value > 0xFFFFF) return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<std::uint16_t> parseRegister(std::string_view text) {
    text = trim(text);
    if (text.size() != 2 || (text[0] != 'R' && text[0] != 'r') || text[1] < '0' || text[1] > '7') return std::nullopt;
    return static_cast<std::uint16_t>(text[1] - '0');
}

std::vector<std::string_view> splitOperands(std::string_view text) {
    std::vector<std::string_view> out;
    text = trim(text);
    if (text.empty()) return out;
    while (true) {
        const std::size_t comma = text.find(',');
        out.push_back(trim(text.substr(0, comma)));
        if (comma == std::string_view::npos) return out;
        text.remove_prefix(comma + 1);
    }
}

std::uint16_t encodeR(Func func, std::uint16_t rd, std::uint16_t rs, std::uint16_t rt) {
    return static_cast<std::uint16_t>((kOpAlu << 12) | (rd << 9) | (rs << 6) | (rt << 3) | func);
}

std::uint16_t encodeI6(Op op, std::uint16_t rd, std::uint16_t rs, int imm) {
    return static_cast<std::uint16_t>((op << 12) | (rd << 9) | (rs << 6) | (static_cast<unsigned>(imm) & 63u));
}

std::uint16_t encodeI9(Op op, std::uint16_t rd, int imm) {
    return static_cast<std::uint16_t>((op << 12) | (rd << 9) | (static_cast<unsigned>(imm) & 511u));
}

std::uint16_t encodeJ(Op op, int target) {
    return static_cast<std::uint16_t>((op << 12) | (static_cast<unsigned>(target) & 4095u));
}

struct Line {
    int number = 0;
    std::string mnemonic;
    std::vector<std::string_view> operands;
    std::size_t address = 0;
};

// How many words a line takes, known in the first pass so that labels get their addresses.
std::size_t sizeOf(const Line& line) {
    if (line.mnemonic == "LIW" && line.operands.size() == 2) {
        const std::optional<int> value = parseNumber(line.operands[1]);
        if (!value) return 1;
        const int low = *value & 127;
        return 1 + static_cast<std::size_t>((low + kImm6Max - 1) / kImm6Max);
    }
    return 1;
}

class Assembler {
public:
    Result run(std::string_view source) {
        collect(source);
        for (const Line& line : lines_) {
            currentLine_ = line.number;
            encode(line);
        }
        return std::move(result_);
    }

private:
    void fail(int line, std::string message) { result_.errors.push_back({line, std::move(message)}); }

    void collect(std::string_view source) {
        int number = 0;
        std::size_t address = 0;
        while (!source.empty() || number == 0) {
            const std::size_t end = source.find('\n');
            std::string_view text = source.substr(0, end);
            source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
            ++number;
            const std::size_t comment = text.find_first_of(";#");  // ; or #, as in many assemblers
            if (comment != std::string_view::npos) text = text.substr(0, comment);
            text = trim(text);

            const std::size_t colon = text.find(':');
            if (colon != std::string_view::npos) {
                const std::string_view name = trim(text.substr(0, colon));
                const std::string key = upper(name);
                if (!isLabelName(name)) {
                    fail(number, i18n::format("Nome de rótulo inválido: {}", {name}));
                } else if (!labels_.emplace(key, static_cast<int>(address)).second) {
                    fail(number, i18n::format("Rótulo repetido: {}", {name}));
                }
                text = trim(text.substr(colon + 1));
            }
            if (text.empty()) continue;

            std::size_t split = 0;
            while (split < text.size() && std::isspace(static_cast<unsigned char>(text[split])) == 0) ++split;
            Line line;
            line.number = number;
            line.mnemonic = upper(text.substr(0, split));
            line.operands = splitOperands(text.substr(split));
            line.address = address;
            address += sizeOf(line);
            lines_.push_back(std::move(line));
        }
    }

    bool expect(const Line& line, std::size_t count) {
        if (line.operands.size() == count) return true;
        fail(line.number, i18n::format("{} espera {} operando(s)", {line.mnemonic, std::to_string(count)}));
        return false;
    }

    std::uint16_t reg(const Line& line, std::string_view text) {
        const std::optional<std::uint16_t> value = parseRegister(text);
        if (value) return *value;
        if (!text.empty() && text[0] == '$') {
            fail(line.number, i18n::format("Registrador inválido: {}. Isso é do MIPS; aqui os registradores são R0 a R7", {text}));
        } else {
            fail(line.number, i18n::format("Registrador inválido: {} (use R0 a R7)", {text}));
        }
        return 0;
    }

    std::optional<int> numberOrLabel(const Line& line, std::string_view text) {
        if (const std::optional<int> number = parseNumber(text)) return number;
        const auto found = labels_.find(upper(text));
        if (found != labels_.end()) return found->second;
        fail(line.number, i18n::format("Valor ou rótulo desconhecido: {}", {text}));
        return std::nullopt;
    }

    int ranged(const Line& line, std::string_view text, int low, int high) {
        const std::optional<int> value = numberOrLabel(line, text);
        if (!value) return 0;
        if (*value < low || *value > high) {
            fail(line.number, i18n::format("{} fora da faixa, de {} a {}", {text, std::to_string(low), std::to_string(high)}));
            return 0;
        }
        return *value;
    }

    void emit(std::uint16_t word) {
        result_.words.push_back(word);
        result_.lines.push_back(currentLine_);
    }

    // imm(Rs), or just (Rs) for an offset of 0.
    bool parseAddress(const Line& line, std::string_view text, int& offset, std::uint16_t& base) {
        const std::size_t open = text.find('(');
        const std::size_t close = text.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
            fail(line.number, i18n::format("Endereço inválido: {} (use 4(R1))", {text}));
            return false;
        }
        const std::string_view number = trim(text.substr(0, open));
        offset = number.empty() ? 0 : ranged(line, number, kImm6Min, kImm6Max);
        base = reg(line, text.substr(open + 1, close - open - 1));
        return true;
    }

    void encode(const Line& line) {
        const std::string& m = line.mnemonic;
        const auto& o = line.operands;
        const auto threeRegs = [&](Func func) {
            if (expect(line, 3)) emit(encodeR(func, reg(line, o[0]), reg(line, o[1]), reg(line, o[2])));
        };
        const auto twoRegs = [&](Func func) {
            if (expect(line, 2)) emit(encodeR(func, reg(line, o[0]), reg(line, o[1]), 0));
        };
        if (m == "ADD") return threeRegs(kAdd);
        if (m == "SUB") return threeRegs(kSub);
        if (m == "AND") return threeRegs(kAnd);
        if (m == "OR") return threeRegs(kOr);
        if (m == "XOR") return threeRegs(kXor);
        if (m == "NOT") return twoRegs(kNot);
        if (m == "SHL") return twoRegs(kShl);
        if (m == "SHR") return twoRegs(kShr);
        if (m == "MOV") {
            if (expect(line, 2)) emit(encodeR(kAdd, reg(line, o[0]), reg(line, o[1]), 0));
            return;
        }
        if (m == "NOP") {
            if (expect(line, 0)) emit(encodeR(kAdd, 0, 0, 0));
            return;
        }
        if (m == "ADDI") {
            if (expect(line, 3)) emit(encodeI6(kOpAddi, reg(line, o[0]), reg(line, o[1]), ranged(line, o[2], kImm6Min, kImm6Max)));
            return;
        }
        if (m == "LI" || m == "LUI") {
            if (expect(line, 2)) emit(encodeI9(m == "LI" ? kOpLi : kOpLui, reg(line, o[0]), ranged(line, o[1], 0, kImm9Max)));
            return;
        }
        if (m == "LIW") return encodeWide(line);
        if (m == "LD" || m == "ST") {
            if (!expect(line, 2)) return;
            int offset = 0;
            std::uint16_t base = 0;
            const std::uint16_t data = reg(line, o[0]);
            if (parseAddress(line, o[1], offset, base)) emit(encodeI6(m == "LD" ? kOpLd : kOpSt, data, base, offset));
            return;
        }
        if (m == "BEQ" || m == "BNE") {
            if (!expect(line, 3)) return;
            const std::optional<int> target = numberOrLabel(line, o[2]);
            if (!target) return;
            const int offset = *target - static_cast<int>(line.address) - 1;
            if (offset < kImm6Min || offset > kImm6Max) {
                fail(line.number, i18n::tr("Desvio longe demais: use JMP para saltos maiores"));
                return;
            }
            emit(encodeI6(m == "BEQ" ? kOpBeq : kOpBne, reg(line, o[0]), reg(line, o[1]), offset));
            return;
        }
        if (m == "JMP" || m == "CALL") {
            if (expect(line, 1)) emit(encodeJ(m == "JMP" ? kOpJmp : kOpCall, ranged(line, o[0], 0, kImm12Max)));
            return;
        }
        if (m == "JR") {
            if (expect(line, 1)) emit(encodeI6(kOpJr, 0, reg(line, o[0]), 0));
            return;
        }
        if (m == "RET") {
            if (expect(line, 0)) emit(encodeI6(kOpJr, 0, 7, 0));
            return;
        }
        if (m == "OUT") {
            if (expect(line, 1)) emit(encodeI6(kOpOut, 0, reg(line, o[0]), 0));
            return;
        }
        if (m == "IN") {
            if (expect(line, 1)) emit(encodeI9(kOpIn, reg(line, o[0]), 0));
            return;
        }
        if (m == "HLT") {
            if (expect(line, 0)) emit(static_cast<std::uint16_t>(kOpHalt << 12));
            return;
        }
        if (m == ".WORD") {
            if (expect(line, 1)) {
                emit(static_cast<std::uint16_t>(ranged(line, o[0], -32768, 65535)));
            }
            return;
        }
        // Someone who knows another assembly, MIPS most of all, is told what this computer has instead.
        static constexpr std::pair<std::string_view, std::string_view> kElsewhere[] = {
            {"LW", "LD"}, {"SW", "ST"}, {"J", "JMP"}, {"B", "JMP"}, {"JAL", "CALL"}, {"JALR", "CALL"}, {"SLL", "SHL"}, {"SRL", "SHR"},
            {"LA", "LIW"}, {"MOVE", "MOV"}, {"ADDU", "ADD"}, {"SUBU", "SUB"}, {"ADDIU", "ADDI"}, {"ANDI", "AND"}, {"ORI", "OR"},
            {"XORI", "XOR"}, {"NOR", "OR e NOT"}, {"MV", "MOV"}, {"BEQZ", "BEQ Rx, R0"}, {"BNEZ", "BNE Rx, R0"}};
        for (const auto& [theirs, ours] : kElsewhere) {
            if (m == theirs) {
                fail(line.number, i18n::format("Instrução desconhecida: {}. Neste computador, use {}", {m, std::string(ours)}));
                return;
            }
        }
        if (m == "BGE" || m == "BGT" || m == "BLE" || m == "BLT" || m == "SLT" || m == "SLTI") {
            fail(line.number, i18n::format("Instrução desconhecida: {}. Aqui só há BEQ e BNE: compare com SUB e veja o resultado", {m}));
            return;
        }
        if (m == "SYSCALL" || m == "ECALL") {
            fail(line.number, i18n::format("Instrução desconhecida: {}. Aqui se usa OUT para mostrar e HLT para parar", {m}));
            return;
        }
        fail(line.number, i18n::format("Instrução desconhecida: {}", {m}));
    }

    // Any 16-bit number: LUI for the high bits, and as many ADDI as the low seven need.
    void encodeWide(const Line& line) {
        if (!expect(line, 2)) return;
        const std::uint16_t rd = reg(line, line.operands[0]);
        const std::optional<int> value = parseNumber(line.operands[1]);
        if (!value || *value < 0 || *value > 65535) {
            fail(line.number, i18n::tr("LIW espera um número de 0 a 65535"));
            return;
        }
        emit(encodeI9(kOpLui, rd, *value >> 7));
        int low = *value & 127;
        while (low > 0) {
            const int step = std::min(low, kImm6Max);
            emit(encodeI6(kOpAddi, rd, rd, step));
            low -= step;
        }
    }

    std::vector<Line> lines_;
    int currentLine_ = 0;
    std::unordered_map<std::string, int> labels_;
    Result result_;
};

}  // namespace

Result assemble(std::string_view source) { return Assembler().run(source); }

std::string disassemble(std::uint16_t word, std::size_t address) {
    const unsigned op = word >> 12;
    const unsigned rd = (word >> 9) & 7u;
    const unsigned rs = (word >> 6) & 7u;
    const unsigned rt = (word >> 3) & 7u;
    const unsigned func = word & 7u;
    const int imm6 = (word & 32u) != 0 ? static_cast<int>(word & 63u) - 64 : static_cast<int>(word & 63u);
    const unsigned imm9 = word & 511u;
    const unsigned imm12 = word & 4095u;

    char out[40];
    const auto format = [&](const auto&... values) {
        std::snprintf(out, sizeof out, values...);
        return std::string(out);
    };
    const auto raw = [&] { return format(".word 0x%04X", static_cast<unsigned>(word)); };

    switch (op) {
        case kOpAlu: {
            static constexpr const char* kThree[] = {"AND", "OR", "XOR", "ADD", "SUB"};
            if (func == kAdd && rd == 0 && rs == 0 && rt == 0) return "NOP";
            if (func == kAdd && rt == 0) return format("MOV R%u, R%u", rd, rs);
            if (func <= kSub) return format("%s R%u, R%u, R%u", kThree[func], rd, rs, rt);
            if (rt != 0) return raw();
            return format("%s R%u, R%u", func == kNot ? "NOT" : func == kShl ? "SHL" : "SHR", rd, rs);
        }
        case kOpAddi: return format("ADDI R%u, R%u, %d", rd, rs, imm6);
        case kOpLi: return format("LI R%u, %u", rd, imm9);
        case kOpLui: return format("LUI R%u, %u", rd, imm9);
        case kOpLd: return format("LD R%u, %d(R%u)", rd, imm6, rs);
        case kOpSt: return format("ST R%u, %d(R%u)", rd, imm6, rs);
        case kOpBeq:
        case kOpBne:
            return format("%s R%u, R%u, %d", op == kOpBeq ? "BEQ" : "BNE", rd, rs, static_cast<int>(address) + 1 + imm6);
        case kOpJmp: return format("JMP %u", imm12);
        case kOpCall: return format("CALL %u", imm12);
        case kOpJr:
            if (rd != 0 || (word & 63u) != 0) return raw();
            return rs == 7 ? "RET" : format("JR R%u", rs);
        case kOpOut:
            if (rd != 0 || (word & 63u) != 0) return raw();
            return format("OUT R%u", rs);
        case kOpIn:
            if (imm9 != 0) return raw();
            return format("IN R%u", rd);
        case kOpHalt:
            if ((word & 4095u) != 0) return raw();
            return "HLT";
        default: return raw();
    }
}

}  // namespace bancada::assembly
