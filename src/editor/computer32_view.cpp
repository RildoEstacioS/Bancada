#include "editor/computer32_view.hpp"

#include <climits>
#include <cstdio>
#include <string_view>

#include "asm/mips.hpp"
#include "editor/computer32.hpp"
#include "i18n/i18n.hpp"

namespace bancada::editor {

namespace {

unsigned pinNamed(const Part& part, std::string_view name) {
    const unsigned count = pinCount(part);
    for (unsigned pin = 0; pin < count; ++pin) {
        if (name == pinName(part, pin)) return pin;
    }
    return UINT_MAX;
}

bool findBox(const Board& board, const ChipBoards& chips, BoxId box, std::vector<PartId>& path, int depth) {
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Box && part.box == box) {
            path.push_back(part.id);
            return true;
        }
    }
    if (depth > 8) return false;
    for (const Part& part : board.parts()) {
        if (part.kind != PartKind::Chip) continue;
        const auto found = chips.find(part.chip);
        if (found == chips.end()) continue;
        path.push_back(part.id);
        if (findBox(*found->second, chips, box, path, depth + 1)) return true;
        path.pop_back();
    }
    return false;
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", static_cast<unsigned>(value));
    return text;
}

std::string number(std::uint32_t value) { return std::to_string(static_cast<std::int32_t>(value)); }

struct Fields {
    std::uint32_t op, funct;
    unsigned rs, rt, rd, shamt;
    std::uint32_t imm;
    std::int32_t simm;
};

Fields fieldsOf(std::uint32_t word) {
    Fields f{};
    f.op = word >> 26;
    f.rs = (word >> 21) & 31u;
    f.rt = (word >> 16) & 31u;
    f.rd = (word >> 11) & 31u;
    f.shamt = (word >> 6) & 31u;
    f.funct = word & 63u;
    f.imm = word & 0xFFFFu;
    f.simm = static_cast<std::int16_t>(static_cast<std::uint16_t>(f.imm));
    return f;
}

}  // namespace

std::optional<Computer32State> readComputer32(const Board& board, const BoardCircuit& circuit, const ChipBoards& chips) {
    const Part* cpu = nullptr;
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Chip && part.interface != nullptr && part.interface->name == kCpu32Name) cpu = &part;
    }
    if (cpu == nullptr) return std::nullopt;
    const unsigned instructionPin = pinNamed(*cpu, "Instr");
    const unsigned pcPin = pinNamed(*cpu, "PC");
    const unsigned haltPin = pinNamed(*cpu, "Parou");
    const unsigned invalidPin = pinNamed(*cpu, "Inválida");
    const unsigned valuePin = pinNamed(*cpu, "Valor");
    if (instructionPin == UINT_MAX || pcPin == UINT_MAX || haltPin == UINT_MAX) return std::nullopt;

    Computer32State state;
    state.cpu = cpu->id;
    const std::optional<std::uint32_t> instructionNet = circuit.netOfPin(cpu->id, instructionPin);
    const std::optional<std::uint32_t> valueNet = valuePin == UINT_MAX ? std::nullopt : circuit.netOfPin(cpu->id, valuePin);
    const Part* rom = nullptr;
    for (const Part& part : board.parts()) {
        if (part.kind != PartKind::Box) continue;
        if (part.box == boxFromKey("rom") && instructionNet && circuit.netOfPin(part.id, inputCount(part)) == instructionNet) rom = &part;
        // The RAM is the one the CPU's Valor goes into, as the data to write.
        if (part.box == boxFromKey("ram") && valueNet && circuit.netOfPin(part.id, 1) == valueNet) state.ram = part.id;
    }
    if (rom == nullptr) return std::nullopt;
    state.rom = rom->id;
    state.pc = static_cast<std::uint32_t>(circuit.pinValue(cpu->id, pcPin));
    state.index = (state.pc >> 2) % static_cast<std::uint32_t>(mips::kMemoryWords);
    state.halted = circuit.pinOn(cpu->id, haltPin);
    state.invalid = invalidPin != UINT_MAX && circuit.pinOn(cpu->id, invalidPin);
    state.instruction = static_cast<std::uint32_t>(circuit.pinValue(cpu->id, instructionPin));
    state.text = mips::disassemble(state.instruction, state.pc);

    const auto inside = chips.find(cpu->chip);
    std::vector<PartId> path{cpu->id};
    if (inside != chips.end() && findBox(*inside->second, chips, boxFromKey("regfile"), path, 0)) {
        for (std::size_t r = 0; r < state.registers.size(); ++r) state.registers[r] = static_cast<std::uint32_t>(circuit.memoryWord(path, r));
    }
    state.source = rom->source;
    if (!state.source.empty()) state.lineOfWord = mips::assemble(state.source).lines;
    if (state.ram != 0) {
        const std::array<PartId, 1> ramPath{state.ram};
        for (std::uint64_t address = 0; address < 16; ++address) state.data.push_back(static_cast<std::uint32_t>(circuit.memoryWord(ramPath, address)));
        const std::uint32_t top = (state.registers[mips::kSp] >> 2) % static_cast<std::uint32_t>(mips::kMemoryWords);
        // Up to the RAM's last word: past it, the addresses come round to the data.
        for (std::uint32_t i = 0; i < 8 && top + i < mips::kMemoryWords; ++i) {
            state.stack.push_back(static_cast<std::uint32_t>(circuit.memoryWord(ramPath, top + i)));
        }
    }
    const unsigned switchesPin = pinNamed(*cpu, "Chaves");
    if (switchesPin != UINT_MAX) state.switches = static_cast<std::uint32_t>(circuit.pinValue(cpu->id, switchesPin));
    return state;
}

