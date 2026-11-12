#include "editor/course.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "asm/assembler.hpp"
#include "editor/computer16.hpp"
#include "editor/gallery.hpp"
#include "editor/layout.hpp"
#include "editor/lessons.hpp"

namespace bancada::editor {

namespace {

using K = TheoryBlockKind;

constexpr std::uint64_t kOhm = 1000;  // resistors keep milliohms
constexpr std::uint8_t kUpright = 1;  // a quarter turn: first pin at the top

// --- The figures: small circuits, drawn live inside the readings --------------------------------------

// A battery, a resistor and an LED in a loop, offset to the right by x. The LED backwards, if asked.
void ledLoop(Layout& l, int x, std::uint64_t ohms, bool forwards = true) {
    const PartId battery = l.part(PartKind::BatteryCell, {x, 10});
    const PartId resistor = l.part(PartKind::Resistor, {x + 8, 2}, 0, ohms * kOhm);
    const PartId light = l.part(PartKind::LedBulb, {x + 16, 2});
    l.wire(battery, 0u, resistor, 0u);
    l.wire(resistor, 1u, light, forwards ? 0u : 1u);
    l.wire(light, forwards ? 1u : 0u, battery, 1u);
}

TheoryFigure firstCircuit() {
    TheoryFigure figure;
    Layout l(figure.board);
    ledLoop(l, 0, 330);
    return figure;
}

TheoryFigure ledBothWays() {
    TheoryFigure figure;
    Layout l(figure.board);
    ledLoop(l, 0, 330, true);
    ledLoop(l, 34, 330, false);
    return figure;
}

TheoryFigure sourceAndGround() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId source = l.part(PartKind::Battery, {0, 10});
    const PartId ground = l.part(PartKind::Ground, {9, 13});
    l.wire(source, 1u, ground, 0u);
    const PartId resistor = l.part(PartKind::Resistor, {8, 2}, 0, 330 * kOhm);
    const PartId light = l.part(PartKind::LedBulb, {16, 2});
    l.wire(source, 0u, resistor, 0u);
    l.wire(resistor, 1u, light, 0u);
    l.wire(light, 1u, l.part(PartKind::Ground, {22, 8}), 0u);
    return figure;
}

TheoryFigure threeResistors() {
    TheoryFigure figure;
    Layout l(figure.board);
    ledLoop(l, 0, 2200);
    ledLoop(l, 34, 1000);
    ledLoop(l, 68, 330);
    return figure;
}

// The LED loop with a switch after the battery's +, open on the left and closed on the right.
TheoryFigure switchOpenAndClosed() {
    TheoryFigure figure;
    Layout l(figure.board);
    for (int side = 0; side < 2; ++side) {
        const int x = side * 36;
        const PartId battery = l.part(PartKind::BatteryCell, {x, 14});
        const PartId toggle = l.part(PartKind::Switch, {x + 4, 4});
        const PartId resistor = l.part(PartKind::Resistor, {x + 14, 0}, 0, 330 * kOhm);
        const PartId light = l.part(PartKind::LedBulb, {x + 22, 0});
        l.wire(battery, 0u, toggle, 0u);
        l.wire(toggle, 1u, resistor, 0u);
        l.wire(resistor, 1u, light, 0u);
        l.wire(light, 1u, battery, 1u);
        if (side == 1) figure.closed.push_back(toggle);
    }
    return figure;
}

// Two switches in a row on the left and side by side on the right, one of each pair closed.
TheoryFigure seriesAndParallel() {
    TheoryFigure figure;
    Layout l(figure.board);
    for (int side = 0; side < 2; ++side) {
        const bool parallel = side == 1;
        const int x = side * 40;
        const PartId battery = l.part(PartKind::BatteryCell, {x, 16});
        const PartId first = l.part(PartKind::Switch, {x + 4, 2});
        const PartId second = l.part(PartKind::Switch, parallel ? Point{x + 4, 7} : Point{x + 11, 2});
        const PartId resistor = l.part(PartKind::Resistor, {x + 18, 2}, 0, 330 * kOhm);
        const PartId light = l.part(PartKind::LedBulb, {x + 25, 0});
        l.wire(battery, 0u, first, 0u);
        if (parallel) {
            l.wire(battery, 0u, second, 0u);
            l.wire(first, 1u, resistor, 0u);
            l.wire(second, 1u, resistor, 0u);
        } else {
            l.wire(first, 1u, second, 0u);
            l.wire(second, 1u, resistor, 0u);
        }
        l.wire(resistor, 1u, light, 0u);
        l.wire(light, 1u, battery, 1u);
        figure.closed.push_back(first);
    }
    return figure;
}

// A circuit of the gallery, in one of its versions, with these inputs switched on besides its own.
TheoryFigure fromGallery(std::string_view id, std::size_t version, std::initializer_list<const char*> on = {}) {
    for (const GalleryItem& item : galleryItems()) {
        if (id != item.id) continue;
        GalleryProject project = buildGalleryItem(item, version);
        TheoryFigure figure;
        for (const auto& [part, value] : project.toggles) {
            if (value != 0) figure.closed.push_back(part);
        }
        for (const char* name : on) {
            bool found = false;
            for (const Part& part : project.board.parts()) {
                if (part.kind == PartKind::Toggle && part.label == name) {
                    figure.closed.push_back(part.id);
                    found = true;
                }
            }
            if (!found) throw std::logic_error("no input " + std::string(name) + " in " + std::string(id));
        }
        figure.board = std::move(project.board);
        figure.chips = std::move(project.chips);
        return figure;
    }
    throw std::logic_error("no gallery item " + std::string(id));
}

// The transistor as a switch: a switch from 5V to the base through 10 kΩ, the LED from 5V to the collector.
void transistorStage(Layout& l, int x, std::vector<PartId>& closed, bool on) {
    const PartId source = l.part(PartKind::Battery, {x + 40, 24});
    l.name(source, 0, "5V", true);
    l.wire(source, 1u, l.part(PartKind::Ground, {x + 46, 27}), 0u);
    Part supply = makePart(PartKind::Tunnel, {x, 14});
    supply.label = "5V";
    const PartId label = l.add(std::move(supply));
    const PartId load = l.part(PartKind::Resistor, {x + 26, 0}, kUpright, 330 * kOhm);
    l.name(load, 0, "5V", true);
    const PartId light = l.part(PartKind::LedBulb, {x + 28, 5});
    l.wire(load, 1u, light, 0u);
    const PartId transistor = l.part(PartKind::Npn, {x + 24, 15});
    const PartId ground = l.part(PartKind::Ground, {x + 26, 24});
    const PartId toggle = l.part(PartKind::Switch, {x + 8, 14});
    const PartId base = l.part(PartKind::Resistor, {x + 15, 14}, 0, 10'000 * kOhm);
    l.wire(label, 0u, toggle, 0u);
    l.wire(toggle, 1u, base, 0u);
    l.wire(base, 1u, transistor, 0u);
    l.wire(transistor, 2u, ground, 0u);
    l.wire(light, 1u, transistor, 1u);
    if (on) closed.push_back(toggle);
}

TheoryFigure transistorOffAndOn() {
    TheoryFigure figure;
    Layout l(figure.board);
    transistorStage(l, 0, figure.closed, false);
    transistorStage(l, 60, figure.closed, true);
    return figure;
}

TheoryFigure notOfTransistor() { return fromGallery("nao", 1); }
TheoryFigure nandOfTransistors() { return fromGallery("nand", 1, {"A"}); }

// The six basic gates, each with its own A and B, all with A on and B off.
TheoryFigure gateSymbols() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartKind kinds[] = {PartKind::And, PartKind::Or, PartKind::Not, PartKind::Nand, PartKind::Nor, PartKind::Xor};
    const char* names[] = {"AND", "OR", "NOT", "NAND", "NOR", "XOR"};
    for (int i = 0; i < 6; ++i) {
        const int x = (i % 3) * 36;
        const int y = (i / 3) * 16;
        const PartId a = l.input("A", {x, y + 2});
        figure.closed.push_back(a);
        if (kinds[i] == PartKind::Not) {
            const PartId gate = l.gate(kinds[i], {x + 12, y + 1});
            l.wire(a, 0u, gate, 0u);
            l.wire(gate, 1u, l.output(names[i], {x + 22, y + 2}), 0u);
            continue;
        }
        const PartId b = l.input("B", {x, y + 8});
        const PartId gate = l.gate(kinds[i], {x + 12, y + 4});
        l.wire(a, 0u, gate, 0u);
        l.wire(b, 0u, gate, 1u);
        l.wire(gate, 2u, l.output(names[i], {x + 22, y + 5}), 0u);
    }
    return figure;
}

// A NAND with both inputs on A: a NOT.
TheoryFigure notFromNand() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId a = l.input("A", {0, 2});
    const PartId gate = l.gate(PartKind::Nand, {12, 4});
    l.wire(a, 0u, gate, 0u);
    l.wire(a, 0u, gate, 1u);
    l.wire(gate, 2u, l.output("S", {22, 5}), 0u);
    return figure;
}

// Two NANDs in a row: the second undoes the first's inversion, and an AND is left.
TheoryFigure andFromNand() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId a = l.input("A", {0, 2});
    const PartId b = l.input("B", {0, 8});
    const PartId first = l.gate(PartKind::Nand, {12, 4});
    const PartId second = l.gate(PartKind::Nand, {24, 4});
    l.wire(a, 0u, first, 0u);
    l.wire(b, 0u, first, 1u);
    l.wire(first, 2u, second, 0u);
    l.wire(first, 2u, second, 1u);
    l.wire(second, 2u, l.output("S", {34, 5}), 0u);
    figure.closed = {a, b};
    return figure;
}

