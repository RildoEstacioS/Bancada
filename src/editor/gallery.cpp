#include "editor/gallery.hpp"

#include <string_view>

#include "asm/assembler.hpp"
#include "editor/boxes.hpp"
#include "editor/computer16.hpp"
#include "editor/gallery_circuits.hpp"

namespace bancada::editor {

namespace {

// --- The computer ---------------------------------------------------------------------------------------

// Two programs with a small operating system at the start of the ROM, as in the last act of the lessons.
constexpr const char* kSystemCalls =
    "; O programa pede, e o sistema operacional faz: ler as chaves, mostrar e encerrar.\n"
    "; Mude as chaves e aperte Zerar para ver outro número.\n"
    "        JMP  programa\n"
    "; Chamada de sistema: R1 diz o serviço, e R2 leva ou traz o número.\n"
    ";   1: mostra R2 na saída   2: lê as chaves em R2   3: encerra\n"
    "sistema: LI   R6, 1\n"
    "        BEQ  R1, R6, mostra\n"
    "        LI   R6, 2\n"
    "        BEQ  R1, R6, leia\n"
    "        HLT                 ; serviço 3: encerra\n"
    "mostra: OUT  R2\n"
    "        JR   R7\n"
    "leia:   IN   R2\n"
    "        JR   R7\n"
    "; ---- O programa ----\n"
    "programa: LI  R1, 2         ; lê as chaves\n"
    "        CALL sistema\n"
    "        LI   R1, 1          ; mostra\n"
    "        CALL sistema\n"
    "        ADD  R2, R2, R2\n"
    "        CALL sistema        ; mostra o dobro\n"
    "        LI   R1, 3          ; encerra\n"
    "        CALL sistema\n";

constexpr const char* kTwoTasks =
    "; Um sistema operacional mínimo, que reveza duas tarefas na CPU.\n"
    "; Cada tarefa mostra um número e passa a vez; o sistema continua a outra.\n"
    "        JMP  programa\n"
    "; Chamada de sistema: R1 diz o serviço, e R2 leva ou traz o número.\n"
    ";   1: mostra R2   2: lê as chaves em R2   3: encerra\n"
    ";   4: cria a tarefa B, que começa no endereço R2\n"
    ";   5: passa a vez para a outra tarefa\n"
    "sistema: LI   R6, 1\n"
    "        BEQ  R1, R6, mostra\n"
    "        LI   R6, 2\n"
    "        BEQ  R1, R6, leia\n"
    "        LI   R6, 4\n"
    "        BEQ  R1, R6, cria\n"
    "        LI   R6, 5\n"
    "        BEQ  R1, R6, troca\n"
    "        HLT                 ; serviço 3: encerra\n"
    "mostra: OUT  R2\n"
    "        JR   R7\n"
    "leia:   IN   R2\n"
    "        JR   R7\n"
    "; A memória do sistema: no endereço 0, a tarefa que está rodando (0 é a A,\n"
    "; 1 é a B); nos endereços 1 e 2, onde a A e a B pararam.\n"
    "cria:   ST   R2, 2(R0)\n"
    "        JR   R7\n"
    "troca:  LD   R6, 0(R0)\n"
    "        BNE  R6, R0, eraB\n"
    "        ST   R7, 1(R0)      ; a A para aqui...\n"
    "        LI   R6, 1\n"
    "        ST   R6, 0(R0)\n"
    "        LD   R7, 2(R0)      ; ...e a B continua de onde parou\n"
    "        JR   R7\n"
    "eraB:   ST   R7, 2(R0)      ; a B para aqui...\n"
    "        ST   R0, 0(R0)\n"
    "        LD   R7, 1(R0)      ; ...e a A continua de onde parou\n"
    "        JR   R7\n"
    "; ---- O seu programa ----\n"
    "programa: LI  R1, 4         ; cria a tarefa B\n"
    "        LI   R2, tarefa_b\n"
    "        CALL sistema\n"
    "; A tarefa A mostra 1, 2 e 3, passando a vez depois de cada um, e encerra.\n"
    "; Ela usa R3 e R4: a tarefa B não pode mexer neles.\n"
    "tarefa_a: LI  R3, 1\n"
    "        LI   R4, 4\n"
    "volta_a: MOV  R2, R3\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R3, R3, 1\n"
    "        BNE  R3, R4, volta_a\n"
    "        LI   R1, 3          ; encerra\n"
    "        CALL sistema\n"
    "; A tarefa B mostra 10, 20, 30... passando a vez depois de cada um. Ela usa R5.\n"
    "tarefa_b: LI  R5, 10\n"
    "volta_b: MOV  R2, R5\n"
    "        LI   R1, 1          ; mostra R2\n"
    "        CALL sistema\n"
    "        LI   R1, 5          ; passa a vez\n"
    "        CALL sistema\n"
    "        ADDI R5, R5, 10\n"
    "        JMP  volta_b\n";

std::vector<ComputerProgram> makePrograms() {
    return {
        {"soma", "Soma de 1 a 10", "O exemplo clássico: um laço que soma",
         nullptr, 0, true},  // the computer's own example program, filled in below
        {"multiplica", "Multiplicação", "6 × 7 somando o 6 sete vezes",
         "; Multiplica 6 por 7: soma o 6 sete vezes.\n"
         "        LI   R1, 6        ; o número\n"
         "        LI   R2, 7        ; quantas vezes\n"
         "        LI   R3, 0        ; o resultado\n"
         "laço:   ADD  R3, R3, R1\n"
         "        OUT  R3\n"
         "        ADDI R2, R2, -1\n"
         "        BNE  R2, R0, laço\n"
         "        HLT\n", 0, true},
        {"fibonacci", "Fibonacci", "Cada número é a soma dos dois anteriores",
         "; A sequência de Fibonacci: 1, 1, 2, 3, 5, 8...\n"
         "; Cada número é a soma dos dois anteriores.\n"
         "        LI   R1, 0        ; o anterior\n"
         "        LI   R2, 1        ; o atual\n"
         "        LI   R4, 12       ; quantos números\n"
         "laço:   OUT  R2\n"
         "        ADD  R3, R1, R2   ; o próximo\n"
         "        MOV  R1, R2\n"
         "        MOV  R2, R3\n"
         "        ADDI R4, R4, -1\n"
         "        BNE  R4, R0, laço\n"
         "        HLT\n", 0, true},
        {"potencias", "Potências de 2", "Dobrar é deslocar um bit para a esquerda",
         "; 1, 2, 4, 8... cada SHL desloca os bits uma casa para a esquerda,\n"
         "; e isso dobra o número.\n"
         "        LI   R1, 1\n"
         "        LI   R2, 15       ; quantas vezes\n"
         "laço:   OUT  R1\n"
         "        SHL  R1, R1\n"
         "        ADDI R2, R2, -1\n"
         "        BNE  R2, R0, laço\n"
         "        OUT  R1\n"
         "        HLT\n", 0, true},
        {"regressiva", "Contagem regressiva", "Começa do número das chaves",
         "; Lê o número das chaves e conta de lá até zero.\n"
         "; Mude as chaves e aperte Zerar para contar de outro número.\n"
         "        IN   R1\n"
         "        BEQ  R1, R0, fim\n"
         "laço:   OUT  R1\n"
         "        ADDI R1, R1, -1\n"
         "        BNE  R1, R0, laço\n"
         "fim:    OUT  R1\n"
         "        HLT\n",
         10, true},
        {"lista", "Soma de uma lista", "Guarda números na memória e soma",
         "; Guarda cinco números na memória e depois soma todos.\n"
         "        LI   R1, 12\n"
         "        ST   R1, 0(R0)\n"
         "        LI   R1, 7\n"
         "        ST   R1, 1(R0)\n"
         "        LI   R1, 30\n"
         "        ST   R1, 2(R0)\n"
         "        LI   R1, 5\n"
         "        ST   R1, 3(R0)\n"
         "        LI   R1, 46\n"
         "        ST   R1, 4(R0)\n"
         "        LI   R2, 0        ; o endereço do número\n"
         "        LI   R3, 5        ; quantos faltam\n"
         "        LI   R4, 0        ; a soma\n"
         "laço:   LD   R1, 0(R2)\n"
         "        ADD  R4, R4, R1\n"
         "        OUT  R4\n"
         "        ADDI R2, R2, 1\n"
         "        ADDI R3, R3, -1\n"
         "        BNE  R3, R0, laço\n"
         "        HLT\n", 0, true},
        {"subrotina", "Sub-rotina", "CALL vai, JR R7 volta",
         "; Uma sub-rotina que triplica R1, chamada quatro vezes.\n"
         "; CALL guarda em R7 para onde voltar, e JR R7 volta.\n"
         "        LI   R1, 1\n"
         "        LI   R5, 4        ; quantas vezes\n"
         "laço:   CALL triplica\n"
         "        OUT  R1\n"
         "        ADDI R5, R5, -1\n"
         "        BNE  R5, R0, laço\n"
         "        HLT\n"
         "triplica: ADD R2, R1, R1\n"
         "        ADD  R1, R2, R1\n"
         "        JR   R7\n", 0, true},
        {"contador", "Contador sem fim", "Um laço que nunca para: acelere o clock",
         "; Conta para sempre. Use o + da barra para acelerar o clock.\n"
         "        LI   R1, 0\n"
         "laço:   OUT  R1\n"
         "        ADDI R1, R1, 1\n"
         "        JMP  laço\n",
         0, false},
        {"binario", "Programa em binário", "Instruções escritas como números",
         "; O laço que mostra 3, 2 e 1, escrito direto em linguagem de máquina.\n"
         "; Cada .word é uma instrução, e o _ só separa os campos.\n"
         "        .word 0b0010_001_000000011    ; LI   R1, 3\n"
         "laço:   .word 0b1011_000_001_000000   ; OUT  R1\n"
         "        .word 0b0001_001_001_111111   ; ADDI R1, R1, -1\n"
         "        .word 0b0111_001_000_111101   ; BNE  R1, R0, laço\n"
         "        .word 0b1111_000000000000     ; HLT\n",
         0, true},
        {"espera", "Esperar pelas chaves", "Lê as chaves até alguém mexer",
         "; Espera uma chave ser ligada, mostra o número e espera de novo.\n"
         "; Rode e mude as chaves no Painel.\n"
         "espera: IN   R1\n"
         "        BEQ  R1, R0, espera   ; nada ainda: lê de novo\n"
         "        OUT  R1               ; chegou um número: mostra\n"
         "solta:  IN   R2\n"
         "        BNE  R2, R0, solta    ; espera as chaves voltarem a 0\n"
         "        JMP  espera\n",
         0, false},
        {"chamadas", "Chamadas de sistema", "O programa pede, o sistema faz", kSystemCalls, 6, true},
        {"tarefas", "Duas tarefas", "O sistema reveza a CPU entre dois programas", kTwoTasks, 0, true},
    };
}

// The programs that run on top of an operating system, in a group of their own.
bool onSystem(std::string_view id) { return id == "chamadas" || id == "tarefas"; }

// What each program does, and how, for the strip over the board.
struct ProgramText {
    const char* id;
    const char* about;
};

constexpr ProgramText kProgramTexts[] = {
    {"soma", "Um laço clássico: R1 conta de 10 até 1, e a cada volta ADD soma R1 ao total em R2, que OUT mostra. ADDI tira 1 de R1, e BNE "
             "volta ao começo do laço enquanto R1 não chega a 0. No fim, a saída mostra 55."},
    {"multiplica", "Esta CPU não tem instrução de multiplicar, então o programa soma: R3 recebe 6 sete vezes, contadas por R2. É assim que "
                   "processadores simples multiplicam, em software, e o resultado é 42."},
    {"fibonacci", "Cada número da sequência é a soma dos dois anteriores. R1 e R2 guardam os dois últimos; ADD calcula o próximo em R3, e dois MOV "
                  "andam a janela uma casa para a frente. R4 conta quantos números faltam."},
    {"potencias", "SHL desloca os bits uma casa para a esquerda, o que dobra o número, como acrescentar um zero dobra um número em binário. "
                  "Depois de 15 vezes, o 1 chega ao bit mais alto: 32768."},
    {"regressiva", "IN lê o número das chaves para R1, e o laço mostra e diminui R1 até 0. BEQ no começo pula o laço se as chaves já estiverem "
                   "em 0, para não contar para baixo de zero."},
    {"lista", "Uma lista na memória: cinco ST gravam os números nos endereços 0 a 4. Depois, R2 anda pelos endereços e LD traz cada número de "
              "volta para somar em R4. Percorrer a memória com um registrador de endereço é a base de vetores e textos."},
    {"subrotina", "triplica é uma sub-rotina, um pedaço de programa usado várias vezes. CALL triplica pula para ela e guarda em R7 o endereço "
                  "da instrução seguinte ao CALL; no fim, JR R7 volta para esse endereço. O laço a chama quatro vezes: 3, 9, 27 e 81."},
    {"contador", "Um laço sem fim: R1 sobe de um em um e OUT mostra cada valor. Sem HLT, o programa roda enquanto o clock rodar, como o laço "
                 "principal de qualquer sistema."},
    {"binario", "O mesmo laço de sempre, só que sem nomes: cada instrução é escrita como o número de 16 bits que a CPU lê. LI é o op 0010, "
                "OUT o 1011, ADDI o 0001 e BNE o 0111; o -3 do BNE é a distância até o laço, em complemento de dois. No Raio-X, a faixa de bits "
                "mostra cada campo."},
    {"espera", "Espera ocupada: o laço lê as chaves sem parar e só segue quando elas deixam de ser 0. Depois de mostrar o número, outro laço "
               "espera as chaves voltarem a 0, para não mostrar o mesmo número de novo. A CPU passa o tempo todo perguntando, e é por isso "
               "que computadores de verdade preferem interrupções."},
    {"chamadas", "Os endereços 0 a 9 são um sistema operacional pequeno; o programa começa no 10. Ele põe o número do serviço em R1 e faz "
                 "CALL sistema: o PC pula para o sistema e R7 guarda o endereço da instrução seguinte. O sistema faz o serviço (ler, mostrar, "
                 "encerrar) e volta com JR R7."},
    {"tarefas", "O sistema reveza duas tarefas na CPU. Ao pedir para passar a vez (CALL sistema), a tarefa deixa em R7 o endereço onde vai "
                "continuar; o sistema guarda esse endereço na memória e pula para onde a outra parou. A saída alterna 1, 10, 2, 20, 3, 30."},
};

const char* programAbout(std::string_view id) {
    for (const ProgramText& text : kProgramTexts) {
        if (id == text.id) return text.about;
    }
    return "";
}

std::vector<GalleryItem> makeItems() {
    std::vector<GalleryItem> items = circuitExamples();
    for (std::size_t i = 0; i < computerPrograms().size(); ++i) {
        const ComputerProgram& program = computerPrograms()[i];
        items.push_back({program.id, onSystem(program.id) ? kGroupSystem : kGroupComputer, program.title, program.line, {}, static_cast<int>(i),
                         programAbout(program.id),
                         "Use Próxima instrução no Painel e acompanhe, no depurador à esquerda, os registradores que mudam. Para mudar o "
                         "programa, selecione a ROM."});
    }
    return items;
}

}  // namespace

const std::vector<ComputerProgram>& computerPrograms() {
    static const std::vector<ComputerProgram> all = [] {
        std::vector<ComputerProgram> programs = makePrograms();
        programs[0].source = exampleProgram();
        return programs;
    }();
    return all;
}

const std::vector<const char*>& galleryGroups() {
    static const std::vector<const char*> groups{"Eletrônica", "Portas lógicas", "Mais portas e seleção", "Contas de 1 bit", "Contas de N bits",
                                                 "Mais contas", "Memória", "Computador", "Sistema"};
    return groups;
}

const std::vector<GalleryItem>& galleryItems() {
    static const std::vector<GalleryItem> items = makeItems();
    return items;
}

GalleryProject buildGalleryItem(const GalleryItem& item, std::size_t version) {
    if (!item.versions.empty()) {
        GalleryProject out = item.versions[version < item.versions.size() ? version : 0].build();
        out.note = item.tryIt;
        return out;
    }
    const ComputerProgram& program = computerPrograms()[static_cast<std::size_t>(item.program)];
    const assembly::Result assembled = assembly::assemble(program.source);
    ComputerProject computer = buildComputer16(assembled.words, program.source);
    GalleryProject out;
    out.board = std::move(computer.board);
    out.chips = std::move(computer.chips);
    if (program.switches != 0) out.toggles.emplace_back(computer.switches, program.switches);
    out.note = item.tryIt;
    out.computer = true;
    return out;
}

}  // namespace bancada::editor
