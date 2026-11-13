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
        const std::uint32_t op = state.instruction >> 26;
        if (op == mips::kOpLw || op == mips::kOpSw) {
            const auto offset = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(state.instruction & 0xFFFFu)));
            const std::uint32_t address = state.registers[(state.instruction >> 21) & 31u] + offset;
            state.memoryAtAddress = static_cast<std::uint32_t>(circuit.memoryWord(ramPath, (address >> 2) % mips::kMemoryWords));
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

Trace32 traceInstruction32(std::uint32_t word, std::uint32_t pc, const std::array<std::uint32_t, 32>& registers, std::uint32_t switches,
                           std::uint32_t memoryAtAddress) {
    using namespace mips;
    using Back = Trace32::Back;
    using Next = Trace32::Next;
    Trace32 t;
    const Fields f = fieldsOf(word);
    t.pc = pc;
    t.pc4 = pc + 4;
    t.word = word;
    t.text = disassemble(word, pc);
    t.name = t.text.substr(0, t.text.find(' '));
    t.op = f.op;
    t.funct = f.funct;
    t.rs = f.rs;
    t.rt = f.rt;
    t.rd = f.rd;
    t.shamt = f.shamt;
    t.imm = f.imm;
    t.format = f.op == kOpSpecial ? 'R' : f.op == kOpJ || f.op == kOpJal ? 'J' : 'I';
    t.branchTarget = t.pc4 + (static_cast<std::uint32_t>(f.simm) << 2);
    t.jumpTarget = (t.pc4 & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
    t.nextPc = t.pc4;

    const auto readA = [&](unsigned r) {
        t.readsA = true;
        t.registerA = r;
        t.valueA = registers[r];
    };
    const auto readB = [&](unsigned r) {
        t.readsB = true;
        t.registerB = r;
        t.valueB = registers[r];
    };
    const auto immediate = [&](bool sign) {
        t.usesImmediate = true;
        t.signExtends = sign;
        t.extended = sign ? static_cast<std::uint32_t>(f.simm) : f.imm;
    };
    const auto alu = [&](const char* operation, std::uint32_t a, std::uint32_t b, std::uint32_t result) {
        t.usesAlu = true;
        t.aluOperation = operation;
        t.aluA = a;
        t.aluB = b;
        t.aluResult = result;
        t.zero = result == 0;
    };
    const auto write = [&](Back back, unsigned destination, unsigned reg, std::uint32_t value) {
        t.destination = destination;
        if (reg == 0) return;
        t.back = back;
        t.written = reg;
        t.writtenValue = value;
    };
    const auto stop = [&](bool invalid) {
        t.halts = true;
        t.invalid = invalid;
        t.next = Next::Stay;
        t.nextPc = pc;
    };
    const auto lessSigned = [](std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b) ? 1u : 0u; };

    if (f.op == kOpSpecial) {
        const auto three = [&](const char* operation, std::uint32_t result) {
            readA(f.rs);
            readB(f.rt);
            alu(operation, t.valueA, t.valueB, result);
            write(Back::Alu, 1, f.rd, result);
        };
        const std::uint32_t a = registers[f.rs];
        const std::uint32_t b = registers[f.rt];
        switch (f.funct) {
            case kFnSll:
            case kFnSrl:
            case kFnSra: {
                readB(f.rt);
                t.shifts = true;
                const std::uint32_t result = f.funct == kFnSll ? b << f.shamt
                                             : f.funct == kFnSrl ? b >> f.shamt
                                                                 : static_cast<std::uint32_t>(static_cast<std::int32_t>(b) >> f.shamt);
                alu(f.funct == kFnSll ? "<<" : f.funct == kFnSrl ? ">>" : ">>±", f.shamt, b, result);
                write(Back::Alu, 1, f.rd, result);
                break;
            }
            case kFnJr:
                readA(f.rs);
                t.next = Next::Register;
                t.nextPc = t.valueA;
                break;
            case kFnSyscall:
                t.syscall = true;
                readA(kV0);
                readB(kA0);
                t.call = t.valueA;
                switch (t.call) {
                    case kPrintInt:
                        t.printsNumber = true;
                        t.printed = t.valueB;
                        break;
                    case kPrintChar:
                        t.printsChar = true;
                        t.printed = t.valueB;
                        break;
                    case kReadInt: write(Back::Switches, 3, kV0, switches); break;
                    case kExit: stop(false); break;
                    default: stop(true); break;
                }
                if (t.back == Back::None) t.destination = 3;
                break;
            case kFnAdd:
            case kFnAddu: three("+", a + b); break;
            case kFnSub:
            case kFnSubu: three("-", a - b); break;
            case kFnAnd: three("E", a & b); break;
            case kFnOr: three("OU", a | b); break;
            case kFnXor: three("OUX", a ^ b); break;
            case kFnNor: three("NOU", ~(a | b)); break;
            case kFnSlt: three("<", lessSigned(a, b)); break;
            case kFnSltu: three("<u", a < b ? 1u : 0u); break;
            default: stop(true); break;
        }
        return t;
    }
    switch (f.op) {
        case kOpJ:
            t.next = Next::Jump;
            t.nextPc = t.jumpTarget;
            break;
        case kOpJal:
            t.next = Next::Jump;
            t.nextPc = t.jumpTarget;
            write(Back::Link, 2, kRa, t.pc4);
            break;
        case kOpBeq:
        case kOpBne: {
            readA(f.rs);
            readB(f.rt);
            immediate(true);
            alu("-", t.valueA, t.valueB, t.valueA - t.valueB);
            t.branches = true;
            t.taken = (f.op == kOpBeq) == (t.valueA == t.valueB);
            if (t.taken) {
                t.next = Next::Branch;
                t.nextPc = t.branchTarget;
            }
            break;
        }
        case kOpAddi:
        case kOpAddiu:
        case kOpSlti:
        case kOpSltiu:
        case kOpAndi:
        case kOpOri:
        case kOpXori:
        case kOpLui: {
            const bool sign = f.op == kOpAddi || f.op == kOpAddiu || f.op == kOpSlti || f.op == kOpSltiu;
            if (f.op != kOpLui) readA(f.rs);
            immediate(sign);
            t.aluTakesImmediate = true;
            const std::uint32_t a = t.valueA;
            const std::uint32_t b = t.extended;
            switch (f.op) {
                case kOpAddi:
                case kOpAddiu: alu("+", a, b, a + b); break;
                case kOpSlti: alu("<", a, b, lessSigned(a, b)); break;
                case kOpSltiu: alu("<u", a, b, a < b ? 1u : 0u); break;
                case kOpAndi: alu("E", a, b, a & b); break;
                case kOpOri: alu("OU", a, b, a | b); break;
                case kOpXori: alu("OUX", a, b, a ^ b); break;
                default:
                    t.shifts = true;
                    alu("<<16", 16, b, b << 16);
                    break;
            }
            write(Back::Alu, 0, f.rt, t.aluResult);
            break;
        }
        case kOpLw:
        case kOpSw:
            readA(f.rs);
            if (f.op == kOpSw) readB(f.rt);
            immediate(true);
            t.aluTakesImmediate = true;
            alu("+", t.valueA, t.extended, t.valueA + t.extended);
            t.address = t.aluResult;
            if (f.op == kOpLw) {
                t.readsMemory = true;
                t.memoryValue = memoryAtAddress;
                write(Back::Memory, 0, f.rt, memoryAtAddress);
            } else {
                t.writesMemory = true;
                t.memoryValue = t.valueB;
            }
            break;
        default: stop(true); break;
    }
    return t;
}