TheoryFigure decoderGates() { return fromGallery("decodificador", 1, {"A0"}); }
TheoryFigure halfAdder() { return fromGallery("meio-somador", 1, {"A", "B"}); }
TheoryFigure fullAdder() { return fromGallery("somador", 1, {"A", "B", "Ce"}); }
TheoryFigure subtractor() { return fromGallery("subtrator", 1, {"B"}); }
TheoryFigure comparator() { return fromGallery("comparador", 1, {"A"}); }
TheoryFigure aluSlice() { return fromGallery("ula1", 1, {"A", "B"}); }
TheoryFigure latch() { return fromGallery("latch", 1, {"S"}); }
TheoryFigure registerReady() { return fromGallery("registrador", 0); }
TheoryFigure counter() { return fromGallery("contador4", 1); }

TheoryFigure parityGates() { return fromGallery("paridade-impar", 1); }
TheoryFigure priorityGates() { return fromGallery("prioridade", 1); }
TheoryFigure negatorGates() { return fromGallery("negador", 1); }
TheoryFigure multiplierGates() { return fromGallery("multiplicador", 0); }
TheoryFigure display7() { return fromGallery("display7", 0); }
TheoryFigure buttonCounter() { return fromGallery("botao", 0); }
TheoryFigure randomShift() { return fromGallery("aleatorio", 1); }
TheoryFigure keyboardTerminal() { return fromGallery("teclado", 0); }

// The 16-bit computer with a three-instruction program in its ROM.
TheoryFigure computerBoard() {
    const char* source = "        LI  R1, 7\n        OUT R1\n        HLT\n";
    ComputerProject project = buildComputer16(assembly::assemble(source).words, source);
    TheoryFigure figure;
    figure.board = std::move(project.board);
    figure.chips = std::move(project.chips);
    return figure;
}

// --- The topics ----------------------------------------------------------------------------------------