int registerWritten32(std::uint32_t word) {
    const Fields f = fieldsOf(word);
    int written = -1;
    if (f.op == mips::kOpSpecial) {
        if (f.funct != mips::kFnJr && f.funct != mips::kFnSyscall) written = static_cast<int>(f.rd);
    } else if (f.op == mips::kOpJal) {
        written = mips::kRa;
    } else if (f.op != mips::kOpJ && f.op != mips::kOpBeq && f.op != mips::kOpBne && f.op != mips::kOpSw) {
        written = static_cast<int>(f.rt);
    }
    return written == 0 ? -1 : written;
}

std::string explainInstruction32(const Computer32State& state) {
    using namespace mips;
    if (state.halted && !state.invalid) return i18n::tr("O programa terminou (syscall 10). Aperte De novo para rodar outra vez.");
    if (state.invalid) return i18n::format("Parado: {} não é algo que este computador saiba fazer.", {state.text});
    const Fields f = fieldsOf(state.instruction);
    const auto& r = state.registers;
    const auto name = [](unsigned index) { return std::string(registerName(index)); };
    const auto with = [&](unsigned index) { return name(index) + " (" + number(r[index]) + ")"; };
    const std::uint32_t next = state.pc + 4;
    if (f.op == kOpSpecial) {
        if (state.instruction == 0) return i18n::tr("nop: não faz nada, e o PC passa para a instrução seguinte.");
        switch (f.funct) {
            case kFnJr: return i18n::format("jr: salta para o endereço em {}: {}.", {name(f.rs), hex(r[f.rs])});
            case kFnSyscall:
                switch (r[kV0]) {
                    case kPrintInt: return i18n::format("syscall 1: mostra no console o número em $a0: {}.", {number(r[kA0])});
                    case kPrintChar: return i18n::format("syscall 11: mostra no console o caractere em $a0 (código {}).", {number(r[kA0])});
                    case kReadInt: return i18n::format("syscall 5: lê as chaves ({}) para $v0.", {number(state.switches)});
                    case kExit: return i18n::tr("syscall 10: termina o programa.");
                    default: return i18n::format("syscall {}: um pedido que este computador não conhece; ele para.", {number(r[kV0])});
                }
            case kFnSll: return i18n::format("sll: {} = {} deslocado {} casas para a esquerda.", {name(f.rd), with(f.rt), std::to_string(f.shamt)});
            case kFnSrl: return i18n::format("srl: {} = {} deslocado {} casas para a direita, entrando zeros.", {name(f.rd), with(f.rt), std::to_string(f.shamt)});
            case kFnSra: return i18n::format("sra: {} = {} deslocado {} casas para a direita, copiando o sinal.", {name(f.rd), with(f.rt), std::to_string(f.shamt)});
            default: break;
        }
        const char* symbol = nullptr;
        std::uint32_t result = 0;
        const std::uint32_t a = r[f.rs];
        const std::uint32_t b = r[f.rt];
        switch (f.funct) {
            case kFnAdd: case kFnAddu: symbol = "+"; result = a + b; break;
            case kFnSub: case kFnSubu: symbol = "-"; result = a - b; break;
            case kFnAnd: symbol = "E"; result = a & b; break;
            case kFnOr: symbol = "OU"; result = a | b; break;
            case kFnXor: symbol = "OU-exclusivo"; result = a ^ b; break;
            case kFnNor: symbol = "NOU"; result = ~(a | b); break;
            case kFnSlt:
                return i18n::format("slt: {} = 1 se {} < {}, senão 0: dá {}.", {name(f.rd), with(f.rs), with(f.rt),
                                                                              std::to_string(static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b) ? 1 : 0)});
            case kFnSltu:
                return i18n::format("sltu: {} = 1 se {} < {} sem sinal, senão 0: dá {}.", {name(f.rd), with(f.rs), with(f.rt), std::to_string(a < b ? 1 : 0)});
            default: return i18n::format("{}: uma instrução que este computador não tem.", {state.text});
        }
        return i18n::format("{}: {} = {} {} {} = {}.", {state.text.substr(0, state.text.find(' ')), name(f.rd), with(f.rs), i18n::tr(symbol), with(f.rt), number(result)});
    }
    const std::uint32_t address = r[f.rs] + static_cast<std::uint32_t>(f.simm);
    switch (f.op) {
        case kOpJ: return i18n::format("j: salta para {}.", {hex((next & 0xF0000000u) | ((state.instruction & 0x03FFFFFFu) << 2))});
        case kOpJal:
            return i18n::format("jal: guarda a volta ({}) em $ra e salta para {}.", {hex(next), hex((next & 0xF0000000u) | ((state.instruction & 0x03FFFFFFu) << 2))});
        case kOpBeq:
        case kOpBne: {
            const bool equal = r[f.rs] == r[f.rt];
            const bool taken = (f.op == kOpBeq) == equal;
            const std::string target = hex(next + static_cast<std::uint32_t>(f.simm * 4));
            const std::string which = f.op == kOpBeq ? "beq" : "bne";
            return taken ? i18n::format("{}: {} e {} são {}: desvia para {}.", {which, with(f.rs), with(f.rt), i18n::tr(equal ? "iguais" : "diferentes"), target})
                         : i18n::format("{}: {} e {} são {}: segue para a instrução seguinte.", {which, with(f.rs), with(f.rt), i18n::tr(equal ? "iguais" : "diferentes")});
        }
        case kOpAddi:
        case kOpAddiu:
            return i18n::format("{}: {} = {} + {} = {}.", {state.text.substr(0, state.text.find(' ')), name(f.rt), with(f.rs), std::to_string(f.simm),
                                                            number(r[f.rs] + static_cast<std::uint32_t>(f.simm))});
        case kOpSlti:
        case kOpSltiu: return i18n::format("{}: {} = 1 se {} < {}, senão 0.", {state.text.substr(0, state.text.find(' ')), name(f.rt), with(f.rs), std::to_string(f.simm)});
        case kOpAndi:
        case kOpOri:
        case kOpXori: return i18n::format("{}: {} = {} com {}, bit a bit.", {state.text.substr(0, state.text.find(' ')), name(f.rt), with(f.rs), hex(f.imm)});
        case kOpLui: return i18n::format("lui: {} = {} nos 16 bits de cima: {}.", {name(f.rt), hex(f.imm), hex(f.imm << 16)});
        case kOpLw: return i18n::format("lw: lê a RAM no endereço {} + {} = {} e guarda em {}.", {with(f.rs), std::to_string(f.simm), hex(address), name(f.rt)});
        case kOpSw: return i18n::format("sw: grava {} na RAM, no endereço {} + {} = {}.", {with(f.rt), with(f.rs), std::to_string(f.simm), hex(address)});
        default: return i18n::format("{}: uma instrução que este computador não tem.", {state.text});
    }
}

}  // namespace bancada::editor
