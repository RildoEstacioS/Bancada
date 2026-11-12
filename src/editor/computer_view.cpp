#include "editor/computer_view.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "asm/assembler.hpp"
#include "editor/computer16.hpp"
#include "i18n/i18n.hpp"

namespace bancada::editor {

namespace {

using namespace assembly;

unsigned pinNamed(const Part& part, std::string_view name) {
    const unsigned count = pinCount(part);
    for (unsigned pin = 0; pin < count; ++pin) {
        if (name == pinName(part, pin)) return pin;
    }
    return UINT_MAX;
}

// The path of chip parts, then the box, from this board down to the first box of this kind.
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

constexpr std::uint32_t bit(unsigned index) { return std::uint32_t{1} << index; }

struct Fields {
    unsigned op, rd, rs, rt, func;
    int imm6;
    unsigned imm9, imm12;
};

Fields fieldsOf(std::uint16_t word) {
    Fields f{};
    f.op = word >> 12;
    f.rd = (word >> 9) & 7u;
    f.rs = (word >> 6) & 7u;
    f.rt = (word >> 3) & 7u;
    f.func = word & 7u;
    f.imm6 = (word & 32u) != 0 ? static_cast<int>(word & 63u) - 64 : static_cast<int>(word & 63u);
    f.imm9 = word & 511u;
    f.imm12 = word & 4095u;
    return f;
}

std::string reg(unsigned index, const ComputerState& state) {
    return "R" + std::to_string(index) + " (" + std::to_string(state.registers[index]) + ")";
}

}  // namespace

std::optional<ComputerState> readComputer(const Board& board, const BoardCircuit& circuit, const ChipBoards& chips) {
    const Part* cpu = nullptr;
    for (const Part& part : board.parts()) {
        if (part.kind == PartKind::Chip && part.interface != nullptr && part.interface->name == kCpuName) cpu = &part;
    }
    if (cpu == nullptr) return std::nullopt;
    const unsigned instructionPin = pinNamed(*cpu, "Instr");
    const unsigned pcPin = pinNamed(*cpu, "PC");
    const unsigned outputPin = pinNamed(*cpu, "Saída");
    // "Pare" is the name in projects saved before the pins were renamed.
    unsigned haltPin = pinNamed(*cpu, "Parou");
    if (haltPin == UINT_MAX) haltPin = pinNamed(*cpu, "Pare");
    const unsigned addressPin = pinNamed(*cpu, "End");
    if (instructionPin == UINT_MAX || pcPin == UINT_MAX || outputPin == UINT_MAX || haltPin == UINT_MAX || addressPin == UINT_MAX) {
        return std::nullopt;
    }

    ComputerState state;
    state.cpu = cpu->id;
    const std::optional<std::uint32_t> instructionNet = circuit.netOfPin(cpu->id, instructionPin);
    const std::optional<std::uint32_t> addressNet = circuit.netOfPin(cpu->id, addressPin);
    const Part* rom = nullptr;
    for (const Part& part : board.parts()) {
        if (part.kind != PartKind::Box) continue;
        if (part.box == boxFromKey("rom") && instructionNet && circuit.netOfPin(part.id, inputCount(part)) == instructionNet) rom = &part;
        if (part.box == boxFromKey("ram") && addressNet && circuit.netOfPin(part.id, 0) == addressNet) state.ram = part.id;
    }
    if (rom == nullptr) return std::nullopt;
    state.rom = rom->id;

    state.pc = static_cast<std::uint16_t>(circuit.pinValue(cpu->id, pcPin));
    state.output = static_cast<std::uint16_t>(circuit.pinValue(cpu->id, outputPin));
    state.halted = circuit.pinOn(cpu->id, haltPin);

    const auto inside = chips.find(cpu->chip);
    std::vector<PartId> path{cpu->id};
    if (inside != chips.end() && findBox(*inside->second, chips, boxFromKey("regfile"), path, 0)) {
        for (std::size_t r = 0; r < state.registers.size(); ++r) state.registers[r] = static_cast<std::uint16_t>(circuit.memoryWord(path, r));
    }
    if (rom->rom != nullptr && state.pc < rom->rom->size()) state.instruction = static_cast<std::uint16_t>((*rom->rom)[state.pc]);
    state.source = rom->source;
    if (!state.source.empty()) state.lineOfWord = assemble(state.source).lines;
    if (state.ram != 0) {
        const std::array<PartId, 1> ramPath{state.ram};
        for (std::uint64_t address = 0; address < 8; ++address) state.memory.push_back(static_cast<std::uint16_t>(circuit.memoryWord(ramPath, address)));
        const Fields f = fieldsOf(state.instruction);
        if (f.op == kOpLd || f.op == kOpSt) {
            const auto address = static_cast<std::uint16_t>(state.registers[f.rs] + static_cast<std::uint16_t>(f.imm6));
            state.memoryAtAddress = static_cast<std::uint16_t>(circuit.memoryWord(ramPath, address));
        }
    }
    const unsigned switchesPin = pinNamed(*cpu, "Chaves");
    if (switchesPin != UINT_MAX) state.switches = static_cast<std::uint16_t>(circuit.pinValue(cpu->id, switchesPin));
    return state;
}

InstructionTrace traceInstruction(const ComputerState& state) {
    static constexpr const char* kNames[] = {"", "ADDI", "LI", "LUI", "LD", "ST", "BEQ", "BNE", "JMP", "CALL", "JR", "OUT", "IN", "", "", "HLT"};
    static constexpr const char* kFunctions[] = {"AND", "OR", "XOR", "ADD", "SUB", "NOT", "SHL", "SHR"};
    static constexpr const char* kOperations[] = {"E", "OU", "XOU", "+", "\xE2\x88\x92", "N\xC3\x83O", "<< 1", ">> 1"};
    static constexpr const char* kOperationsEnglish[] = {"AND", "OR", "XOR", "+", "\xE2\x88\x92", "NOT", "<< 1", ">> 1"};
    const bool english = i18n::language() == i18n::Language::English;

    const Fields f = fieldsOf(state.instruction);
    InstructionTrace t;
    t.pc = state.pc;
    t.pcPlusOne = static_cast<std::uint16_t>(state.pc + 1);
    t.word = state.instruction;
    t.text = disassemble(state.instruction, state.pc);
    t.op = f.op;
    t.rd = f.rd;
    t.rs = f.rs;
    t.rt = f.rt;
    t.func = f.func;
    t.name = f.op == kOpAlu ? kFunctions[f.func] : kNames[f.op];
    t.nextPc = t.pcPlusOne;

    const auto readA = [&](unsigned r) {
        t.readsA = true;
        t.registerA = r;
        t.valueA = state.registers[r];
    };
    const auto readB = [&](unsigned r) {
        t.readsB = true;
        t.registerB = r;
        t.valueB = state.registers[r];
    };
    const auto immediate = [&](std::uint16_t value, const char* field) {
        t.usesImmediate = true;
        t.immediate = value;
        t.immediateField = field;
    };
    const auto alu = [&](std::uint16_t a, std::uint16_t b, std::uint16_t result, const char* operation) {
        t.usesAlu = true;
        t.aluA = a;
        t.aluB = b;
        t.aluResult = result;
        t.aluOperation = operation;
    };
    const auto write = [&](unsigned r, std::uint16_t value, InstructionTrace::WriteBack from) {
        if (r == 0) return;  // R0 stays 0, so nothing is written
        t.writeBack = from;
        t.writtenRegister = r;
        t.writtenValue = value;
    };
    const auto imm6 = static_cast<std::uint16_t>(f.imm6);

    switch (f.op) {
        case kOpAlu: {
            readA(f.rs);
            if (f.func < kNot) readB(f.rt);
            const std::uint16_t a = t.valueA;
            const std::uint16_t b = state.registers[f.rt];
            std::uint16_t result = 0;
            switch (f.func) {
                case kAnd: result = a & b; break;
                case kOr: result = a | b; break;
                case kXor: result = a ^ b; break;
                case kAdd: result = static_cast<std::uint16_t>(a + b); break;
                case kSub: result = static_cast<std::uint16_t>(a - b); break;
                case kNot: result = static_cast<std::uint16_t>(~a); break;
                case kShl: result = static_cast<std::uint16_t>(a << 1); break;
                default: result = static_cast<std::uint16_t>(a >> 1); break;
            }
            alu(a, b, result, english ? kOperationsEnglish[f.func] : kOperations[f.func]);
            write(f.rd, result, InstructionTrace::WriteBack::Alu);
            break;
        }
        case kOpAddi:
            readA(f.rs);
            immediate(imm6, "imm6");
            t.aluTakesImmediate = true;
            alu(t.valueA, imm6, static_cast<std::uint16_t>(t.valueA + imm6), "+");
            write(f.rd, t.aluResult, InstructionTrace::WriteBack::Alu);
            break;
        case kOpLi:
        case kOpLui: {
            const auto value = static_cast<std::uint16_t>(f.op == kOpLi ? f.imm9 : f.imm9 << 7);
            immediate(value, f.op == kOpLi ? "imm9" : "imm9 \xC3\x97 128");
            t.aluTakesImmediate = true;
            t.aluZeroesA = true;
            alu(0, value, value, "+");
            write(f.rd, value, InstructionTrace::WriteBack::Alu);
            break;
        }
        case kOpLd:
        case kOpSt:
            readA(f.rs);
            if (f.op == kOpSt) readB(f.rd);
            immediate(imm6, "imm6");
            t.aluTakesImmediate = true;
            alu(t.valueA, imm6, static_cast<std::uint16_t>(t.valueA + imm6), "+");
            t.address = t.aluResult;
            if (f.op == kOpLd) {
                t.readsMemory = true;
                t.memoryValue = state.memoryAtAddress;
                write(f.rd, t.memoryValue, InstructionTrace::WriteBack::Memory);
            } else {
                t.writesMemory = true;
                t.memoryValue = t.valueB;
            }
            break;
        case kOpBeq:
        case kOpBne: {
            // "BNE R1, R0" compares R1 with R0: read in the order they're written.
            readA(f.rd);
            readB(f.rs);
            immediate(imm6, "imm6");
            alu(t.valueA, t.valueB, static_cast<std::uint16_t>(t.valueA - t.valueB), "\xE2\x88\x92");
            t.branches = true;
            t.branchTarget = static_cast<std::uint16_t>(t.pcPlusOne + imm6);
            const bool equal = t.valueA == t.valueB;
            t.branchTaken = f.op == kOpBeq ? equal : !equal;
            if (t.branchTaken) {
                t.next = InstructionTrace::Next::Branch;
                t.nextPc = t.branchTarget;
            }
            break;
        }
        case kOpJmp:
        case kOpCall:
            immediate(static_cast<std::uint16_t>(f.imm12), "imm12");
            t.next = InstructionTrace::Next::Jump;
            t.nextPc = static_cast<std::uint16_t>(f.imm12);
            if (f.op == kOpCall) write(7, t.pcPlusOne, InstructionTrace::WriteBack::Link);
            break;
        case kOpJr:
            readA(f.rs);
            t.next = InstructionTrace::Next::Register;
            t.nextPc = t.valueA;
            break;
        case kOpOut:
            readA(f.rs);
            t.shows = true;
            t.shown = t.valueA;
            break;
        case kOpIn: write(f.rd, state.switches, InstructionTrace::WriteBack::Switches); break;
        case kOpHalt:
            t.halts = true;
            t.next = InstructionTrace::Next::Stay;
            t.nextPc = t.pc;
            break;
        default: t.notAnInstruction = true; break;
    }
    return t;
}

namespace {

std::string signedText(std::uint16_t value) { return std::to_string(static_cast<std::int16_t>(value)); }

// A signed number in the middle of a sum: in parentheses when negative, as in 6 + (-4).
std::string operand(std::uint16_t value) {
    const std::string text = signedText(value);
    return text[0] == '-' ? "(" + text + ")" : text;
}

std::string hexWord(std::uint16_t value) {
    char text[8];
    std::snprintf(text, sizeof text, "%04X", static_cast<unsigned>(value));
    return text;
}

}  // namespace

std::string explainStage(const InstructionTrace& t, int stage) {
    using WriteBack = InstructionTrace::WriteBack;
    using Next = InstructionTrace::Next;
    using i18n::format;
    using i18n::tr;
    using std::to_string;
    const auto r = [](unsigned index) { return "R" + to_string(index); };
    const auto join = [](const std::vector<std::string>& parts, const char* between) {
        std::string all;
        for (std::size_t i = 0; i < parts.size(); ++i) all += std::string(i == 0 ? "" : between) + parts[i];
        return all;
    };
    const bool storesResult = t.op == kOpAlu || t.op == kOpAddi || t.op == kOpLi || t.op == kOpLui || t.op == kOpLd || t.op == kOpIn;
    const std::string immediateText = std::string(t.immediateField) == "imm6" ? signedText(t.immediate) : to_string(t.immediate);
    switch (stage) {
        case 0:
            return format("O PC vale {}: é o endereço da instrução atual. A ROM, onde o programa fica gravado, entrega a palavra desse endereço, {} "
                          "(16 bits, escritos em hexadecimal), que é {}. Ao mesmo tempo, o somador PC + 1 calcula {}, o endereço da instrução seguinte.",
                          {to_string(t.pc), hexWord(t.word), t.text, to_string(t.pcPlusOne)});
        case 1: {
            if (t.notAnInstruction) {
                return format("Os 4 bits de cima da palavra, o campo op, valem {}. Nenhuma instrução usa esse código: o controle não liga nada, "
                              "e o computador só passa para o endereço seguinte.",
                              {to_string(t.op)});
            }
            // What each field of the word says.
            std::vector<std::string> fields{format("op = {} diz qual é a instrução: {}", {to_string(t.op), t.name})};
            if (storesResult) fields.push_back(format("rd = {} diz quem recebe o resultado: {}", {to_string(t.rd), r(t.rd)}));
            if (t.readsA && t.readsB) fields.push_back(format("os registradores lidos são {} e {}", {r(t.registerA), r(t.registerB)}));
            else if (t.readsA) fields.push_back(format("o registrador lido é {}", {r(t.registerA)}));
            if (t.usesImmediate) {
                fields.push_back(format("{} = {} é o imediato: um número escrito dentro da própria instrução ({}), não guardado num registrador",
                                        {t.immediateField, immediateText, t.text}));
            }
            // And what the control turns on because of them.
            std::vector<std::string> signals;
            if (t.writeBack != WriteBack::None) signals.push_back(tr("escreve reg (guardar num registrador)"));
            if (t.aluZeroesA) signals.push_back(tr("ZeraA (o MUX A manda 0 para a ULA)"));
            if (t.aluTakesImmediate) signals.push_back(tr("UsaImm (a ULA recebe o número da instrução)"));
            if (t.readsMemory) signals.push_back(tr("lê RAM"));
            if (t.writesMemory) signals.push_back(tr("grava RAM"));
            if (t.branches) signals.push_back(tr(t.op == kOpBeq ? "desvia se forem iguais" : "desvia se forem diferentes"));
            if (t.next == Next::Jump) signals.push_back(tr("pula para imm12"));
            if (t.next == Next::Register) signals.push_back(format("pula para o endereço em {}", {r(t.registerA)}));
            if (t.shows) signals.push_back(tr("mostra na saída"));
            if (t.writeBack == WriteBack::Switches) signals.push_back(tr("lê as chaves"));
            if (t.halts) signals.push_back(tr("para o clock"));
            const std::string control = signals.empty() ? tr("O controle não liga nenhum sinal.")
                                                        : format("Com isso, o controle liga: {}.", {join(signals, ", ")});
            return format("A palavra é cortada em campos: {}.", {join(fields, "; ")}) + " " + control;
        }
        case 2:
            if (t.shows) {
                return format("OUT não faz conta: a ULA fica parada. {} vale {}, e esse valor vai direto para o registrador de saída, que o guarda e "
                              "mostra até o próximo OUT.",
                              {r(t.registerA), to_string(t.valueA)});
            }
            if (t.branches) {
                return format("Para comparar, a ULA subtrai: {} ({}) − {} ({}) = {}. ",
                              {r(t.registerA), to_string(t.aluA), r(t.registerB), to_string(t.aluB), signedText(t.aluResult)}) +
                       tr(t.aluResult == 0 ? "Deu zero, então os dois são iguais. O sinal Z = 1 desce até o Controle, que o junta com o tipo de desvio (BEQ desvia se Z = 1, BNE se "
                                           "Z = 0) e manda o MUX do Próximo PC desviar ou não."
                                           : "Não deu zero, então os dois são diferentes. O sinal Z = 0 desce até o Controle, que o junta com o tipo de desvio (BEQ desvia se Z = 1, "
                                           "BNE se Z = 0) e manda o MUX do Próximo PC desviar ou não.");
            }
            if (!t.usesAlu) {
                if (t.writeBack == WriteBack::Switches || t.op == kOpIn) return tr("IN não faz conta: a ULA fica parada. O valor vem das chaves, na etapa Escreve.");
                return tr("Esta instrução não faz conta: a ULA fica parada nesta etapa.");
            }
            if (t.aluZeroesA) {
                return format("Para pôr um número num registrador, o controle liga ZeraA: o MUX A deixa passar o 0 fixo em vez de um registrador. E liga "
                              "UsaImm: o MUX B deixa passar o número da instrução, {}. A ULA soma 0 + {} = {}: o número passa direto.",
                              {to_string(t.aluB), to_string(t.aluB), to_string(t.aluResult)});
            }
            if (t.func >= kNot && t.op == kOpAlu) {
                return format("A entrada A da ULA recebe {} ({}). Só ela importa: o controle manda a ULA fazer {}, e o resultado é {}.",
                              {r(t.registerA), to_string(t.aluA), t.aluOperation, to_string(t.aluResult)});
            }
            {
                std::string text =
                    t.aluTakesImmediate
                        ? format("A entrada A da ULA recebe {} ({}). A entrada B recebe o número da instrução, {}, porque o controle ligou UsaImm no "
                                 "MUX B. A ULA calcula {} {} {} = {}.",
                                 {r(t.registerA), to_string(t.aluA), signedText(t.aluB), to_string(t.aluA), t.aluOperation, operand(t.aluB),
                                  to_string(t.aluResult)})
                        : format("A entrada A da ULA recebe {} ({}) e a entrada B recebe {} ({}): UsaImm está desligado, então o MUX B deixa passar o "
                                 "registrador. A ULA calcula {} {} {} = {}.",
                                 {r(t.registerA), to_string(t.aluA), r(t.registerB), to_string(t.aluB), to_string(t.aluA), t.aluOperation,
                                  to_string(t.aluB), to_string(t.aluResult)});
                if (t.readsMemory || t.writesMemory) text += std::string(" ") + tr("Esse resultado não vai para um registrador: é o endereço na RAM.");
                return text;
            }
        case 3:
            if (t.readsMemory) {
                return format("O resultado da ULA, {}, é o endereço. O controle liga \"lê RAM\", e a RAM entrega o valor guardado nesse endereço: {}.",
                              {to_string(t.address), to_string(t.memoryValue)});
            }
            if (t.writesMemory) {
                return format("O resultado da ULA, {}, é o endereço. O controle liga \"grava RAM\", e a RAM guarda nesse endereço o valor de {}: {}.",
                              {to_string(t.address), r(t.registerB), to_string(t.memoryValue)});
            }
            return tr("Esta instrução não usa a memória: a RAM fica parada. Só LD (ler) e ST (gravar) usam a RAM.");
        case 4:
            switch (t.writeBack) {
                case WriteBack::Alu:
                    return format("O MUX \"o que volta\" escolhe a saída da ULA, e o controle liga \"escreve reg\": no fim do ciclo, {} recebe {}.",
                                  {r(t.writtenRegister), to_string(t.writtenValue)});
                case WriteBack::Memory:
                    return format("O MUX \"o que volta\" escolhe o que veio da RAM, e o controle liga \"escreve reg\": no fim do ciclo, {} recebe {}.",
                                  {r(t.writtenRegister), to_string(t.writtenValue)});
                case WriteBack::Link:
                    return format("O MUX \"o que volta\" escolhe PC+1, e R7 guarda {}: o endereço da instrução logo depois deste CALL. Quando a "
                                  "sub-rotina acabar, JR R7 volta para lá.",
                                  {to_string(t.writtenValue)});
                case WriteBack::Switches:
                    return format("O MUX \"o que volta\" escolhe as chaves, e o controle liga \"escreve reg\": {} recebe {}, o número que as chaves "
                                  "formam.",
                                  {r(t.writtenRegister), to_string(t.writtenValue)});
                case WriteBack::None: break;
            }
            if (storesResult) return tr("O destino é R0, que vale sempre 0: o controle não liga \"escreve reg\", e nada muda.");
            return format("{} não guarda resultado em registrador: \"escreve reg\" fica desligado, e nenhum registrador muda.", {t.name});
        default:
            switch (t.next) {
                case Next::Branch:
                    return format("O desvio é tomado ({}): o MUX do Próximo PC escolhe o Alvo do desvio em vez de PC + 1. O PC recebe {} + {} = {}.",
                                  {tr(t.aluResult == 0 ? "os dois são iguais" : "os dois são diferentes"), to_string(t.pcPlusOne), operand(t.immediate),
                                   to_string(t.nextPc)});
                case Next::Jump:
                    return format("O PC recebe {}, o número imm12 escrito na própria instrução: a próxima instrução vem desse endereço.",
                                  {to_string(t.nextPc)});
                case Next::Register:
                    return format("O PC recebe {}, o valor guardado em {}: a próxima instrução vem desse endereço.", {to_string(t.nextPc), r(t.registerA)});
                case Next::Stay: return format("HLT: o PC continua em {} e o clock para. O programa acabou.", {to_string(t.nextPc)});
                case Next::Plain: break;
            }
            if (t.branches) {
                return format("O desvio não é tomado ({}): o MUX do Próximo PC escolhe PC + 1, e o PC recebe {}, a instrução seguinte.",
                              {tr(t.aluResult == 0 ? "os dois são iguais" : "os dois são diferentes"), to_string(t.nextPc)});
            }
            return format("O MUX do Próximo PC escolhe PC + 1: o PC recebe {}, o endereço seguinte.", {to_string(t.nextPc)});
    }
}

DatapathUse datapathUse(std::uint16_t instruction) {
    const Fields f = fieldsOf(instruction);
    DatapathUse use;
    use.blocks = bit(kBlockPc) | bit(kBlockRom) | bit(kBlockDecoder) | bit(kBlockControl);
    use.paths = bit(kPathPcRom) | bit(kPathRomDecoder) | bit(kPathDecoderControl);
    const auto add = [&](std::initializer_list<unsigned> blocks, std::initializer_list<unsigned> paths) {
        for (unsigned b : blocks) use.blocks |= bit(b);
        for (unsigned p : paths) use.paths |= bit(p);
    };
    switch (f.op) {
        case kOpAlu: add({kBlockRegisters, kBlockAlu}, {kPathDecoderRegisters, kPathRegistersAlu, kPathWriteBack}); break;
        case kOpAddi: add({kBlockRegisters, kBlockAlu}, {kPathDecoderRegisters, kPathRegistersAlu, kPathImmediate, kPathWriteBack}); break;
        case kOpLi:
        case kOpLui: add({kBlockRegisters, kBlockAlu}, {kPathImmediate, kPathWriteBack}); break;
        case kOpLd: add({kBlockRegisters, kBlockAlu, kBlockRam}, {kPathDecoderRegisters, kPathRegistersAlu, kPathImmediate, kPathAddress, kPathLoad}); break;
        case kOpSt: add({kBlockRegisters, kBlockAlu, kBlockRam}, {kPathDecoderRegisters, kPathRegistersAlu, kPathImmediate, kPathAddress, kPathStore}); break;
        case kOpBeq:
        case kOpBne: add({kBlockRegisters, kBlockAlu}, {kPathDecoderRegisters, kPathRegistersAlu, kPathBranch}); break;
        case kOpJmp: add({}, {kPathJump}); break;
        case kOpCall: add({kBlockRegisters}, {kPathJump, kPathLink}); break;
        case kOpJr: add({kBlockRegisters}, {kPathDecoderRegisters, kPathJumpRegister}); break;
        case kOpOut: add({kBlockRegisters, kBlockOutput}, {kPathDecoderRegisters, kPathOutput}); break;
        case kOpIn: add({kBlockRegisters, kBlockSwitches}, {kPathSwitches}); break;
        default: break;
    }
    return use;
}

int registerWritten(std::uint16_t instruction) {
    const Fields f = fieldsOf(instruction);
    switch (f.op) {
        case kOpAlu:
        case kOpAddi:
        case kOpLi:
        case kOpLui:
        case kOpLd:
        case kOpIn: return f.rd == 0 ? -1 : static_cast<int>(f.rd);
        case kOpCall: return 7;
        default: return -1;
    }
}

std::string explainInstruction(const ComputerState& state) {
    using i18n::format;
    using i18n::tr;
    using std::to_string;
    if (state.halted) return tr("O computador parou. Zerar começa de novo.");
    const Fields f = fieldsOf(state.instruction);
    const std::string rd = "R" + to_string(f.rd);
    const auto target = [&](int offset) { return to_string(static_cast<int>(state.pc) + 1 + offset); };
    switch (f.op) {
        case kOpAlu: {
            if (state.instruction == 0x0003) return tr("Não faz nada.");
            static constexpr const char* kSentences[] = {"Faz E bit a bit entre {} e {} e guarda em {}.", "Faz OU bit a bit entre {} e {} e guarda em {}.",
                                                         "Faz OU exclusivo entre {} e {} e guarda em {}.", "Soma {} e {} e guarda em {}.",
                                                         "Subtrai {} e {} e guarda em {}."};
            if (f.func == kAdd && f.rt == 0) return format("Copia {} para {}.", {reg(f.rs, state), rd});
            if (f.func == kSub) return format("Calcula {} menos {} e guarda em {}.", {reg(f.rs, state), reg(f.rt, state), rd});
            if (f.func < 5) return format(kSentences[f.func], {reg(f.rs, state), reg(f.rt, state), rd});
            if (f.func == kNot) return format("Inverte os bits de {} e guarda em {}.", {reg(f.rs, state), rd});
            if (f.func == kShl) return format("Dobra {}, deslocando um bit, e guarda em {}.", {reg(f.rs, state), rd});
            return format("Divide {} por 2, deslocando um bit, e guarda em {}.", {reg(f.rs, state), rd});
        }
        case kOpAddi: return format("Soma {} a {} e guarda em {}.", {to_string(f.imm6), reg(f.rs, state), rd});
        case kOpLi: return format("Coloca {} em {}.", {to_string(f.imm9), rd});
        case kOpLui: return format("Coloca {} vezes 128, {}, em {}.", {to_string(f.imm9), to_string(f.imm9 * 128u), rd});
        case kOpLd: {
            const auto address = static_cast<std::uint16_t>(state.registers[f.rs] + f.imm6);
            return format("Lê a memória no endereço {} e guarda em {}.", {to_string(address), rd});
        }
        case kOpSt: {
            const auto address = static_cast<std::uint16_t>(state.registers[f.rs] + f.imm6);
            return format("Grava {} na memória, no endereço {}.", {reg(f.rd, state), to_string(address)});
        }
        case kOpBeq:
        case kOpBne: {
            const bool equal = state.registers[f.rd] == state.registers[f.rs];
            const bool taken = f.op == kOpBeq ? equal : !equal;
            return format(f.op == kOpBeq ? "Se {} for igual a {}, pula para {}." : "Se {} for diferente de {}, pula para {}.",
                          {reg(f.rd, state), reg(f.rs, state), target(f.imm6)}) +
                   " " + tr(taken ? "Vai pular." : "Não vai pular.");
        }
        case kOpJmp: return format("Pula para o endereço {}.", {to_string(f.imm12)});
        case kOpCall: return format("Chama a sub-rotina em {} e guarda em R7 onde voltar, {}.", {to_string(f.imm12), to_string(state.pc + 1)});
        case kOpJr:
            if (f.rs == 7) return format("Volta para {}, o endereço guardado em R7.", {to_string(state.registers[7])});
            return format("Pula para o endereço em {}.", {reg(f.rs, state)});
        case kOpOut: return format("Mostra {} na saída.", {reg(f.rs, state)});
        case kOpIn: return format("Lê as chaves e guarda em {}.", {rd});
        case kOpHalt: return tr("Para o computador.");
        default: return tr("Esta palavra não é uma instrução: o computador segue para a próxima.");
    }
}

}  // namespace bancada::editor