std::vector<Topic> makeTopics() {
    std::vector<Topic> all;

    // Act 1: electricity.
    all.push_back(Topic{
        "eletricidade",
        0,
        "Corrente, bateria e LED",
        "O que é um circuito, a bateria, a fonte, o terra e o LED",
        {
            {K::Heading, "O que é eletricidade"},
            {K::Text,
             "Todo material é feito de átomos, e os átomos têm elétrons. Num metal, como o cobre dos fios, parte desses elétrons está solta e "
             "pode andar. Quando os elétrons andam todos para o mesmo lado, isso é uma corrente elétrica."},
            {K::Text,
             "Uma comparação ajuda: pense em água num cano. A tensão é a pressão que empurra a água; a corrente é quanto de água passa por "
             "segundo. Sem pressão a água fica parada, e sem tensão não há corrente."},
            {K::Table,
             "Grandeza|O que é|Unidade\n"
             "Tensão|o empurrão que faz a corrente andar|volt (V)\n"
             "Corrente|quanto de carga passa por segundo|ampère (A); 1 mA é um milésimo de ampère\n"
             "Resistência|o quanto uma peça freia a corrente|ohm (Ω)"},
            {K::Heading, "Um circuito é um caminho fechado"},
            {K::Text,
             "A corrente só anda se tiver por onde ir e por onde voltar. Ela sai da fonte de energia, passa pelas peças e volta para a mesma "
             "fonte. Esse caminho fechado se chama circuito. Se ele se abre em qualquer ponto, como um fio solto, a corrente para em todo o "
             "caminho, e não só depois do corte."},
            {K::Figure, "O primeiro circuito: a corrente sai do + da bateria, passa pelo resistor e pelo LED e volta ao -.", firstCircuit, 190},
            {K::Heading, "A bateria"},
            {K::Text,
             "A bateria é a fonte de energia. Ela tem dois polos: o + (positivo) e o - (negativo). Por dentro, uma reação química mantém uma "
             "diferença de tensão entre os dois. Por convenção, a corrente sai pelo + e volta pelo -. A bateria deste jogo dá 5 V, a "
             "tensão com que trabalham muitos circuitos digitais."},
            {K::Heading, "O LED"},
            {K::Text,
             "LED quer dizer diodo emissor de luz. Um diodo é uma peça que só deixa a corrente passar num sentido, como uma válvula de mão "
             "única. A perna do + (o anodo, a perna mais comprida num LED de verdade) tem que ficar do lado do + da bateria; a outra (o "
             "catodo) do lado do -. Ao contrário, o LED não conduz e fica apagado, sem estragar."},
            {K::Figure, "O mesmo circuito duas vezes: à esquerda o LED do jeito certo, aceso; à direita invertido, apagado.", ledBothWays, 190},
            {K::Text,
             "Um LED aceso fica com cerca de 2 V entre as pernas e gosta de uma corrente entre 5 e 20 mA. Com corrente demais ele esquenta e "
             "queima. É por isso que todo LED vai acompanhado de um resistor, a peça que segura a corrente. Como escolher esse resistor é "
             "assunto da próxima aula; por enquanto, use o de 330 Ω."},
            {K::Heading, "Fonte e terra: o jeito dos esquemas"},
            {K::Text,
             "Nos esquemas, os desenhos técnicos de circuitos, a bateria aparece como uma fonte: dois traços paralelos, o comprido é o + e o "
             "curto é o -. O - da fonte costuma ir num terra, o símbolo de três linhas que vão diminuindo."},
            {K::Text,
             "O terra é o ponto de 0 V, a referência de onde se medem todas as tensões. Todos os terras de um esquema são o mesmo ponto, "
             "ligados entre si mesmo sem um fio desenhado. Assim, em vez de puxar um fio comprido de cada peça de volta até o - da fonte, "
             "basta ligar a peça num terra perto dela."},
            {K::Figure, "O mesmo circuito, desenhado com fonte e terra: a corrente volta ao - da fonte pelos terras.", sourceAndGround, 190},
            {K::Tip,
             "Bateria ou fonte com terra: são duas formas de desenhar a mesma coisa. Nos exercícios, passe o mouse sobre qualquer peça para "
             "ler a tensão e a corrente nela."},
        },
        {"acender-led", "fonte-e-terra"},
    });

    all.push_back(Topic{
        "resistor-e-chave",
        0,
        "Resistor, lei de Ohm, chave e capacitor",
        "Quanto de corrente passa, e como ligar e desligar",
        {
            {K::Heading, "O resistor"},
            {K::Text,
             "O resistor é a peça que freia a corrente. Quanto maior a resistência, medida em ohms (Ω), menos corrente passa. Os resistores de "
             "verdade são pequenos cilindros com faixas coloridas; as cores dizem o valor. 1 kΩ (um quilo-ohm) são 1000 Ω."},
            {K::Heading, "A lei de Ohm"},
            {K::Text,
             "Tensão, corrente e resistência estão presas por uma conta simples, a lei de Ohm: a tensão sobre uma peça é a resistência vezes a "
             "corrente que passa nela. Escrita de outro jeito, a corrente é a tensão dividida pela resistência."},
            {K::Tip, "V = R × I, ou I = V ÷ R. Com V em volts e R em ohms, I sai em ampères."},
            {K::Text,
             "Veja o circuito do LED. A bateria dá 5 V e o LED aceso fica com uns 2 V; os 3 V que sobram ficam no resistor. Com 330 Ω, a "
             "corrente é 3 V ÷ 330 Ω ≈ 0,009 A, ou 9 mA: bem no meio da faixa boa do LED. Como a corrente é a mesma em todas as peças de um "
             "caminho único, esses 9 mA também passam pelo LED."},
            {K::Table,
             "Resistor|Corrente no LED|Resultado\n"
             "1 kΩ|3 V ÷ 1000 Ω = 3 mA|acende fraco\n"
             "330 Ω|3 V ÷ 330 Ω ≈ 9 mA|na medida\n"
             "100 Ω|3 V ÷ 100 Ω = 30 mA|corrente demais: queima\n"
             "10 Ω|3 V ÷ 10 Ω = 300 mA|queima na hora"},
            {K::Figure, "Três resistores lado a lado: 2,2 kΩ, 1 kΩ e 330 Ω. Menos ohms, mais corrente e mais brilho. Com 100 Ω ou menos, o LED queimaria.", threeResistors, 190},
            {K::Heading, "A chave"},
            {K::Text,
             "A chave abre ou fecha o caminho. Fechada, ela é como um pedaço de fio, e a corrente passa. Aberta, ela corta o circuito, e a "
             "corrente para no caminho todo. O interruptor de luz da sua casa é exatamente isso. Nos circuitos digitais, uma chave fechada "
             "costuma valer 1, e uma aberta, 0."},
            {K::Figure, "A mesma chave aberta, à esquerda, e fechada, à direita: só o circuito fechado acende.", switchOpenAndClosed, 200},
            {K::Heading, "O capacitor"},
            {K::Text,
             "O capacitor é um pequeno reservatório de carga: duas placas de metal separadas por um isolante. Ligado a uma tensão, ele vai se "
             "enchendo de carga, e depois devolve essa carga quando o resto do circuito pede. Sua capacidade se mede em farads (F); os comuns "
             "têm microfarads (µF)."},
            {K::Text,
             "Encher um capacitor por um resistor leva tempo: quanto maiores o resistor e o capacitor, mais devagar. Por isso o capacitor serve "
             "para marcar tempo. O pisca-pisca da Galeria usa dois deles para acender os LEDs um de cada vez, e o mesmo princípio está por trás "
             "do clock, o relógio que dá o ritmo de um computador."},
        },
        {"resistor-certo", "chave"},
    });

    all.push_back(Topic{
        "serie-e-paralelo",
        0,
        "Chaves em série e em paralelo",
        "Ligar duas chaves de dois jeitos: a primeira lógica",
        {
            {K::Heading, "Em série: uma depois da outra"},
            {K::Text,
             "Duas peças estão em série quando a corrente atravessa uma e depois a outra, num caminho só. Com duas chaves em série, basta uma "
             "aberta para cortar o caminho: o LED só acende quando a primeira E a segunda estão fechadas."},
            {K::Heading, "Em paralelo: lado a lado"},
            {K::Text,
             "Duas peças estão em paralelo quando ligam os mesmos dois pontos, cada uma oferecendo um caminho próprio. Com duas chaves em "
             "paralelo, basta uma fechada para a corrente passar: o LED acende quando a primeira OU a segunda está fechada."},
            {K::Figure, "Uma chave fechada de cada lado: em série (à esquerda) o LED fica apagado; em paralelo (à direita) ele acende.",
             seriesAndParallel, 200},
            {K::Heading, "Tabela-verdade"},
            {K::Text,
             "Para descrever um circuito assim, escreve-se uma tabela com todas as combinações das entradas e o que a saída faz em cada uma. "
             "Usando 1 para chave fechada ou LED aceso, e 0 para chave aberta ou LED apagado, as duas montagens ficam assim:"},
            {K::Table,
             "A|B|Série: A E B|Paralelo: A OU B\n"
             "0|0|0|0\n"
             "0|1|0|1\n"
             "1|0|0|1\n"
             "1|1|1|1"},
            {K::Text,
             "Essa tabela é a tabela-verdade, e as duas funções que ela mostra, E (AND) e OU (OR), são a base de toda a lógica digital. Os "
             "primeiros computadores faziam essas funções com chaves movidas por eletroímãs, os relés. No próximo ato, quem vai abrir e fechar "
             "o caminho é um transistor, uma chave sem partes móveis, comandada por outra corrente."},
            {K::Tip, "Em série: todas precisam estar fechadas (E). Em paralelo: basta uma (OU)."},
        },
        {"chaves-e", "chaves-ou"},
    });

    // Act 2: transistors.
    all.push_back(Topic{
        "o-transistor",
        1,
        "O transistor: uma chave comandada",
        "Base, coletor e emissor: uma corrente pequena que controla uma grande",
        {
            {K::Heading, "Uma chave sem as mãos"},
            {K::Text,
             "Nas chaves do Ato 1, era a sua mão que abria e fechava o caminho. Um computador precisa de milhões de chaves que se abram e "
             "fechem sozinhas, bilhões de vezes por segundo, comandadas pelos próprios sinais do circuito. Essa chave é o transistor."},
            {K::Text,
             "O transistor é feito de silício, um semicondutor: um material que conduz ou deixa de conduzir conforme é preparado e conforme o "
             "que recebe. Inventado em 1947, ele substituiu as válvulas e os relés, e hoje um processador tem bilhões deles."},
            {K::Heading, "As três pernas do NPN"},
            {K::Table,
             "Perna|O que faz\n"
             "Base (B)|a entrada de comando: uma corrente pequena aqui liga o transistor\n"
             "Coletor (C)|por onde entra a corrente grande, a da carga (o LED)\n"
             "Emissor (E)|por onde as duas correntes saem, em direção ao terra"},
            {K::Text,
             "No transistor NPN, o tipo usado aqui, a seta do símbolo fica no emissor e aponta para fora. Quando a base recebe corrente, mesmo "
             "pouca, o caminho entre o coletor e o emissor se abre e a corrente da carga passa. Sem corrente na base, o caminho fica fechado e "
             "nada passa."},
            {K::Tip, "Uma corrente pequena na base controla uma corrente grande do coletor para o emissor. É isso que faz do transistor uma chave "
                     "elétrica."},
            {K::Heading, "O resistor da base"},
            {K::Text,
             "A base se comporta como um diodo: a partir de uns 0,7 V ela conduz, e sem nada para limitar a corrente ela se estraga. Por isso "
             "o sinal chega à base através de um resistor, em geral de alguns quilo-ohms. Com 10 kΩ e 5 V, passa por volta de 0,4 mA na base: "
             "o bastante para o transistor deixar passar os 9 mA do LED."},
            {K::Figure,
             "À esquerda, a chave aberta: sem corrente na base, o transistor não conduz e o LED fica apagado. À direita, a chave fechada leva "
             "corrente à base, e o transistor acende o LED.",
             transistorOffAndOn, 240},
            {K::Text,
             "Repare que a chave agora só comanda a base: a corrente do LED nem passa por ela. Ela vem do 5V, atravessa o LED e o transistor "
             "e vai ao terra. O rótulo 5V funciona como o terra: todos os pontos com o mesmo nome estão ligados, sem precisar de fio."},
        },
        {"transistor-chave"},
    });

    all.push_back(Topic{
        "portas-de-transistores",
        1,
        "Portas feitas de transistores",
        "0 e 1 como tensões, o resistor de pull-up e as portas NÃO, NAND e NOR",
        {
            {K::Heading, "0 e 1 viram tensões"},
            {K::Text,
             "Dentro de um circuito digital só existem dois valores. Perto de 0 V vale 0: desligado, falso. Perto de 5 V vale 1: ligado, "
             "verdadeiro. Cada um desses valores é um bit. Uma porta lógica é um circuito que recebe bits e entrega outro bit, seguindo uma "
             "regra."},
            {K::Heading, "O inversor: a porta NÃO"},
            {K::Text,
             "Ligue a saída S no 5 V através de um resistor e ponha um transistor entre S e o terra. Com a entrada A em 0, o transistor fica "
             "desligado e o resistor puxa S para 5 V: S vale 1. Com A em 1, o transistor liga e puxa S para perto de 0 V: S vale 0. A saída é "
             "sempre o contrário da entrada. Esse resistor, que puxa para cima quando ninguém puxa para baixo, se chama resistor de pull-up."},
            {K::Figure, "O inversor com A em 0: o transistor desligado, e o resistor de pull-up mantém S em 1.", notOfTransistor, 240},
            {K::Table, "A|S = NÃO A\n0|1\n1|0"},
            {K::Heading, "Dois transistores: NAND e NOR"},
            {K::Text,
             "Com dois transistores em série entre S e o terra, S só vai a 0 quando os dois ligam, quando A E B valem 1. Em qualquer outro "
             "caso o pull-up mantém S em 1. É um E com a saída invertida: a porta NAND (NÃO-E)."},
            {K::Text,
             "Com os dois transistores em paralelo, basta um ligar para puxar S para 0, e S só vale 1 quando A e B valem 0. É um OU invertido: "
             "a porta NOR (NÃO-OU). São as chaves em série e em paralelo do Ato 1, agora comandadas por sinais."},
            {K::Figure, "A NAND de transistores com A em 1 e B em 0: um transistor ligado sozinho não basta para puxar S para 0.", nandOfTransistors,
             260},
            {K::Table, "A|B|NAND|NOR\n0|0|1|1\n0|1|1|0\n1|0|1|0\n1|1|0|0"},
            {K::Tip, "Em série, os transistores fazem uma NAND; em paralelo, uma NOR. Toda a lógica de um computador pode ser montada com só uma "
                     "dessas duas portas."},
        },
        {"inversor", "nand-transistores", "nor-transistores"},
    });

    // Act 3: logic gates.
    all.push_back(Topic{
        "portas-logicas",
        2,
        "As portas lógicas",
        "Símbolos, tabelas-verdade, expressões e as seis portas básicas",
        {
            {K::Heading, "Do transistor ao símbolo"},
            {K::Text,
             "Desenhar cada transistor de um circuito grande seria impossível. Por isso cada porta tem um símbolo próprio, e a partir daqui "
             "as portas vêm prontas na biblioteca. Por dentro elas continuam sendo transistores e resistores, como você montou no Ato 2."},
            {K::Table,
             "Porta|A saída é 1 quando...|Expressão\n"
             "E (AND)|todas as entradas são 1|S = A · B\n"
             "OU (OR)|pelo menos uma entrada é 1|S = A + B\n"
             "NÃO (NOT)|a entrada é 0|S = NÃO A\n"
             "NAND|nem todas as entradas são 1|S = NÃO (A · B)\n"
             "NOR|nenhuma entrada é 1|S = NÃO (A + B)\n"
             "OU exclusivo (XOR)|as entradas são diferentes|S = A XOR B"},
            {K::Figure,
             "As seis portas com A = 1 e B = 0: acendem OR, NAND e XOR. A NOT só olha A. A bolinha na saída de NOT, NAND e NOR quer dizer: "
             "invertido.",
             gateSymbols, 270},
            {K::Heading, "A álgebra das portas"},
            {K::Text,
             "Em 1854, George Boole mostrou que dá para fazer contas com verdadeiro e falso. Nessa álgebra booleana, o E se escreve como uma "
             "multiplicação (A · B), porque só dá 1 quando os dois são 1; o OU, como uma soma (A + B); e o NÃO, como NÃO A. Toda tabela-verdade "
             "pode ser escrita como uma expressão assim, e toda expressão vira um circuito de portas."},
            {K::Tip, "Para ler um circuito de portas, siga da entrada para a saída e escreva a expressão de cada fio. Para montar um, faça o "
                     "contrário: comece pela tabela-verdade."},
        },
        {"porta-e"},
    });

    all.push_back(Topic{
        "nand-universal",
        2,
        "NAND, a porta universal, e o OU exclusivo",
        "Qualquer porta feita só de NAND, e como combinar portas",
        {
            {K::Heading, "Uma porta para fazer todas"},
            {K::Text,
             "A porta NAND tem uma propriedade especial: com ela, e só com ela, dá para montar qualquer outra porta. Por isso se diz que ela é "
             "universal. As fábricas de chips aproveitam isso: um único tipo de bloco, repetido milhões de vezes, faz o circuito inteiro."},
            {K::Table,
             "Para ter...|Monte com NAND\n"
             "NÃO A|uma NAND com as duas entradas ligadas em A\n"
             "A E B|uma NAND e, depois dela, outra NAND que inverte a saída\n"
             "A OU B|inverta A e B com duas NAND e junte as duas numa terceira NAND"},
            {K::Figure, "Uma NAND com as duas entradas no mesmo fio: NÃO (A E A) é o mesmo que NÃO A.", notFromNand, 150},
            {K::Figure, "Duas NAND em fila: a segunda desfaz a inversão da primeira, e sobra o E. Com A e B em 1, a saída acende.", andFromNand,
             150},
            {K::Text,
             "A lei de De Morgan explica o OU: NÃO (NÃO A E NÃO B) é igual a A OU B. Em palavras: dizer que não é verdade que os dois estão "
             "desligados é o mesmo que dizer que pelo menos um está ligado."},
            {K::Heading, "OU exclusivo"},
            {K::Text,
             "O XOR dá 1 quando as entradas são diferentes. Ele pode ser montado com as portas que você já conhece: (A OU B) E NÃO (A E B), "
             "ou seja, pelo menos um ligado, mas não os dois. O XOR aparece em todo lugar: na soma de números, na comparação e na detecção de "
             "erros."},
            {K::Table, "A|B|A XOR B\n0|0|0\n0|1|1\n1|0|1\n1|1|0"},
        },
        {"nao-com-nand", "e-com-nand", "ou-com-nand", "xor"},
    });

    all.push_back(Topic{
        "decodificador-e-mux",
        2,
        "Decodificador e multiplexador",
        "Números de vários bits: escolher uma saída e escolher uma entrada",
        {
            {K::Heading, "Um número com vários bits"},
            {K::Text,
             "Um bit sozinho só diz sim ou não. Com dois bits juntos, A1 e A0, há quatro combinações: 00, 01, 10 e 11, que valem 0, 1, 2 e 3. "
             "Cada bit a mais dobra a quantidade: três bits contam até 7, oito bits até 255. É assim que os computadores escrevem números, e o "
             "Ato 4 mostra como fazer contas com eles."},
            {K::Heading, "O decodificador"},
            {K::Text,
             "O decodificador recebe um número e acende só a saída daquele número. Com 2 bits de entrada ele tem 4 saídas, Y0 a Y3. Cada saída "
             "é uma porta E que reconhece uma combinação: Y2, por exemplo, quer A1 = 1 e A0 = 0, então é A1 E NÃO A0. Na CPU, um decodificador "
             "escolhe qual registrador vai ser escrito."},
            {K::Figure, "O decodificador 2 para 4 feito de portas, com A0 = 1 e A1 = 0: o número é 1, e só Y1 acende.", decoderGates, 260},
            {K::Heading, "O multiplexador"},
            {K::Text,
             "O multiplexador, ou MUX, faz o contrário: tem várias entradas de dados e uma só saída, e um número de seleção escolhe qual "
             "entrada passa. Um MUX 4:1 tem as entradas D0 a D3 e a seleção S1 S0. Por dentro, é um decodificador da seleção, uma porta E por "
             "entrada e uma porta OU juntando tudo."},
            {K::Table, "S1|S0|A saída Y é\n0|0|D0\n0|1|D1\n1|0|D2\n1|1|D3"},
            {K::Tip, "Decodificador: um número escolhe uma saída. Multiplexador: um número escolhe uma entrada. A CPU do Ato 6 está cheia dos dois: "
                     "no Raio-X, cada MUX é um deles."},
        },
        {"decodificador", "mux4"},
    });

    // Act 4: arithmetic.
    all.push_back(Topic{
        "numeros-binarios",
        3,
        "Números binários e a soma",
        "Contar em base 2, o meio somador, o somador completo e a soma de vários bits",
        {
            {K::Heading, "Contar com dois dígitos"},
            {K::Text,
             "No sistema decimal, cada casa vale dez vezes a da direita: unidades, dezenas, centenas. No binário só há dois dígitos, 0 e 1, e "
             "cada casa vale o dobro da anterior: 1, 2, 4, 8, 16... Para ler um número binário, some o valor das casas que têm 1."},
            {K::Table,
             "Binário|8|4|2|1|Valor\n"
             "0101|0|1|0|1|4 + 1 = 5\n"
             "0110|0|1|1|0|4 + 2 = 6\n"
             "1011|1|0|1|1|8 + 2 + 1 = 11\n"
             "1111|1|1|1|1|15"},
            {K::Text,
             "Somar em binário é como somar no papel: casa por casa, da direita para a esquerda, com vai-um. A diferença é que 1 + 1 já não cabe "
             "numa casa: dá 10, ou seja, 0 e vai 1."},
            {K::Table, "A|B|Soma S|Vai-um C\n0|0|0|0\n0|1|1|0\n1|0|1|0\n1|1|0|1"},
            {K::Heading, "O meio somador"},
            {K::Text,
             "Olhe a tabela de novo: a soma S é 1 quando A e B são diferentes, um XOR; e o vai-um C é 1 só quando os dois são 1, um E. Duas "
             "portas fazem a soma de dois bits. Esse circuito se chama meio somador, porque ainda não recebe o vai-um que vem da casa de baixo."},
            {K::Figure, "O meio somador com A = 1 e B = 1: S apaga e C acende. 1 + 1 = 10 em binário, que é 2.", halfAdder, 200},
            {K::Heading, "O somador completo"},
            {K::Text,
             "Da segunda casa em diante, a soma tem três bits para juntar: A, B e o vai-um que veio da direita, Ce (vai-um de entrada). O "
             "somador completo soma os três e entrega S e o vai-um de saída, Cs. Ele é feito de dois meios somadores e uma porta OU que junta "
             "os dois vai-uns."},
            {K::Figure, "O somador completo com A, B e Ce em 1: 1 + 1 + 1 = 3, que é 11 em binário. S e Cs acendem.", fullAdder, 240},
            {K::Heading, "Somar números inteiros"},
            {K::Text,
             "Para somar números de 4 bits, ponha quatro somadores completos lado a lado e ligue o Cs de cada um no Ce do seguinte. O vai-um "
             "passa de casa em casa, como no papel: por isso esse circuito se chama somador com propagação do vai-um. A CPU do jogo faz "
             "exatamente isso, com 16 bits."},
            {K::Tip, "S = A XOR B XOR Ce, e Cs = (A E B) OU (Ce E (A XOR B)). Com isso dá para somar números de qualquer tamanho."},
        },
        {"meio-somador", "somador-completo", "somador-4-bits"},
    });

    all.push_back(Topic{
        "subtrair-comparar-ula",
        3,
        "Subtrair, comparar e a ULA",
        "Números negativos, o subtrator, o comparador e a unidade lógica e aritmética",
        {
            {K::Heading, "Subtrair com empréstimo"},
            {K::Text,
             "Subtrair bit a bit funciona como no papel, com empréstimo em vez de vai-um. 0 − 1 não dá: a casa pede 1 emprestado à de cima, e "
             "o resultado é 1, com empréstimo. O subtrator completo recebe A, B e o pedido que vem de baixo, Ve, e entrega a diferença D e o "
             "pedido para cima, Vs. A diferença é de novo um XOR: D = A XOR B XOR Ve."},
            {K::Figure, "O subtrator com A = 0 e B = 1: 0 − 1 dá D = 1 e pede emprestado, e Vs acende.", subtractor, 230},
            {K::Heading, "Números negativos"},
            {K::Text,
             "Com 4 bits, depois de 1111 o próximo número volta a 0000, como um hodômetro. Os computadores aproveitam isso para guardar números "
             "negativos: −1 é o número que, somado a 1, dá 0, ou seja, 1111. Esse jeito se chama complemento de dois. Para achar −N, inverta "
             "todos os bits de N e some 1."},
            {K::Table,
             "Bits|Sem sinal|Com sinal (complemento de dois)\n"
             "0011|3|3\n0001|1|1\n0000|0|0\n1111|15|−1\n1101|13|−3\n1000|8|−8"},
            {K::Text,
             "A vantagem é que o mesmo somador serve para os dois: A − B é A + (NÃO B) + 1. É por isso que a CPU tem uma só ULA para somar e "
             "subtrair."},
            {K::Heading, "Comparar"},
            {K::Text,
             "O comparador diz se A é maior, igual ou menor que B. Para um bit: A > B quando A = 1 e B = 0 (A E NÃO B); A < B no caso "
             "contrário (NÃO A E B); e A = B quando os dois são iguais, um XNOR. Para números de vários bits, compara-se do bit mais alto para "
             "o mais baixo: o primeiro bit diferente decide."},
            {K::Figure, "O comparador com A = 1 e B = 0: acende A>B.", comparator, 220},
            {K::Heading, "A ULA"},
            {K::Text,
             "A ULA, unidade lógica e aritmética, é onde a CPU faz as contas. Ela calcula ao mesmo tempo várias operações sobre A e B, como "
             "E, OU, XOR e soma, e um multiplexador escolhe qual resultado sai, de acordo com o código da operação, Op. Uma fatia de 1 bit faz "
             "isso para uma casa; lado a lado, com o vai-um passando entre elas, as fatias formam a ULA de 16 bits do computador."},
            {K::Figure, "A ULA de 1 bit montada com blocos: as quatro operações lado a lado e o MUX que escolhe uma delas pelo Op.", aluSlice, 280},
            {K::Table, "Op1|Op0|Operação\n0|0|A E B\n0|1|A OU B\n1|0|A XOR B\n1|1|A + B"},
        },
        {"subtrator", "comparador", "ula-1-bit"},
    });

    // Act 5: memory.
    all.push_back(Topic{
        "latches",
        4,
        "Circuitos que lembram",
        "Realimentação, o latch SR e o latch D",
        {
            {K::Heading, "Uma saída que volta para a entrada"},
            {K::Text,
             "Todos os circuitos até aqui são combinacionais: a saída depende só das entradas de agora. Para funcionar, um computador precisa "
             "lembrar: o número que está contando, a instrução em que parou. O truque é a realimentação: ligar a saída de uma porta na entrada "
             "de outra, que por sua vez volta para a primeira."},
            {K::Heading, "O latch SR"},
            {K::Text,
             "Duas portas NOR, cada uma com uma entrada vinda da saída da outra, formam um latch SR. S (set, ligar) põe Q em 1. R (reset, "
             "desligar) põe Q em 0. Com S e R em 0, as duas portas se seguram uma à outra e Q fica como estava: o circuito lembra o último "
             "comando. A outra saída é sempre o contrário de Q."},
            {K::Figure, "O latch SR feito de duas NOR, com S = 1: Q acende. Soltando S, Q continua aceso.", latch, 220},
            {K::Table, "S|R|Q depois\n0|0|fica como estava\n1|0|1\n0|1|0\n1|1|proibido: as duas saídas vão a 0"},
            {K::Heading, "O latch D"},
            {K::Text,
             "No latch SR é fácil errar e mandar S e R juntos. O latch D resolve isso com uma só entrada de dado, D, e uma entrada de "
             "permissão, E. Enquanto E vale 1, Q copia D; quando E volta a 0, Q guarda o último valor. Por dentro, são duas portas E na frente "
             "de um latch SR: uma manda S = D, a outra R = NÃO D, e as duas só deixam passar com E ligado."},
            {K::Tip, "Um latch guarda um bit. Oito latches lado a lado guardam um byte: a memória é feita de muitos deles."},
        },
        {"latch-sr", "latch-d"},
    });

    all.push_back(Topic{
        "clock-e-registradores",
        4,
        "Clock, flip-flop, registrador e contador",
        "O ritmo do computador e os circuitos que guardam números",
        {
            {K::Heading, "O clock"},
            {K::Text,
             "O clock é um sinal que alterna entre 0 e 1 sem parar, como um metrônomo. Cada passagem de 0 para 1 é uma borda de subida, e o "
             "computador inteiro anda no ritmo dela: a cada borda, um passo. A frequência diz quantos passos por segundo: o computador do jogo "
             "vai de 1 Hz a milhares de Hz, e um processador de verdade passa de 3 GHz, três bilhões de passos por segundo."},
            {K::Heading, "O flip-flop"},
            {K::Text,
             "O latch D copia D durante todo o tempo em que E vale 1, e isso é um problema quando a saída volta para a entrada: o valor dá "
             "voltas antes de E desligar. O flip-flop D resolve isso copiando D só no instante da borda de subida do clock. São dois latches em "
             "fila, o mestre e o escravo, com o clock invertido entre eles: um abre enquanto o outro fecha."},
            {K::Table, "Clock|O que Q faz\nparado em 0 ou em 1|guarda o valor\nborda de subida (de 0 para 1)|copia D\nborda de descida (de 1 para 0)|guarda o valor"},
            {K::Heading, "O registrador"},
            {K::Text,
             "Um registrador é um grupo de flip-flops com o mesmo clock: com 8 flip-flops ele guarda um número de 8 bits, todo de uma vez, a "
             "cada borda. Uma entrada de habilitação decide se ele carrega o valor novo ou mantém o que tem. Os oito registradores R0 a R7 da "
             "CPU são exatamente isso, com 16 bits cada."},
            {K::Figure, "Um registrador de 4 bits pronto: o número na entrada D só entra quando o clock sobe.", registerReady, 220},
            {K::Heading, "O contador"},
            {K::Text,
             "Ligue a saída de um registrador num somador que soma 1, e a saída do somador de volta na entrada do registrador. A cada borda do "
             "clock, o registrador guarda o próprio valor mais 1: é um contador. O PC, o contador de programa da CPU, funciona assim: a cada "
             "instrução ele avança para o endereço seguinte, a não ser que um desvio mande outro valor."},
            {K::Figure, "Um contador de 4 bits feito de um registrador e um somador. Na Galeria, aperte Rodar para vê-lo contar.", counter, 240},
            {K::Tip, "Combinacional: a saída depende só das entradas de agora. Sequencial: depende também do que foi guardado. Um computador é os "
                     "dois juntos, no ritmo do clock."},
        },
        {"flip-flop", "registrador", "contador"},
    });

    // Act 6: machine code.
    all.push_back(Topic{
        "a-cpu-por-dentro",
        5,
        "A CPU por dentro",
        "As peças do computador de 16 bits e o ciclo de cada instrução",
        {
            {K::Heading, "Um programa é uma lista de números"},
            {K::Text,
             "Tudo o que você montou até aqui se junta agora num computador. A ideia que torna isso possível é a do programa armazenado: as "
             "instruções não são fios, são números guardados numa memória. Para mudar o que o computador faz, basta mudar esses números. Cada "
             "instrução deste computador é uma palavra de 16 bits na ROM, a memória do programa."},
            {K::Figure,
             "O computador de 16 bits na placa: a CPU, a ROM com o programa, a RAM com os dados, as chaves de entrada e a saída. Dentro da CPU "
             "estão as peças dos atos anteriores.",
             computerBoard, 280},
            {K::Heading, "As peças da CPU"},
            {K::Table,
             "Peça|O que faz|Onde você viu\n"
             "PC (contador de programa)|guarda o endereço da instrução atual|Ato 5: o contador\n"
             "ROM|guarda o programa, uma palavra por endereço|Ato 5: memória\n"
             "Decodificador|corta a palavra em campos|Ato 3: o decodificador\n"
             "Controle|liga os sinais de que cada instrução precisa|Ato 3: as portas\n"
             "Registradores R0 a R7|guardam os números com que a CPU trabalha|Ato 5: o registrador\n"
             "ULA|faz as contas e as comparações|Ato 4: a ULA\n"
             "MUX|escolhem o valor que segue por cada caminho|Ato 3: o multiplexador\n"
             "RAM|guarda os dados do programa|Ato 5: memória"},
            {K::Heading, "O ciclo de uma instrução"},
            {K::Text, "A CPU repete sempre os mesmos passos, uma instrução depois da outra. São as seis etapas que o Raio-X mostra:"},
            {K::Table,
             "Etapa|O que acontece\n"
             "1 Busca|o PC manda o endereço para a ROM, que devolve a palavra da instrução\n"
             "2 Decodifica|o decodificador separa os campos, e o controle decide quais sinais ligar\n"
             "3 Executa|a ULA faz a conta com os registradores ou com o número da instrução\n"
             "4 Memória|LD lê e ST grava na RAM; as outras instruções pulam esta etapa\n"
             "5 Escreve|o resultado volta para um registrador\n"
             "6 Próximo PC|o PC vai para a instrução seguinte, ou para onde um desvio mandar"},
            {K::Text,
             "Neste computador, cada instrução leva um ciclo de clock inteiro: as seis etapas acontecem de uma vez, como água correndo pelos "
             "canos, e o Raio-X só as separa para você acompanhar. Processadores maiores dividem as etapas e trabalham em várias instruções ao "
             "mesmo tempo, numa linha de montagem chamada pipeline."},
            {K::Heading, "Toda instrução é um número"},
            {K::Text,
             "Os 4 bits de cima de cada palavra são o código da operação, o op: ele diz qual instrução é. Com 4 bits há 16 códigos, e este "
             "computador usa 14. O resto da palavra traz os detalhes: quais registradores e, às vezes, um número escrito na própria instrução, "
             "o imediato."},
            {K::Table,
             "op|Binário|Instrução\n"
             "0|0000|contas da ULA: ADD, SUB, AND...\n"
             "1|0001|ADDI\n2|0010|LI\n3|0011|LUI\n4|0100|LD\n5|0101|ST\n6|0110|BEQ\n7|0111|BNE\n"
             "8|1000|JMP\n9|1001|CALL\n10|1010|JR\n11|1011|OUT\n12|1100|IN\n15|1111|HLT"},
            {K::Word, "LI R1, 7: op 2 (LI), rd 1 (o registrador R1) e imm9 7 (o número). Em hexadecimal, 2207.", nullptr, 0, 0x2207},
            {K::Tip, "Linguagem de máquina são os números que a CPU executa. Assembly é a mesma coisa escrita com nomes, para pessoas lerem; o "
                     "montador traduz um no outro."},
        },
        {"binario-primeiro"},
    });

    all.push_back(Topic{
        "formatos-das-instrucoes",
        5,
        "Os formatos das instruções",
        "Campos, registradores, func, imediatos e números em hexadecimal",
        {
            {K::Heading, "Quatro jeitos de dividir 16 bits"},
            {K::Text,
             "Toda instrução começa com o op, mas os outros 12 bits se dividem de jeitos diferentes, conforme o que a instrução precisa. Esses "
             "jeitos são os formatos. Ter poucos formatos deixa o decodificador simples: cada campo está sempre no mesmo lugar."},
            {K::Table,
             "Formato|Campos|Instruções\n"
             "R: três registradores|op(4) rd(3) rs(3) rt(3) func(3)|ADD, SUB, AND, OR, XOR, NOT, SHL, SHR\n"
             "I6: dois registradores e um número|op(4) rd(3) rs(3) imm6(6)|ADDI, LD, ST, BEQ, BNE\n"
             "I9: um registrador e um número|op(4) rd(3) imm9(9)|LI, LUI\n"
             "J: um endereço|op(4) imm12(12)|JMP, CALL"},
            {K::Heading, "Os registradores"},
            {K::Text,
             "Um campo de 3 bits escolhe um entre 8 registradores: 000 é R0, 111 é R7. rd é o destino, onde o resultado vai ficar; rs e rt são "
             "as fontes, de onde vêm os números. Por isso este computador tem exatamente oito registradores: é o que cabe em 3 bits."},
            {K::Heading, "func: a operação da ULA"},
            {K::Text,
             "Todas as contas entre registradores usam o mesmo op, 0000. Quem diz qual conta fazer é o campo func, os 3 bits do fim, que vai "
             "direto para a seleção do multiplexador da ULA."},
            {K::Table, "func|Binário|Conta\n0|000|AND\n1|001|OR\n2|010|XOR\n3|011|ADD\n4|100|SUB\n5|101|NOT\n6|110|SHL\n7|111|SHR"},
            {K::Word, "ADD R3, R1, R2: op 0, rd 3, rs 1, rt 2 e func 3 (ADD). Em hexadecimal, 0653.", nullptr, 0, 0x0653},
            {K::Heading, "Os imediatos"},
            {K::Text,
             "Um imediato é um número que vem dentro da instrução, com o tamanho que sobra: 6 bits no formato I6, 9 no I9 e 12 no J. O imm6 "
             "tem sinal, em complemento de dois, e vai de -32 a 31; o imm9 vai de 0 a 511. Antes de chegar à ULA, o extensor completa o "
             "imediato até 16 bits. Num número negativo, ele repete o bit de sinal nos bits de cima, para que -1 continue sendo -1."},
            {K::Word, "ADDI R1, R1, -1: o -1 em 6 bits é 111111. Em hexadecimal, 127F.", nullptr, 0, 0x127F},
            {K::Heading, "Hexadecimal"},
            {K::Text,
             "Escrever 16 bits cansa. O hexadecimal, a base 16, junta os bits de quatro em quatro: cada grupo vira um dígito de 0 a F, em que A "
             "vale 10 e F vale 15. Assim, 0010 0010 0000 0111 vira 2207. No assembly do jogo, 0x indica hexadecimal e 0b indica binário: "
             "0x2207 e 0b0010_001_000000111 são o mesmo número, e o _ só separa os campos para ler melhor."},
            {K::Table,
             "Binário|Hex|Binário|Hex\n"
             "0000|0|1000|8\n0001|1|1001|9\n0010|2|1010|A\n0011|3|1011|B\n0100|4|1100|C\n0101|5|1101|D\n0110|6|1110|E\n0111|7|1111|F"},
        },
        {"binario-soma"},
    });

    all.push_back(Topic{
        "desvios-e-montador",
        5,
        "Desvios, saltos e o montador",
        "Distâncias, endereços, sub-rotinas e o programa que traduz o assembly",
        {
            {K::Heading, "Desvios guardam distâncias"},
            {K::Text,
             "BEQ e BNE comparam dois registradores e, se a condição valer, desviam. O imm6 deles não é o endereço de destino, e sim a "
             "distância até ele, contada a partir da instrução seguinte: destino = PC + 1 + imm6. Um desvio para trás tem distância negativa. "
             "Guardar a distância, e não o endereço, faz o desvio caber em 6 bits e deixa o programa funcionar em qualquer lugar da ROM."},
            {K::Word, "BNE R1, R0, laço, com o laço três instruções antes da seguinte: imm6 = -3, ou 111101. Em hexadecimal, 723D.", nullptr, 0,
             0x723D},
            {K::Text,
             "No Raio-X, a conta aparece no somador Alvo do desvio: ele soma o PC+1 com o imm6 que vem do extensor. O sinal Z da ULA desce até o "
             "Controle, que o junta com o tipo de desvio e manda o MUX do Próximo PC escolher entre esse alvo e o PC + 1."},
            {K::Heading, "Saltos guardam endereços"},
            {K::Text,
             "JMP e CALL usam o formato J: 12 bits só para o endereço, de 0 a 4095, a ROM inteira. CALL ainda guarda em R7 o endereço da "
             "instrução seguinte, o PC+1, para que a sub-rotina possa voltar com JR R7. JR pega o endereço de um registrador, e por isso pode "
             "voltar para qualquer lugar."},
            {K::Word, "CALL 10: op 9 e imm12 10. Em hexadecimal, 900A.", nullptr, 0, 0x900A},
            {K::Heading, "O montador"},
            {K::Text,
             "O montador é o programa que traduz assembly em linguagem de máquina. Ele lê o texto duas vezes. Na primeira, só conta as "
             "instruções, para saber o endereço de cada rótulo. Na segunda, monta cada palavra: procura o op do nome, põe os números dos "
             "registradores nos campos certos e, nos desvios, calcula a distância até o rótulo."},
            {K::Text,
             "Alguns nomes nem são instruções da CPU: são pseudo-instruções, que o montador troca por instruções de verdade. MOV R2, R1 vira "
             "ADD R2, R1, R0; NOP vira ADD R0, R0, R0; RET vira JR R7. E .word grava um número qualquer na ROM, sem tradução: é o que você vai "
             "usar nos exercícios."},
            {K::Table,
             "Assembly|Endereço|Palavra\n"
             "LI R1, 3|0|0010 001 000000011\n"
             "laço: OUT R1|1|1011 000 001 000000\n"
             "ADDI R1, R1, -1|2|0001 001 001 111111\n"
             "BNE R1, R0, laço|3|0111 001 000 111101\n"
             "HLT|4|1111 000000000000"},
            {K::Tip, "Rótulos viram endereços, nomes viram op e func, registradores viram 3 bits. O montador só faz a conta que você vai fazer à "
                     "mão."},
        },
        {"binario-laco"},
    });

    // Act 7: programming.
    all.push_back(Topic{
        "assembly",
        6,
        "Assembly: programar com nomes",
        "Registradores, instruções, o editor e o depurador",
        {
            {K::Heading, "Do binário para os nomes"},
            {K::Text,
             "No ato anterior você escreveu instruções bit a bit. Funciona, mas ninguém programa assim: um bit errado e o programa faz outra "
             "coisa. O assembly dá um nome a cada instrução e deixa o montador fazer a tradução. Cada linha de assembly vira exatamente uma "
             "palavra na ROM."},
            {K::Code,
             "; mostra 42 na saída\n"
             "        LI   R1, 42     ; R1 recebe 42\n"
             "        OUT  R1         ; a saída mostra R1\n"
             "        HLT             ; o computador para"},
            {K::Heading, "Registradores são as variáveis"},
            {K::Text,
             "A CPU só faz contas com o que está nos registradores, R0 a R7. Pense neles como oito caixinhas de 16 bits, cada uma com um número "
             "de 0 a 65535. R0 é especial: vale sempre 0, e o que se grava nele se perde. Isso é útil: ADD R2, R1, R0 copia R1 para R2, e "
             "BEQ R1, R0 testa se R1 é zero."},
            {K::Table,
             "Instrução|O que faz|Exemplo\n"
             "LI Rd, n|põe um número de 0 a 511 em Rd|LI R1, 300\n"
             "ADD Rd, Rs, Rt|Rd = Rs + Rt|ADD R3, R1, R2\n"
             "SUB Rd, Rs, Rt|Rd = Rs - Rt|SUB R3, R1, R2\n"
             "ADDI Rd, Rs, n|Rd = Rs + n, com n de -32 a 31|ADDI R1, R1, 1\n"
             "OUT Rs|mostra Rs na saída|OUT R3\n"
             "HLT|para o computador|HLT"},
            {K::Heading, "Números maiores que 511"},
            {K::Text,
             "O LI só tem 9 bits para o número, então vai até 511. Para números maiores, a ULA ajuda: guarde dois pedaços e some, como no "
             "exercício. Outro jeito é o LUI, que põe n × 128 num registrador, seguido de um ADDI com a parte de baixo. A pseudo-instrução LIW "
             "faz essa conta sozinha."},
            {K::Heading, "Montar, rodar e depurar"},
            {K::Text,
             "No editor, Ctrl+Enter monta o programa e grava as palavras na ROM; os erros aparecem embaixo, com a linha. Para rodar, aperte "
             "Rodar, ou use o depurador do Painel: Instrução executa uma instrução por vez, e Etapa mostra cada uma das seis etapas no Raio-X. "
             "Os registradores e a memória aparecem à esquerda, mudando a cada passo. O botão Ajuda do editor lista todas as instruções."},
            {K::Tip, "Quando um programa não faz o que você quer, rode uma instrução por vez e olhe os registradores. O erro está no primeiro número "
                     "diferente do que você esperava."},
        },
        {"primeiro-programa", "conta-grande"},
    });

    all.push_back(Topic{
        "lacos-e-memoria",
        6,
        "Laços, decisões e memória",
        "Rótulos, desvios, contadores e a RAM",
        {
            {K::Heading, "Rótulos e desvios"},
            {K::Text,
             "Um rótulo é um nome seguido de dois-pontos no começo de uma linha: ele marca o endereço daquela instrução. Os desvios usam rótulos "
             "como destino. BEQ Ra, Rb, lugar desvia se os dois forem iguais; BNE, se forem diferentes; JMP desvia sempre."},
            {K::Heading, "O laço"},
            {K::Text,
             "Um laço repete instruções. O padrão quase sempre é o mesmo: um registrador conta, o corpo do laço faz o trabalho, a conta avança, "
             "e um desvio volta para o começo enquanto ela não chega ao fim."},
            {K::Code,
             "        LI   R1, 1        ; o contador começa em 1\n"
             "        LI   R2, 6        ; e para quando chegar a 6\n"
             "laço:   OUT  R1           ; o trabalho: mostrar o contador\n"
             "        ADDI R1, R1, 1    ; a conta avança\n"
             "        BNE  R1, R2, laço ; volta enquanto R1 for diferente de 6\n"
             "        HLT"},
            {K::Heading, "Decisões: se e senão"},
            {K::Text,
             "Não existe uma instrução se. Uma decisão é feita com um desvio que pula o trecho que não deve rodar. Para fazer \"se R1 = R2, "
             "mostre 1; senão, mostre 0\", desvie para o caso igual e, no fim do outro caso, pule por cima dele com JMP."},
            {K::Code,
             "        BEQ  R1, R2, igual\n"
             "        LI   R3, 0        ; senão: diferentes\n"
             "        JMP  fim\n"
             "igual:  LI   R3, 1        ; se: iguais\n"
             "fim:    OUT  R3"},
            {K::Heading, "A memória"},
            {K::Text,
             "Oito registradores acabam rápido. A RAM guarda muito mais: cada endereço tem uma palavra de 16 bits. ST Rd, n(Rs) grava Rd no "
             "endereço Rs + n, e LD Rd, n(Rs) lê de volta. Usar um registrador como base permite andar pela memória: com R4 apontando para o "
             "começo de uma lista, LD R1, 0(R4) lê o primeiro item, e ADDI R4, R4, 1 passa para o seguinte."},
            {K::Tip, "Registradores são rápidos e poucos; a memória é maior e precisa de LD e ST. Programas de verdade carregam da memória, calculam "
                     "nos registradores e guardam de volta."},
        },
        {"conte-ate-cinco", "guarde-na-memoria"},
    });

    all.push_back(Topic{
        "sub-rotinas",
        6,
        "Sub-rotinas",
        "CALL, JR, o endereço de volta e sub-rotinas que chamam outras",
        {
            {K::Heading, "Um pedaço de programa com nome"},
            {K::Text,
             "Quando o mesmo trecho aparece várias vezes, ele vira uma sub-rotina: escrito uma vez só, com um rótulo, e chamado de onde for "
             "preciso. É a mesma ideia das funções nas linguagens de programação."},
            {K::Heading, "Ir e voltar"},
            {K::Text,
             "CALL rótulo faz duas coisas: guarda em R7 o endereço da instrução seguinte e pula para o rótulo. No fim da sub-rotina, JR R7 pula "
             "para o endereço guardado, e o programa continua logo depois do CALL. RET é outro nome para JR R7."},
            {K::Code,
             "        LI   R1, 3\n"
             "        CALL dobra        ; R7 = endereço do OUT; vai para dobra\n"
             "        OUT  R1           ; mostra 6\n"
             "        HLT\n"
             "dobra:  ADD  R1, R1, R1   ; a sub-rotina\n"
             "        JR   R7           ; volta para o OUT"},
            {K::Heading, "Combinados"},
            {K::Text,
             "Para conversar, quem chama e a sub-rotina combinam onde ficam os números. Um costume comum: os argumentos vão em R1 e R2, e o "
             "resultado volta em R1. A sub-rotina pode usar outros registradores à vontade, desde que quem chama saiba que eles podem mudar."},
            {K::Heading, "Uma sub-rotina que chama outra"},
            {K::Text,
             "Se uma sub-rotina fizer CALL, o R7 recebe o novo endereço de volta, e o antigo se perde. Antes de chamar outra, ela precisa guardar "
             "R7 na memória e trazê-lo de volta depois. Os processadores organizam isso numa pilha: cada chamada empilha o seu endereço de volta, "
             "e cada retorno desempilha o último."},
            {K::Tip, "Ponha o HLT antes das sub-rotinas. Se não, o programa termina o trabalho e cai dentro delas sem ter sido chamado."},
        },
        {"sub-rotina"},
    });

    // Act 8: the operating system.
    all.push_back(Topic{
        "entrada-saida-espera",
        7,
        "Entrada, saída e espera",
        "As portas de entrada e saída, e como esperar pelo mundo",
        {
            {K::Heading, "Conversar com o mundo"},
            {K::Text,
             "Um computador que só faz contas consigo mesmo não serve para muito. As portas de entrada e saída ligam a CPU ao mundo: IN Rd lê o "
             "número que as chaves formam, e OUT Rs mostra um número na saída. Em computadores de verdade, essas portas levam ao teclado, à "
             "tela, ao disco e à rede."},
            {K::Heading, "Esperar: a espera ocupada"},
            {K::Text,
             "O mundo é lento perto da CPU. Para esperar alguém mexer nas chaves, o programa pode ler a entrada sem parar até ela mudar. Isso se "
             "chama espera ocupada: funciona, mas a CPU passa o tempo todo só olhando."},
            {K::Code,
             "espera: IN   R1             ; lê as chaves\n"
             "        BEQ  R1, R0, espera ; enquanto forem 0, lê de novo\n"
             "        OUT  R1             ; chegou um número: mostra"},
            {K::Heading, "Interrupções"},
            {K::Text,
             "Os processadores de verdade têm outro jeito: a interrupção. O dispositivo avisa a CPU com um sinal elétrico quando tem algo novo; a "
             "CPU para o que estava fazendo, guarda onde estava, atende o pedido e volta. Assim ela não precisa ficar olhando. Este computador não "
             "tem interrupções, e por isso usa a espera ocupada."},
            {K::Tip, "Espera ocupada: a CPU pergunta o tempo todo. Interrupção: o dispositivo avisa quando precisa."},
        },
        {"entrada-e-saida", "esperar-entrada"},
    });

    all.push_back(Topic{
        "sistema-operacional",
        7,
        "O sistema operacional",
        "Chamadas de sistema, tarefas e a troca entre elas",
        {
            {K::Heading, "O programa que cuida dos outros"},
            {K::Text,
             "Quando vários programas dividem o mesmo computador, alguém precisa organizar: quem usa a tela, quem lê o teclado, quem roda agora. "
             "Esse alguém é o sistema operacional, um programa que fica sempre na máquina e atende os outros. Windows, Linux, Android e macOS "
             "são sistemas operacionais."},
            {K::Heading, "Chamadas de sistema"},
            {K::Text,
             "Em vez de mexer direto na entrada e na saída, o programa pede ao sistema. Aqui, o pedido é assim: o número do serviço vai em R1, o "
             "dado em R2, e CALL sistema entra no sistema, que faz o serviço e volta com JR R7. Como só o sistema mexe nos dispositivos, ele pode "
             "conferir cada pedido e impedir que um programa atrapalhe o outro."},
            {K::Table, "R1|Serviço|R2\n1|mostrar na saída|o número a mostrar\n2|ler as chaves|volta com o número lido\n3|encerrar|não usa"},
            {K::Code,
             "        LI   R1, 1        ; serviço 1: mostrar\n"
             "        LI   R2, 42       ; o número\n"
             "        CALL sistema      ; o sistema mostra 42 e volta"},
            {K::Heading, "Várias tarefas numa CPU só"},
            {K::Text,
             "Uma CPU roda uma instrução por vez, mas pode alternar entre programas tão depressa que parece rodar todos juntos. Cada programa em "
             "andamento é uma tarefa. Para trocar de tarefa, o sistema guarda onde a atual parou e pula para onde a outra tinha parado. O que "
             "precisa ser guardado de uma tarefa é o seu contexto: o PC e os registradores que ela estava usando."},
            {K::Text,
             "Aqui as tarefas pedem a troca por conta própria, com uma chamada de sistema: é a multitarefa cooperativa. Nos sistemas de hoje, um "
             "relógio interrompe a tarefa de tempos em tempos e o sistema troca à força, mesmo que ela não peça: é a multitarefa preemptiva."},
            {K::Tip, "Trocar de tarefa é guardar o contexto da que sai e restaurar o da que entra. Assim, cada tarefa continua exatamente de onde "
                     "parou."},
        },
        {"chamadas-de-sistema", "varias-tarefas", "sistema-minimo"},
    });

    // Act 9: more parts.
    all.push_back(Topic{
        "paridade-mux-prioridade",
        8,
        "Paridade, multiplexador e prioridade",
        "Detectar erros, escolher um sinal e atender quem pede primeiro",
        {
            {K::Heading, "Paridade: um bit que vigia os outros"},
            {K::Text,
             "Quando dados viajam por um fio ou ficam guardados muito tempo, um bit pode virar. A paridade detecta isso: junto com os dados vai um "
             "bit a mais, que diz se a quantidade de 1 é ímpar. Na chegada, conta-se de novo; se não bater, houve erro. O circuito é uma fila de "
             "XOR, porque o XOR de vários bits dá 1 exatamente quando há uma quantidade ímpar de 1."},
            {K::Figure, "A paridade ímpar de 4 bits, feita com portas XOR em fila.", parityGates, 220},
            {K::Heading, "O multiplexador de dois"},
            {K::Text,
             "O MUX de duas entradas é o mais usado de todos: S = 0 deixa passar A, e S = 1 deixa passar B. Em expressão, Y = (A E NÃO S) OU "
             "(B E S). Os MUX do Raio-X, que escolhem entre registrador e número ou entre ULA e RAM, são feitos assim, um por bit."},
            {K::Table, "S|A saída Y\n0|A\n1|B"},
            {K::Heading, "O codificador de prioridade"},
            {K::Text,
             "Quando vários pedem ao mesmo tempo, quem é atendido? O codificador de prioridade olha várias entradas e devolve o número da mais "
             "importante que está ligada, além de um sinal V dizendo se alguma pediu. É assim que um processador escolhe qual interrupção "
             "atender primeiro."},
            {K::Figure, "O codificador de prioridade de 4 entradas feito de portas: a entrada ligada de número mais alto vence.", priorityGates, 260},
        },
        {"paridade", "multiplexador", "prioridade"},
    });

    all.push_back(Topic{
        "negativos-multiplicacao",
        8,
        "Negativos e multiplicação",
        "O complemento de dois na prática, e multiplicar somando",
        {
            {K::Heading, "Trocar o sinal"},
            {K::Text,
             "No complemento de dois, −N é NÃO N + 1: inverta todos os bits e some 1. O circuito são inversores na frente de um somador que "
             "recebe 1 no vai-um de entrada. Com 4 bits, 5 é 0101; invertendo, 1010; somando 1, 1011, que é −5."},
            {K::Figure, "O negador: inversores e um somador com 1 no vai-um de entrada.", negatorGates, 240},
            {K::Heading, "Multiplicar é somar deslocado"},
            {K::Text,
             "Multiplicar em binário é como no papel, só que mais fácil: cada algarismo do multiplicador é 0 ou 1. Para cada bit 1 de B, soma-se "
             "A deslocado até a casa daquele bit; para cada 0, nada. Deslocar uma casa para a esquerda é multiplicar por 2. Assim, 5 × 6 = "
             "5 × 2 + 5 × 4, porque os bits 1 e 2 de 6 (110) estão ligados."},
            {K::Table,
             "Bit de B (6 = 110)|Parcela\n"
             "bit 0 = 0|0\n"
             "bit 1 = 1|A deslocado 1 casa: 1010 (10)\n"
             "bit 2 = 1|A deslocado 2 casas: 10100 (20)\n"
             "soma|11110 (30)"},
            {K::Figure, "O multiplicador de 4 bits pronto. Na Galeria, a versão Portas e somadores mostra por dentro: portas E fazem as parcelas, e "
                         "somadores juntam tudo.",
             multiplierGates, 220},
            {K::Tip, "A CPU deste jogo não tem instrução de multiplicar: um programa multiplica com SHL, ADD e um laço, do mesmo jeito que o "
                     "circuito."},
        },
        {"negativo", "multiplicar"},
    });

    // Act 10: input and output.
    all.push_back(Topic{
        "display-7-segmentos",
        9,
        "O display de 7 segmentos",
        "Desenhar números com sete luzes",
        {
            {K::Heading, "Sete luzes formam um número"},
            {K::Text,
             "O display de 7 segmentos tem sete LEDs em forma de barra, chamados de a até g: a em cima, depois b, c, d e e, f dando a volta, e "
             "g no meio. Acendendo os certos, aparece qualquer algarismo de 0 a 9, e até as letras de A a F do hexadecimal. É o display de "
             "relógios, calculadoras e painéis."},
            {K::Table,
             "Número|Segmentos acesos\n"
             "0|a b c d e f\n1|b c\n2|a b d e g\n3|a b c d g\n4|b c f g\n5|a c d f g\n6|a c d e f g\n7|a b c\n8|todos\n9|a b c d f g"},
            {K::Heading, "Um decodificador para 7 segmentos"},
            {K::Text,
             "Cada segmento é uma função lógica do número de 4 bits: o segmento a, por exemplo, acende para 0, 2, 3, 5, 6, 7, 8 e 9. São sete "
             "tabelas-verdade, uma por segmento, e cada uma vira um circuito de portas. Juntas, formam o decodificador para 7 segmentos."},
            {K::Figure, "O display com o decodificador: o número da entrada aparece desenhado.", display7, 260},
        },
        {"sete-segmentos"},
    });

    all.push_back(Topic{
        "botoes-aleatorio",
        9,
        "Botões e números ao acaso",
        "Contar apertos e gerar números que parecem sorteados",
        {
            {K::Heading, "Um botão é um pulso"},
            {K::Text,
             "O botão vale 1 só enquanto está apertado. Ligado no clock de um contador, cada aperto vira uma borda de subida, e o contador avança "
             "uma vez por aperto. Botões mecânicos de verdade trepidam ao fechar e dão vários pulsos seguidos; os circuitos reais filtram isso, "
             "num processo chamado debounce."},
            {K::Figure, "O botão no clock de um contador: cada aperto soma 1.", buttonCounter, 240},
            {K::Heading, "Números que parecem ao acaso"},
            {K::Text,
             "Um circuito digital sempre faz a mesma coisa; não existe sorteio de verdade. Mas dá para gerar uma sequência que parece "
             "aleatória: o registrador de deslocamento com realimentação, o LFSR. A cada clock os bits andam uma casa, e o bit que entra é um XOR "
             "de alguns bits do próprio registrador. Com as posições certas, ele passa por quase todos os valores antes de repetir."},
            {K::Figure, "O gerador de números ao acaso: um registrador de deslocamento com XOR na entrada.", randomShift, 260},
            {K::Tip, "Pseudoaleatório: parece sorteado, mas é uma sequência fixa. Começando do mesmo valor, ela se repete igualzinha."},
        },
        {"contador-botao", "aleatorio"},
    });

    all.push_back(Topic{
        "teclado-terminal",
        9,
        "Teclado e terminal",
        "Letras como números, e uma tela que guarda texto",
        {
            {K::Heading, "Letras são números"},
            {K::Text,
             "Para um computador, uma letra é um número. A tabela ASCII, de 1963, dá um código de 7 bits a cada letra, algarismo e sinal: A é "
             "65, B é 66, a é 97, o algarismo 0 é 48 e o espaço é 32. Um texto é uma lista desses números."},
            {K::Table, "Caractere|Código|Binário\nA|65|1000001\nB|66|1000010\na|97|1100001\n0|48|0110000\nespaço|32|0100000"},
            {K::Heading, "O teclado"},
            {K::Text,
             "A peça teclado entrega duas coisas: o código da última tecla, em Código, e um sinal Tecla que vale 1 enquanto ela está apertada. "
             "Com a peça selecionada, as teclas de verdade do seu teclado chegam ao circuito."},
            {K::Heading, "O terminal"},
            {K::Text,
             "O terminal é uma tela de texto com memória própria: a cada pulso de clock com Write ligado, ele guarda o caractere da entrada e "
             "avança uma posição, como uma máquina de escrever. Ligando o código do teclado na entrada e o sinal Tecla na escrita, cada tecla "
             "apertada aparece na tela."},
            {K::Figure, "O teclado ligado ao terminal: o que se digita aparece na tela.", keyboardTerminal, 280},
            {K::Tip, "Teclado, código, memória e tela: é o caminho de cada letra que você digita num computador de verdade."},
        },
        {"teclado-terminal"},
    });

    return all;
}

}  // namespace