Trace32 traceInstruction32(const Computer32State& state) {
    return traceInstruction32(state.instruction, state.pc, state.registers, state.switches, state.memoryAtAddress);
}

std::string explainStage32(const Trace32& t, int stage) {
    using Back = Trace32::Back;
    using Next = Trace32::Next;
    using i18n::format;
    using i18n::tr;
    const auto name = [](unsigned index) { return std::string(mips::registerName(index)); };
    const auto with = [&](unsigned index, std::uint32_t value) { return name(index) + " (" + number(value) + ")"; };
    switch (stage) {
        case 0:
            return format("O PC vale {}: é o endereço da instrução. A ROM de instruções entrega a palavra desse endereço, {}, que é {}. Ao mesmo "
                          "tempo, o somador PC + 4 calcula {}: cada instrução ocupa 4 bytes, então a seguinte está 4 endereços adiante.",
                          {hex(t.pc), hex(t.word), t.text, hex(t.pc4)});
        case 1: {
            if (t.invalid && !t.syscall) {
                return format("Os 6 bits de cima, op = {}, e o funct = {} não formam nenhuma instrução deste computador: o controle liga Inválida, e ele para.",
                              {std::to_string(t.op), std::to_string(t.funct)});
            }
            std::string fields;
            if (t.format == 'R') {
                fields = format("Os 6 bits de cima, op = 0, dizem que é do tipo R: o funct = {}, nos 6 de baixo, diz qual é: {}.", {std::to_string(t.funct), t.name});
            } else if (t.format == 'J') {
                fields = format("Os 6 bits de cima, op = {}, dizem que é do tipo J: {}, com um alvo de 26 bits.", {std::to_string(t.op), t.name});
            } else {
                fields = format("Os 6 bits de cima, op = {}, dizem que é do tipo I: {}, com um imediato de 16 bits.", {std::to_string(t.op), t.name});
            }
            if (t.syscall) {
                fields += " " + format("Num syscall, o controle faz os registradores lerem $v0 ({}) e $a0 ({}).", {number(t.valueA), number(t.valueB)});
            } else if (t.readsA && t.readsB) {
                fields += " " + format("Os registradores lidos são {} e {}.", {with(t.registerA, t.valueA), with(t.registerB, t.valueB)});
            } else if (t.readsA) {
                fields += " " + format("O registrador lido é {}.", {with(t.registerA, t.valueA)});
            } else if (t.readsB) {
                fields += " " + format("O registrador lido é {}.", {with(t.registerB, t.valueB)});
            }
            if (t.usesImmediate) {
                fields += " " + format("O imediato {} vira 32 bits no extensor, {}: {}.",
                                       {hex(t.imm).substr(6), tr(t.signExtends ? "copiando o sinal" : "com zeros em cima"), number(t.extended)});
            }
            if (t.back != Back::None) {
                static constexpr const char* kWhich[] = {"rt", "rd", "$ra", "$v0"};
                fields += " " + format("O resultado vai para {}: o MUX do destino escolhe {}.", {name(t.written), kWhich[t.destination]});
            }
            return fields;
        }
        case 2: {
            if (t.syscall) {
                switch (t.call) {
                    case mips::kPrintInt: return format("A unidade Sistema vê $v0 = 1: liga ImprimeNum, e o console mostra o número em $a0, {}.", {number(t.printed)});
                    case mips::kPrintChar: return format("A unidade Sistema vê $v0 = 11: liga ImprimeCar, e o console mostra o caractere de código {}.", {number(t.printed)});
                    case mips::kReadInt: return tr("A unidade Sistema vê $v0 = 5: liga LeChaves, e o que as chaves formam vai voltar para $v0.");
                    case mips::kExit: return tr("A unidade Sistema vê $v0 = 10: liga Pare, e o PC deixa de andar.");
                    default: return format("A unidade Sistema não conhece o código $v0 = {}: liga Desconhecida, e o computador para.", {number(t.call)});
                }
            }
            if (t.next == Next::Jump) {
                return format("O alvo do salto junta os 4 bits de cima de PC + 4 com o alvo × 4: {}. A ULA não é usada.", {hex(t.jumpTarget)});
            }
            if (t.next == Next::Register) return format("jr não usa a ULA: o endereço de volta já está em {}: {}.", {name(t.registerA), hex(t.valueA)});
            if (!t.usesAlu) return tr("Esta instrução não usa a ULA.");
            if (t.branches) {
                return format("A ULA subtrai para comparar: {} - {} = {}, então Z = {}. Ao mesmo tempo, o somador do desvio calcula PC + 4 + imediato × 4 = {}.",
                              {number(t.aluA), number(t.aluB), number(t.aluResult), std::to_string(t.zero ? 1 : 0), hex(t.branchTarget)});
            }
            if (t.shifts) {
                return format("O deslocador da ULA move {} {} casas ({}): dá {}.", {hex(t.aluB), number(t.aluA), t.aluOperation, hex(t.aluResult)});
            }
            const std::string second = t.aluTakesImmediate ? tr("o imediato") : tr("o registrador B");
            std::string sentence = format("A ULA faz {} {} {} = {}. A entrada B é {}, escolhido pelo MUX B.",
                                          {number(t.aluA), tr(t.aluOperation.c_str()), number(t.aluB), number(t.aluResult), second});
            if (t.readsMemory || t.writesMemory) sentence += " " + format("O resultado é um endereço: {}.", {hex(t.address)});
            return sentence;
        }
        case 3:
            if (t.readsMemory) return format("A RAM de dados lê o endereço {} e entrega {}.", {hex(t.address), number(t.memoryValue)});
            if (t.writesMemory) return format("A RAM de dados grava {} no endereço {}, na subida do clock.", {number(t.memoryValue), hex(t.address)});
            return tr("Esta instrução não usa a RAM.");
        case 4: {
            if (t.back == Back::None) return tr("Nada é gravado nos registradores.");
            const char* from = t.back == Back::Alu ? "o resultado da ULA" : t.back == Back::Memory ? "o que veio da RAM" : t.back == Back::Link ? "PC + 4, o endereço de volta" : "as chaves";
            return format("O MUX de volta escolhe {}, {}, e ele é gravado em {} na subida do clock.", {tr(from), number(t.writtenValue), name(t.written)});
        }
        default:
            switch (t.next) {
                case Next::Branch: return format("O desvio é tomado: o MUX do próximo PC escolhe o alvo do desvio, e o PC recebe {}.", {hex(t.nextPc)});
                case Next::Jump: return format("O MUX do próximo PC escolhe o alvo do salto: o PC recebe {}.", {hex(t.nextPc)});
                case Next::Register: return format("O MUX do próximo PC escolhe o registrador: o PC recebe {}.", {hex(t.nextPc)});
                case Next::Stay: return format("O computador para: o PC fica em {}.", {hex(t.pc)});
                default:
                    if (t.branches) return format("O desvio não é tomado: o MUX do próximo PC escolhe PC + 4, e o PC recebe {}.", {hex(t.nextPc)});
                    return format("O MUX do próximo PC escolhe PC + 4: o PC recebe {}.", {hex(t.nextPc)});
            }
    }
}

}  // namespace bancada::editor