const std::vector<TheoryFigure (*)()>& backdropFigures() {
    static const std::vector<TheoryFigure (*)()> all{
        [] { return fromGallery("somador", 1, {"A", "Ce"}); },
        computerBoard,
        [] { return fromGallery("pisca", 0); },
        latch,
        gateSymbols,
        [] { return fromGallery("ula1", 1, {"A"}); },
        nandOfTransistors,
        counter,
        decoderGates,
    };
    return all;
}

const std::vector<Topic>& topics() {
    static const std::vector<Topic> all = makeTopics();
    return all;
}

int topicIndex(std::string_view id) {
    const std::vector<Topic>& all = topics();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (id == all[i].id) return static_cast<int>(i);
    }
    return -1;
}

int topicOfLesson(int lesson) {
    if (lesson < 0 || lesson >= static_cast<int>(lessons().size())) return -1;
    const std::string_view id = lessons()[static_cast<std::size_t>(lesson)].id;
    const std::vector<Topic>& all = topics();
    for (std::size_t t = 0; t < all.size(); ++t) {
        for (const char* exercise : all[t].exercises) {
            if (id == exercise) return static_cast<int>(t);
        }
    }
    return -1;
}

std::string readingId(const Topic& topic) { return std::string("leitura:") + topic.id; }

std::vector<int> actLessons(int act) {
    std::vector<int> found;
    for (std::size_t i = 0; i < lessons().size(); ++i) {
        if (lessons()[i].act == act) found.push_back(static_cast<int>(i));
    }
    return found;
}

int actDone(int act, std::span<const std::string> finished) {
    int done = 0;
    for (const int i : actLessons(act)) {
        const std::string_view id = lessons()[static_cast<std::size_t>(i)].id;
        done += std::find(finished.begin(), finished.end(), id) != finished.end() ? 1 : 0;
    }
    return done;
}

bool actOpen(int act, std::span<const std::string> finished) {
    if (act <= 0) return true;
    if (actDone(act, finished) > 0) return true;
    return actDone(act - 1, finished) == static_cast<int>(actLessons(act - 1).size());
}

}  // namespace bancada::editor
