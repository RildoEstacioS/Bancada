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

// --- Bits ---

// Two inputs and two lights: A on, its wire bright and its light lit; B off, dark.
TheoryFigure oneBitTwoWays() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId a = l.input("A", {0, 1});
    const PartId b = l.input("B", {0, 9});
    l.wire(a, 0u, l.output("S", {16, 1}), 0u);
    l.wire(b, 0u, l.output("T", {16, 9}), 0u);
    figure.closed.push_back(a);
    return figure;
}

// The inputs worth 1, 2, 4 and 8 into a joiner, and the number on a display: 4 and 1 on make 5.
TheoryFigure fourSwitchesFive() {
    TheoryFigure figure;
    Layout l(figure.board);
    const char* names[] = {"1", "2", "4", "8"};
    const PartId join = l.bundle(PartKind::Joiner, {16, 2}, 4, 4);
    for (unsigned i = 0; i < 4; ++i) {
        const PartId bit = l.input(names[i], {0, static_cast<int>(1 + 5 * i)});
        l.wire(bit, 0u, join, i);
        if (i == 0 || i == 2) figure.closed.push_back(bit);
    }
    l.wire(join, 4u, l.output("N", {28, l.pinY(join, 4) - 1}, 4), 0u);
    return figure;
}

// A number of four bits, 13, opened by a splitter into a light for each bit.
TheoryFigure busToLeds() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId number = l.input("N", {0, 7}, 4);
    const PartId split = l.bundle(PartKind::Splitter, {16, 3}, 4, 4);
    l.wire(number, 0u, split, 0u);
    const char* names[] = {"1", "2", "4", "8"};
    for (unsigned i = 0; i < 4; ++i) l.wire(split, 1 + i, l.output(names[i], {28, static_cast<int>(1 + 5 * i)}), 0u);
    figure.values.emplace_back(number, 13);
    return figure;
}

// Displays show their numbers in hexadecimal: 42 on eight bits is 2A, 11 on four bits is B.
TheoryFigure hexProbes() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId byte = l.input("A", {0, 1}, 8);
    const PartId nibble = l.input("B", {0, 9}, 4);
    l.wire(byte, 0u, l.output("A", {28, 1}, 8), 0u);
    l.wire(nibble, 0u, l.output("B", {28, 9}, 4), 0u);
    figure.values.emplace_back(byte, 42);
    figure.values.emplace_back(nibble, 11);
    return figure;
}
TheoryFigure nandOfTransistors() { return fromGallery("nand", 1, {"A"}); }
TheoryFigure andOfTransistors() { return fromGallery("e", 1, {"A"}); }
TheoryFigure orOfTransistors() { return fromGallery("ou", 1, {"A"}); }

// The CMOS inverter: a PMOS from 5V to the output, an NMOS from the output to ground, both gates on A.
TheoryFigure cmosInverter() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId a = l.input("A", {0, 6});
    const PartId p = l.part(PartKind::Pmos, {12, 0});
    const PartId n = l.part(PartKind::Nmos, {12, 10});
    const PartId out = l.output("S", {26, 8});
    const PartId ground = l.part(PartKind::Ground, {14, 17});
    l.wire(a, 0u, p, 0u);
    l.wire(a, 0u, n, 0u);
    l.wire(p, 2u, n, 1u);
    l.wire(n, 2u, ground, 0u);
    l.wire(p, 2u, out, 0u);
    const PartId battery = l.part(PartKind::Battery, {22, -8});
    l.wire(battery, 0u, p, 1u);
    const PartId otherGround = l.part(PartKind::Ground, {30, -6});
    l.wire(battery, 1u, otherGround, 0u);
    return figure;
}

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
// The Memory act's figures: their clocks are switches, to click. The latches and the flip-flop are made of gates.
TheoryFigure srLatchFigure() { return fromGallery("latch", 1); }
TheoryFigure dLatchFigure() { return fromGallery("latch-d", 1); }
TheoryFigure flipFlopFigure() { return fromGallery("flip-flop", 1); }

// A 4-bit register, with 5 waiting at its data input.
TheoryFigure registerFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId data = l.input("Dado", {0, 1}, 4);
    const PartId write = l.input("Grava", {0, 8});
    const PartId clock = l.input("Clk", {0, 14});
    const PartId keep = l.box("register", {16, 3}, 4);
    l.wire(data, 0u, keep, "D");
    l.wire(write, 0u, keep, "En");
    l.wire(clock, 0u, keep, "Clk");
    l.wire(keep, "Q", l.output("Q", {36, 3}, 4), 0u);
    figure.values.emplace_back(data, 5);
    return figure;
}

// A 4-bit counter: a register and an adder of 1, the register's output back into its input.
TheoryFigure counterFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId count = l.input("Conta", {0, 1});
    const PartId clock = l.input("Clk", {0, 9});
    const PartId keep = l.box("register", {32, 2}, 4);
    const PartId adder = l.box("adder_n", {14, 14}, 4);
    l.wire(keep, "Q", adder, "A");
    l.wire(adder, "S", keep, "D");
    l.wire(count, 0u, adder, "Ce");
    l.wire(count, 0u, keep, "En");
    l.wire(clock, 0u, keep, "Clk");
    l.wire(keep, "Q", l.output("N", {52, 2}, 4), 0u);
    figure.closed.push_back(count);
    return figure;
}

// A RAM of 8 words of 4 bits, with an address and a word ready to write.
TheoryFigure ramFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId address = l.input("A", {0, 1}, 3);
    const PartId data = l.input("D", {0, 8}, 4);
    const PartId write = l.input("We", {0, 15});
    const PartId clock = l.input("Clk", {0, 21});
    const PartId memory = l.box("ram", {18, 5}, 4, 3);
    l.wire(address, 0u, memory, "A");
    l.wire(data, 0u, memory, "D");
    l.wire(write, 0u, memory, "We");
    l.wire(clock, 0u, memory, "Clk");
    l.wire(memory, "Q", l.output("Q", {38, 6}, 4), 0u);
    figure.values.emplace_back(address, 2);
    figure.values.emplace_back(data, 9);
    return figure;
}

// A register file of 8 registers of 4 bits: two read, one written.
TheoryFigure registerFileFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId readA = l.input("Ra", {0, 0}, 3);
    const PartId readB = l.input("Rb", {0, 6}, 3);
    const PartId written = l.input("Rw", {0, 12}, 3);
    const PartId data = l.input("D", {0, 18}, 4);
    const PartId write = l.input("We", {0, 24});
    const PartId clock = l.input("Clk", {0, 30});
    const PartId file = l.box("regfile", {18, 8}, 4, 3);
    l.wire(readA, 0u, file, "Ra");
    l.wire(readB, 0u, file, "Rb");
    l.wire(written, 0u, file, "Rw");
    l.wire(data, 0u, file, "D");
    l.wire(write, 0u, file, "We");
    l.wire(clock, 0u, file, "Clk");
    l.wire(file, "A", l.output("A", {40, 8}, 4), 0u);
    l.wire(file, "B", l.output("B", {40, 18}, 4), 0u);
    figure.values.emplace_back(written, 3);
    figure.values.emplace_back(data, 6);
    return figure;
}
TheoryFigure counter() { return fromGallery("contador4", 1); }

TheoryFigure parityGates() { return fromGallery("paridade-impar", 1); }
TheoryFigure priorityGates() { return fromGallery("prioridade", 1); }
TheoryFigure negatorGates() { return fromGallery("negador", 1); }
TheoryFigure multiplierGates() { return fromGallery("multiplicador", 0); }
TheoryFigure display7() { return fromGallery("display7", 0); }
TheoryFigure buttonCounter() { return fromGallery("botao", 0); }
TheoryFigure randomShift() { return fromGallery("aleatorio", 1); }
TheoryFigure keyboardTerminal() { return fromGallery("teclado", 0); }

// Shifting one place is only wiring: each bit goes one place up, and a 0 comes in at the bottom.
TheoryFigure shiftWiringFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId number = l.input("A", {0, 3}, 4);
    const PartId split = l.bundle(PartKind::Splitter, {12, 1}, 4, 4);
    const PartId join = l.bundle(PartKind::Joiner, {30, 1}, 4, 4);
    const PartId zero = l.constant(0, {20, 12}, 1);
    l.wire(number, 0u, split, 0u);
    l.wire(zero, 0u, join, 0u);
    for (unsigned i = 0; i < 3; ++i) l.wire(split, 1 + i, join, i + 1);
    l.wire(join, 4u, l.output("Q", {42, 3}, 4), 0u);
    figure.values.emplace_back(number, 5);
    return figure;
}

// The library's shifter: N places at once, to the side Dir picks.
TheoryFigure shifterFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId number = l.input("A", {0, 1}, 8);
    const PartId places = l.input("N", {0, 8}, 3);
    const PartId side = l.input("Dir", {0, 15});
    const PartId shift = l.box("shifter", {20, 3}, 8, 3);
    l.wire(number, 0u, shift, "A");
    l.wire(places, 0u, shift, "N");
    l.wire(side, 0u, shift, "Dir");
    l.wire(shift, "Q", l.output("Q", {40, 3}, 8), 0u);
    figure.values.emplace_back(number, 5);
    figure.values.emplace_back(places, 1);
    return figure;
}

// The extender from 4 bits to 8, set to −3 with Sinal on.
TheoryFigure extenderFigure() {
    TheoryFigure figure;
    Layout l(figure.board);
    const PartId number = l.input("A", {0, 1}, 4);
    const PartId sign = l.input("Sinal", {0, 9});
    const PartId extend = l.box("extender", {16, 3}, 8, 4);
    l.wire(number, 0u, extend, "A");
    l.wire(sign, 0u, extend, "Sinal");
    l.wire(extend, "Q", l.output("Q", {36, 3}, 8), 0u);
    figure.values.emplace_back(number, 13);
    figure.closed.push_back(sign);
    return figure;
}

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
        "Corrente, circuito e LED",
        "O que anda no fio, por onde anda, e a primeira luz",
        {
            {K::Page, "O que é eletricidade"},
            {K::Text, "Todo material é feito de átomos, e os átomos têm elétrons. Num metal, como o cobre dos fios, parte desses "
             "elétrons está solta e pode andar. Quando eles andam todos para o mesmo lado, isso é uma corrente elétrica."},
            {K::Text, "Pense em água num cano. A tensão é a pressão que empurra a água; a corrente é quanto de água passa por "
             "segundo. Sem pressão a água fica parada, e sem tensão não há corrente."},
            {K::Table, "Grandeza|O que é|Unidade\nTensão|o empurrão que faz a corrente andar|volt (V)\nCorrente|quanto de carga passa "
             "por segundo|ampère (A); 1 mA é um milésimo\nResistência|o quanto uma peça freia a corrente|ohm (Ω)"},
            {K::Question, "Na comparação com a água num cano, a tensão é...\n*a pressão que empurra a água\nquanto de água passa por "
             "segundo\no tamanho do cano\n=A tensão empurra; a corrente é o que passa por causa desse empurrão.\n?Sem ela, a "
             "água fica parada."},
            {K::Page, "Um circuito é um caminho fechado"},
            {K::Text, "A corrente só anda se tiver por onde ir e por onde voltar. Ela sai da fonte de energia, passa pelas peças e "
             "volta para a mesma fonte. Esse caminho fechado é o circuito."},
            {K::Figure, "O primeiro circuito: a corrente sai do + da bateria, passa pelo resistor e pelo LED e volta ao -.", firstCircuit, 190},
            {K::Question, "Um fio se solta entre o LED e o - da bateria. Onde a corrente para?\nSó depois do fio solto\nSó antes do fio "
             "solto\n*No caminho todo: o LED apaga\n=Sem caminho de volta, a corrente não anda em lugar nenhum do circuito, "
             "nem antes do corte.\n?A corrente precisa de um caminho fechado inteiro para andar."},
            {K::Page, "A bateria e o LED"},
            {K::Text, "A bateria é a fonte de energia. Ela tem dois polos: o + e o -. Por dentro, uma reação química mantém uma "
             "diferença de tensão entre os dois. Por convenção, a corrente sai pelo + e volta pelo -. A bateria deste jogo "
             "dá 5 V, a tensão de muitos circuitos digitais."},
            {K::Text, "LED quer dizer diodo emissor de luz. Um diodo só deixa a corrente passar num sentido, como uma válvula de mão "
             "única. A perna do + (o anodo, a mais comprida num LED de verdade) fica do lado do + da bateria; a outra (o "
             "catodo), do lado do -."},
            {K::Figure, "O mesmo circuito duas vezes: à esquerda o LED do jeito certo, aceso; à direita invertido, apagado.", ledBothWays, 190},
            {K::Text, "Um LED aceso fica com cerca de 2 V entre as pernas e gosta de uma corrente entre 5 e 20 mA. Com corrente "
             "demais ele esquenta e queima. Por isso todo LED vai com um resistor, a peça que segura a corrente; como "
             "escolhê-lo é assunto de daqui a duas aulas. Por enquanto, use o de 330 Ω."},
            {K::Mistake, "Achar que o LED invertido queima\nAo contrário, o diodo simplesmente não conduz: o LED fica apagado e nada "
             "estraga. Vire-o e ele acende."},
            {K::Mistake, "Ligar o LED direto na bateria, sem resistor\nNada segura a corrente, que passa muito dos 20 mA: o LED queima. "
             "O resistor não é enfeite."},
            {K::Summary, "Corrente: elétrons andando num fio. Tensão: o empurrão. Resistência: o freio.\nA corrente só anda num caminho "
             "fechado; um corte em qualquer ponto a para no caminho todo.\nA bateria tem + e -: a corrente sai pelo + e "
             "volta pelo -.\nO LED só acende no sentido certo, e sempre com um resistor."},
        },
        {"acender-led"},
    });

    all.push_back(Topic{
        "esquemas",
        0,
        "Fonte e terra: o jeito dos esquemas",
        "O mesmo circuito, desenhado como nos livros",
        {
            {K::Page, "A fonte"},
            {K::Text, "Nos esquemas, os desenhos técnicos de circuitos, a bateria aparece como uma fonte: dois traços paralelos, o "
             "comprido é o + e o curto é o -. Ela continua sendo a mesma coisa: dois polos e uma tensão entre eles."},
            {K::Question, "No símbolo da fonte, qual traço é o +?\n*O comprido\nO curto\nTanto faz\n=O traço comprido é o +, o curto é o "
             "-.\n?Um deles é maior que o outro de propósito."},
            {K::Page, "O terra"},
            {K::Text, "O - da fonte costuma ir num terra, o símbolo de três linhas que vão diminuindo. O terra é o ponto de 0 V, a "
             "referência de onde se medem todas as tensões. Todos os terras de um esquema são o mesmo ponto, ligados entre "
             "si mesmo sem um fio desenhado."},
            {K::Text, "Assim, em vez de puxar um fio comprido de cada peça de volta até o - da fonte, basta ligar a peça num terra "
             "perto dela. O esquema fica mais limpo, e o circuito é o mesmo."},
            {K::Figure, "O mesmo circuito, desenhado com fonte e terra: a corrente volta ao - da fonte pelos terras.", sourceAndGround, 190},
            {K::Question, "Num esquema, o LED vai num terra em baixo à direita, e a fonte num terra em baixo à esquerda. A corrente "
             "consegue voltar à fonte?\n*Sim: todos os terras são o mesmo ponto\nNão: falta um fio entre os dois terras\nSó "
             "se os terras estiverem perto\n=Os terras estão todos ligados, mesmo sem fio desenhado. O caminho está "
             "fechado.\n?O que o esquema quer dizer quando desenha o mesmo símbolo de terra em dois lugares?"},
            {K::Mistake, "Achar que o terra é o chão, ou o fio de terra da tomada\nNo esquema, terra é só o ponto de 0 V do circuito, o "
             "- da fonte. Não precisa estar ligado ao chão de verdade."},
            {K::Summary, "A fonte é a bateria desenhada como nos esquemas: traço comprido +, curto -.\nO terra é o ponto de 0 V; todos "
             "os terras são o mesmo ponto.\nBateria ou fonte com terra: duas formas de desenhar o mesmo circuito."},
        },
        {"fonte-e-terra"},
    });

    all.push_back(Topic{
        "resistor-e-chave",
        0,
        "O resistor e a lei de Ohm",
        "Quanto de corrente passa, e como escolher o resistor",
        {
            {K::Page, "O resistor"},
            {K::Text, "O resistor é a peça que freia a corrente. Quanto maior a resistência, medida em ohms (Ω), menos corrente "
             "passa. Os resistores de verdade são pequenos cilindros com faixas coloridas; as cores dizem o valor. 1 kΩ (um "
             "quilo-ohm) são 1000 Ω."},
            {K::Question, "Trocando um resistor de 330 Ω por um de 1 kΩ, o que acontece com a corrente?\nAumenta\n*Diminui\nFica "
             "igual\n=1 kΩ = 1000 Ω, uma resistência maior: freia mais, e passa menos corrente.\n?Quanto maior a "
             "resistência..."},
            {K::Page, "A lei de Ohm"},
            {K::Text, "Tensão, corrente e resistência estão presas por uma conta simples, a lei de Ohm: a tensão sobre uma peça é a "
             "resistência vezes a corrente que passa nela. Escrita de outro jeito, a corrente é a tensão dividida pela "
             "resistência."},
            {K::Tip, "V = R × I, ou I = V ÷ R, ou R = V ÷ I. Com V em volts e R em ohms, I sai em ampères."},
            {K::Example, "A corrente no circuito do LED\nA bateria dá 5 V. O LED aceso fica com uns 2 V.\nOs 3 V que sobram ficam no "
             "resistor: 5 - 2 = 3 V.\nCom 330 Ω: I = 3 ÷ 330 ≈ 0,009 A, ou 9 mA.\nNum caminho único a corrente é a mesma em "
             "todas as peças: os 9 mA também passam no LED, bem na faixa boa de 5 a 20 mA."},
            {K::Question, "Com 3 V sobre um resistor de 150 Ω, quanto vale a corrente?\n2 mA\n*20 mA\n450 mA\n=I = 3 ÷ 150 = 0,02 A = 20 "
             "mA. Bem no limite do LED.\n?I = V ÷ R. Depois, ampères vezes 1000 dá miliampères."},
            {K::Page, "Escolher o resistor"},
            {K::Table, "Resistor|Corrente no LED|Resultado\n1 kΩ|3 V ÷ 1000 Ω = 3 mA|acende fraco\n330 Ω|3 V ÷ 330 Ω ≈ 9 mA|na "
             "medida\n100 Ω|3 V ÷ 100 Ω = 30 mA|corrente demais: queima\n10 Ω|3 V ÷ 10 Ω = 300 mA|queima na hora"},
            {K::Figure, "Três resistores lado a lado: 2,2 kΩ, 1 kΩ e 330 Ω. Menos ohms, mais corrente e mais brilho.", threeResistors, 190},
            {K::Mistake, "Fazer a conta com os 5 V inteiros\nO LED aceso fica com uns 2 V; só os 3 V que sobram ficam no resistor. Com 5 "
             "V na conta, a corrente sai maior do que a de verdade."},
            {K::Mistake, "Esquecer de converter kΩ e mA\nNa lei de Ohm, use ohms e ampères: 2,2 kΩ são 2200 Ω, e 15 mA são 0,015 A. Uma "
             "vírgula fora do lugar muda o resultado mil vezes."},
            {K::Summary, "O resistor freia a corrente: mais ohms, menos corrente.\nLei de Ohm: V = R × I, I = V ÷ R, R = V ÷ I, em "
             "volts, ohms e ampères.\nNo circuito do LED, o resistor fica com a tensão que sobra: 5 V - 2 V = 3 V.\n330 Ω dá "
             "uns 9 mA: o LED acende bem e não queima."},
        },
        {"lei-de-ohm", "resistor-certo"},
    });

    all.push_back(Topic{
        "chave-e-capacitor",
        0,
        "A chave e o capacitor",
        "Ligar, desligar e guardar carga",
        {
            {K::Page, "A chave"},
            {K::Text, "A chave abre ou fecha o caminho. Fechada, ela é como um pedaço de fio, e a corrente passa. Aberta, ela corta o "
             "circuito, e a corrente para no caminho todo. O interruptor de luz da sua casa é exatamente isso."},
            {K::Figure, "Duas chaves: abra e feche cada uma e veja só o circuito fechado acender.", switchOpenAndClosed, 200},
            {K::Text, "Nos circuitos digitais, uma chave fechada costuma valer 1, e uma aberta, 0. É o primeiro passo para os bits."},
            {K::Question, "Com a chave aberta, quanto de corrente passa pelo LED?\nUm pouco, por isso ele fica fraco\n*Nenhuma: o caminho "
             "está cortado\nA mesma de quando está fechada\n=Aberta, a chave corta o caminho: não passa corrente nenhuma, e "
             "o LED apaga.\n?Uma chave aberta é como um fio cortado."},
            {K::Page, "O capacitor"},
            {K::Text, "O capacitor é um pequeno reservatório de carga: duas placas de metal separadas por um isolante. Ligado a uma "
             "tensão, ele vai se enchendo de carga, e depois devolve essa carga quando o resto do circuito pede. Sua "
             "capacidade se mede em farads (F); os comuns têm microfarads (µF)."},
            {K::Text, "Encher um capacitor por um resistor leva tempo: quanto maiores o resistor e o capacitor, mais devagar. Por "
             "isso ele serve para marcar tempo. O pisca-pisca da Galeria usa dois deles para acender os LEDs um de cada vez, "
             "e a mesma ideia está por trás do clock, o relógio que dá o ritmo de um computador."},
            {K::Question, "Para o capacitor encher mais devagar, o que você faz com o resistor na frente dele?\n*Usa um de mais ohms\nUsa "
             "um de menos ohms\nTira o resistor\n=Mais ohms, menos corrente: a carga chega mais devagar, e o capacitor "
             "demora mais para encher.\n?O resistor freia a corrente que enche o capacitor."},
            {K::Mistake, "Achar que o capacitor é uma bateria\nEle guarda pouca carga e a devolve depressa; a bateria produz energia por "
             "uma reação química por muito tempo. O capacitor serve para segurar a tensão por um instante e para marcar "
             "tempo, não para alimentar o circuito."},
            {K::Summary, "Chave fechada: um fio, a corrente passa. Aberta: o caminho se corta.\nFechada costuma valer 1, aberta 0.\nO "
             "capacitor guarda carga e a devolve; encher por um resistor leva tempo.\nResistor e capacitor juntos marcam "
             "tempo: é a ideia por trás do clock."},
        },
        {"chave"},
    });

    all.push_back(Topic{
        "serie-e-paralelo",
        0,
        "Chaves em série e em paralelo",
        "Ligar duas chaves de dois jeitos: a primeira lógica",
        {
            {K::Page, "Em série: uma depois da outra"},
            {K::Text, "Duas peças estão em série quando a corrente atravessa uma e depois a outra, num caminho só. Com duas chaves em "
             "série, basta uma aberta para cortar o caminho: o LED só acende quando a primeira E a segunda estão fechadas."},
            {K::Question, "Duas chaves em série: a primeira fechada, a segunda aberta. O LED acende?\nSim, uma chave fechada basta\n*Não: "
             "a aberta corta o único caminho\nAcende fraco\n=Em série a corrente precisa passar pelas duas. A aberta corta o "
             "caminho, e o LED fica apagado.\n?Em série, a corrente tem um caminho só."},
            {K::Page, "Em paralelo: lado a lado"},
            {K::Text, "Duas peças estão em paralelo quando ligam os mesmos dois pontos, cada uma oferecendo um caminho próprio. Com "
             "duas chaves em paralelo, basta uma fechada para a corrente passar: o LED acende quando a primeira OU a segunda "
             "está fechada."},
            {K::Figure, "À esquerda, duas chaves em série; à direita, em paralelo. Clique nelas e compare os dois LEDs.", seriesAndParallel, 200},
            {K::Page, "A tabela-verdade"},
            {K::Text, "Para descrever um circuito assim, escreve-se uma tabela com todas as combinações das entradas e o que a saída "
             "faz em cada uma. Com 1 para chave fechada ou LED aceso, e 0 para chave aberta ou LED apagado:"},
            {K::Table, "A|B|Série: A E B|Paralelo: A OU B\n0|0|0|0\n0|1|0|1\n1|0|0|1\n1|1|1|1"},
            {K::Example, "Lendo a linha A = 0, B = 1\nEm série: a corrente teria que passar por A, que está aberta. O caminho se corta: "
             "0.\nEm paralelo: a corrente passa pelo caminho de B, que está fechada. O LED acende: 1.\nPor isso, na mesma "
             "linha, a coluna E dá 0 e a coluna OU dá 1."},
            {K::Question, "Em quantas linhas da tabela o circuito em paralelo acende?\n1\n2\n*3\n=Em todas menos a primeira: basta uma "
             "chave fechada. Só com as duas abertas ele apaga.\n?Conte os 1 da coluna do paralelo."},
            {K::Mistake, "Trocar série e paralelo\nEm série, todas precisam estar fechadas: E. Em paralelo, basta uma: OU. Para lembrar: "
             "em série a corrente tem um caminho só, que qualquer chave corta; em paralelo tem vários."},
            {K::Summary, "Em série: um caminho só, e todas as chaves precisam estar fechadas. É o E (AND).\nEm paralelo: vários "
             "caminhos, e basta uma fechada. É o OU (OR).\nA tabela-verdade lista cada combinação das entradas e a saída de "
             "cada uma.\nNo próximo ato, quem abre e fecha o caminho é um transistor."},
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
            {K::Page, "Uma chave sem as mãos"},
            {K::Text, "Nas chaves do Ato 1, era a sua mão que abria e fechava o caminho. Um computador precisa de milhões de chaves "
             "que se abram e fechem sozinhas, bilhões de vezes por segundo, comandadas pelos próprios sinais do circuito. "
             "Essa chave é o transistor."},
            {K::Text, "Ele é feito de silício, um semicondutor: um material que conduz ou deixa de conduzir conforme é preparado e "
             "conforme o que recebe. Inventado em 1947, o transistor substituiu as válvulas e os relés, e hoje um "
             "processador tem bilhões deles."},
            {K::Question, "O que faz do transistor uma peça tão importante para o computador?\nEle guarda energia como uma bateria\n*Ele "
             "é uma chave que um sinal elétrico abre e fecha, sem mãos\nEle acende como um LED\n=Um computador são milhões "
             "de chaves que se comandam umas às outras: cada uma delas é um transistor.\n?Pense nas chaves do Ato 1: quem as "
             "mexia?"},
            {K::Page, "As três pernas do NPN"},
            {K::Table, "Perna|O que faz\n@npn:B Base (B)|a entrada de comando: uma corrente pequena aqui liga o transistor\n@npn:C Coletor (C)|por "
             "onde entra a corrente grande, a da carga (o LED)\n@npn:E Emissor (E)|por onde as duas correntes saem, em direção ao "
             "terra"},
            {K::Text, "No transistor NPN, o tipo usado aqui, a seta do símbolo fica no emissor e aponta para fora. Quando a base "
             "recebe corrente, mesmo pouca, o caminho entre o coletor e o emissor se abre e a corrente da carga passa. Sem "
             "corrente na base, o caminho fica fechado."},
            {K::Tip, "Uma corrente pequena na base controla uma corrente grande do coletor para o emissor."},
            {K::Question, "Num transistor NPN ligando um LED, por qual perna a corrente do LED entra?\nPela base\n*Pelo coletor\nPelo "
             "emissor\n=A corrente da carga entra pelo coletor e sai pelo emissor; a base só comanda.\n?A base é o comando; "
             "a carga fica do outro lado."},
            {K::Page, "O resistor da base"},
            {K::Text, "A base se comporta como um diodo: a partir de uns 0,7 V ela conduz, e sem nada que limite a corrente ela se "
             "estraga. Por isso o sinal chega à base por um resistor, em geral de alguns quilo-ohms."},
            {K::Example, "A corrente na base com 10 kΩ\nO sinal vale 5 V e a base fica com uns 0,7 V.\nSobram 5 - 0,7 = 4,3 V no "
             "resistor da base.\nI = 4,3 ÷ 10000 ≈ 0,00043 A, ou 0,43 mA.\nÉ pouca corrente, mas basta para o transistor "
             "deixar passar os 9 mA do LED: mais de vinte vezes mais."},
            {K::Figure, "À esquerda e à direita, o mesmo circuito. Clique nas chaves: com a chave fechada, a base recebe corrente e o "
             "transistor acende o LED.", transistorOffAndOn, 240},
            {K::Text, "Repare que a chave agora só comanda a base: a corrente do LED nem passa por ela. Ela vem do 5V, atravessa o "
             "LED e o transistor e vai ao terra. O rótulo 5V funciona como o terra: todos os pontos com o mesmo nome estão "
             "ligados, sem precisar de fio."},
            {K::Mistake, "Ligar a base direto no 5 V, sem resistor\nA base conduz como um diodo, e sem resistor a corrente nela dispara: "
             "o transistor esquenta e queima. Sempre um resistor na base."},
            {K::Mistake, "Achar que a corrente do LED passa pela chave\nA chave só leva a corrente pequena da base. A corrente do LED "
             "vai do 5V ao terra pelo coletor e pelo emissor. É isso que deixa um sinal fraco comandar uma carga forte."},
            {K::Page, "Da válvula ao chip"},
            {K::Text, "Os primeiros computadores, nos anos 1940, usavam válvulas: tubos de vidro que esquentavam, gastavam muita "
             "energia e queimavam. O transistor fez a mesma coisa menor, mais frio e mais confiável. Depois veio o circuito "
             "integrado: muitos transistores e as ligações entre eles fabricados juntos, num pedaço só de silício, o chip."},
            {K::Table, "Época|Peça|Quantos por chip\nanos 1940|válvula|uma por vez\nanos 1950|transistor avulso|um por peça\nanos "
             "1960|circuito integrado pequeno|dezenas\nanos 1970|microprocessador|milhares\nhoje|processador|bilhões"},
            {K::Text, "Em 1965, Gordon Moore, que depois fundou a Intel, notou que o número de transistores num chip dobrava a cada "
             "ano ou dois. Essa lei de Moore valeu por décadas: transistores menores ficam mais perto, trocam mais rápido e "
             "gastam menos."},
            {K::Question, "Se um chip tem 1000 transistores e o número dobra a cada dois anos, quantos terá depois de seis anos?\n3000\n"
             "6000\n*8000\n=Seis anos são três dobras: 1000, 2000, 4000, 8000.\n?Quantas vezes dois anos cabem em seis?"},
            {K::Summary, "O transistor é uma chave comandada por um sinal elétrico.\nNPN: corrente na base liga o caminho do coletor "
             "para o emissor.\nA base sempre leva um resistor, de alguns kΩ.\nUma corrente pequena comanda uma grande: o "
             "sinal não carrega a carga."},
        },
        {"transistor-chave"},
    });

    all.push_back(Topic{
        "mosfet-e-cmos",
        1,
        "MOSFET e CMOS: o transistor dos chips",
        "Comandado por tensão: NMOS, PMOS e o par que forma o inversor CMOS",
        {
            {K::Page, "Outro tipo de transistor"},
            {K::Text, "O NPN da aula anterior é um transistor bipolar: quem o liga é uma corrente na base. Os chips de hoje usam "
             "outro tipo, o MOSFET. Ele também tem três pernas, a porta (G, de gate), o dreno (D) e a fonte (S, de source), "
             "mas quem o liga é uma tensão na porta, não uma corrente."},
            {K::Text, "A porta do MOSFET é separada do resto por uma camada finíssima de óxido, um isolante. Por isso quase nenhuma "
             "corrente entra por ela: basta pôr a tensão, e o transistor obedece. É como um interruptor que se aperta sem "
             "gastar força para mantê-lo apertado."},
            {K::Table, "Tipo|Conduz quando...|Comandado por\n@npn NPN|entra corrente na base|corrente\n@pnp PNP|sai corrente da "
             "base|corrente\n@nmos NMOS|a porta está em 1, perto de 5 V|tensão\n@pmos PMOS|a porta está em 0, perto de 0 "
             "V|tensão"},
            {K::Question, "O que liga um MOSFET?\nUma corrente na porta\n*Uma tensão na porta\nUma corrente no dreno\n=A porta é "
             "isolada: quase não entra corrente. Quem manda é a tensão nela.\n?A porta é separada do resto por um isolante."},
            {K::Page, "NMOS e PMOS: um o contrário do outro"},
            {K::Text, "A NMOS conduz quando a porta está em 1 e corta com a porta em 0, parecida com o NPN. A PMOS faz o contrário: "
             "conduz com a porta em 0 e corta com a porta em 1. No símbolo, a PMOS tem uma bolinha na porta, a mesma "
             "bolinha de invertido das portas lógicas."},
            {K::Mistake, "Deixar a porta solta\nUma porta que não está ligada a nada não está em 0: fica com uma tensão qualquer, e o "
             "transistor pode ligar sozinho. Quando a chave está aberta, um resistor da porta até o terra, o resistor de "
             "pull-down, garante o 0."},
            {K::Page, "CMOS: os dois juntos"},
            {K::Text, "Ponha uma PMOS entre o 5 V e a saída, e uma NMOS entre a saída e o terra, com as duas portas no mesmo sinal "
             "A. Com A em 0, a PMOS conduz e a NMOS corta: a saída vai a 5 V. Com A em 1 é o contrário, e a saída vai ao "
             "terra. É um inversor, e sem resistor de pull-up."},
            {K::Figure, "O inversor CMOS: a PMOS em cima, a NMOS embaixo, as duas portas em A. Clique em A e veja S fazer o contrário.", cmosInverter, 220},
            {K::Example, "Seguindo o inversor CMOS\nA = 0: a porta da PMOS está em 0, e ela conduz. A porta da NMOS está em 0, e ela "
             "corta.\nA saída fica ligada ao 5 V pela PMOS: S = 1.\nA = 1: agora a NMOS conduz e a PMOS corta, e a saída "
             "fica ligada ao terra: S = 0.\nNos dois casos, um dos transistores está cortado: não há caminho direto do 5 V "
             "ao terra."},
            {K::Text, "Isso faz toda a diferença. O inversor de NPN com pull-up gasta energia o tempo todo em que a saída está em 0, "
             "porque a corrente atravessa o resistor até o terra. O CMOS quase só gasta na hora de trocar de estado. É por "
             "isso que todo processador moderno é feito em CMOS, com bilhões de pares assim, e não esquenta como "
             "esquentaria com resistores."},
            {K::Question, "No inversor CMOS com A em 1, qual transistor conduz?\nA PMOS\n*A NMOS\nOs dois\n=A NMOS conduz com a porta em "
             "1 e puxa a saída para o terra; a PMOS corta.\n?A PMOS é a que tem a bolinha: ela conduz com 0."},
            {K::Summary, "O MOSFET é comandado por tensão: a porta é isolada e quase não puxa corrente.\nNMOS conduz com a porta em 1; "
             "PMOS, com a porta em 0.\nUma porta solta não vale 0: use um resistor de pull-down.\nCMOS: uma PMOS em cima e "
             "uma NMOS embaixo fazem um inversor que quase não gasta energia parado.\nTodo processador de hoje é CMOS."},
        },
        {"nmos-chave", "inversor-cmos"},
    });
    // Act 3: bits.
    all.push_back(Topic{
        "o-bit",
        2,
        "O bit: zero ou um",
        "Por que o computador usa só dois valores, e como o simulador mostra cada um",
        {
            {K::Page, "Um fio, dois valores"},
            {K::Text, "No Ato 2, a saída do transistor ficava perto de 5 V, com o LED aceso, ou perto de 0 V, com o LED apagado. "
             "Ninguém precisa saber a tensão exata: basta saber se ela está alta ou baixa. Alta vale 1, baixa vale 0. Esse "
             "valor, que só pode ser 0 ou 1, é o bit (de binary digit, dígito binário): a menor informação que existe, sim "
             "ou não."},
            {K::Figure, "O transistor do Ato 2, desligado à esquerda e ligado à direita. Clique nas chaves: cada ponto do circuito "
             "fica perto de 5 V ou perto de 0 V, e só isso importa.", transistorOffAndOn, 240},
            {K::Text, "Por que só dois valores? Porque alto e baixo são fáceis de distinguir mesmo com ruído, fios compridos e peças "
             "imperfeitas: 4,6 V e 5,2 V são os dois 1, e 0,1 V e 0,4 V são os dois 0. Se o fio pudesse ter dez níveis, um "
             "desvio pequeno já trocaria um número por outro."},
            {K::Table, "Tensão|Bit|Também chamado\nperto de 5 V|1|alto, ligado, verdadeiro\nperto de 0 V|0|baixo, desligado, falso"},
            {K::Question, "Por que o computador usa só dois níveis de tensão, e não dez?\nPorque é mais barato fazer fios para dois "
             "níveis\n*Porque dois níveis bem separados aguentam ruído sem trocar o valor\nPorque um transistor só aguenta 5 "
             "V\n=Com só alto e baixo, um desvio de alguns décimos de volt não muda o bit. Com dez níveis, mudaria.\n?Pense "
             "no que acontece quando a tensão desvia um pouco."},
            {K::Page, "Como o simulador mostra um bit"},
            {K::Text, "Daqui em diante os circuitos usam peças lógicas, que já trabalham com bits em vez de tensões. A Entrada é uma "
             "chave que vale 0 ou 1: um clique nela troca o valor. A Luz acende com 1 e apaga com 0. E os fios mostram o bit "
             "que levam: um fio com 1 fica âmbar claro, um fio com 0 fica âmbar escuro."},
            {K::Figure, "Duas Entradas e duas Luzes. Clique em A e em B e veja a cor dos fios mudar com o bit.", oneBitTwoWays, 140},
            {K::Question, "Um fio âmbar escuro está levando qual bit?\n*0\n1\nNenhum: está desligado da placa\n=Âmbar escuro é 0, âmbar "
             "claro é 1.\n?Claro é ligado."},
            {K::Page, "Dois bits, quatro combinações"},
            {K::Text, "Um bit sozinho só diz sim ou não. Com dois bits, cada um pode ser 0 ou 1, e isso dá quatro combinações: 00, "
             "01, 10 e 11. Com três bits são oito, e cada bit a mais dobra a conta."},
            {K::Table, "B1|B0|Combinação\n0|0|00\n0|1|01\n1|0|10\n1|1|11"},
            {K::Example, "Contando as combinações de três bits\nCom o primeiro bit há 2 jeitos: 0 ou 1.\nPara cada um deles, o segundo "
             "bit dá mais 2: 2 × 2 = 4.\nPara cada uma dessas 4, o terceiro dá mais 2: 4 × 2 = 8.\nSão elas: 000, 001, 010, "
             "011, 100, 101, 110 e 111."},
            {K::Question, "Quantas combinações têm quatro bits?\n8\n*16\n32\n=Cada bit dobra: 2, 4, 8, 16.\n?Três bits dão 8. Um bit a "
             "mais..."},
            {K::Mistake, "Achar que n bits dão n combinações, ou 2 vezes n\nCada bit a mais dobra, não soma: 3 bits dão 8, não 3 nem 6. "
             "É uma multiplicação: 2 × 2 × 2."},
            {K::Page, "Bits, bytes e palavras"},
            {K::Text, "Os bits andam em grupos. Oito bits formam um byte, a unidade com que se medem memórias e arquivos. Quatro "
             "bits, meio byte, às vezes se chamam nibble. E a palavra é quantos bits o computador trata de uma vez: 16 no "
             "computador de 16 bits deste jogo, 32 no MIPS."},
            {K::Table, "Nome|Bits|Combinações\nbit|1|2\nnibble|4|16\nbyte|8|256\npalavra de 16 bits|16|65 536\npalavra de 32 "
             "bits|32|mais de 4 bilhões"},
            {K::Text, "Nas memórias, os prefixos crescem em potências de 2: um quilobyte de memória são 1024 bytes, 2 elevado a 10, "
             "e um megabyte são 1024 quilobytes."},
            {K::Question, "Quantos bits há em 4 bytes?\n4\n16\n*32\n=Cada byte tem 8 bits: 4 × 8 = 32, uma palavra do MIPS.\n?Um byte "
             "são 8 bits."},
            {K::Summary, "Bit: o valor de um fio, 0 (baixo) ou 1 (alto).\nDois níveis bem separados aguentam ruído: é por isso que o "
             "computador é binário.\nNo simulador: fio âmbar claro é 1, escuro é 0; a Entrada troca com um clique; a Luz "
             "acende com 1.\nn bits dão 2 elevado a n combinações."},
        },
        {"dois-bits"},
    });

    all.push_back(Topic{
        "bits-viram-numeros",
        2,
        "Bits viram números",
        "Os pesos 1, 2, 4 e 8, o barramento, o Juntador e o Separador",
        {
            {K::Page, "Cada casa tem um peso"},
            {K::Text, "No número 352, o 3 vale 300 porque está na casa das centenas: cada casa vale dez vezes a da direita. Em "
             "binário é igual, só que cada casa vale duas vezes a da direita: 1, 2, 4, 8, 16, 32, 64, 128. Para saber quanto "
             "um número binário vale, some os pesos das casas que têm 1."},
            {K::Table, "Binário|8|4|2|1|Valor\n0101|0|1|0|1|4 + 1 = 5\n1001|1|0|0|1|8 + 1 = 9\n1100|1|1|0|0|8 + 4 = 12\n1111|1|1|1|1|8 "
             "+ 4 + 2 + 1 = 15"},
            {K::Text, "O bit da direita, que vale 1, é o bit 0, o menos significativo. O da esquerda é o mais significativo: num "
             "número de 4 bits é o bit 3, que vale 8. No papel, o número começa sempre pelo mais significativo, como no "
             "decimal."},
            {K::Question, "Quanto vale 0110?\n3\n*6\n110\n=Os 1 estão nas casas do 4 e do 2: 4 + 2 = 6.\n?As casas valem 8, 4, 2 e 1, da "
             "esquerda para a direita."},
            {K::Page, "Do decimal para o binário"},
            {K::Text, "Para escrever um número em binário, tire dele o maior peso que cabe, depois o maior que cabe no que sobrou, "
             "até sobrar zero. As casas usadas ficam com 1, as outras com 0."},
            {K::Example, "Escrevendo 13 em binário\nCabe o 8: 13 - 8 = 5. A casa do 8 fica com 1.\nCabe o 4: 5 - 4 = 1. A casa do 4 fica "
             "com 1.\nO 2 não cabe em 1: a casa do 2 fica com 0.\nCabe o 1: 1 - 1 = 0. A casa do 1 fica com 1.\n13 = 8 + 4 + "
             "1 = 1101."},
            {K::Question, "Como se escreve 10 em binário, com 4 bits?\n0110\n*1010\n1100\n=10 = 8 + 2: acendem as casas do 8 e do "
             "2.\n?Comece tirando o maior peso que cabe em 10."},
            {K::Mistake, "Ler o binário da direita para a esquerda\nO número se escreve como no decimal: o mais significativo à "
             "esquerda. 0001 é 1, não 8."},
            {K::Page, "O barramento: vários bits num fio só"},
            {K::Text, "Um número de 4 bits precisa de 4 fios. Para não desenhar quatro fios lado a lado pela placa toda, o simulador "
             "junta os bits num barramento: um fio mais grosso, azul, que leva todos de uma vez. Ele fica azul-claro quando "
             "algum bit é 1, e azul-escuro quando todos são 0. A Saída, ligada a um barramento, mostra o número que ele "
             "leva."},
            {K::Text, "O Juntador junta fios de um bit num barramento, e o Separador faz o contrário. Nos dois, a ponta de cima é o "
             "bit 0, o que vale 1, e a de baixo é o mais significativo."},
            {K::Figure, "Clique nas Entradas que valem 1, 2, 4 e 8: o Juntador forma o barramento e a Saída N mostra o número.", fourSwitchesFive, 190},
            {K::Figure, "O caminho de volta: clique nos quadradinhos da Entrada N; o Separador abre o barramento, e cada Luz mostra um "
             "bit.", busToLeds, 190},
            {K::Question, "No Juntador, qual ponta recebe o bit que vale 1?\n*A de cima\nA de baixo\nA do meio\n=A ponta de cima é o bit "
             "0, o que vale 1; a de baixo é o mais significativo.\n?Bit 0 é o primeiro."},
            {K::Table, "No simulador|O que mostra\n@wire1 Fio âmbar claro|um bit 1\n@wire0 Fio âmbar escuro|um bit 0\n@bus Fio grosso azul|um barramento, "
             "com vários bits\n@bus1 Azul-claro|algum bit do barramento é 1\n@bus0 Azul-escuro|todos os bits são 0\n@input Entrada de vários "
             "bits|um quadradinho por bit\n@probe Saída|o número que chega pelo barramento\n@led Luz|1 acesa, 0 apagada"},
            {K::Page, "Depois da vírgula"},
            {K::Text, "Os pesos continuam para a direita da vírgula, sempre pela metade: depois do 1 vêm 1/2, 1/4, 1/8. Assim 10,1 "
             "em binário é 2 + 1/2 = 2,5. É o mesmo sistema posicional do decimal, onde depois da vírgula vêm décimos e "
             "centésimos."},
            {K::Text, "Nem toda fração cabe certinha: 0,1 em decimal vira em binário uma dízima que nunca acaba, 0,000110011... Por "
             "isso um computador às vezes diz que 0,1 + 0,2 não dá exatamente 0,3."},
            {K::Question, "Quanto vale 1,11 em binário?\n1,11\n1,5\n*1,75\n=1 + 1/2 + 1/4 = 1,75.\n?Depois da vírgula, os pesos são 1/2 "
             "e 1/4."},
            {K::Summary, "Em binário cada casa vale o dobro da da direita: 1, 2, 4, 8...\nPara ler, some os pesos das casas com 1. Para "
             "escrever, tire o maior peso que cabe, até sobrar zero.\nO barramento leva vários bits num fio azul; a Saída "
             "mostra o número.\nJuntador e Separador: a ponta de cima é o bit 0."},
        },
        {"quatro-bits-numero", "numero-em-bits", "quanto-vale"},
    });

    all.push_back(Topic{
        "contar-e-hexadecimal",
        2,
        "Contar, tamanho e hexadecimal",
        "O vai-um, quantos números cabem em n bits, e os dígitos de 0 a F",
        {
            {K::Page, "Contar em binário"},
            {K::Text, "Contar é somar 1, de novo e de novo. No decimal, 9 + 1 não cabe numa casa: dá 0 e vai 1, e o número fica 10. "
             "No binário isso acontece já no 1: 1 + 1 = 10. Para somar 1, troque os 1 da direita por 0 até encontrar um 0, e "
             "troque esse 0 por 1."},
            {K::Example, "Somando 1 a 0111\nO último bit é 1: vira 0 e vai um.\nO próximo também é 1: vira 0 e vai um.\nO próximo "
             "também: vira 0 e vai um.\nO primeiro é 0: vira 1, e o vai-um acaba. 0111 + 1 = 1000, ou seja, 7 + 1 = 8."},
            {K::Figure, "Um contador de 4 bits. Ligue Conta e clique no Clk: a cada subida ele soma 1, e os bits mudam como na tabela.", counterFigure, 230},
            {K::Table, "Decimal|Binário|Hexadecimal\n0|0000|0\n1|0001|1\n2|0010|2\n3|0011|3\n4|0100|4\n5|0101|5\n6|0110|6\n7|0111|7\n8|1000|8\n9|1001|9\n10|1010|A\n11|1011|B\n12|1100|C\n13|1101|D\n14|1110|E\n15|1111|F"},
            {K::Question, "Quanto dá 1011 + 1?\n1012\n*1100\n1111\n=Os dois 1 da direita viram 0, e o 0 seguinte vira 1: 1100. Em "
             "decimal, 11 + 1 = 12.\n?Troque os 1 da direita por 0 até achar um 0."},
            {K::Page, "Quantos números cabem"},
            {K::Text, "Com n bits há 2 elevado a n combinações. O menor número é 0 e o maior é 2 elevado a n, menos 1, com todos os "
             "bits em 1. Quando a conta passa do maior, o vai-um sai pela esquerda, não tem onde ficar, e o número volta a "
             "0: é o estouro."},
            {K::Table, "Bits|Combinações|Maior número|Nome\n1|2|1|bit\n4|16|15|um dígito "
             "hexadecimal\n8|256|255|byte\n16|65.536|65.535|a palavra da CPU de 16 bits\n32|4.294.967.296|4.294.967.295|a "
             "palavra da CPU MIPS"},
            {K::Question, "Num número de 8 bits valendo 255, você soma 1. O que fica?\n256\n*0\n255\n=255 é 11111111. Somando 1, todos "
             "viram 0 e o vai-um sai pela esquerda: estouro, e o número volta a 0.\n?256 cabe em 8 bits?"},
            {K::Mistake, "Achar que o maior número de 8 bits é 256\nSão 256 combinações, mas uma delas é o 0: os números vão de 0 a 255. "
             "O maior é sempre 2 elevado a n, menos 1."},
            {K::Page, "Hexadecimal: 4 bits por dígito"},
            {K::Text, "Números binários ficam compridos: 42 é 00101010. Por isso eles costumam ser escritos em hexadecimal, a base "
             "16, com os dígitos 0 a 9 e depois A (10), B (11), C (12), D (13), E (14) e F (15). Cada grupo de 4 bits vira "
             "exatamente um dígito: 0010 1010 é 2 e A, ou 0x2A. O 0x na frente avisa que o número está em hexadecimal."},
            {K::Text, "A Saída do simulador mostra os números assim, em hexadecimal, com um dígito para cada 4 bits do barramento."},
            {K::Figure, "Uma Entrada de 8 bits e outra de 4. Clique nos bits e veja as Saídas mostrarem os números em hexadecimal.", hexProbes, 150},
            {K::Example, "Do hexadecimal para o decimal: 0x2A\nCada dígito vale 16 vezes o da direita: o 2 está na casa do 16, o A na "
             "casa do 1.\n2 × 16 = 32.\nA vale 10: 10 × 1 = 10.\n32 + 10 = 42."},
            {K::Question, "Como se escreve 1111 0011 em hexadecimal?\n*F3\n153\n3F\n=1111 é F, e 0011 é 3: F3, na mesma ordem dos "
             "bits.\n?Troque cada grupo de 4 bits pelo seu dígito, da esquerda para a direita."},
            {K::Mistake, "Agrupar os bits a partir da esquerda num número que não tem múltiplo de 4 bits\nOs grupos de 4 se contam a "
             "partir da direita, do bit 0: 110101 é 11 0101, ou 0x35, e não 1101 01."},
            {K::Page, "Do decimal para o binário, por divisões"},
            {K::Text, "Tirar o maior peso que cabe funciona bem com números pequenos. Para números grandes há um jeito mecânico: "
             "divida o número por 2, anote o resto, e repita com o quociente até ele chegar a 0. Os restos, lidos de baixo "
             "para cima, são os bits, do mais significativo para o menos."},
            {K::Example, "13 em binário, por divisões\n13 ÷ 2 = 6, resto 1.\n6 ÷ 2 = 3, resto 0.\n3 ÷ 2 = 1, resto 1.\n1 ÷ 2 = 0, resto "
             "1.\nLendo os restos de baixo para cima: 1101. Confira: 8 + 4 + 1 = 13."},
            {K::Question, "Por divisões, qual é o primeiro resto de 6, o do bit que vale 1?\n*0\n1\n3\n=6 ÷ 2 = 3, resto 0: o bit que "
             "vale 1 é 0. 6 é 110.\n?Um número par dividido por 2 deixa que resto?"},
            {K::Summary, "Contar é somar 1: os 1 da direita viram 0 e o primeiro 0 vira 1.\nn bits: 2 elevado a n combinações, de 0 a 2 "
             "elevado a n menos 1. Passou disso, estouro.\nHexadecimal: base 16, dígitos 0 a F, um dígito para cada 4 "
             "bits.\nA Saída do simulador mostra os números em hexadecimal."},
        },
        {"contar-binario", "hexadecimal-perguntas"},
    });

    // Act 4: logic gates.
    all.push_back(Topic{
        "portas-logicas",
        3,
        "As portas lógicas",
        "Símbolos, tabelas-verdade, expressões e as seis portas básicas",
        {
            {K::Page, "O que é uma porta lógica"},
            {K::Text, "Uma porta lógica é um circuito que recebe um ou mais bits e entrega um bit, seguindo uma regra simples. É o "
             "bloco de que tudo no computador é feito: somadores, memórias e a CPU inteira são portas ligadas umas nas "
             "outras."},
            {K::Text, "Cada porta pode ser descrita de três jeitos, e vale saber passar de um para o outro: pelo símbolo, que é como "
             "ela aparece no desenho do circuito; pela expressão, que é como se escreve a sua conta; e pela tabela-verdade, "
             "que lista a saída para cada combinação das entradas."},
            {K::Table, "Porta|A saída é 1 quando...|Expressão\n@and E (AND)|todas as entradas são 1|S = A · B\n@or OU (OR)|pelo menos "
             "uma entrada é 1|S = A + B\n@not NÃO (NOT)|a entrada é 0|S = NÃO A\n@nand NAND|nem todas as entradas são 1|S = "
             "NÃO (A · B)\n@nor NOR|nenhuma entrada é 1|S = NÃO (A + B)\n@xor OU exclusivo (XOR)|as entradas são "
             "diferentes|S = A XOR B"},
            {K::Figure, "As seis portas, com as mesmas entradas A e B. Clique em A e em B e veja quais acendem. A bolinha na saída "
             "quer dizer: invertido.", gateSymbols, 270},
            {K::Question, "Com A = 1 e B = 1, qual destas portas dá 0?\nE (AND)\nOU (OR)\n*XOR\n=O XOR só dá 1 quando as entradas são "
             "diferentes. Iguais, dá 0.\n?Qual delas quer as entradas diferentes?"},
            {K::Page, "Uma porta de cada vez"},
            {K::Text, "E (AND): a saída só é 1 quando todas as entradas são 1, como as chaves em série do Ato 1. OU (OR): basta uma "
             "entrada em 1, como as chaves em paralelo. NÃO (NOT): tem uma entrada só e entrega o contrário dela; também se "
             "chama inversor."},
            {K::Text, "As outras três vêm dessas. A NAND é um E com a saída invertida (NÃO-E), a NOR é um OU invertido (NÃO-OU), e o "
             "XOR, o OU exclusivo, dá 1 só quando as entradas são diferentes. No desenho, a bolinha na ponta da saída "
             "sempre quer dizer invertido."},
            {K::Example, "Uma linha da tabela, seis portas\nCom A = 1 e B = 0.\nE: nem todas as entradas são 1, então dá 0. A NAND "
             "inverte: 1.\nOU: pelo menos uma é 1, então dá 1. A NOR inverte: 0.\nXOR: as entradas são diferentes, então dá "
             "1. E o NÃO de A dá 0."},
            {K::Mistake, "Esquecer a bolinha\nA bolinha na saída muda tudo: sem ela a porta é um E, com ela é uma NAND, e as duas "
             "tabelas são opostas linha a linha. Antes de ler um circuito, olhe a ponta de cada porta."},
            {K::Tip, "Na bancada, um clique duplo numa porta mostra o que há dentro dela. Mais adiante, neste ato, você mesmo monta "
             "as portas com transistores."},
            {K::Summary, "Uma porta recebe bits e entrega um bit, seguindo uma regra.\nCada porta tem símbolo, expressão e "
             "tabela-verdade.\nE: todas em 1. OU: pelo menos uma em 1. NÃO: o contrário.\nNAND e NOR são o E e o OU "
             "invertidos; o XOR quer as entradas diferentes.\nA bolinha na saída quer dizer invertido."},
        },
        {"portas-perguntas", "porta-e", "porta-ou", "porta-nao", "porta-nand", "porta-nor"},
    });

    all.push_back(Topic{
        "algebra-booleana",
        3,
        "A álgebra das portas",
        "Boole e Shannon, as regras das contas com 0 e 1, e portas de várias entradas",
        {
            {K::Page, "Contas com verdadeiro e falso"},
            {K::Text, "Em 1854, George Boole mostrou que dá para fazer contas com verdadeiro e falso. Nessa álgebra booleana, o E se "
             "escreve como uma multiplicação (A · B), porque só dá 1 quando os dois são 1; o OU, como uma soma (A + B); e o "
             "NÃO, como NÃO A. Toda tabela-verdade pode ser escrita como uma expressão assim, e toda expressão vira um "
             "circuito de portas."},
            {K::Text, "Em 1938, Claude Shannon, um estudante do MIT, mostrou que essa álgebra descreve circuitos de chaves. Desde "
             "então ela serve para duas coisas: analisar um circuito, dizendo numa expressão curta o que ele faz, e "
             "projetar um, partindo da função desejada até um circuito simples que a faça."},
            {K::Text, "Como na aritmética, há uma ordem: sem parênteses, o E vem antes do OU, assim como a multiplicação vem antes "
             "da soma. A + B · C quer dizer A + (B · C). E quando não há confusão, o ponto some: AB é o mesmo que A · B."},
            {K::Example, "Lendo um circuito: (A E B) OU C\nSiga da entrada para a saída. A porta E recebe A e B: o fio que sai dela é A "
             "· B.\nA porta OU recebe esse fio e C: a saída é A · B + C.\nCom A = 1, B = 0 e C = 1: A · B = 0, e 0 + 1 = 1. "
             "A saída acende.\nCom A = 1, B = 0 e C = 0: 0 + 0 = 0. Apaga."},
            {K::Question, "Em S = A · B + C, com A = 1, B = 1 e C = 0, quanto vale S?\n0\n*1\n2\n=A · B = 1, e 1 + 0 = 1. Na álgebra "
             "booleana não existe 2: tudo é 0 ou 1.\n?Faça primeiro o E, depois o OU."},
            {K::Mistake, "Fazer o + do OU como uma soma de números\nNa álgebra booleana, 1 + 1 = 1: o OU só diz se pelo menos um é 1. O "
             "+ é só a forma de escrever; não há vai-um."},
            {K::Tip, "Para ler um circuito de portas, siga da entrada para a saída e escreva a expressão de cada fio. Para montar "
             "um, faça o contrário: comece pela tabela-verdade."},
            {K::Page, "As regras da álgebra"},
            {K::Text, "As regras da álgebra booleana vêm em pares: troque cada · por +, cada 0 por 1, e a regra continua valendo. "
             "Algumas são iguais às da aritmética; outras, não. Para conferir qualquer uma, basta trocar as letras por 0 e "
             "1 e testar todas as combinações."},
            {K::Table, "Regra|Com E|Com OU\nComutativa|A · B = B · A|A + B = B + A\nAssociativa|A · (B · C) = (A · B) · C|A + (B + C) "
             "= (A + B) + C\nDistributiva|A · (B + C) = AB + AC|A + BC = (A + B) · (A + C)\nElemento neutro|1 · A = A|0 + A "
             "= A\nAnulação|0 · A = 0|1 + A = 1\nIdempotência|A · A = A|A + A = A\nComplemento|A · NÃO A = 0|A + NÃO A = 1\n"
             "De Morgan|NÃO (A · B) = NÃO A + NÃO B|NÃO (A + B) = NÃO A · NÃO B"},
            {K::Example, "Conferindo De Morgan com A = 1 e B = 0\nDo lado esquerdo: A · B = 1 · 0 = 0, e NÃO 0 = 1.\nDo lado direito: "
             "NÃO A = 0 e NÃO B = 1, e 0 + 1 = 1.\nOs dois lados dão 1. Repita com as outras três combinações e eles sempre "
             "empatam: a regra vale.\nEm palavras: uma NAND é um OU das entradas invertidas."},
            {K::Question, "Quanto vale A + NÃO A, qualquer que seja A?\n0\n*1\nDepende de A\n=Um dos dois sempre é 1: se A é 0, NÃO A é "
             "1. E um OU com um 1 dá 1.\n?Teste com A = 0 e depois com A = 1."},
            {K::Mistake, "Usar a distributiva só do jeito da aritmética\nNa aritmética, A + B · C não se abre. Na álgebra booleana se "
             "abre: A + BC = (A + B) · (A + C). É a regra que mais surpreende, e a que mais ajuda a simplificar."},
            {K::Page, "Mais entradas, e o atraso"},
            {K::Text, "Toda porta, menos o NÃO, pode ter mais de duas entradas. Um E de três entradas faz A · B · C de uma vez, sem "
             "precisar de duas portas em fila. No simulador, selecione a porta e mude as Entradas no painel da direita."},
            {K::Table, "Porta|Com várias entradas, a saída é 1 quando\n@and E|todas são 1\n@or OU|pelo menos uma é 1\n@nand NAND|pelo "
             "menos uma é 0\n@nor NOR|todas são 0\n@xor XOR|o número de entradas em 1 é ímpar"},
            {K::Text, "Uma porta também não responde no mesmo instante. A saída muda alguns bilionésimos de segundo depois da "
             "entrada: é o atraso de porta. Num caminho com várias portas em fila, os atrasos se somam, e é isso que limita "
             "a velocidade de um circuito: quanto mais portas no caminho mais longo, mais tempo até a saída ficar certa."},
            {K::Question, "Um XOR de três entradas recebe 1, 1 e 1. Quanto dá?\n0\n*1\nNão existe XOR de três entradas\n=Três entradas "
             "em 1 é um número ímpar de uns: o XOR dá 1.\n?Conte quantas entradas estão em 1."},
            {K::Summary, "Boole criou a álgebra (1854); Shannon a levou aos circuitos (1938).\nO E vem antes do OU; AB é A · B.\nAs "
             "regras vêm em pares; De Morgan troca E por OU invertendo tudo.\nA + BC = (A + B) · (A + C), ao contrário da "
             "aritmética.\nPortas podem ter várias entradas; o XOR de várias dá 1 com um número ímpar de uns.\nCada porta "
             "atrasa um pouco o sinal, e os atrasos se somam."},
        },
        {"porta-tres-entradas", "expressao"},
    });

    all.push_back(Topic{
        "portas-de-transistores",
        3,
        "Portas de transistores: o NÃO",
        "Por dentro de uma porta: o bit vira tensão, e um transistor faz o inversor",
        {
            {K::Page, "Por dentro de uma porta"},
            {K::Text, "Até aqui as portas eram caixinhas prontas. Por dentro, cada uma é feita de transistores e resistores, como os "
             "do Ato 2, e o bit volta a ser o que ele é de verdade: uma tensão. Perto de 0 V vale 0; perto de 5 V vale 1. "
             "Uma porta lógica é um circuito que recebe essas tensões e entrega outra, seguindo uma regra."},
            {K::Text, "Perto quer dizer com folga: 0,2 V ainda é 0, e 4,8 V ainda é 1. Essa folga é o que deixa o digital robusto: um "
             "pouco de ruído no fio não muda o bit."},
            {K::Question, "Num circuito de 5 V, uma saída mede 4,7 V. Que bit ela é?\n0\n*1\nNenhum dos dois\n=4,7 V está perto de 5 V: é "
             "1. O digital tem folga justamente para isso.\n?De qual dos dois extremos ela está perto?"},
            {K::Page, "O inversor: a porta NÃO"},
            {K::Text, "Ligue a saída S no 5 V por um resistor e ponha um transistor entre S e o terra. Com a entrada A em 0, o "
             "transistor fica desligado e o resistor puxa S para 5 V: S vale 1. Com A em 1, o transistor liga e puxa S para "
             "perto de 0 V: S vale 0. A saída é sempre o contrário da entrada."},
            {K::Text, "Esse resistor, que puxa para cima quando ninguém puxa para baixo, se chama resistor de pull-up."},
            {K::Figure, "O inversor feito de um transistor. Clique na chave A e veja S fazer o contrário.", notOfTransistor, 240},
            {K::Table, "A|S = NÃO A\n0|1\n1|0"},
            {K::Example, "Seguindo o inversor\nA = 0: a base não recebe corrente, e o transistor fica desligado.\nSem caminho para o "
             "terra, nenhuma corrente passa no pull-up, e ele não perde tensão: S fica com 5 V, ou seja, 1.\nA = 1: a base "
             "recebe corrente e o transistor liga, ligando S ao terra.\nAgora a corrente passa pelo pull-up e o transistor, "
             "e S cai para perto de 0 V: 0."},
            {K::Question, "No inversor, com A em 1, onde está S?\nPerto de 5 V\n*Perto de 0 V\nNo meio, 2,5 V\n=Com A em 1 o transistor "
             "liga e puxa S para o terra: S vale 0.\n?Com A em 1, o transistor está ligado ou desligado?"},
            {K::Mistake, "Esquecer o resistor de pull-up\nSem ele, com o transistor desligado, nada liga S ao 5 V: S fica solto, sem "
             "valor definido. O pull-up garante o 1 quando ninguém puxa para baixo."},
            {K::Summary, "No digital, perto de 0 V é 0 e perto de 5 V é 1, com folga.\nUma porta lógica recebe bits e entrega um bit, "
             "seguindo uma regra.\nO inversor: um transistor puxa S para o terra, e o pull-up puxa S para 5 V quando o "
             "transistor está desligado.\nS é sempre o contrário de A: é a porta NÃO."},
        },
        {"inversor"},
    });

    all.push_back(Topic{
        "nand-e-nor",
        3,
        "NAND, NOR, E e OU de transistores",
        "Dois transistores em série ou em paralelo, e um inversor a mais",
        {
            {K::Page, "Em série: a NAND"},
            {K::Text, "Ponha dois transistores em série entre S e o terra, com o pull-up em cima. S só vai a 0 quando os dois ligam "
             "juntos, quando A E B valem 1: só então existe um caminho até o terra. Em qualquer outro caso, o pull-up mantém "
             "S em 1."},
            {K::Text, "É um E com a saída invertida: a porta NAND, NÃO-E. São as chaves em série do Ato 1, agora comandadas por "
             "sinais e puxando a saída para baixo."},
            {K::Figure, "A NAND de transistores. Clique em A e em B: só com os dois ligados S vai a 0.", nandOfTransistors, 260},
            {K::Question, "Na NAND de transistores, com A = 1 e B = 0, quanto vale S?\n*1\n0\nDepende de qual transistor está em "
             "cima\n=Um transistor ligado sozinho não fecha o caminho até o terra: o pull-up mantém S em 1.\n?Em série, a "
             "corrente precisa passar pelos dois."},
            {K::Page, "Em paralelo: a NOR"},
            {K::Text, "Com os dois transistores em paralelo, cada um num caminho próprio até o terra, basta um ligar para puxar S "
             "para 0. S só vale 1 quando A e B valem 0. É um OU invertido: a porta NOR, NÃO-OU."},
            {K::Table, "A|B|NAND|NOR\n0|0|1|1\n0|1|1|0\n1|0|1|0\n1|1|0|0"},
            {K::Example, "Lendo a linha A = 0, B = 1\nNAND, em série: o transistor de A está desligado, e o caminho até o terra se "
             "corta. O pull-up ganha: S = 1.\nNOR, em paralelo: o transistor de B está ligado e abre um caminho até o terra. "
             "S cai: S = 0.\nPor isso, na mesma linha, a NAND dá 1 e a NOR dá 0."},
            {K::Question, "Em quantas linhas da tabela a NOR dá 1?\n*1\n2\n3\n=Só na primeira, com A e B em 0: qualquer transistor ligado "
             "puxa S para 0.\n?Conte os 1 da coluna NOR."},
            {K::Page, "E e OU: inverter outra vez"},
            {K::Text, "Com transistores, a NAND e a NOR saem direto, e o E e o OU não: a inversão vem de graça, e para desfazê-la é "
             "preciso mais um inversor na saída. O E de transistores é uma NAND seguida de um NÃO, três transistores ao "
             "todo; o OU é uma NOR seguida de um NÃO."},
            {K::Figure, "O E de transistores: a NAND de dois transistores, e um terceiro que inverte a saída. Clique em A e em B.", andOfTransistors, 260},
            {K::Figure, "O OU de transistores: a NOR, e o inversor depois dela. Clique em A e em B.", orOfTransistors, 260},
            {K::Question, "Quantos transistores tem o E feito de transistores?\n2\n*3\n4\n=Dois em série para a NAND e um terceiro para "
             "inverter a saída.\n?É uma NAND e mais um NÃO."},
            {K::Text, "É por isso que os chips são feitos de NAND e NOR: são as portas mais baratas, com menos transistores. Um E ou "
             "um OU custa um inversor a mais."},
            {K::Mistake, "Achar que transistores em série dão um E\nDariam, se acendessem a saída. Mas aqui eles puxam S para baixo, "
             "contra o pull-up: em série dão NÃO-E, a NAND; em paralelo, NÃO-OU, a NOR. A inversão vem de graça."},
            {K::Tip, "Toda a lógica de um computador pode ser montada só com NAND, ou só com NOR. A próxima aula mostra como."},
            {K::Summary, "Dois transistores em série puxando S para baixo: NAND, que só dá 0 com A e B em 1.\nEm paralelo: NOR, que só "
             "dá 1 com A e B em 0.\nO pull-up dá o 1; os transistores dão o 0.\nE e OU de transistores: a NAND ou a NOR, e "
             "mais um inversor.\nNAND e NOR são universais: com uma delas se faz todo o resto."},
        },
        {"tabelas-transistores", "nand-transistores", "nor-transistores"},
    });
    all.push_back(Topic{
        "nand-universal",
        3,
        "NAND, a porta universal, e o OU exclusivo",
        "Qualquer porta feita só de NAND, e como combinar portas",
        {
            {K::Page, "Uma porta para fazer todas"},
            {K::Text, "A porta NAND tem uma propriedade especial: com ela, e só com ela, dá para montar qualquer outra porta. Por "
             "isso se diz que ela é universal. As fábricas de chips aproveitam isso: um único tipo de bloco, repetido "
             "milhões de vezes, faz o circuito inteiro."},
            {K::Table, "Para ter...|Monte com NAND\nNÃO A|uma NAND com as duas entradas ligadas em A\nA E B|uma NAND e, depois dela, "
             "outra NAND que inverte a saída\nA OU B|inverta A e B com duas NAND e junte as duas numa terceira NAND"},
            {K::Figure, "Uma NAND com as duas entradas no mesmo fio. Clique em A: a saída é sempre o contrário.", notFromNand, 150},
            {K::Question, "Uma NAND com as duas entradas em A. Com A = 1, quanto dá a saída?\n1\n*0\nDepende de B\n=NÃO (1 E 1) = NÃO 1 = "
             "0. Com as duas entradas no mesmo fio, a NAND vira um NÃO.\n?A NAND só dá 0 quando todas as entradas são 1."},
            {K::Page, "O E e o OU feitos de NAND"},
            {K::Figure, "Duas NAND em fila: a segunda desfaz a inversão da primeira, e sobra o E. Clique em A e em B.", andFromNand, 150},
            {K::Text, "O OU sai da lei de De Morgan: NÃO (NÃO A E NÃO B) é igual a A OU B. Em palavras: dizer que não é verdade que "
             "os dois estão desligados é o mesmo que dizer que pelo menos um está ligado."},
            {K::Example, "O OU de três NAND, com A = 0 e B = 1\nA primeira NAND inverte A: NÃO 0 = 1.\nA segunda inverte B: NÃO 1 = "
             "0.\nA terceira recebe 1 e 0: NÃO (1 E 0) = NÃO 0 = 1.\nA OU B, com A = 0 e B = 1, dá 1: certo."},
            {K::Question, "Quantas NAND são precisas para fazer um OU?\n1\n2\n*3\n=Duas para inverter A e B, e uma terceira para "
             "juntá-las.\n?Olhe a última linha da tabela."},
            {K::Page, "OU exclusivo"},
            {K::Text, "O XOR dá 1 quando as entradas são diferentes. Ele pode ser montado com as portas que você já conhece: (A OU B) "
             "E NÃO (A E B), ou seja, pelo menos um ligado, mas não os dois. O XOR aparece em todo lugar: na soma de "
             "números, na comparação e na detecção de erros."},
            {K::Table, "A|B|A OU B|A XOR B\n0|0|0|0\n0|1|1|1\n1|0|1|1\n1|1|1|0"},
            {K::Mistake, "Achar que XOR e OU são a mesma coisa\nSó diferem numa linha, mas ela importa: com as duas entradas em 1, o OU "
             "dá 1 e o XOR dá 0. É essa linha que faz do XOR o bit da soma: 1 + 1 = 10, e o bit da direita é 0."},
            {K::Page, "Só com NOR também"},
            {K::Text, "A NOR é universal como a NAND. Uma NOR com as duas entradas juntas é um NÃO; uma NOR seguida desse NÃO é um "
             "OU; e, pela lei de De Morgan, uma NOR das entradas invertidas é um E. Um conjunto de portas que faz qualquer "
             "função se diz funcionalmente completo."},
            {K::Table, "Conjunto|Por que basta\nE, OU, NÃO|são as três operações da álgebra booleana\nE, NÃO|o OU sai de De Morgan: "
             "NÃO (NÃO A E NÃO B)\nOU, NÃO|o E sai de De Morgan: NÃO (NÃO A OU NÃO B)\nsó NAND|faz o NÃO, o E e o OU\nsó "
             "NOR|faz o NÃO, o OU e o E"},
            {K::Question, "Quantas NOR são precisas para fazer um E?\n1\n2\n*3\n=Duas para inverter A e B, e uma terceira juntando: NOR "
             "(NÃO A, NÃO B) = A E B.\n?É o mesmo truque do OU de NAND, com as portas trocadas."},
            {K::Summary, "NAND é universal: NÃO, E, OU e todas as outras saem só dela.\nNÃO: uma NAND com as entradas juntas. E: NAND e "
             "mais uma NAND. OU: três NAND, pela lei de De Morgan.\nXOR dá 1 quando as entradas são diferentes: (A OU B) E "
             "NÃO (A E B)."},
        },
        {"nao-com-nand", "e-com-nand", "ou-com-nand", "xor"},
    });

    all.push_back(Topic{
        "decodificador-e-mux",
        3,
        "Decodificador e multiplexador",
        "Números de vários bits: escolher uma saída e escolher uma entrada",
        {
            {K::Page, "O decodificador"},
            {K::Text, "O decodificador recebe um número e acende só a saída daquele número. Com 2 bits de entrada, A1 e A0, ele tem 4 "
             "saídas, Y0 a Y3. Cada saída é uma porta E que reconhece uma combinação. Na CPU, um decodificador escolhe qual "
             "registrador vai ser escrito."},
            {K::Figure, "O decodificador 2 para 4 feito de portas. Clique em A0 e A1: só a saída do número acende.", decoderGates, 260},
            {K::Example, "A porta de Y2\nY2 deve acender quando o número é 2, ou seja, A1 = 1 e A0 = 0.\nA porta E de Y2 recebe A1 "
             "direto e A0 invertido: Y2 = A1 E NÃO A0.\nCom A1 = 1 e A0 = 0: 1 E NÃO 0 = 1 E 1 = 1. Acende.\nCom qualquer "
             "outro número, uma das entradas da porta é 0, e Y2 apaga."},
            {K::Question, "Num decodificador 2 para 4, com A1 = 1 e A0 = 1, qual saída acende?\nY1\nY2\n*Y3\n=11 em binário é 3: acende "
             "Y3, e só ela.\n?Que número 11 é em binário?"},
            {K::Page, "O multiplexador"},
            {K::Text, "O multiplexador, ou MUX, faz o contrário: tem várias entradas de dados e uma só saída, e um número de seleção "
             "escolhe qual entrada passa. Um MUX 4:1 tem as entradas D0 a D3 e a seleção S1 S0. Por dentro, é um "
             "decodificador da seleção, uma porta E por entrada e uma porta OU juntando tudo."},
            {K::Table, "S1|S0|A saída Y é\n0|0|D0\n0|1|D1\n1|0|D2\n1|1|D3"},
            {K::Question, "Num MUX 4:1 com S1 = 0 e S0 = 1, D1 = 0 e D2 = 1. Quanto vale Y?\n*0\n1\nOs dois juntos\n=A seleção 01 escolhe "
             "D1, que vale 0. O 1 de D2 nem chega à saída.\n?Primeiro descubra qual entrada a seleção escolhe."},
            {K::Mistake, "Confundir decodificador e multiplexador\nO decodificador tem uma entrada de número e várias saídas: acende "
             "uma. O multiplexador tem várias entradas de dados e uma saída: deixa passar uma. Um escolhe para onde; o "
             "outro, de onde."},
            {K::Tip, "A CPU do Ato 7 está cheia dos dois: no Raio-X, cada MUX é um multiplexador, e o banco de registradores tem um "
             "decodificador por dentro."},
            {K::Page, "Onde eles aparecem"},
            {K::Text, "Uma memória grande é feita de vários chips menores. Os bits mais altos do endereço entram num decodificador, "
             "e cada saída dele liga um chip só: com 2 bits, um decodificador 2 para 4 escolhe entre quatro chips, e os "
             "bits de baixo escolhem a palavra dentro do chip escolhido."},
            {K::Text, "O multiplexador aparece sempre que um lugar pode receber de várias fontes: o próximo valor do PC vem da soma "
             "de 1, do desvio ou do salto; o que se grava num registrador vem da ULA, da memória ou do imediato. Um MUX por "
             "escolha, e o controle da CPU manda na seleção."},
            {K::Question, "Uma memória de quatro chips usa os 2 bits mais altos do endereço para escolher o chip. Com esses bits em 10, "
             "qual chip responde?\nO chip 1\n*O chip 2\nOs chips 1 e 0\n=10 em binário é 2: o decodificador liga só a saída "
             "2.\n?Que número é 10 em binário?"},
            {K::Summary, "Decodificador: um número acende uma saída. Cada saída é uma E que reconhece uma combinação.\nMultiplexador: um "
             "número escolhe qual entrada passa para a saída.\nPor dentro do MUX: um decodificador, uma E por entrada e uma "
             "OU.\nOs dois estão em toda a CPU."},
        },
        {"decodificador", "mux4"},
    });
    // Act 5: arithmetic.
    all.push_back(Topic{
        "numeros-binarios",
        4,
        "Somar em binário",
        "A soma no papel, o meio somador, o somador completo e números inteiros",
        {
            {K::Page, "Somar como no papel"},
            {K::Text, "Somar em binário é como somar no papel: casa por casa, da direita para a esquerda, com vai-um. A diferença é "
             "que 1 + 1 já não cabe numa casa: dá 10, ou seja, 0 e vai 1."},
            {K::Table, "A|B|Soma S|Vai-um C\n0|0|0|0\n0|1|1|0\n1|0|1|0\n1|1|0|1"},
            {K::Example, "0101 + 0011 (5 + 3)\nCasa do 1: 1 + 1 = 10. Fica 0, vai 1.\nCasa do 2: 0 + 1 + o vai-um 1 = 10. Fica 0, vai "
             "1.\nCasa do 4: 1 + 0 + 1 = 10. Fica 0, vai 1.\nCasa do 8: 0 + 0 + 1 = 1. Fica 1.\nResultado: 1000, que é 8. "
             "Confere: 5 + 3 = 8."},
            {K::Question, "Quanto dá 1 + 1 + 1 em binário?\n3\n*11\n111\n=1 + 1 = 10, e mais 1 dá 11: fica 1 e vai 1. É o 3 em "
             "binário.\n?Some dois primeiro, depois o terceiro."},
            {K::Page, "O meio somador"},
            {K::Text, "Olhe a tabela de novo: a soma S é 1 quando A e B são diferentes, um XOR; e o vai-um C é 1 só quando os dois "
             "são 1, um E. Duas portas fazem a soma de dois bits. Esse circuito se chama meio somador, porque ainda não "
             "recebe o vai-um que vem da casa de baixo."},
            {K::Figure, "O meio somador: um XOR e um E. Clique em A e em B; com os dois em 1, S apaga e C acende: 1 + 1 = 10.", halfAdder, 200},
            {K::Question, "No meio somador, que porta dá o vai-um?\nXOR\n*E (AND)\nOU (OR)\n=O vai-um só é 1 quando A e B são 1: é um "
             "E.\n?Em que linha da tabela o vai-um é 1?"},
            {K::Page, "O somador completo"},
            {K::Text, "Da segunda casa em diante, a soma tem três bits para juntar: A, B e o vai-um que veio da direita, Ce. O "
             "somador completo soma os três e entrega S e o vai-um de saída, Cs. Ele é feito de dois meios somadores e uma "
             "porta OU que junta os dois vai-uns."},
            {K::Figure, "O somador completo. Clique em A, B e Ce: com os três em 1, 1 + 1 + 1 = 11, e S e Cs acendem.", fullAdder, 240},
            {K::Question, "No somador completo, com A = 1, B = 0 e Ce = 1, quanto valem S e Cs?\nS = 1, Cs = 0\n*S = 0, Cs = 1\nS = 1, Cs "
             "= 1\n=1 + 0 + 1 = 2, que é 10: S = 0 e vai um, Cs = 1.\n?Some os três e escreva o resultado em binário."},
            {K::Page, "Somar números inteiros"},
            {K::Text, "Para somar números de 4 bits, ponha quatro somadores completos lado a lado e ligue o Cs de cada um no Ce do "
             "seguinte. O vai-um passa de casa em casa, como no papel: é o somador com propagação do vai-um. A CPU do jogo "
             "faz exatamente isso, com 16 bits."},
            {K::Tip, "S = A XOR B XOR Ce, e Cs = (A E B) OU (Ce E (A XOR B)). Com isso dá para somar números de qualquer tamanho."},
            {K::Mistake, "Deixar solto o Ce da primeira casa\nNa casa do 1 não chega vai-um nenhum: o Ce dela tem que valer 0. Solto, "
             "ele não tem valor definido, e a soma pode sair errada."},
            {K::Mistake, "Achar que a soma sempre cabe\n1111 + 0001 dá 10000, que tem 5 bits. Com 4, sobra 0000 e o vai-um sai pelo "
             "último Cs: é o estouro. Quem soma tem que olhar esse Cs."},
            {K::Page, "O vai-um que atravessa"},
            {K::Text, "No somador em fila, cada somador completo espera o vai-um do vizinho da direita. Num somador de 32 bits, o "
             "vai-um pode ter de atravessar os 32, um depois do outro, e cada passagem custa o atraso de algumas portas. É "
             "o somador com propagação do vai-um (ripple carry): simples, mas lento."},
            {K::Text, "Os processadores usam o vai-um antecipado (carry lookahead). Cada casa diz, só com os seus A e B, se gera um "
             "vai-um (A E B) ou se deixa passar um que chegue (A OU B). Com essas duas informações de todas as casas, um "
             "circuito calcula todos os vai-uns de uma vez, sem esperar a fila: mais portas, muito menos tempo."},
            {K::Question, "Num somador de 4 bits em fila, quantas vezes o vai-um pode ter de passar de uma casa para a seguinte?\n1\n*3\n"
             "8\n=Da casa 0 para a 1, da 1 para a 2 e da 2 para a 3: três passagens, uma depois da outra.\n?Conte as "
             "fronteiras entre quatro casas."},
            {K::Summary, "Em binário, 1 + 1 = 10: fica 0 e vai 1.\nMeio somador: S é um XOR, o vai-um é um E.\nSomador completo: soma A, "
             "B e o vai-um de entrada; dois meios somadores e um OU.\nLado a lado, com o Cs de cada um no Ce do seguinte, "
             "somam números inteiros."},
        },
        {"somas-binarias", "meio-somador", "somador-completo", "somador-4-bits"},
    });

    all.push_back(Topic{
        "subtrair-comparar-ula",
        4,
        "Subtrair e números negativos",
        "O empréstimo, o complemento de dois e a mesma conta para somar e subtrair",
        {
            {K::Page, "Subtrair com empréstimo"},
            {K::Text, "Subtrair bit a bit funciona como no papel, com empréstimo em vez de vai-um. 0 - 1 não dá: a casa pede 1 "
             "emprestado à de cima, e o resultado é 1, com empréstimo. O subtrator completo recebe A, B e o pedido que vem "
             "de baixo, Ve, e entrega a diferença D e o pedido para cima, Vs. A diferença é de novo um XOR: D = A XOR B XOR "
             "Ve."},
            {K::Figure, "O subtrator. Clique em A, B e Ve: com A = 0 e B = 1, D = 1 e Vs acende, pedindo emprestado.", subtractor, 230},
            {K::Question, "No subtrator, com A = 1, B = 1 e Ve = 0, quanto valem D e Vs?\n*D = 0, Vs = 0\nD = 1, Vs = 1\nD = 0, Vs = "
             "1\n=1 - 1 = 0, sem precisar pedir emprestado.\n?1 - 1 precisa de empréstimo?"},
            {K::Page, "Números negativos"},
            {K::Text, "Com 4 bits, depois de 1111 o próximo número volta a 0000, como um hodômetro. Os computadores aproveitam isso "
             "para guardar números negativos: -1 é o número que, somado a 1, dá 0, ou seja, 1111. Esse jeito se chama "
             "complemento de dois. Para achar -N, inverta todos os bits de N e some 1."},
            {K::Table, "Bits|Sem sinal|Com sinal (complemento de "
             "dois)\n0011|3|3\n0001|1|1\n0000|0|0\n1111|15|-1\n1101|13|-3\n1000|8|-8"},
            {K::Example, "Achando -3 com 4 bits\n3 em binário é 0011.\nInvertendo todos os bits: 1100.\nSomando 1: 1101.\nConfere: 0011 "
             "+ 1101 = 10000, e com 4 bits sobra 0000. 3 + (-3) = 0."},
            {K::Question, "Com 4 bits em complemento de dois, como se escreve -2?\n1010\n*1110\n0010\n=2 é 0010; invertendo, 1101; "
             "somando 1, 1110.\n?Inverta os bits de 2 e some 1."},
            {K::Page, "O mesmo somador para os dois"},
            {K::Text, "A vantagem do complemento de dois é que o mesmo somador serve para somar e subtrair: A - B é A + (NÃO B) + 1. "
             "O NÃO inverte os bits de B, e o + 1 entra pelo vai-um de entrada da primeira casa. É por isso que a CPU tem "
             "uma só ULA para as duas contas."},
            {K::Mistake, "Inverter os bits e esquecer o + 1\nSó inverter dá o complemento de um: NÃO 0011 = 1100, que é -4, não -3. O + "
             "1 é parte da regra."},
            {K::Mistake, "Achar que 1111 é sempre 15\nOs bits são os mesmos; quem diz o que eles valem é a forma de ler. Sem sinal, 1111 "
             "é 15; com sinal, é -1. A CPU soma do mesmo jeito, e o programa decide como ler."},
            {K::Page, "Quando o resultado não cabe: o estouro"},
            {K::Text, "Com 4 bits em complemento de dois cabem os números de -8 a 7. Em geral, com n bits, vão de -2 elevado a n-1 "
             "até 2 elevado a n-1, menos 1. Uma conta cujo resultado sai dessa faixa dá estouro (overflow): os bits que "
             "sobram dizem outro número."},
            {K::Text, "A regra para perceber: se os dois números somados têm o mesmo sinal e o resultado tem o sinal oposto, houve "
             "estouro. Somando um positivo com um negativo, nunca há."},
            {K::Example, "5 + 4 com 4 bits\n5 = 0101 e 4 = 0100, os dois positivos.\n0101 + 0100 = 1001.\n1001 começa com 1: lido com "
             "sinal, é -7.\nDois positivos deram um negativo: estouro. O 9 não cabe em 4 bits com sinal."},
            {K::Text, "Por que não guardar só um bit de sinal e o tamanho, como no papel? Porque aí existem dois zeros, +0 e -0, e "
             "somar exige olhar os sinais antes. No complemento de dois há um zero só, e o mesmo somador serve para tudo."},
            {K::Question, "Com 4 bits em complemento de dois, -6 + (-5) dá estouro?\n*Sim\nNão\nSó se o vai-um for 1\n=-11 não cabe "
             "entre -8 e 7. E a regra mostra: dois negativos dão 0101, um positivo.\n?Qual é o menor número de 4 bits com "
             "sinal?"},
            {K::Summary, "Subtrair é como somar, com empréstimo: D = A XOR B XOR Ve.\nComplemento de dois: -N é NÃO N mais 1. -1 é "
             "1111.\nA - B = A + (NÃO B) + 1: o mesmo somador faz as duas contas.\nOs mesmos bits podem ser lidos com ou sem "
             "sinal."},
        },
        {"complemento-de-dois", "subtrator"},
    });

    all.push_back(Topic{
        "comparador-e-ula",
        4,
        "Comparar e a ULA",
        "Maior, igual ou menor, e a peça que faz todas as contas da CPU",
        {
            {K::Page, "Comparar"},
            {K::Text, "O comparador diz se A é maior, igual ou menor que B. Para um bit: A > B quando A = 1 e B = 0 (A E NÃO B); A < "
             "B no caso contrário (NÃO A E B); e A = B quando os dois são iguais, um XNOR. Para números de vários bits, "
             "compara-se do bit mais alto para o mais baixo: o primeiro bit diferente decide."},
            {K::Figure, "O comparador de 1 bit. Clique em A e em B e veja qual das três saídas acende.", comparator, 220},
            {K::Example, "Comparando 0110 e 0101\nBit 3: 0 e 0, iguais. Segue.\nBit 2: 1 e 1, iguais. Segue.\nBit 1: 1 e 0, diferentes. "
             "Quem tem o 1 é o maior: 0110 > 0101.\nO bit 0 nem precisa ser olhado. Em decimal: 6 > 5."},
            {K::Question, "Comparando 1001 e 0111, qual é o maior?\n*1001\n0111\nSão iguais\n=O primeiro bit diferente é o bit 3: 1001 "
             "tem 1 ali. 9 > 7, mesmo com mais 1 no 0111.\n?Comece pelo bit da esquerda."},
            {K::Page, "A ULA"},
            {K::Text, "A ULA, unidade lógica e aritmética, é onde a CPU faz as contas. Ela calcula ao mesmo tempo várias operações "
             "sobre A e B, como E, OU, XOR e soma, e um multiplexador escolhe qual resultado sai, de acordo com o código da "
             "operação, Op. Uma fatia de 1 bit faz isso para uma casa; lado a lado, com o vai-um passando entre elas, as "
             "fatias formam a ULA de 16 bits do computador."},
            {K::Figure, "A ULA de 1 bit montada com blocos: as quatro operações lado a lado e o MUX que escolhe uma pelo Op. Clique nas "
             "chaves.", aluSlice, 280},
            {K::Table, "Op1|Op0|Operação\n0|0|A E B\n0|1|A OU B\n1|0|A XOR B\n1|1|A + B"},
            {K::Question, "Na fatia de ULA, com A = 1, B = 1 e Op = 10, quanto sai em R?\n1\n*0\n10\n=Op = 10 escolhe o XOR, e 1 XOR 1 = "
             "0.\n?Primeiro veja na tabela qual operação o Op escolhe."},
            {K::Mistake, "Achar que a ULA calcula só a operação escolhida\nEla calcula todas ao mesmo tempo, o tempo todo; o Op só diz "
             "ao multiplexador qual resultado deixar passar. Escolher é mais rápido que calcular sob encomenda."},
            {K::Page, "O que a ULA conta além do resultado"},
            {K::Text, "Além do resultado, a ULA entrega alguns bits sobre ele. Zero (Z): o resultado é 0. Vai-um (C): sobrou um bit "
             "na soma. Sinal (N): o resultado é negativo, com o bit mais alto em 1. Estouro (V): o resultado não coube, com "
             "sinal. A ULA da biblioteca tem a saída Z, e o vai-um sai em Cs."},
            {K::Text, "Muitos processadores guardam esses bits num registrador de sinalizadores (flags), e os desvios olham para "
             "eles: comparar é subtrair e ver se deu zero ou negativo. O computador deste jogo e o MIPS fazem diferente: o "
             "desvio compara dois registradores direto, sem guardar sinalizadores."},
            {K::Question, "A ULA fez A − B e a saída Z acendeu. O que se sabe?\nA é maior que B\n*A é igual a B\nA é zero\n=Z quer dizer "
             "resultado zero: A − B = 0, então A = B.\n?Quando uma subtração dá 0?"},
            {K::Summary, "Comparador de 1 bit: A > B é A E NÃO B; A < B é NÃO A E B; A = B é um XNOR.\nPara vários bits, o primeiro bit "
             "diferente, a partir do mais alto, decide.\nA ULA calcula várias operações ao mesmo tempo, e um MUX escolhe uma "
             "pelo Op.\nFatias de 1 bit lado a lado formam a ULA da CPU."},
        },
        {"comparador", "ula-1-bit", "calculadora-bits"},
    });
    // Act 6: memory.
    all.push_back(Topic{
        "latches",
        5,
        "Circuitos que lembram",
        "Realimentação e o latch SR",
        {
            {K::Page, "Até aqui, todo circuito esquecia"},
            {K::Text, "Em tudo o que você montou até agora, a saída depende só das entradas deste instante. Desligue a chave que "
             "acendia o LED e ele apaga: o circuito não guarda nada do que aconteceu antes. Circuitos assim se chamam "
             "combinacionais."},
            {K::Text, "Um computador precisa lembrar: em que instrução está, os números dos registradores, tudo o que está na "
             "memória. Para isso ele usa circuitos sequenciais, em que a saída depende também do que aconteceu antes. Esta "
             "aula mostra o menor deles."},
            {K::Question, "Num circuito combinacional, você liga a entrada que acende o LED e depois a desliga. O que o LED faz?\n*Apaga: "
             "a saída volta a ser o que as entradas de agora mandam\nContinua aceso, lembrando que a entrada esteve "
             "ligada\nFica pela metade, nem aceso nem apagado\n=Isso: num circuito combinacional, a saída só depende das "
             "entradas deste instante. Ele não lembra de nada.\n?Pense numa porta E: se uma entrada volta a 0, a saída dela "
             "continua em 1?"},
            {K::Page, "A saída que volta para a entrada"},
            {K::Text, "O truque para lembrar é a realimentação: ligar a saída de volta numa entrada. Pegue duas portas NÃO em roda, a "
             "saída de cada uma na entrada da outra. Se a primeira recebe 1, ela dá 0; a segunda recebe esse 0 e devolve 1 "
             "para a primeira. Tudo concorda, e o circuito fica assim para sempre."},
            {K::Example, "Duas portas NÃO em roda\nComece com 1 na entrada da primeira NÃO. Ela dá 0.\nEsse 0 entra na segunda NÃO, que "
             "dá 1.\nEsse 1 volta para a primeira, que já estava recebendo 1: nada muda. O 1 está guardado.\nComece com 0 e "
             "acontece o mesmo ao contrário: o 0 fica guardado. São dois estados que se sustentam: um bit."},
            {K::Text, "Só que duas NÃO em roda não têm por onde mudar o valor guardado. Para poder ligar e desligar o bit, troque "
             "cada NÃO por uma porta NOR, a NÃO-OU, que no jogo aparece como NOU: ela dá 1 só quando as duas entradas são 0. A entrada livre de cada uma vira um botão."},
            {K::Page, "O latch SR"},
            {K::Text, "Duas portas NOR com as saídas cruzadas formam o latch SR. S (set, ligar) põe Q em 1. R (reset, desligar) põe Q em 0. "
             "Com S e R em 0, cada porta segura a outra, e Q fica como estava. Experimente na figura: ligue S, desligue, e "
             "veja Q continuar aceso."},
            {K::Figure, "O latch SR feito de duas portas NOR. Clique em S e em R.", srLatchFigure, 230},
            {K::Table, "S|R|Q depois\n1|0|1: liga\n0|1|0: desliga\n0|0|o mesmo de antes: guarda\n1|1|proibido: não use"},
            {K::Question, "Q está em 1. Você põe S e R em 0. O que acontece com Q?\nVolta para 0, porque nada o liga\n*Continua em 1: é "
             "isso que o latch guarda\nFica piscando entre 0 e 1\n=Com S e R em 0, cada NOR segura a outra no estado em que "
             "está. Q continua em 1 até alguém ligar R.\n?Olhe a linha 0 e 0 da tabela."},
            {K::Page, "Passo a passo, porta por porta"},
            {K::Example, "Ligando o latch com S\nAntes: S = 0, R = 0 e Q = 0. A NOR de cima recebe S = 0 e Q = 0, e dá !Q = 1.\nLigue S. "
             "A NOR de cima agora recebe um 1: NOR dá 0, então !Q = 0.\nA NOR de baixo recebe R = 0 e !Q = 0: dá Q = "
             "1.\nDesligue S. A NOR de cima recebe S = 0 e Q = 1: continua dando 0. Nada muda: Q ficou em 1."},
            {K::Mistake, "Ligar S e R ao mesmo tempo\nAs duas NOR dão 0, e Q e !Q deixam de ser opostos. Pior: quando os dois voltam a 0 "
             "juntos, não dá para saber para que lado o latch cai. Por isso essa combinação é proibida. A próxima aula "
             "resolve isso com o latch D."},
            {K::Mistake, "Achar que o latch guarda o bit com o computador desligado\nEle guarda enquanto houver energia: é a corrente "
             "nas portas que mantém o estado. Desligou, perdeu. A memória RAM do computador funciona assim também."},
            {K::Page, "O mesmo latch, feito de NAND"},
            {K::Text, "O latch também pode ser feito com duas NAND cruzadas, em vez de duas NOR. A ideia é a mesma, com tudo "
             "invertido: as entradas agem quando vão a 0, não a 1. Em repouso as duas ficam em 1, um 0 em S liga, um 0 em R "
             "desliga, e os dois em 0 ao mesmo tempo é o caso proibido. Por isso, nos desenhos, essas entradas levam uma "
             "barra por cima ou uma bolinha."},
            {K::Table, "Feito de|Em repouso|Liga com|Desliga com|Proibido\nNOR|S = 0, R = 0|S = 1|R = 1|S = R = 1\nNAND|S = 1, R = "
             "1|S = 0|R = 0|S = R = 0"},
            {K::Question, "Num latch de NAND, as duas entradas estão em 1. O que acontece com Q?\nVai a 1\nVai a 0\n*Guarda o que tinha\n"
             "=Em 1, as entradas de um latch de NAND estão em repouso: ele guarda.\n?No latch de NAND, quem age é o 0."},
            {K::Summary, "Combinacional: a saída depende só das entradas de agora. Sequencial: depende também do que veio "
             "antes.\nRealimentação, a saída voltando para a entrada, é o que faz um circuito lembrar.\nO latch SR, duas NOR "
             "cruzadas, guarda um bit: S liga, R desliga, com os dois em 0 ele guarda.\nS e R juntos em 1 é proibido."},
        },
        {"latch-sr"},
    });
    all.push_back(Topic{
        "latch-d",
        5,
        "O latch D",
        "Um dado e uma permissão para guardá-lo",
        {
            {K::Page, "O problema do latch SR"},
            {K::Text, "O latch SR tem dois botões, e uma combinação deles é proibida. O que um computador quer é mais simples: um fio "
             "com o dado, D, e outro que diz quando guardá-lo, En (enable, permissão). Com En ligado, Q copia D. Com En "
             "desligado, Q guarda o último valor, mude D como mudar."},
            {K::Question, "Para que serve a entrada En do latch D?\nPara ligar e desligar a energia do latch\n*Para dizer quando o latch "
             "pode copiar D\nPara inverter o valor guardado\n=En é a permissão: com ela ligada, Q segue D; desligada, Q fica "
             "parado com o último D.\n?En vem de enable, que quer dizer habilitar, permitir."},
            {K::Page, "Como ele é feito"},
            {K::Text, "Na frente de um latch SR, duas portas E. Uma faz S = D E En; a outra, R = (NÃO D) E En. Com En em 0, as duas E "
             "dão 0, e o latch guarda. Com En em 1, se D é 1 a primeira liga S; se D é 0 a segunda liga R. S e R nunca ficam "
             "em 1 juntos: o problema do SR some."},
            {K::Figure, "O latch D feito de portas. Ligue En e mexa em D; depois desligue En e mexa de novo.", dLatchFigure, 260},
            {K::Table, "En|D|Q\n0|qualquer|guarda o que tinha\n1|0|0\n1|1|1"},
            {K::Page, "Enquanto En estiver ligado, ele é transparente"},
            {K::Text, "Repare: com En em 1 o latch não guarda um instante, ele deixa passar. Q acompanha D o tempo todo, como se o "
             "latch fosse um fio. Diz-se que ele é transparente. Só quando En volta a 0 é que o último D fica preso."},
            {K::Example, "Seguindo o latch no tempo\nEn = 1, D = 1: Q = 1.\nAinda com En = 1, D vai a 0: Q vai a 0 junto. "
             "Transparente.\nD volta a 1: Q volta a 1.\nEn vai a 0. Agora D vai a 0: Q continua 1. Guardou o último D, o 1."},
            {K::Question, "Com En em 1, D vai de 0 para 1 e volta para 0. Depois En vai para 0. O que Q guarda?\n*0\n1\nDepende de quanto "
             "tempo D ficou em 1\n=Com En ligado, Q seguiu D até o fim; quando En desligou, D era 0. É o 0 que fica.\n?O que "
             "vale é o D do instante em que En desliga."},
            {K::Mistake, "Achar que o latch D só olha o D no instante em que En liga\nNão: durante todo o tempo em que En está em 1, "
             "qualquer mudança de D passa para Q. Isso vira um problema quando Q volta para D por algum caminho, como num "
             "contador: o valor dá voltas sem parar. O flip-flop, duas aulas adiante, resolve."},
            {K::Summary, "O latch D tem um dado, D, e uma permissão, En.\nCom En em 1, Q copia D o tempo todo: ele é transparente.\nCom "
             "En em 0, Q guarda o último D.\nPor dentro: duas E na frente de um latch SR, que nunca recebe S e R juntos."},
        },
        {"latch-d"},
    });
    all.push_back(Topic{
        "clock-e-registradores",
        5,
        "O clock: o compasso do computador",
        "Bordas, ciclos e frequência",
        {
            {K::Page, "Por que um compasso"},
            {K::Text, "Num computador, milhares de fios mudam ao mesmo tempo, e cada sinal leva um tempinho diferente para atravessar "
             "as portas. Se cada parte guardasse o que chega na hora em que chega, umas guardariam o valor novo e outras o "
             "velho. O jeito é combinar um instante para todos guardarem juntos."},
            {K::Text, "Esse instante é marcado pelo clock: um sinal que alterna entre 0 e 1 sem parar, como um metrônomo. Entre um "
             "tique e outro os sinais têm tempo de se acomodar; no tique, tudo é guardado de uma vez."},
            {K::Page, "Bordas"},
            {K::Text, "A passagem de 0 para 1 se chama borda de subida; a de 1 para 0, borda de descida. Um ciclo vai de uma subida "
             "até a seguinte. Os circuitos deste jogo, como os da maioria dos computadores, guardam na borda de subida."},
            {K::Table, "Instante|1|2|3|4|5|6|7|8\nClock|0|1|1|0|1|0|0|1\nBorda|-|sobe|-|desce|sobe|desce|-|sobe"},
            {K::Question, "Na tabela, quantas bordas de subida o clock teve?\n2\n*3\n4\n=Nos instantes 2, 5 e 8 o clock passou de 0 para "
             "1. Ficar em 1, como no instante 3, não é borda.\n?Conte só as passagens de 0 para 1."},
            {K::Page, "Frequência e período"},
            {K::Text, "A frequência diz quantos ciclos o clock dá por segundo, em hertz (Hz). O período é quanto dura um ciclo: 1 "
             "dividido pela frequência. O computador do jogo vai de 1 Hz a milhares de Hz; um processador de verdade passa "
             "de 3 GHz, três bilhões de ciclos por segundo."},
            {K::Example, "Um clock de 4 Hz\n4 Hz quer dizer 4 ciclos por segundo, ou seja, 4 bordas de subida a cada segundo.\nEm 3 "
             "segundos: 4 × 3 = 12 bordas de subida.\nO período: 1 segundo dividido em 4 ciclos = 0,25 segundo, ou 250 "
             "milissegundos por ciclo."},
            {K::Mistake, "Achar que dá para acelerar o clock à vontade\nEntre duas bordas, o sinal mais lento do circuito precisa chegar "
             "ao fim do seu caminho de portas. Se o clock for mais rápido que isso, a borda guarda um valor ainda pela "
             "metade da conta. Esse caminho mais lento, o caminho crítico, é que limita a frequência."},
            {K::Page, "Quão rápido pode ser o clock"},
            {K::Text, "Entre uma borda e a seguinte, o sinal sai de um registrador, atravessa portas e chega ao próximo registrador. "
             "Cada porta tem o seu atraso, e eles se somam. O caminho com mais atraso, o caminho crítico, decide tudo: se a "
             "borda chegar antes de o sinal terminar de atravessá-lo, o registrador guarda um valor pela metade."},
            {K::Example, "O clock máximo de um circuito\nO caminho mais lento passa por 10 portas, e cada uma atrasa 1 nanossegundo.\nO "
             "sinal leva 10 ns para atravessá-lo: o período tem de ser de pelo menos 10 ns.\nFrequência = 1 ÷ período = 1 ÷ "
             "0,000 000 01 s = 100 milhões de vezes por segundo.\nO clock pode ir até 100 MHz, e não mais."},
            {K::Text, "Por isso quem projeta processadores corta o caminho crítico: usa portas mais rápidas, menos portas em fila e "
             "divide o trabalho em etapas, cada uma com o seu registrador."},
            {K::Question, "Se o caminho crítico de um circuito cai pela metade, o que acontece com o clock máximo?\nCai pela metade\n"
             "Fica igual\n*Dobra\n=O período mínimo cai pela metade, e a frequência é 1 dividido pelo período: dobra.\n"
             "?Frequência é o inverso do período."},
            {K::Summary, "O clock alterna entre 0 e 1 e marca quando tudo é guardado ao mesmo tempo.\nBorda de subida: de 0 para 1. É "
             "nela que os circuitos do jogo guardam.\nFrequência: ciclos por segundo (Hz). Período: 1 ÷ frequência.\nO "
             "caminho mais lento entre duas bordas limita a frequência."},
        },
        {"clock-perguntas"},
    });
    all.push_back(Topic{
        "flip-flop-d",
        5,
        "O flip-flop D",
        "Guardar só na borda de subida",
        {
            {K::Page, "Por que o latch não basta"},
            {K::Text, "Imagine um latch D cujo Q passa por um somador que soma 1 e volta para o D. Com En ligado, o latch é "
             "transparente: o valor sai, ganha 1, entra, sai de novo, ganha mais 1... e dispara, sem parar, enquanto En "
             "estiver ligado. Nunca dá para saber onde vai parar."},
            {K::Text, "O que se quer é guardar num instante só, a borda, e no resto do tempo não deixar nada passar. Esse é o "
             "flip-flop D: Q copia D na borda de subida do clock, e só nela."},
            {K::Page, "Mestre e escravo"},
            {K::Text, "Por dentro, o flip-flop são dois latches D em fila. O primeiro, o mestre, recebe o clock invertido: fica "
             "aberto com o clock em 0. O segundo, o escravo, recebe o clock direto: fica aberto com o clock em 1. Os dois "
             "nunca estão abertos ao mesmo tempo, e por isso nada atravessa os dois de uma vez."},
            {K::Figure, "O flip-flop D: dois latches, o mestre e o escravo. Mude D com Clk em 0 e veja que Q não muda; depois ligue "
             "Clk.", flipFlopFigure, 300},
            {K::Table, "Clock|Mestre|Escravo|Q\nem 0|aberto: copia D|fechado: guarda|não muda\nsobe|fecha com o último D|abre: copia o "
             "mestre|recebe o D da borda\nem 1|fechado: guarda|aberto, mas o mestre não muda|não muda"},
            {K::Question, "O clock está parado em 1 e D muda de 0 para 1. O que Q faz?\nVira 1 na hora\n*Nada: só a próxima borda de "
             "subida leva o novo D\nVira 1 quando o clock descer\n=Com o clock em 1 o mestre está fechado: o D novo nem "
             "entra. Ele entra no mestre quando o clock descer e chega em Q na próxima subida.\n?Com o clock em 1, quem está "
             "fechado: o mestre ou o escravo?"},
            {K::Page, "Seguindo no tempo"},
            {K::Example, "Q, borda a borda\nQ começa em 0. Clock em 0, D vai a 1: o mestre copia o 1, mas o escravo está fechado. Q = "
             "0.\nO clock sobe: o mestre fecha com o 1, o escravo abre e copia. Q = 1.\nCom o clock ainda em 1, D vai a 0: o "
             "mestre está fechado, nada muda. Q = 1.\nO clock desce: o mestre abre e pega o 0. O escravo fecha. Q = 1.\nO "
             "clock sobe de novo: o 0 do mestre passa. Q = 0."},
            {K::Mistake, "Confundir latch com flip-flop\nO latch guarda pelo nível: enquanto En está em 1, deixa passar. O flip-flop "
             "guarda pela borda: só no instante da subida. Nos esquemas, o flip-flop tem um triângulo pequeno na entrada do "
             "clock."},
            {K::Mistake, "Mudar D bem na hora da borda\nNum circuito de verdade, D precisa estar parado um pouquinho antes da borda e um "
             "pouquinho depois. Se mudar bem em cima, o flip-flop pode guardar qualquer um dos dois. Por isso o clock dá "
             "tempo para os sinais se acomodarem antes de subir."},
            {K::Page, "Outro flip-flop: o J-K"},
            {K::Text, "Além do D, os livros mostram o flip-flop J-K. Na borda do clock, J = 1 liga, K = 1 desliga, os dois em 0 "
             "mantêm, e os dois em 1, que no latch SR era proibido, aqui têm uso: a saída troca, vira o contrário do que "
             "era."},
            {K::Table, "J|K|Q depois da borda\n0|0|o mesmo: guarda\n0|1|0\n1|0|1\n1|1|o contrário: troca"},
            {K::Text, "Um J-K com J e K presos em 1 troca a cada borda: a saída faz um pulso a cada dois do clock. É um divisor de "
             "frequência, e uma fila deles conta em binário."},
            {K::Question, "Um J-K com J = 1 e K = 1 está com Q = 0. Depois de três bordas de subida, quanto vale Q?\n0\n*1\nDepende de "
             "D\n=Cada borda troca: 0, 1, 0, 1. Depois de três, Q = 1.\n?Com J e K em 1, cada borda troca a saída."},
            {K::Summary, "O flip-flop D copia D na borda de subida do clock, e só nela.\nPor dentro: dois latches D, o mestre (clock "
             "invertido) e o escravo (clock direto).\nOs dois nunca ficam abertos juntos, e nada atravessa o flip-flop fora "
             "da borda.\nLatch: guarda pelo nível. Flip-flop: guarda pela borda."},
        },
        {"flip-flop", "flip-flop-latches"},
    });
    all.push_back(Topic{
        "registrador",
        5,
        "O registrador",
        "Vários flip-flops guardando um número",
        {
            {K::Page, "Um flip-flop para cada bit"},
            {K::Text, "Um flip-flop guarda um bit. Para guardar um número de 4 bits, ponha quatro flip-flops lado a lado, cada um com "
             "o seu bit do dado, e todos no mesmo clock. Na borda de subida, os quatro guardam juntos: o número inteiro "
             "entra de uma vez. Isso é um registrador."},
            {K::Question, "Quantos flip-flops tem um registrador de 16 bits, como os R0 a R7 da CPU?\n4\n8\n*16\n=Um flip-flop por bit: "
             "16 bits, 16 flip-flops, todos no mesmo clock.\n?Cada flip-flop guarda quantos bits?"},
            {K::Page, "Gravar ou manter"},
            {K::Text, "Um registrador de verdade não pode guardar o dado a cada borda: a CPU tem oito deles, e numa instrução só um "
             "muda. Por isso ele tem a entrada Grava (En). Na frente de cada flip-flop, um multiplexador escolhe: com Grava "
             "em 1, passa o bit novo do dado; com Grava em 0, devolve ao flip-flop o próprio Q, e na borda ele guarda o que "
             "já tinha."},
            {K::Figure, "Um registrador de 4 bits. Escolha um Dado, ligue Grava e clique no Clk duas vezes (sobe e desce).", registerFigure, 240},
            {K::Table, "Grava|O que entra no flip-flop|Q depois da borda\n0|o próprio Q|não muda\n1|o Dado|o Dado"},
            {K::Question, "O registrador tem Q = 5. Você põe Dado = 9 com Grava = 0 e dá uma borda. Quanto fica Q?\n*5\n9\n0\n=Com Grava "
             "em 0, cada multiplexador devolve o próprio bit ao flip-flop: a borda guarda o 5 de novo.\n?Olhe a linha Grava "
             "= 0 da tabela."},
            {K::Page, "Na CPU"},
            {K::Example, "ADD R3, R1, R2, do ponto de vista de R3\nDurante a instrução, a ULA calcula R1 + R2, e o resultado chega no "
             "Dado de R3.\nO controle liga o Grava de R3, e só o dele. Os outros sete ficam com Grava em 0.\nNa borda de "
             "subida, R3 guarda a soma. Os outros guardam o que já tinham.\nA próxima instrução já encontra o valor novo em "
             "R3."},
            {K::Mistake, "Mudar o Dado com Grava ligado e esperar que Q mude na hora\nNão muda: o registrador só olha o Dado na borda de "
             "subida. Até lá, Q mostra o valor antigo, mexa o Dado como mexer."},
            {K::Page, "O registrador de deslocamento"},
            {K::Text, "Há registradores que, em vez de gravar todos os bits de uma vez, empurram os bits uma casa a cada borda: a "
             "saída de cada flip-flop é a entrada do vizinho. Um bit que entra por uma ponta sai pela outra depois de "
             "tantas bordas quantos forem os flip-flops."},
            {K::Text, "É assim que um número viaja por um fio só, um bit de cada vez, numa porta serial ou num cabo USB: de um lado "
             "um registrador de deslocamento põe os bits no fio, do outro outro registrador os junta de volta."},
            {K::Question, "Um registrador de deslocamento de 4 bits começa com 0000 e recebe 1, 0, 1, 1, um bit por borda, pela "
             "esquerda. O que ele guarda no fim?\n1011\n*1101\n0000\n=O último a entrar fica na ponta esquerda: 1, depois "
             "01, 101, 1101.\n?Cada bit novo entra na ponta e empurra os outros."},
            {K::Summary, "Registrador: um flip-flop por bit, todos no mesmo clock.\nA entrada Grava (En) decide: guardar o Dado novo ou "
             "manter o valor.\nPor dentro, um multiplexador na frente de cada flip-flop faz essa escolha.\nOs registradores "
             "R0 a R7 da CPU são assim, com 16 bits cada."},
        },
        {"registrador", "registrador-flip-flops"},
    });
    all.push_back(Topic{
        "contador",
        5,
        "O contador",
        "Um registrador que soma 1 a cada pulso",
        {
            {K::Page, "Um registrador que soma 1"},
            {K::Text, "Ligue a saída Q de um registrador num somador que soma 1, e a saída do somador de volta no Dado do "
             "registrador. A cada borda do clock, o registrador guarda o próprio valor mais 1: é um contador. Como é um "
             "flip-flop, e não um latch, ele soma só uma vez por borda, sem disparar."},
            {K::Figure, "Um contador de 4 bits: um registrador e um somador. Ligue Conta e clique no Clk: cada subida soma 1.", counterFigure, 250},
            {K::Question, "Um contador de 4 bits está em 15. Com Conta ligado, vem uma borda. Quanto ele passa a "
             "mostrar?\n16\n*0\n15\n=15 + 1 = 16, que precisa de 5 bits: 10000. O registrador só tem 4, e guarda 0000. O "
             "contador dá a volta, como um hodômetro.\n?Qual é o maior número que cabe em 4 bits? E o que sobra de 16 nesses "
             "4 bits?"},
            {K::Page, "O contador de programa"},
            {K::Text, "O PC, o contador de programa da CPU, é um contador assim: guarda o endereço da instrução atual e, a cada "
             "instrução, avança para o seguinte. A diferença é que um desvio ou um salto pode mandar um valor no lugar do PC "
             "+ 1: um multiplexador escolhe o que entra no registrador."},
            {K::Example, "Um contador de 3 bits\nComeça em 000 = 0. Cada borda soma 1: 001, 010, 011, 100, 101, 110, 111.\nEm 111 = 7, a "
             "próxima borda dá 1000, mas só cabem 3 bits: volta a 000.\nCom 3 bits ele conta de 0 a 7, oito valores, e "
             "recomeça."},
            {K::Mistake, "Achar que o contador conta sozinho, o tempo todo\nEle só muda na borda do clock, e só com Conta ligado. Parado "
             "o clock, parado o contador: é por isso que o computador do jogo só anda quando você aperta Rodar ou Passo."},
            {K::Page, "Contadores com e sem compasso"},
            {K::Text, "O contador que você montou é síncrono: todos os flip-flops recebem o mesmo clock e mudam juntos. Há também o "
             "contador assíncrono, ou em cascata: cada flip-flop troca quando o anterior cai de 1 para 0, como dominós. Ele "
             "é mais simples, mas a mudança se espalha aos poucos, e por um instante a contagem passa por valores errados."},
            {K::Text, "Por isso a CPU usa contadores síncronos: no fim de cada ciclo todos os bits estão certos ao mesmo tempo, e o "
             "PC nunca aponta, nem por um instante, para um endereço que não devia."},
            {K::Question, "Num contador assíncrono de 3 bits que vai de 011 para 100, o que pode aparecer por um instante?\nSó 011 e "
             "100\n*Valores como 010 ou 000 no meio do caminho\nNada: ele muda tudo junto\n=Os bits trocam um depois do "
             "outro: 011, 010, 000 e só então 100.\n?No assíncrono, cada bit espera o anterior cair."},
            {K::Summary, "Contador: registrador + somador de 1, com a saída voltando para a entrada.\nSoma 1 a cada borda de subida, se "
             "Conta (En) estiver ligado.\nCom n bits, conta de 0 até 2^n - 1 e dá a volta.\nO PC da CPU é um contador que "
             "também pode receber o endereço de um desvio."},
        },
        {"contador"},
    });
    all.push_back(Topic{
        "ram-e-rom",
        5,
        "RAM e ROM",
        "Muitas palavras, um endereço para cada",
        {
            {K::Page, "Palavras e endereços"},
            {K::Text, "Uma memória é uma fileira de registradores numerados. Cada registrador guarda uma palavra, um número com um "
             "certo número de bits, e o número dele é o endereço. Para ler ou gravar, você diz o endereço: a memória escolhe "
             "aquela palavra."},
            {K::Table, "Endereço|Palavra (4 bits)\n0|0101\n1|0000\n2|1001\n3|0111"},
            {K::Text, "Com n bits de endereço há 2^n endereços: 1 bit escolhe entre 2 palavras, 2 bits entre 4, 3 bits entre 8. Cada "
             "bit a mais dobra o tamanho da memória."},
            {K::Question, "Uma memória com 4 bits de endereço tem quantas palavras?\n4\n8\n*16\n=2 × 2 × 2 × 2 = 16: os endereços vão de "
             "0 a 15.\n?Cada bit de endereço dobra o número de palavras. Comece de 1 e dobre 4 vezes."},
            {K::Mistake, "Confundir o endereço com o conteúdo\nO endereço é onde está; o conteúdo é o que está lá. No endereço 3 pode "
             "estar o número 7. Um não tem nada a ver com o outro, como o número de uma casa e quem mora nela."},
            {K::Page, "Como a memória escolhe a palavra"},
            {K::Text, "Para gravar, um decodificador, aquele da aula de seleção, transforma o endereço numa única linha ligada: só o "
             "registrador daquele endereço recebe a permissão de gravar. Para ler, um multiplexador, também já visto, "
             "escolhe a saída do registrador daquele endereço e a entrega em Q."},
            {K::Page, "Ler e gravar na RAM"},
            {K::Text, "A RAM (memória de acesso aleatório) lê e grava. A leitura é na hora: mude o endereço A, e Q já mostra a "
             "palavra dele. A gravação é como num registrador: com We (write enable, permissão de gravar) em 1, a borda de "
             "subida do clock guarda D no endereço A."},
            {K::Figure, "Uma RAM de 8 palavras de 4 bits. Escolha A e D, ligue We e clique no Clk; depois mude A e volte para ler.", ramFigure, 260},
            {K::Question, "Com We em 0, você dá uma borda no clock. A palavra do endereço A muda?\n*Não: sem a permissão de gravar, a "
             "borda não grava nada\nSim, recebe o D\nSim, vira 0\n=Com We em 0 nenhuma linha recebe a permissão de gravar, e "
             "todas as palavras guardam o que tinham.\n?We é a permissão de gravar. Sem ela..."},
            {K::Page, "A ROM"},
            {K::Text, "A ROM (memória só de leitura) tem o conteúdo gravado quando é construída, e depois só lê. É onde fica o "
             "programa do computador do jogo: cada endereço guarda uma instrução, e o PC é o endereço que se lê."},
            {K::Table, "Memória|Lê|Grava|Desligou, perdeu?\nRAM|sim|sim|perde\nROM|sim|não|guarda"},
            {K::Page, "SRAM, DRAM e as ROMs"},
            {K::Text, "Há dois jeitos de fazer RAM. A SRAM guarda cada bit num flip-flop, como os que você montou: é rápida e não "
             "precisa de cuidado, mas cada bit gasta vários transistores. A DRAM guarda cada bit como carga num capacitor "
             "minúsculo, com um transistor só: cabe muito mais num chip, mas a carga escapa, e a memória precisa ser relida "
             "e regravada, renovada, milhares de vezes por segundo."},
            {K::Table, "Tipo|Como guarda|Onde se usa\nSRAM|um flip-flop por bit|o cache, perto da CPU\nDRAM|carga num capacitor, "
             "renovada|a memória principal\nROM|gravada na fábrica|programas fixos\nPROM, EPROM|gravada uma vez, ou apagada "
             "com luz UV|aparelhos antigos\nEEPROM, flash|apagada e regravada eletricamente|pen drives, SSDs, celulares"},
            {K::Text, "RAM e ROM também diferem no que acontece ao desligar: a RAM é volátil e perde tudo; as ROMs, inclusive a "
             "flash, guardam."},
            {K::Question, "Por que a memória principal de um computador é DRAM, e não SRAM?\nPorque a DRAM é mais rápida\n*Porque cabem "
             "muito mais bits por chip, a um preço menor\nPorque a DRAM não perde nada ao desligar\n=Um transistor e um "
             "capacitor por bit contra vários transistores: a DRAM é densa e barata. A SRAM, mais rápida, fica para o "
             "cache.\n?Compare quantos transistores cada uma gasta por bit."},
            {K::Summary, "Memória: muitas palavras, cada uma num endereço.\nn bits de endereço: 2^n palavras.\nUm decodificador escolhe "
             "quem grava; um multiplexador escolhe o que se lê.\nRAM lê na hora e grava na borda, com We em 1. ROM só lê, e "
             "guarda o programa."},
        },
        {"ram-perguntas", "ram"},
    });
    all.push_back(Topic{
        "banco-de-registradores",
        5,
        "O banco de registradores",
        "Ler dois e gravar um, no mesmo ciclo",
        {
            {K::Page, "Oito registradores numa peça"},
            {K::Text, "Uma instrução como ADD R3, R1, R2 lê dois registradores e grava um, tudo na mesma instrução. A peça que faz "
             "isso é o banco de registradores: uma pequena memória com duas saídas de leitura e uma entrada de gravação."},
            {K::Figure, "Um banco de 8 registradores de 4 bits. Ra e Rb escolhem o que sai em A e B; Rw, D e We gravam no clique do "
             "Clk.", registerFileFigure, 300},
            {K::Table, "Pino|Para que serve\nRa|qual registrador sai em A\nRb|qual registrador sai em B\nRw|qual registrador é "
             "gravado\nD|o valor que vai ser gravado\nWe|permissão de gravar\nClk|grava na borda de subida"},
            {K::Page, "Uma instrução, três registradores"},
            {K::Question, "Em ADD R3, R1, R2, que números vão em Ra, Rb e Rw?\n*Ra = 1, Rb = 2, Rw = 3\nRa = 3, Rb = 1, Rw = 2\nRa = 1, "
             "Rb = 3, Rw = 2\n=R1 e R2 são lidos, um em cada saída; R3 recebe a soma, então é o registrador gravado.\n?Quem "
             "é lido e quem recebe o resultado?"},
            {K::Example, "Ler e gravar o mesmo registrador\nADD R1, R1, R2 lê R1 e grava R1. Durante a instrução, A mostra o R1 antigo, "
             "e a ULA soma com ele.\nO resultado chega no D, com Rw = 1 e We = 1.\nSó na borda de subida R1 recebe o valor "
             "novo. A leitura nunca vê o meio do caminho."},
            {K::Text, "No banco do computador do jogo, o R0 é especial: gravar nele não faz nada, e ele vale sempre 0. Isso é útil: "
             "LI R1, 5 vira, por dentro, 0 + 5. No Raio-X, as saídas A e B do banco são as portas A e B que você vê levando "
             "os valores para a ULA."},
            {K::Mistake, "Achar que dá para gravar dois registradores numa instrução\nO banco tem só uma entrada de gravação: Rw, D e "
             "We. Por isso cada instrução da CPU muda no máximo um registrador."},
            {K::Page, "Registradores que o programa não vê"},
            {K::Text, "Os registradores do banco, R0 a R7, são os que o programa usa pelo nome: registradores visíveis. A CPU tem "
             "outros que nenhuma instrução cita, mas sem os quais nada anda. São os registradores de controle."},
            {K::Table, "Registrador|Guarda\nPC, contador de programa|o endereço da próxima instrução\nIR, registrador de instrução|a "
             "instrução que está sendo executada\nMAR, endereço de memória|o endereço que vai para a memória\nMBR, dado de "
             "memória|a palavra que vem da memória ou vai para ela"},
            {K::Question, "Na busca, a CPU lê a palavra da ROM. Onde ela fica enquanto a instrução é executada?\nNo PC\n*No IR, o "
             "registrador de instrução\nEm R1\n=O IR guarda a instrução atual, para o decodificador ler os seus campos.\n?O "
             "PC guarda o endereço, não a instrução."},
            {K::Summary, "Banco de registradores: vários registradores numa peça, com duas leituras e uma gravação.\nRa e Rb leem na "
             "hora; Rw, D e We gravam na borda de subida.\nNuma instrução, os dois lidos e o gravado podem até ser o mesmo: "
             "a leitura vê o valor antigo.\nNo computador do jogo, R0 vale sempre 0."},
        },
        {"banco"},
    });

    // Act 7: machine code.
    all.push_back(Topic{
        "a-cpu-por-dentro",
        6,
        "A CPU por dentro",
        "As peças do computador de 16 bits e o ciclo de cada instrução",
        {
            {K::Page, "Um programa é uma lista de números"},
            {K::Text, "Tudo o que você montou até aqui se junta agora num computador. A ideia que torna isso possível é a do programa "
             "armazenado: as instruções não são fios, são números guardados numa memória. Para mudar o que o computador faz, "
             "basta mudar esses números. Cada instrução deste computador é uma palavra de 16 bits na ROM, a memória do "
             "programa."},
            {K::Figure, "O computador de 16 bits na placa: a CPU, a ROM com o programa, a RAM com os dados, as chaves de entrada e a "
             "saída. Dentro da CPU estão as peças dos atos anteriores.", computerBoard, 280},
            {K::Question, "Para o computador fazer outra coisa, o que precisa mudar?\nOs fios da CPU\n*Os números guardados na ROM\nO "
             "clock\n=É a ideia do programa armazenado: o programa são números numa memória, e a mesma CPU faz o que eles "
             "mandarem.\n?Onde fica o programa?"},
            {K::Page, "As peças da CPU"},
            {K::Table, "Peça|O que faz|Onde você viu\nPC (contador de programa)|guarda o endereço da instrução atual|Ato 6: o "
             "contador\nROM|guarda o programa, uma palavra por endereço|Ato 6: RAM e ROM\nDecodificador|corta a palavra em "
             "campos|Ato 4: o decodificador\nControle|liga os sinais de que cada instrução precisa|Ato 4: as "
             "portas\nRegistradores R0 a R7|guardam os números com que a CPU trabalha|Ato 6: o banco de "
             "registradores\nULA|faz as contas e as comparações|Ato 5: a ULA\nMUX|escolhem o valor que segue por cada "
             "caminho|Ato 4: o multiplexador\nRAM|guarda os dados do programa|Ato 6: RAM e ROM"},
            {K::Question, "Que peça guarda o endereço da instrução que está sendo executada?\n*O PC\nA ROM\nA ULA\n=O PC, o contador de "
             "programa, guarda esse endereço e o manda para a ROM.\n?É um contador."},
            {K::Page, "O ciclo de uma instrução"},
            {K::Text, "A CPU repete sempre os mesmos passos, uma instrução depois da outra. São as seis etapas que o Raio-X mostra:"},
            {K::Table, "Etapa|O que acontece\n1 Busca|o PC manda o endereço para a ROM, que devolve a palavra da instrução\n2 "
             "Decodifica|o decodificador separa os campos, e o controle decide quais sinais ligar\n3 Executa|a ULA faz a "
             "conta com os registradores ou com o número da instrução\n4 Memória|LD lê e ST grava na RAM; as outras "
             "instruções pulam esta etapa\n5 Escreve|o resultado volta para um registrador\n6 Próximo PC|o PC vai para a "
             "instrução seguinte, ou para onde um desvio mandar"},
            {K::Example, "LI R1, 7 pelas seis etapas\nBusca: o PC vale 0, e a ROM entrega a palavra do endereço 0, 0x2207.\nDecodifica: "
             "o op 2 diz LI; o controle liga a escrita em registrador e manda a ULA somar 0 + imediato.\nExecuta: a ULA faz "
             "0 + 7 = 7.\nMemória: LI não usa a RAM; nada acontece.\nEscreve: o 7 vai para R1.\nPróximo PC: o PC vai para 1, "
             "a instrução seguinte."},
            {K::Question, "Em que etapa a instrução LD lê a RAM?\nBusca\nExecuta\n*Memória\n=Na etapa Memória. Antes disso, em Executa, a "
             "ULA calcula o endereço que vai ser lido.\n?LD é a instrução que carrega um valor da memória de dados."},
            {K::Mistake, "Achar que cada etapa leva um ciclo de clock\nNeste computador, a instrução inteira leva um ciclo: as seis "
             "etapas acontecem de uma vez, como água correndo pelos canos, e o Raio-X só as separa para você acompanhar. "
             "Processadores maiores dividem as etapas numa linha de montagem, o pipeline."},
            {K::Page, "Toda instrução é um número"},
            {K::Text, "Os 4 bits de cima de cada palavra são o código da operação, o op: ele diz qual instrução é. Com 4 bits há 16 "
             "códigos, e este computador usa 14. O resto da palavra traz os detalhes: quais registradores e, às vezes, um "
             "número escrito na própria instrução, o imediato."},
            {K::Table, "op|Binário|Instrução\n0|0000|contas da ULA: ADD, SUB, "
             "AND...\n1|0001|ADDI\n2|0010|LI\n3|0011|LUI\n4|0100|LD\n5|0101|ST\n6|0110|BEQ\n7|0111|BNE\n8|1000|JMP\n9|1001|CALL\n10|1010|JR\n11|1011|OUT\n12|1100|IN\n15|1111|HLT"},
            {K::Word, "LI R1, 7: op 2 (LI), rd 1 (o registrador R1) e imm9 7 (o número). Em hexadecimal, 2207.", nullptr, 0, 0x2207},
            {K::Question, "Uma palavra começa com 1011. Que instrução é?\nLD\n*OUT\nHLT\n=1011 é 11, o op de OUT.\n?Leia os 4 bits como "
             "número e procure na tabela."},
            {K::Tip, "Linguagem de máquina são os números que a CPU executa. Assembly é a mesma coisa escrita com nomes, para "
             "pessoas lerem; o montador traduz um no outro."},
            {K::Page, "A ideia de von Neumann"},
            {K::Text, "Os primeiros computadores eram programados mudando fios de lugar. Em 1945, John von Neumann descreveu o "
             "programa armazenado: as instruções ficam na memória, como números, ao lado dos dados, e a CPU as busca uma a "
             "uma. Mudar o programa passou a ser só gravar outros números. Quase todo computador desde então segue essa "
             "arquitetura."},
            {K::Text, "Ela tem três partes: a CPU, que faz as contas e comanda; a memória, que guarda instruções e dados; e a "
             "entrada e saída, que conversa com o mundo. Ligando tudo, os barramentos: fios que levam endereços, dados e "
             "sinais de controle."},
            {K::Text, "O computador deste jogo guarda o programa numa ROM e os dados numa RAM, separadas: é a variante de Harvard, "
             "comum em microcontroladores. A ideia do programa armazenado é a mesma."},
            {K::Question, "O que o programa armazenado mudou?\nAs contas ficaram mais rápidas\n*Trocar de programa passou a ser gravar "
             "outros números na memória, sem mexer em fios\nA CPU passou a ter registradores\n=As instruções viraram dados "
             "na memória: um programa novo é só uma sequência nova de números.\n?Como se programava antes dele?"},
            {K::Summary, "Programa armazenado: as instruções são números de 16 bits na ROM.\nA CPU junta as peças dos atos anteriores: "
             "PC, decodificador, controle, registradores, ULA, MUX e memórias.\nCada instrução passa por seis etapas: busca, "
             "decodifica, executa, memória, escreve e próximo PC.\nOs 4 bits de cima são o op, que diz qual instrução é."},
        },
        {"cpu-perguntas", "binario-primeiro"},
    });

    all.push_back(Topic{
        "formatos-das-instrucoes",
        6,
        "Os formatos das instruções",
        "Campos, registradores, func, imediatos e números em hexadecimal",
        {
            {K::Page, "Quatro jeitos de dividir 16 bits"},
            {K::Text, "Toda instrução começa com o op, mas os outros 12 bits se dividem de jeitos diferentes, conforme o que a "
             "instrução precisa. Esses jeitos são os formatos. Ter poucos formatos deixa o decodificador simples: cada campo "
             "está sempre no mesmo lugar."},
            {K::Table, "Formato|Campos|Instruções\nR: três registradores|op(4) rd(3) rs(3) rt(3) func(3)|ADD, SUB, AND, OR, XOR, NOT, "
             "SHL, SHR\nI6: dois registradores e um número|op(4) rd(3) rs(3) imm6(6)|ADDI, LD, ST, BEQ, BNE\nI9: um "
             "registrador e um número|op(4) rd(3) imm9(9)|LI, LUI\nJ: um endereço|op(4) imm12(12)|JMP, CALL"},
            {K::Question, "Em que formato está LI R1, 7?\nR\nI6\n*I9\n=LI precisa de um registrador e de um número: formato I9, com 9 "
             "bits para o número.\n?Quantos registradores LI usa?"},
            {K::Page, "Os registradores e o func"},
            {K::Text, "Um campo de 3 bits escolhe um entre 8 registradores: 000 é R0, 111 é R7. rd é o destino, onde o resultado vai "
             "ficar; rs e rt são as fontes. Todas as contas entre registradores usam o mesmo op, 0000; quem diz qual conta "
             "fazer é o func, os 3 bits do fim, que vai direto para a ULA."},
            {K::Table, "func|Binário|Conta\n0|000|AND\n1|001|OR\n2|010|XOR\n3|011|ADD\n4|100|SUB\n5|101|NOT\n6|110|SHL\n7|111|SHR"},
            {K::Example, "Montando ADD R3, R1, R2 campo a campo\nop: as contas da ULA usam 0000.\nrd = 3: 011. rs = 1: 001. rt = 2: "
             "010.\nfunc de ADD = 3: 011.\nJuntando: 0000 011 001 010 011.\nDe quatro em quatro: 0000 0110 0101 0011, que é "
             "0x0653."},
            {K::Word, "ADD R3, R1, R2: op 0, rd 3, rs 1, rt 2 e func 3 (ADD). Em hexadecimal, 0653.", nullptr, 0, 0x0653},
            {K::Question, "Por que este computador tem exatamente 8 registradores?\nPorque a ULA só tem 8 entradas\n*Porque o campo do "
             "registrador tem 3 bits\nPorque 8 é o tamanho da ROM\n=3 bits escrevem 8 números, de 0 a 7: um para cada "
             "registrador.\n?Quantos números cabem em 3 bits?"},
            {K::Page, "Os imediatos"},
            {K::Text, "Um imediato é um número que vem dentro da instrução, com o tamanho que sobra: 6 bits no formato I6, 9 no I9 e "
             "12 no J. O imm6 tem sinal, em complemento de dois, e vai de -32 a 31; o imm9 vai de 0 a 511. Antes de chegar à "
             "ULA, o extensor completa o imediato até 16 bits. Num número negativo, ele repete o bit de sinal nos bits de "
             "cima, para que -1 continue sendo -1."},
            {K::Word, "ADDI R1, R1, -1: o -1 em 6 bits é 111111. Em hexadecimal, 127F.", nullptr, 0, 0x127F},
            {K::Mistake, "Completar um imm6 negativo com zeros\n111111 com zeros na frente vira 0000000000111111, que é 63, não -1. O "
             "extensor repete o bit de sinal: 1111111111111111, -1."},
            {K::Question, "LI R1, 600 cabe numa instrução?\nSim, o imm9 vai até 999\n*Não: o imm9 vai só até 511\nSim, se o número for "
             "escrito em hexadecimal\n=9 bits guardam de 0 a 511. Para 600 é preciso LUI e depois uma soma.\n?Qual é o maior "
             "número de 9 bits?"},
            {K::Page, "Hexadecimal"},
            {K::Text, "Escrever 16 bits cansa. Em hexadecimal, cada grupo de quatro bits vira um dígito de 0 a F, e a palavra inteira "
             "cabe em quatro dígitos: 0010 0010 0000 0111 vira 2207. No assembly do jogo, 0x indica hexadecimal e 0b indica "
             "binário: 0x2207 e 0b0010_001_000000111 são o mesmo número, e o _ só separa os campos para ler melhor."},
            {K::Question, "Em 0x127F, qual é a instrução?\n*ADDI: o primeiro dígito, 1, é o op\nLI: o 2 é o op\nHLT: o F é o op\n=O "
             "primeiro dígito hexadecimal são os 4 bits de cima, o op: 1 é ADDI.\n?O op são os 4 bits de cima: um dígito só."},
            {K::Page, "Onde a instrução busca os valores"},
            {K::Text, "Toda instrução diz o que fazer (o op), onde estão os valores de entrada, onde pôr o resultado e, às vezes, "
             "qual é a próxima instrução. O jeito de dizer onde está um valor se chama modo de endereçamento, e cada um tem "
             "o seu uso."},
            {K::Table, "Modo|O valor está...|Neste computador\nImediato|dentro da própria instrução|LI R1, 5\nRegistrador|num "
             "registrador|ADD R1, R2, R3\nDeslocamento|na memória, em registrador mais n|LD R1, 4(R2)\nIndireto por "
             "registrador|no endereço que o registrador guarda|JR R7\nRelativo ao PC|a uma distância da instrução atual|BEQ "
             "R1, R2, laço"},
            {K::Text, "O imediato é o mais rápido, mas o número tem de caber na instrução. O registrador também é rápido e o campo é "
             "curto. O deslocamento alcança a memória toda, ao custo de uma ida até ela. Por isso os processadores oferecem "
             "vários modos."},
            {K::Question, "Em LD R1, 4(R2), com R2 = 10, de qual endereço da RAM vem o valor?\n4\n10\n*14\n=Deslocamento: o endereço é "
             "R2 + 4 = 14.\n?Some o n ao registrador entre parênteses."},
            {K::Summary, "Quatro formatos: R (três registradores e func), I6, I9 e J.\nrd é o destino, rs e rt as fontes; 3 bits "
             "escolhem um de 8 registradores.\nO func diz qual conta a ULA faz nas instruções com op 0.\nO imm6 tem sinal; o "
             "extensor repete o bit de sinal até 16 bits.\nEm hexadecimal, um dígito para cada 4 bits: o primeiro dígito é o "
             "op."},
        },
        {"campos-perguntas", "binario-soma"},
    });

    all.push_back(Topic{
        "desvios-e-montador",
        6,
        "Desvios, saltos e o montador",
        "Distâncias, endereços, sub-rotinas e o programa que traduz o assembly",
        {
            {K::Page, "Desvios guardam distâncias"},
            {K::Text, "BEQ e BNE comparam dois registradores e, se a condição valer, desviam. O imm6 deles não é o endereço de "
             "destino, e sim a distância até ele, contada a partir da instrução seguinte: destino = PC + 1 + imm6. Um desvio "
             "para trás tem distância negativa. Guardar a distância, e não o endereço, faz o desvio caber em 6 bits e deixa "
             "o programa funcionar em qualquer lugar da ROM."},
            {K::Example, "O imm6 de um BNE que volta três instruções\nO BNE está no endereço 4, e o laço começa no endereço 2.\nA "
             "instrução seguinte ao BNE é a 5: a conta parte daí.\nDe 5 para 2 são 3 para trás: imm6 = -3.\nEm 6 bits, -3 é "
             "111101. Confere: 4 + 1 + (-3) = 2."},
            {K::Word, "BNE R1, R0, laço, com o laço três instruções antes da seguinte: imm6 = -3, ou 111101. Em hexadecimal, 723D.", nullptr, 0, 0x723D},
            {K::Text, "No Raio-X, a conta aparece no somador Alvo do desvio: ele soma o PC+1 com o imm6 que vem do extensor. O sinal "
             "Z da ULA desce até o Controle, que o junta com o tipo de desvio e calcula o sinal Desvia, que escolhe o "
             "próximo PC."},
            {K::Question, "Um BEQ no endereço 3, com imm6 = 4, desvia para qual endereço?\n7\n*8\n4\n=3 + 1 + 4 = 8. A distância conta a "
             "partir da instrução seguinte.\n?Destino = PC + 1 + imm6."},
            {K::Mistake, "Contar a distância a partir do próprio desvio\nA conta parte da instrução seguinte, PC + 1, porque é ela que o "
             "PC já está apontando quando o desvio decide. Esquecer o + 1 manda o programa uma instrução para o lado errado."},
            {K::Page, "Saltos guardam endereços"},
            {K::Text, "JMP e CALL usam o formato J: 12 bits só para o endereço, de 0 a 4095, a ROM inteira. CALL ainda guarda em R7 o "
             "endereço da instrução seguinte, o PC+1, para que a sub-rotina possa voltar com JR R7. JR pega o endereço de um "
             "registrador, e por isso pode voltar para qualquer lugar."},
            {K::Word, "CALL 10: op 9 e imm12 10. Em hexadecimal, 900A.", nullptr, 0, 0x900A},
            {K::Question, "Um CALL 10 no endereço 4. Que valor fica em R7?\n4\n*5\n10\n=CALL guarda o endereço da instrução seguinte, 4 + "
             "1 = 5: é para onde JR R7 vai voltar.\n?R7 guarda para onde voltar."},
            {K::Page, "O montador"},
            {K::Text, "O montador é o programa que traduz assembly em linguagem de máquina. Ele lê o texto duas vezes. Na primeira, "
             "só conta as instruções, para saber o endereço de cada rótulo. Na segunda, monta cada palavra: procura o op do "
             "nome, põe os números dos registradores nos campos certos e, nos desvios, calcula a distância até o rótulo."},
            {K::Text, "Alguns nomes nem são instruções da CPU: são pseudo-instruções, que o montador troca por instruções de verdade. "
             "MOV R2, R1 vira ADD R2, R1, R0; NOP vira ADD R0, R0, R0; RET vira JR R7. E .word grava um número qualquer na "
             "ROM, sem tradução."},
            {K::Table, "Assembly|Endereço|Palavra\nLI R1, 3|0|0010 001 000000011\nlaço: OUT R1|1|1011 000 001 000000\nADDI R1, R1, "
             "-1|2|0001 001 001 111111\nBNE R1, R0, laço|3|0111 001 000 111101\nHLT|4|1111 000000000000"},
            {K::Question, "Por que o montador lê o programa duas vezes?\nPara conferir se não há erros de digitação\n*Porque um desvio "
             "pode apontar para um rótulo que só aparece mais adiante\nPorque a ROM grava metade da palavra de cada vez\n=Na "
             "primeira passada ele descobre o endereço de todos os rótulos; só na segunda dá para calcular as "
             "distâncias.\n?Pense num desvio para a frente."},
            {K::Tip, "Rótulos viram endereços, nomes viram op e func, registradores viram 3 bits. O montador só faz a conta que você "
             "vai fazer à mão."},
            {K::Summary, "Desvios (BEQ, BNE) guardam a distância: destino = PC + 1 + imm6.\nSaltos (JMP, CALL) guardam o endereço em 12 "
             "bits; CALL guarda a volta em R7, e JR R7 volta.\nO montador lê duas vezes: primeiro os rótulos, depois as "
             "palavras.\nPseudo-instruções como MOV e RET viram instruções de verdade."},
        },
        {"desvios-perguntas", "binario-laco"},
    });
    // Act 8: programming.
    all.push_back(Topic{
        "assembly",
        7,
        "Assembly: programar com nomes",
        "Registradores, instruções, o editor e o depurador",
        {
            {K::Page, "Do binário para os nomes"},
            {K::Text, "No ato anterior você escreveu instruções bit a bit. Funciona, mas ninguém programa assim: um bit errado e o "
             "programa faz outra coisa. O assembly dá um nome a cada instrução e deixa o montador fazer a tradução. Cada "
             "linha de assembly vira exatamente uma palavra na ROM."},
            {K::Code, "; mostra 42 na saída\n        LI   R1, 42     ; R1 recebe 42\n        OUT  R1         ; a saída mostra R1\n   "
             "     HLT             ; o computador para"},
            {K::Question, "O programa acima tem três linhas de instrução. Quantas palavras ele ocupa na ROM?\n1\n*3\n16\n=Cada linha de "
             "instrução vira uma palavra de 16 bits. Os comentários, depois do ;, não viram nada.\n?Cada instrução é uma "
             "palavra."},
            {K::Page, "Registradores são as variáveis"},
            {K::Text, "A CPU só faz contas com o que está nos registradores, R0 a R7: oito caixinhas de 16 bits. R0 é especial: vale "
             "sempre 0, e o que se grava nele se perde. Isso é útil: ADD R2, R1, R0 copia R1 para R2, e BEQ R1, R0 testa se "
             "R1 é zero."},
            {K::Table, "Instrução|O que faz|Exemplo\nLI Rd, n|põe um número de 0 a 511 em Rd|LI R1, 300\nADD Rd, Rs, Rt|Rd = Rs + "
             "Rt|ADD R3, R1, R2\nSUB Rd, Rs, Rt|Rd = Rs - Rt|SUB R3, R1, R2\nADDI Rd, Rs, n|Rd = Rs + n, com n de -32 a "
             "31|ADDI R1, R1, 1\nOUT Rs|mostra Rs na saída|OUT R3\nHLT|para o computador|HLT"},
            {K::Example, "Seguindo um programa à mão\nLI R1, 7: R1 = 7.\nLI R2, 5: R2 = 5.\nSUB R3, R1, R2: R3 = 7 - 5 = 2.\nOUT R3: a "
             "saída mostra 2.\nHLT: o computador para. Anote o valor de cada registrador a cada linha: é o que o depurador "
             "mostra."},
            {K::Question, "O que ADD R2, R1, R0 faz?\nSoma R1 com ele mesmo\n*Copia R1 para R2\nZera R2\n=R0 vale sempre 0: R2 = R1 + 0 "
             "= R1. É assim que se copia um registrador.\n?Quanto vale R0?"},
            {K::Mistake, "Guardar um valor em R0\nLI R0, 9 não dá erro, mas o 9 se perde: R0 continua 0. Para guardar um número, use R1 "
             "a R7."},
            {K::Page, "Números maiores que 511"},
            {K::Text, "O LI só tem 9 bits para o número, então vai até 511. Para números maiores, a ULA ajuda: guarde dois pedaços e "
             "some. Outro jeito é o LUI, que põe n × 128 num registrador, seguido de um ADDI com a parte de baixo. A "
             "pseudo-instrução LIW faz essa conta sozinha."},
            {K::Question, "Como pôr 1000 em R1?\nLI R1, 1000\n*LI R1, 500 e depois ADD R1, R1, R1\nADDI R1, R0, 1000\n=LI vai até 511 e "
             "ADDI até 31; 500 + 500 = 1000 cabe.\n?Quais números cabem no LI e no ADDI?"},
            {K::Page, "Montar, rodar e depurar"},
            {K::Text, "No editor, Ctrl+Enter monta o programa e grava as palavras na ROM; os erros aparecem embaixo, com a linha. "
             "Para rodar, aperte Rodar, ou use o depurador do Painel: Instrução executa uma instrução por vez, e Etapa "
             "mostra cada uma das seis etapas no Raio-X. O botão Ajuda do editor lista todas as instruções."},
            {K::Mistake, "Esquecer o HLT no fim\nSem ele, o PC passa da última instrução e continua lendo a ROM, executando o que "
             "estiver lá. Todo programa termina com HLT, ou com um laço que não acaba de propósito."},
            {K::Tip, "Quando um programa não faz o que você quer, rode uma instrução por vez e olhe os registradores. O erro está "
             "no primeiro número diferente do que você esperava."},
            {K::Page, "Do texto ao programa rodando"},
            {K::Text, "Num computador grande, o caminho do texto até a execução tem mais etapas. O montador traduz cada arquivo para "
             "linguagem de máquina. O ligador junta os pedaços de vários arquivos e bibliotecas, acertando os endereços de "
             "um que o outro usa. E o carregador põe o programa pronto na memória e manda a CPU começar."},
            {K::Text, "Aqui o programa é um arquivo só e vai direto para a ROM, então o botão Montar e gravar faz o trabalho dos "
             "três de uma vez."},
            {K::Question, "Seu programa chama uma sub-rotina escrita em outro arquivo. Quem descobre o endereço dela?\nO montador\n*O "
             "ligador\nO carregador\n=O montador vê um arquivo de cada vez; é o ligador que junta os arquivos e acerta os "
             "endereços entre eles.\n?Qual deles junta vários arquivos?"},
            {K::Summary, "Assembly: um nome para cada instrução; cada linha vira uma palavra na ROM.\nAs contas são feitas nos "
             "registradores R1 a R7; R0 vale sempre 0.\nLI vai até 511; números maiores se montam com somas ou com LUI.\n"
             "Ctrl+Enter monta; o depurador roda uma instrução por vez."},
        },
        {"assembly-perguntas", "primeiro-programa", "conta-grande"},
    });

    all.push_back(Topic{
        "lacos-e-memoria",
        7,
        "Laços, decisões e memória",
        "Rótulos, desvios, contadores e a RAM",
        {
            {K::Page, "Rótulos e desvios"},
            {K::Text, "Um rótulo é um nome seguido de dois-pontos no começo de uma linha: ele marca o endereço daquela instrução. Os "
             "desvios usam rótulos como destino. BEQ Ra, Rb, lugar desvia se os dois forem iguais; BNE, se forem "
             "diferentes; JMP desvia sempre."},
            {K::Question, "BNE R1, R2, volta desvia quando...\nR1 e R2 são iguais\n*R1 e R2 são diferentes\nR1 é zero\n=BNE é branch if "
             "not equal: desvia se forem diferentes.\n?NE vem de not equal."},
            {K::Page, "O laço"},
            {K::Text, "Um laço repete instruções. O padrão quase sempre é o mesmo: um registrador conta, o corpo do laço faz o "
             "trabalho, a conta avança, e um desvio volta para o começo enquanto ela não chega ao fim."},
            {K::Code, "        LI   R1, 1        ; o contador começa em 1\n        LI   R2, 6        ; e para quando chegar a 6\n"
             "laço:   OUT  R1           ; o trabalho: mostrar o contador\n        ADDI R1, R1, 1    ; a conta avança\n      "
             "  BNE  R1, R2, laço ; volta enquanto R1 for diferente de 6\n        HLT"},
            {K::Example, "O laço volta por volta\nVolta 1: mostra 1, R1 vira 2. 2 é diferente de 6: volta.\nVoltas 2, 3 e 4: mostram 2, "
             "3 e 4, e R1 chega a 5.\nVolta 5: mostra 5, R1 vira 6. 6 é igual a 6: o BNE não desvia.\nO programa segue para "
             "o HLT. Mostrou 1, 2, 3, 4 e 5: cinco números."},
            {K::Question, "Trocando LI R2, 6 por LI R2, 4, quantos números o laço mostra?\n4\n*3\n2\n=Mostra 1, 2 e 3; quando R1 chega a "
             "4, o BNE para de voltar.\n?O laço para quando R1 fica igual a R2."},
            {K::Mistake, "Esquecer de avançar o contador\nSem o ADDI, R1 nunca muda, nunca chega a R2, e o laço não acaba: o programa "
             "fica mostrando o mesmo número para sempre."},
            {K::Page, "Decisões: se e senão"},
            {K::Text, "Não existe uma instrução se. Uma decisão é feita com um desvio que pula o trecho que não deve rodar. Para "
             "fazer \"se R1 = R2, mostre 1; senão, mostre 0\", desvie para o caso igual e, no fim do outro caso, pule por "
             "cima dele com JMP."},
            {K::Code, "        BEQ  R1, R2, igual\n        LI   R3, 0        ; senão: diferentes\n        JMP  fim\nigual:  LI   R3, "
             "1        ; se: iguais\nfim:    OUT  R3"},
            {K::Question, "Sem o JMP fim, com R1 diferente de R2, o que a saída mostraria?\n0\n*1\nNada\n=Depois do LI R3, 0 o programa "
             "cairia direto no LI R3, 1, e mostraria 1. O JMP pula o caso igual.\n?Siga as linhas uma depois da outra, sem "
             "o JMP."},
            {K::Page, "A memória"},
            {K::Text, "Oito registradores acabam rápido. A RAM guarda muito mais: cada endereço tem uma palavra de 16 bits. ST Rd, "
             "n(Rs) grava Rd no endereço Rs + n, e LD Rd, n(Rs) lê de volta. Usar um registrador como base permite andar "
             "pela memória: com R4 apontando para o começo de uma lista, LD R1, 0(R4) lê o primeiro item, e ADDI R4, R4, 1 "
             "passa para o seguinte."},
            {K::Question, "Com R4 = 10, em que endereço ST R1, 2(R4) grava?\n2\n10\n*12\n=O endereço é a base mais o deslocamento: 10 + "
             "2 = 12.\n?Endereço = Rs + n."},
            {K::Tip, "Registradores são rápidos e poucos; a memória é maior e precisa de LD e ST. Programas de verdade carregam da "
             "memória, calculam nos registradores e guardam de volta."},
            {K::Page, "Vetores: uma base e um índice"},
            {K::Text, "Um vetor, ou array, é uma fila de valores guardados em endereços seguidos. Para chegar ao elemento i, basta "
             "somar i ao endereço do primeiro, a base. É exatamente o que LD Rd, n(Rs) faz: com a base em n e o índice em "
             "Rs, ou o contrário."},
            {K::Example, "Somar o vetor que começa no endereço 20\nR1 é o índice, de 0 em diante, e R2 a soma.\nLD R3, 20(R1) lê o "
             "elemento R1: no endereço 20 + R1.\nADD R2, R2, R3 soma, e ADDI R1, R1, 1 passa ao próximo.\nO laço para "
             "quando R1 chega ao tamanho do vetor."},
            {K::Question, "Um vetor começa no endereço 30. Em que endereço está o elemento 4?\n4\n30\n*34\n=Base mais índice: 30 + 4 = "
             "34. O elemento 0 está no 30.\n?O primeiro elemento é o de índice 0."},
            {K::Summary, "Rótulos marcam endereços; BEQ, BNE e JMP desviam para eles.\nLaço: contador, corpo, avança, e um desvio que "
             "volta enquanto não acabou.\nDecisão: um desvio pula o trecho que não deve rodar, e um JMP pula o outro caso.\n"
             "LD e ST leem e gravam a RAM no endereço base + deslocamento."},
        },
        {"lacos-perguntas", "conte-ate-cinco", "guarde-na-memoria"},
    });

    all.push_back(Topic{
        "sub-rotinas",
        7,
        "Sub-rotinas",
        "CALL, JR, o endereço de volta e sub-rotinas que chamam outras",
        {
            {K::Page, "Ir e voltar"},
            {K::Text, "Quando o mesmo trecho aparece várias vezes, ele vira uma sub-rotina: escrito uma vez só, com um rótulo, e "
             "chamado de onde for preciso. É a mesma ideia das funções nas linguagens de programação."},
            {K::Text, "CALL rótulo faz duas coisas: guarda em R7 o endereço da instrução seguinte e pula para o rótulo. No fim da "
             "sub-rotina, JR R7 pula para o endereço guardado, e o programa continua logo depois do CALL. RET é outro nome "
             "para JR R7."},
            {K::Code, "        LI   R1, 3\n        CALL dobra        ; R7 = endereço do OUT; vai para dobra\n        OUT  R1         "
             "  ; mostra 6\n        HLT\ndobra:  ADD  R1, R1, R1   ; a sub-rotina\n        JR   R7           ; volta para o "
             "OUT"},
            {K::Example, "Seguindo a chamada\nEndereço 0, LI R1, 3: R1 = 3.\nEndereço 1, CALL dobra: R7 = 2, o endereço do OUT, e o PC "
             "vai para dobra, no endereço 4.\nEndereço 4, ADD R1, R1, R1: R1 = 6.\nEndereço 5, JR R7: o PC vai para 2.\n"
             "Endereço 2, OUT R1: mostra 6. Depois, o HLT."},
            {K::Question, "Se o programa fizesse CALL dobra duas vezes seguidas antes do OUT, o que a saída mostraria?\n6\n*12\n9\n=A "
             "primeira chamada dobra 3 para 6, a segunda dobra 6 para 12.\n?Cada chamada dobra o valor que está em R1."},
            {K::Mistake, "Esquecer o HLT antes das sub-rotinas\nSem ele, o programa termina o trabalho e cai dentro da sub-rotina sem "
             "ter sido chamado; o JR R7 então pula para um endereço velho."},
            {K::Page, "Combinados e chamadas dentro de chamadas"},
            {K::Text, "Para conversar, quem chama e a sub-rotina combinam onde ficam os números. Um costume comum: os argumentos vão "
             "em R1 e R2, e o resultado volta em R1. A sub-rotina pode usar outros registradores à vontade, desde que quem "
             "chama saiba que eles podem mudar."},
            {K::Text, "Se uma sub-rotina fizer CALL, o R7 recebe o novo endereço de volta, e o antigo se perde. Antes de chamar "
             "outra, ela precisa guardar R7 na memória e trazê-lo de volta depois. Os processadores organizam isso numa "
             "pilha: cada chamada empilha o seu endereço de volta, e cada retorno desempilha o último."},
            {K::Question, "Uma sub-rotina A chama outra, B, sem guardar R7. O que acontece quando A faz JR R7?\nVolta para quem chamou "
             "A, normalmente\n*Volta para dentro de A, logo depois do CALL B, e fica presa\nO computador para\n=O CALL B "
             "trocou o R7 pelo endereço de volta dentro de A. O JR R7 de A volta para ali, e não para quem chamou A.\n?O "
             "que o CALL B escreve em R7?"},
            {K::Page, "O quadro de cada chamada"},
            {K::Text, "Cada chamada de sub-rotina precisa de um pouco de memória só sua: o endereço de volta, os registradores que "
             "ela vai usar e precisa devolver como estavam, e as variáveis locais. Esse pedaço da pilha é o quadro da "
             "chamada (stack frame). Ao entrar, a sub-rotina o cria; ao sair, o desfaz, e a pilha volta a ser o que era."},
            {K::Text, "É por isso que uma sub-rotina pode chamar outra, e até a si mesma: cada chamada tem o seu quadro, empilhado "
             "sobre o anterior. Uma sub-rotina que chama a si mesma é recursiva."},
            {K::Question, "Uma sub-rotina A chama B, que chama C. Na hora em que C está rodando, quantos quadros há na pilha?\n1\n2\n*3\n"
             "=Um para cada chamada ainda aberta: A, B e C.\n?Nenhuma das três terminou ainda."},
            {K::Summary, "Sub-rotina: um trecho com rótulo, chamado de onde for preciso.\nCALL guarda em R7 o endereço da instrução "
             "seguinte e pula; JR R7 (RET) volta.\nCombine onde vão os argumentos e o resultado, como R1 e R2.\nAntes de "
             "chamar outra sub-rotina, guarde R7: é o começo da pilha.\nO HLT vai antes das sub-rotinas."},
        },
        {"sub-rotinas-perguntas", "sub-rotina"},
    });
    // Act 9: the operating system.
    all.push_back(Topic{
        "entrada-saida-espera",
        8,
        "Entrada, saída e espera",
        "As portas de entrada e saída, e como esperar pelo mundo",
        {
            {K::Page, "Conversar com o mundo"},
            {K::Text, "Um computador que só faz contas consigo mesmo não serve para muito. As portas de entrada e saída ligam a CPU "
             "ao mundo: IN Rd lê o número que as chaves formam, e OUT Rs mostra um número na saída. Em computadores de "
             "verdade, essas portas levam ao teclado, à tela, ao disco e à rede."},
            {K::Question, "De onde IN R1 tira o número que põe em R1?\nDa RAM\n*Das chaves de entrada\nDa ROM\n=IN lê a porta de "
             "entrada, que neste computador são as chaves.\n?IN vem de input, entrada."},
            {K::Page, "Esperar: a espera ocupada"},
            {K::Text, "O mundo é lento perto da CPU. Para esperar alguém mexer nas chaves, o programa pode ler a entrada sem parar "
             "até ela mudar. Isso se chama espera ocupada: funciona, mas a CPU passa o tempo todo só olhando."},
            {K::Code, "espera: IN   R1             ; lê as chaves\n        BEQ  R1, R0, espera ; enquanto forem 0, lê de novo\n      "
             "  OUT  R1             ; chegou um número: mostra"},
            {K::Example, "A espera, com as chaves valendo 0, 0, 0 e depois 9\nIN lê 0; BEQ vê R1 igual a R0 e volta.\nMais duas voltas "
             "iguais, lendo 0.\nAlguém liga as chaves: IN lê 9. BEQ vê 9 diferente de 0 e não volta.\nOUT mostra 9. O IN "
             "executou quatro vezes: três só para descobrir que nada tinha mudado."},
            {K::Question, "Se ninguém mexer nas chaves, o que o programa de espera faz?\nPara com um erro\n*Fica no laço para sempre, "
             "lendo 0\nMostra 0 e segue\n=Enquanto R1 for 0 o BEQ volta: a CPU fica presa olhando, sem fazer mais nada.\n"
             "?Quando o BEQ deixa de voltar?"},
            {K::Mistake, "Pôr o IN fora do laço\nSe o IN vem antes do rótulo, o laço testa sempre o mesmo valor antigo: nunca vê a "
             "chave mudar, e não sai mais. O IN tem que estar dentro, para ler de novo a cada volta."},
            {K::Page, "Interrupções"},
            {K::Text, "Os processadores de verdade têm outro jeito: a interrupção. O dispositivo avisa a CPU com um sinal elétrico "
             "quando tem algo novo; a CPU para o que estava fazendo, guarda onde estava, atende o pedido e volta. Assim ela "
             "não precisa ficar olhando. Este computador não tem interrupções, e por isso usa a espera ocupada."},
            {K::Question, "Qual é a vantagem da interrupção sobre a espera ocupada?\nA CPU lê o dispositivo mais vezes\n*A CPU pode "
             "fazer outra coisa até o dispositivo avisar\nO dispositivo fica mais rápido\n=Com a interrupção, quem avisa é "
             "o dispositivo; a CPU não gasta tempo perguntando.\n?Quem pergunta, e quem avisa?"},
            {K::Tip, "Espera ocupada: a CPU pergunta o tempo todo. Interrupção: o dispositivo avisa quando precisa."},
            {K::Page, "Três jeitos de fazer entrada e saída"},
            {K::Text, "Na E/S programada, a CPU pergunta ao dispositivo de novo e de novo se ele está pronto: é a espera ocupada, "
             "simples e cara. Na E/S por interrupção, a CPU segue trabalhando e o dispositivo avisa quando precisa dela. E "
             "no acesso direto à memória (DMA), um circuito à parte copia blocos inteiros entre o dispositivo e a memória, "
             "e a CPU só fica sabendo no fim."},
            {K::Table, "Jeito|Quem fica esperando|Bom para\nProgramada|a CPU, perguntando sem parar|coisas raras e simples\nPor "
             "interrupção|ninguém: o dispositivo avisa|teclado, mouse, rede\nDMA|ninguém: um circuito copia o bloco|disco, "
             "vídeo, blocos grandes"},
            {K::Question, "Para ler um arquivo grande do disco, qual jeito deixa a CPU mais livre?\nE/S programada\nPor interrupção, um "
             "byte de cada vez\n*DMA\n=Com DMA o bloco inteiro vai para a memória sem a CPU copiar byte a byte; ela recebe "
             "um aviso só, no fim.\n?Qual deles copia sem a CPU?"},
            {K::Summary, "IN lê as chaves; OUT mostra na saída: as portas ligam a CPU ao mundo.\nEspera ocupada: um laço que lê a "
             "entrada até ela mudar. O IN fica dentro do laço.\nInterrupção: o dispositivo avisa, e a CPU não precisa ficar "
             "olhando."},
        },
        {"espera-perguntas", "entrada-e-saida", "esperar-entrada"},
    });

    all.push_back(Topic{
        "sistema-operacional",
        8,
        "O sistema operacional",
        "Chamadas de sistema, tarefas e a troca entre elas",
        {
            {K::Page, "O programa que cuida dos outros"},
            {K::Text, "Quando vários programas dividem o mesmo computador, alguém precisa organizar: quem usa a tela, quem lê o "
             "teclado, quem roda agora. Esse alguém é o sistema operacional, um programa que fica sempre na máquina e "
             "atende os outros. Windows, Linux, Android e macOS são sistemas operacionais."},
            {K::Question, "O sistema operacional é...\nUma peça de hardware da CPU\n*Um programa que organiza e atende os outros "
             "programas\nO primeiro programa que o usuário escreve\n=É software: um programa como os outros, só que com a "
             "tarefa de cuidar da máquina e dos outros programas.\n?Ele roda na CPU como qualquer programa."},
            {K::Page, "Chamadas de sistema"},
            {K::Text, "Em vez de mexer direto na entrada e na saída, o programa pede ao sistema. Aqui, o pedido é assim: o número do "
             "serviço vai em R1, o dado em R2, e CALL sistema entra no sistema, que faz o serviço e volta com JR R7. Como "
             "só o sistema mexe nos dispositivos, ele pode conferir cada pedido e impedir que um programa atrapalhe o outro."},
            {K::Table, "R1|Serviço|R2\n1|mostrar na saída|o número a mostrar\n2|ler as chaves|volta com o número lido\n3|encerrar|não "
             "usa"},
            {K::Code, "        LI   R1, 1        ; serviço 1: mostrar\n        LI   R2, 42       ; o número\n        CALL sistema    "
             "  ; o sistema mostra 42 e volta"},
            {K::Example, "Pedindo para ler as chaves\nO programa põe 2 em R1: o serviço de ler as chaves.\nCALL sistema guarda em R7 "
             "onde voltar e entra no sistema.\nO sistema vê R1 = 2, faz IN e põe o número lido em R2.\nJR R7 volta para o "
             "programa, que encontra o número em R2."},
            {K::Question, "Para encerrar o programa pelo sistema, que número vai em R1?\n1\n2\n*3\n=O serviço 3 é encerrar; R2 nem é "
             "usado.\n?Olhe a tabela de serviços."},
            {K::Mistake, "Usar OUT ou IN direto num programa que roda sob o sistema\nFunciona na CPU, mas passa por cima de quem "
             "organiza a máquina: se outro programa estiver usando a saída, os dois se atropelam. Por isso, nos exercícios, "
             "o programa só pode pedir ao sistema."},
            {K::Page, "Várias tarefas numa CPU só"},
            {K::Text, "Uma CPU roda uma instrução por vez, mas pode alternar entre programas tão depressa que parece rodar todos "
             "juntos. Cada programa em andamento é uma tarefa. Para trocar de tarefa, o sistema guarda onde a atual parou e "
             "pula para onde a outra tinha parado. O que precisa ser guardado de uma tarefa é o seu contexto: o PC e os "
             "registradores que ela estava usando."},
            {K::Text, "Aqui as tarefas pedem a troca por conta própria, com uma chamada de sistema: é a multitarefa cooperativa. Nos "
             "sistemas de hoje, um relógio interrompe a tarefa de tempos em tempos e o sistema troca à força, mesmo que ela "
             "não peça: é a multitarefa preemptiva."},
            {K::Question, "Uma tarefa usa R1, R2 e R3 e para no meio. O que o sistema precisa guardar para ela continuar depois?\nSó o "
             "PC\nSó R1, R2 e R3\n*O PC e R1, R2 e R3\n=O contexto é onde ela parou, o PC, e os valores com que ela "
             "trabalhava. Faltando qualquer um, ela volta errada.\n?O contexto é tudo o que a tarefa precisa para continuar "
             "exatamente de onde parou."},
            {K::Mistake, "Esquecer um registrador ao trocar de tarefa\nSe o sistema não guarda um registrador que a tarefa usava, a "
             "outra tarefa o sobrescreve, e a primeira volta com um valor que não é dela. O erro aparece longe de onde "
             "nasceu, o que o torna difícil de achar."},
            {K::Tip, "Trocar de tarefa é guardar o contexto da que sai e restaurar o da que entra. Assim, cada tarefa continua "
             "exatamente de onde parou."},
            {K::Page, "Os estados de uma tarefa"},
            {K::Text, "Para revezar a CPU, o sistema operacional sabe em que estado está cada tarefa, ou processo. Pronta: podia "
             "rodar, e espera a vez. Rodando: está na CPU agora. Bloqueada: espera algo de fora, uma tecla ou um disco, e "
             "não adianta dar-lhe a CPU. Uma tarefa nova entra como pronta, e uma que terminou sai da lista."},
            {K::Table, "De|Para|Quando\nPronta|Rodando|o sistema escolhe a tarefa\nRodando|Pronta|a vez dela acaba\n"
             "Rodando|Bloqueada|ela pede E/S e precisa esperar\nBloqueada|Pronta|o que ela esperava chega"},
            {K::Question, "Uma tarefa pediu uma tecla e ninguém apertou nada ainda. Em que estado ela está?\nRodando\nPronta\n"
             "*Bloqueada\n=Ela espera algo de fora: até a tecla chegar, dar-lhe a CPU seria desperdício.\n?Adianta dar a "
             "CPU a quem espera uma tecla?"},
            {K::Summary, "O sistema operacional é o programa que organiza a máquina e atende os outros.\nChamada de sistema: serviço em "
             "R1, dado em R2, CALL sistema; o sistema volta com JR R7.\nTarefas se revezam na CPU; trocar é guardar e "
             "restaurar o contexto: o PC e os registradores.\nCooperativa: a tarefa pede a troca. Preemptiva: um relógio "
             "força a troca."},
        },
        {"sistema-perguntas", "chamadas-de-sistema", "varias-tarefas", "sistema-minimo"},
    });
    // Act 10: more parts.
    all.push_back(Topic{
        "paridade-mux-prioridade",
        9,
        "Paridade, multiplexador e prioridade",
        "Detectar erros, escolher um sinal e atender quem pede primeiro",
        {
            {K::Page, "Paridade: um bit que vigia os outros"},
            {K::Text, "Quando dados viajam por um fio ou ficam guardados muito tempo, um bit pode virar. A paridade detecta isso: "
             "junto com os dados vai um bit a mais, P, escolhido para que o total de 1s, contando o próprio P, seja par. É "
             "a paridade par, e com ela P vale 1 quando os dados têm uma quantidade ímpar de 1s. Na paridade ímpar, o total "
             "fica ímpar e P é o contrário. Na chegada, conta-se de novo: se o total não bater, houve erro."},
            {K::Text, "O circuito é uma fila de XOR, porque o XOR de vários bits dá 1 exatamente quando há uma quantidade ímpar de "
             "1: é o P da paridade par."},
            {K::Figure, "O bit da paridade par de 4 bits, feito com portas XOR em fila. Clique nos bits de D: o LED S acende quando a "
             "quantidade de 1 é ímpar, e D com S fica sempre com uma quantidade par.", parityGates, 220},
            {K::Example, "Mandando 1011 com paridade par\nHá três 1 em 1011: ímpar, então P = 1, e o total fica quatro, par. Vão 1011 e "
             "P = 1.\nNo caminho, o terceiro bit vira: chega 1001.\nNa chegada, 1001 e P = 1 têm três 1 ao todo: ímpar.\nO "
             "total devia ser par. Não bate: o receptor sabe que algo veio errado."},
            {K::Question, "Com paridade par, chegam os dados 0110 com P = 1. Houve erro?\n*Sim\nNão\nNão dá para saber\n=0110 e P = 1 "
             "têm três 1 ao todo, uma quantidade ímpar: na paridade par o total tinha de ser par. Algum bit virou.\n?Conte "
             "os 1 dos dados e do P juntos."},
            {K::Mistake, "Achar que a paridade pega qualquer erro\nSe dois bits viram, a quantidade de 1 muda duas vezes e volta a ter "
             "a mesma paridade: o erro passa sem ser notado. A paridade só garante pegar um bit trocado. Para mais, usam-se "
             "códigos maiores, com vários bits de verificação."},
            {K::Page, "O multiplexador de dois"},
            {K::Text, "O MUX de duas entradas é o mais usado de todos: S = 0 deixa passar A, e S = 1 deixa passar B. Em expressão, Y "
             "= (A E NÃO S) OU (B E S): de cada vez, só uma das duas E pode deixar passar."},
            {K::Table, "S|A saída Y\n0|A\n1|B"},
            {K::Question, "Com S = 1, A = 0 e B = 1, quanto vale Y?\n0\n*1\nDepende de A\n=S = 1 escolhe B, que vale 1. A não importa.\n"
             "?Qual entrada S = 1 escolhe?"},
            {K::Tip, "Para escolher números de 16 bits, usam-se 16 MUX de um bit lado a lado, todos com o mesmo S. Os MUX do "
             "Raio-X, que escolhem entre registrador e número ou entre ULA e RAM, são feitos assim."},
            {K::Page, "O codificador de prioridade"},
            {K::Text, "Quando vários pedem ao mesmo tempo, quem é atendido? O codificador de prioridade olha várias entradas e "
             "devolve o número da mais importante que está ligada, além de um sinal V dizendo se alguma pediu. É assim que "
             "um processador escolhe qual interrupção atender primeiro."},
            {K::Figure, "O codificador de prioridade de 4 entradas feito de portas: a entrada ligada de número mais alto vence. Ligue "
             "várias e veja quem ganha.", priorityGates, 260},
            {K::Example, "D1 e D2 pedem juntas\nDas ligadas, a de número mais alto é D2.\n2 em binário é 10: A1 = 1 e A0 = 0.\nAlguém "
             "pediu, então V = 1. D1 espera a vez."},
            {K::Question, "D0 e D3 estão ligadas. Que número sai em A1 A0?\n0\n1\n*3\n=D3 tem o número mais alto entre as ligadas: sai "
             "3, que é 11.\n?Vence a de número mais alto."},
            {K::Mistake, "Esquecer o V\nSó com D0 ligada, a saída é 00. Sem nada ligado, a saída também é 00. Sem o V, as duas "
             "situações ficam iguais: é ele que diz que há mesmo um pedido."},
            {K::Page, "Além de achar o erro: corrigi-lo"},
            {K::Text, "O bit de paridade avisa que um bit mudou, mas não diz qual, e não percebe quando dois mudam juntos. Memórias "
             "que não podem errar usam um código de Hamming: vários bits de verificação, cada um vigiando um grupo "
             "diferente de bits de dados. Os grupos se cruzam de modo que o padrão de verificações que falham aponta o bit "
             "errado, e basta invertê-lo."},
            {K::Table, "Bits de dados|Bits de verificação para corrigir 1 erro\n4|3\n8|4\n16|5\n32|6\n64|7"},
            {K::Text, "Com um bit de paridade a mais sobre tudo, o código também percebe dois erros, mesmo sem corrigi-los. É o que "
             "fazem as memórias ECC dos servidores."},
            {K::Question, "Com um bit de paridade, chegam dados com dois bits trocados. O que acontece?\nA paridade aponta os dois bits\n"
             "A paridade corrige os dois\n*A paridade não percebe nada\n=Dois bits trocados mantêm a quantidade de 1s par "
             "(ou ímpar): o erro passa despercebido.\n?Trocar dois bits muda a contagem de 1s em quanto?"},
            {K::Summary, "Paridade par: um bit a mais que deixa o total de 1s par; vale 1 quando os dados têm uma quantidade ímpar de "
             "1s. Na paridade ímpar, o total fica ímpar. Pega um bit trocado, não dois.\nMUX 2:1: S = 0 passa A, S = 1 "
             "passa B. Para N bits, N MUX com o mesmo S.\nCodificador de prioridade: o número da entrada ligada mais alta, "
             "e V avisa que alguma pediu."},
        },
        {"paridade-perguntas", "paridade", "multiplexador", "prioridade"},
    });

    all.push_back(Topic{
        "negativos-multiplicacao",
        9,
        "Negativos e multiplicação",
        "O complemento de dois na prática, e multiplicar somando",
        {
            {K::Page, "Trocar o sinal"},
            {K::Text, "No complemento de dois, −N é NÃO N + 1: inverta todos os bits e some 1. O circuito são inversores na frente "
             "de um somador que recebe 1 no vai-um de entrada."},
            {K::Figure, "O negador: inversores e um somador com 1 no vai-um de entrada.", negatorGates, 240},
            {K::Example, "−3 em 4 bits\n3 é 0011.\nInvertendo todos os bits: 1100.\nSomando 1: 1101, que sem sinal é 13.\nConferindo: 3 "
             "+ 13 = 16, que em 4 bits é 0000. Somar 3 com −3 dá 0, como deve."},
            {K::Question, "Como fica −2 em 4 bits?\n1101\n*1110\n0010\n=2 é 0010; invertendo, 1101; somando 1, 1110.\n?Inverta 0010 e "
             "some 1."},
            {K::Mistake, "Esquecer o + 1\nSó inverter os bits dá NÃO N, que é −N − 1: um a menos do que devia. Em 4 bits, NÃO 0011 é "
             "1100, que vale −4, não −3."},
            {K::Page, "Multiplicar é somar deslocado"},
            {K::Text, "Multiplicar em binário é como no papel, só que mais fácil: cada algarismo do multiplicador é 0 ou 1. Para "
             "cada bit 1 de B, soma-se A deslocado até a casa daquele bit; para cada 0, nada. Deslocar uma casa para a "
             "esquerda é multiplicar por 2. Assim, 5 × 6 = 5 × 2 + 5 × 4, porque os bits 1 e 2 de 6 (110) estão ligados."},
            {K::Table, "Bit de B (6 = 110)|Parcela\nbit 0 = 0|0\nbit 1 = 1|A deslocado 1 casa: 1010 (10)\nbit 2 = 1|A deslocado 2 "
             "casas: 10100 (20)\nsoma|11110 (30)"},
            {K::Example, "3 × 5\n5 é 101: os bits 0 e 2 estão ligados.\nBit 0: soma-se 3 sem deslocar, 11 (3).\nBit 2: soma-se 3 "
             "deslocado 2 casas, 1100 (12).\nTotal: 3 + 12 = 15, que é 1111."},
            {K::Question, "Quanto dá 7 × 4 somando deslocado?\n11\n*28\n14\n=4 é 100: só o bit 2. 7 deslocado 2 casas é 7 × 4 = 28.\n"
             "?Quais bits de 4 (100) estão ligados?"},
            {K::Mistake, "Deslocar para o lado errado\nPara a esquerda, cada casa multiplica por 2. Para a direita, divide por 2 e o "
             "bit de baixo se perde: 5 para a direita vira 2."},
            {K::Figure, "O multiplicador de 4 bits pronto. Na Galeria, a versão Portas e somadores mostra por dentro: portas E fazem "
             "as parcelas, e somadores juntam tudo.", multiplierGates, 220},
            {K::Tip, "A CPU deste jogo não tem instrução de multiplicar: um programa multiplica com SHL, ADD e um laço, do mesmo "
             "jeito que o circuito."},
            {K::Page, "O tamanho do produto, e o sinal"},
            {K::Text, "Multiplicar dois números de n bits pode dar um resultado de até 2n bits: 15 × 15 = 225 precisa de 8 bits, não "
             "de 4. Por isso os processadores guardam o produto em dois registradores, ou num registrador do dobro do "
             "tamanho."},
            {K::Text, "Com sinal, o somar e deslocar simples erra: ele trata o bit de sinal como um peso positivo. Um jeito é "
             "multiplicar os valores sem sinal e acertar o sinal no fim. Outro é o algoritmo de Booth, que olha os bits do "
             "multiplicador em pares e soma ou subtrai o multiplicando, funcionando direto em complemento de dois. O "
             "computador deste jogo não tem multiplicador: lá se multiplica somando num laço."},
            {K::Question, "Quantos bits podem ser precisos para o produto de dois números de 8 bits?\n8\n9\n*16\n=Até o dobro: 255 × 255 "
             "= 65 025, que precisa de 16 bits.\n?O maior produto é 255 × 255."},
            {K::Summary, "−N é NÃO N + 1: inverta e some 1. Sem o + 1, fica um a menos.\nMultiplicar: para cada bit 1 de B, some A "
             "deslocado até a casa daquele bit.\nDeslocar para a esquerda multiplica por 2; para a direita, divide."},
        },
        {"multiplicar-perguntas", "negativo", "multiplicar"},
    });
    all.push_back(Topic{
        "deslocar-e-estender",
        9,
        "Deslocar e estender",
        "Mover os bits de casa, e passar um número para mais bits",
        {
            {K::Page, "Deslocar é só mudar os fios"},
            {K::Text, "Deslocar uma casa para a esquerda é passar cada bit para a casa de cima e pôr um 0 embaixo: o número dobra, "
             "como acrescentar um zero no fim de um número decimal multiplica por 10. Para a direita, cada bit desce uma "
             "casa, o de baixo se perde e entra um 0 em cima: o número cai pela metade. No computador de 16 bits, SHL e SHR "
             "fazem isso, uma casa por vez."},
            {K::Figure, "Deslocar uma casa para a esquerda só com fios: um separador, um juntador com cada bit uma casa acima e um 0 "
             "embaixo. 5 (0101) vira 10 (1010). Clique nos bits de A.", shiftWiringFigure, 200},
            {K::Example, "Deslocando 13 para a esquerda, em 4 bits\n13 é 1101.\nCada bit sobe uma casa e entra 0 embaixo: 1010.\nO 1 de "
             "cima não tem para onde ir e se perde.\nO resultado é 10, não 26: 26 não cabe em 4 bits."},
            {K::Question, "Em 8 bits, quanto dá 00010110 (22) deslocado uma casa para a direita?\n*00001011 (11)\n00101100 (44)\n"
             "00010111 (23)\n=Cada bit desce uma casa e entra 0 em cima: 00001011, a metade de 22.\n?Para a direita, o "
             "número cai pela metade."},
            {K::Mistake, "Esperar que a direita arredonde\nO bit que sai pela direita se perde: 7 (0111) deslocado para a direita dá 3 "
             "(0011), não 3,5 nem 4. Deslocar para a direita é a divisão por 2 que joga fora o resto."},
            {K::Page, "O deslocador: várias casas de uma vez"},
            {K::Text, "Para deslocar N casas de uma vez, o Deslocador da biblioteca usa estágios: um que desloca 1 casa, outro 2, "
             "outro 4. Cada estágio é um MUX que escolhe entre o número como está e o número deslocado, e cada bit de N "
             "liga um estágio. Para 5 casas (101), agem o estágio de 1 e o de 4; o de 2 deixa passar. Com N de 3 bits, ele "
             "desloca de 0 a 7 casas, e Dir escolhe o lado."},
            {K::Figure, "O Deslocador de 8 bits, com A = 5 e N = 1. Clique nos bits de N e em Dir e veja Q.", shifterFigure, 220},
            {K::Example, "Deslocando 3 por 6 casas para a esquerda, em 8 bits\n6 é 110: agem os estágios de 2 e de 4, e o de 1 deixa "
             "passar.\nEstágio de 1: nada muda, fica 3.\nEstágio de 2: 3 vira 12 (00001100).\nEstágio de 4: 12 vira 192 "
             "(11000000). É 3 × 64, e 64 é 2 elevado a 6."},
            {K::Question, "Para deslocar 3 casas, quais estágios agem?\n*O de 1 e o de 2\nSó o de 4\nO de 1, o de 2 e o de 4\n=3 é 011: "
             "os bits 0 e 1 de N estão ligados, e eles comandam os estágios de 1 e de 2. 1 + 2 = 3.\n?Escreva 3 em binário."},
            {K::Tip, "Deslocar N casas para a esquerda multiplica por 2 elevado a N. Os computadores usam isso o tempo todo: no "
             "MIPS, para passar de número de palavra a endereço, multiplica-se por 4, que é deslocar 2 casas."},
            {K::Page, "O extensor: mais bits sem mudar o número"},
            {K::Text, "Às vezes um número de poucos bits tem de entrar num barramento maior: o imm6 de uma instrução vai para a ULA "
             "de 16 bits. Os bits novos, em cima, não podem ser quaisquer. Num número positivo, enchem-se de 0. Num "
             "negativo em complemento de dois, de 1: repete-se o bit de cima, o de sinal. É a extensão de sinal. O Extensor "
             "de bits faz isso com Sinal ligado; desligado, ele enche de 0."},
            {K::Table, "A (4 bits)|Valor|Com Sinal (8 bits)|Sem Sinal (8 bits)\n0101|5|00000101 (5)|00000101 (5)\n1101|−3|11111101 "
             "(−3)|00001101 (13)\n1000|−8|11111000 (−8)|00001000 (8)"},
            {K::Figure, "O Extensor de 4 para 8 bits, com A = 1101 (−3) e Sinal ligado: a saída é FD, 11111101. Desligue Sinal e ela "
             "vira 0D, 13.", extenderFigure, 200},
            {K::Example, "O imm6 −2 chegando à ULA de 16 bits\n−2 em 6 bits é 111110.\nO bit de cima, o de sinal, é 1.\nO extensor o "
             "repete nos 10 bits novos: 1111111111111110.\nEm hexadecimal, FFFE: o −2 de 16 bits. A ULA soma certo."},
            {K::Question, "Estendendo 1010 (4 bits, −6) para 8 bits, com sinal, o que sai?\n*11111010\n00001010\n10100000\n=O bit de "
             "sinal é 1, e ele se repete nos quatro bits novos: 11111010, que é −6 em 8 bits.\n?Repita o bit de cima."},
            {K::Mistake, "Encher de 0 um número negativo\n1101 (−3) com zeros em cima vira 00001101, que é 13: o sinal se perdeu. Por "
             "isso o extensor tem Sinal. Para números que nunca são negativos, como endereços, ele fica desligado."},
            {K::Page, "Deslocamento lógico, aritmético e rotação"},
            {K::Text, "Deslocar para a direita abre uma casa à esquerda. O deslocamento lógico a preenche com 0: certo para números "
             "sem sinal. O deslocamento aritmético copia o bit de sinal: assim um número negativo continua negativo, e "
             "deslocar uma casa divide por 2 também com sinal. Na rotação, o bit que sai de um lado entra pelo outro, e "
             "nada se perde."},
            {K::Example, "-8 deslocado uma casa para a direita, com 4 bits\n-8 é 1000.\nLógico: entra 0 à esquerda, 0100, que é 4. O "
             "sinal se perdeu.\nAritmético: entra uma cópia do bit de sinal, 1100, que é -4.\n-8 ÷ 2 = -4: o aritmético "
             "acertou."},
            {K::Question, "No MIPS, qual instrução divide um número com sinal por 2?\nsrl, deslocamento lógico\n*sra, deslocamento "
             "aritmético\nsll, para a esquerda\n=sra copia o bit de sinal ao deslocar para a direita: -6 vira -3.\n?Qual "
             "delas mantém o sinal?"},
            {K::Summary, "Deslocar para a esquerda multiplica por 2; para a direita, divide e joga fora o resto. Uma casa é só fio.\nO "
             "Deslocador faz N casas de uma vez, com um estágio de MUX por bit de N: 1, 2, 4 casas.\nEstender com sinal "
             "repete o bit de cima; sem sinal, enche de 0."},
        },
        {"deslocar-perguntas", "deslocar", "estender"},
    });
    // Act 11: input and output.
    all.push_back(Topic{
        "display-7-segmentos",
        10,
        "O display de 7 segmentos",
        "Desenhar números com sete luzes",
        {
            {K::Page, "Sete luzes formam um número"},
            {K::Text, "O display de 7 segmentos tem sete LEDs em forma de barra, chamados de a até g: a em cima, b e c à direita, d "
             "embaixo, e e f à esquerda, g no meio. Acendendo os certos, aparece qualquer algarismo de 0 a 9, e até as "
             "letras de A a F do hexadecimal. É o display de relógios, calculadoras e painéis."},
            {K::Table, "Número|Segmentos acesos\n0|a b c d e f\n1|b c\n2|a b d e g\n3|a b c d g\n4|b c f g\n5|a c d f g\n6|a c d e f "
             "g\n7|a b c\n8|todos\n9|a b c d f g"},
            {K::Example, "Desenhando o 4\nO 4 tem as duas barras da direita inteiras: b e c.\nEm cima, à esquerda, a perna do 4: f.\nO "
             "traço do meio liga as duas: g.\nFicam acesos b, c, f e g, e apagados a, d e e."},
            {K::Question, "Quais segmentos acendem para o 1?\n*b e c\ne e f\na, b e c\n=O 1 é só a barra da direita, de cima a baixo: b "
             "e c. Com a também, vira um 7.\n?b e c ficam à direita."},
            {K::Mistake, "Trocar os lados\nb e c ficam à direita; e e f, à esquerda. Trocando, o 1 aparece do lado errado e o 2 vira um "
             "5."},
            {K::Page, "Um decodificador para 7 segmentos"},
            {K::Text, "Cada segmento é uma função lógica do número de 4 bits: o segmento a, por exemplo, acende para 0, 2, 3, 5, 6, "
             "7, 8 e 9. São sete tabelas-verdade, uma por segmento, e cada uma vira um circuito de portas. Juntas, formam o "
             "decodificador para 7 segmentos."},
            {K::Figure, "O display com o decodificador: o número da entrada aparece desenhado.", display7, 260},
            {K::Example, "A tabela-verdade do segmento g\nOlhe a tabela de números e procure o g em cada linha.\nEle aparece no 2, 3, "
             "4, 5, 6, 8 e 9.\nFica apagado só no 0, no 1 e no 7.\nA tabela-verdade de g tem 1 nessas sete linhas e 0 nas "
             "outras três: é a função que o circuito de g calcula."},
            {K::Question, "Em quantos dos algarismos de 0 a 9 o segmento e acende?\n*4\n6\n8\n=e, embaixo à esquerda, acende no 0, 2, 6 "
             "e 8.\n?Procure o e em cada linha da tabela."},
            {K::Tip, "Um decodificador para 7 segmentos é só um conjunto de tabelas-verdade. Montar cada segmento é o mesmo "
             "trabalho do Ato das portas lógicas: achar quando ele vale 1 e juntar com E, OU e NÃO."},
            {K::Page, "Uma tabela vira uma ROM"},
            {K::Text, "O decodificador de 7 segmentos é só uma tabela: para cada número de 4 bits, quais segmentos acendem. Toda "
             "tabela assim pode ser feita com portas, mas também com uma ROM: o número entra como endereço, e a palavra "
             "guardada nele é a linha da tabela, um bit por segmento."},
            {K::Text, "A ROM troca o projeto de portas por uma lista de palavras: mudar o desenho de um dígito é só regravar uma "
             "palavra. Circuitos programáveis, como as PLAs e os FPGAs, levam essa ideia adiante."},
            {K::Question, "Uma ROM faz o decodificador de 7 segmentos para números de 4 bits. Quantas palavras ela precisa, e de quantos "
             "bits?\n4 palavras de 7 bits\n*16 palavras de 7 bits\n7 palavras de 16 bits\n=Uma palavra por número de "
             "entrada, 2 elevado a 4 = 16, e um bit por segmento, 7.\n?Quantos endereços 4 bits formam?"},
            {K::Summary, "Sete barras, a até g: a em cima, b e c à direita, d embaixo, e e f à esquerda, g no meio.\nCada algarismo "
             "acende um conjunto de segmentos.\nO decodificador tem uma tabela-verdade por segmento, feita de portas."},
        },
        {"display-perguntas", "sete-segmentos"},
    });

    all.push_back(Topic{
        "botoes-aleatorio",
        10,
        "Botões e números ao acaso",
        "Contar apertos e gerar números que parecem sorteados",
        {
            {K::Page, "Um botão é um pulso"},
            {K::Text, "O botão vale 1 só enquanto está apertado. Ligado no clock de um contador, cada aperto vira uma borda de "
             "subida, e o contador avança uma vez por aperto."},
            {K::Figure, "O botão no clock de um contador: cada aperto soma 1.", buttonCounter, 240},
            {K::Question, "O botão fica apertado por dois segundos. Quantas vezes o contador soma?\n*1\n2\nUma vez a cada clock\n=O "
             "contador só anda na borda de subida, quando o botão passa de 0 para 1. Segurar não cria novas bordas.\n"
             "?Quantas bordas de subida há num aperto?"},
            {K::Mistake, "Esquecer que botões de verdade trepidam\nAo fechar, o contato de metal bate e quica algumas vezes em "
             "milésimos de segundo, e cada quique é um pulso: um aperto conta 3 ou 4. Os circuitos reais filtram isso, "
             "esperando o sinal parar quieto. O nome disso é debounce. No jogo, o botão já é limpo."},
            {K::Page, "Números que parecem ao acaso"},
            {K::Text, "Um circuito digital sempre faz a mesma coisa; não existe sorteio de verdade. Mas dá para gerar uma sequência "
             "que parece aleatória: o registrador de deslocamento com realimentação, o LFSR. A cada clock os bits sobem uma "
             "casa, e o bit que entra embaixo é calculado de alguns bits do próprio registrador. Com as posições certas, "
             "ele passa por quase todos os valores antes de repetir."},
            {K::Figure, "O gerador de 8 bits: um registrador de deslocamento com XNOR na entrada. Ele passa por 255 valores antes de "
             "repetir.", randomShift, 260},
            {K::Example, "Um LFSR de 4 bits, partindo de 0\nO bit novo é uma XNOR de q3 e q2: vale 1 quando os dois são iguais.\n0000: "
             "q3 e q2 iguais, entra 1. Fica 0001, que é 1.\n0001 e 0011: q3 e q2 ainda iguais, entra 1. Vêm 3 e depois "
             "0111, 7.\n0111: q3 = 0 e q2 = 1, diferentes, entra 0. Fica 1110, 14. E segue: 13, 11, 6..."},
            {K::Question, "Nesse LFSR de 4 bits, o que vem depois de 14 (1110)?\n*13\n12\n7\n=q3 = 1 e q2 = 1 são iguais: entra 1. Os "
             "bits sobem e fica 1101, que é 13.\n?Veja q3 e q2 de 1110, e lembre que os bits sobem uma casa."},
            {K::Mistake, "Começar no valor preso\nCom XNOR, 1111 é uma armadilha: q3 e q2 são iguais, entra 1, e fica 1111 de novo, "
             "para sempre. Por isso o LFSR de 4 bits passa por 15 valores, e não 16: o 1111 fica de fora."},
            {K::Tip, "Pseudoaleatório: parece sorteado, mas é uma sequência fixa. Começando do mesmo valor, ela se repete "
             "igualzinha."},
            {K::Page, "O repique do botão"},
            {K::Text, "Um botão de verdade não fecha de uma vez. Os contatos de metal batem e voltam várias vezes em poucos "
             "milissegundos, e o circuito vê uma rajada de pulsos em vez de um só. Um contador ligado direto num botão "
             "assim conta três ou quatro apertos a cada toque."},
            {K::Text, "A solução, o debounce, é esperar o sinal se acalmar: um capacitor e um resistor que alisam a rajada, um latch "
             "que guarda o primeiro contato, ou um programa que só aceita o botão depois de ele ficar parado alguns "
             "milissegundos. O Botão do simulador já vem sem repique."},
            {K::Question, "Um contador ligado direto num botão de verdade sobe 3 a cada aperto. Por quê?\nO contador está errado\n*Os "
             "contatos repicam e fazem vários pulsos\nO botão está sem resistor\n=O metal bate e volta: cada aperto vira "
             "uma rajada de pulsos.\n?Um contato de metal fecha de uma vez só?"},
            {K::Summary, "Um botão no clock de um contador soma 1 por aperto: conta a borda de subida.\nBotões de verdade trepidam; o "
             "debounce filtra os quiques.\nLFSR: os bits sobem e entra um bit calculado dos próprios bits. Parece ao acaso, "
             "mas se repete."},
        },
        {"aleatorio-perguntas", "contador-botao", "aleatorio"},
    });

    all.push_back(Topic{
        "teclado-terminal",
        10,
        "Teclado e terminal",
        "Letras como números, e uma tela que guarda texto",
        {
            {K::Page, "Letras são números"},
            {K::Text, "Para um computador, uma letra é um número. A tabela ASCII, de 1963, dá um código de 7 bits a cada letra, "
             "algarismo e sinal: A é 65, B é 66, a é 97, o algarismo 0 é 48 e o espaço é 32. Um texto é uma lista desses "
             "números."},
            {K::Table, "Caractere|Código|Binário\nA|65|1000001\nB|66|1000010\na|97|1100001\n0|48|0110000\nespaço|32|0100000"},
            {K::Example, "Achando o código de uma letra\nAs letras vêm em ordem: se A é 65, B é 66, C é 67, e assim por diante.\nAs "
             "minúsculas também: a é 97, b é 98, c é 99.\nEntre a maiúscula e a minúscula a diferença é sempre 32: 97 − 65 "
             "= 32.\nEm binário, isso é um bit só: o bit 5 desliga em A (1000001) e liga em a (1100001)."},
            {K::Question, "Qual é o código ASCII da letra C maiúscula?\n*67\n99\n3\n=A é 65, B é 66, C é 67. O 99 é o c minúsculo.\n"
             "?Conte a partir de A, que é 65."},
            {K::Mistake, "Confundir o algarismo com o número\nA tecla 7 manda o código 55, não o número 7. Para virar número, "
             "subtrai-se 48, o código do 0: 55 − 48 = 7. Somar dois códigos de algarismos direto dá um valor sem sentido."},
            {K::Page, "O teclado e o terminal"},
            {K::Text, "A peça teclado entrega duas coisas: o código da última tecla, em Código, e um sinal Tecla que vale 1 enquanto "
             "ela está apertada. Com a peça selecionada, as teclas de verdade do seu teclado chegam ao circuito."},
            {K::Text, "O terminal é uma tela de texto com memória própria: a cada pulso de clock com Write ligado, ele guarda o "
             "caractere da entrada e avança uma posição, como uma máquina de escrever. Ligando o código do teclado na "
             "entrada e o sinal Tecla no clock, cada tecla apertada aparece na tela."},
            {K::Figure, "O teclado ligado ao terminal: o que se digita aparece na tela.", keyboardTerminal, 280},
            {K::Question, "Com o Write do terminal em 0, o que acontece quando se aperta uma tecla?\nA letra aparece duas vezes\n*Nada é "
             "guardado\nAparece o código em vez da letra\n=Sem Write, o terminal ignora o pulso: não guarda o caractere e "
             "não avança.\n?Write é a ordem de guardar."},
            {K::Tip, "Teclado, código, memória e tela: é o caminho de cada letra que você digita num computador de verdade."},
            {K::Page, "Quando o ASCII não basta"},
            {K::Text, "O ASCII tem 7 bits e 128 códigos: letras sem acento, algarismos, sinais e alguns comandos. Não tem ç, ã nem "
             "é. Durante anos cada país usou o oitavo bit do seu jeito, e um texto escrito num lugar aparecia trocado em "
             "outro."},
            {K::Text, "O Unicode resolveu isso dando um número a cada caractere de todas as línguas, mais de cem mil. O UTF-8, o "
             "jeito mais usado de guardá-lo, escreve os caracteres do ASCII num byte só, igual ao ASCII, e os outros em "
             "dois a quatro bytes. O ç, por exemplo, ocupa dois."},
            {K::Question, "Quantos caracteres diferentes cabem no ASCII de 7 bits?\n7\n*128\n256\n=2 elevado a 7 = 128 códigos.\n"
             "?Quantas combinações têm 7 bits?"},
            {K::Summary, "Cada caractere é um número, o código ASCII: A é 65, a é 97, 0 é 48, espaço é 32.\nMaiúscula e minúscula "
             "diferem em 32; o algarismo vira número subtraindo 48.\nO teclado dá o código e o sinal Tecla; o terminal "
             "guarda um caractere por pulso, com Write ligado."},
        },
        {"ascii-perguntas", "teclado-terminal"},
    });

    // Act 12: the 32-bit computer, in MIPS.
    all.push_back(Topic{
        "o-computador-de-32-bits",
        11,
        "O computador de 32 bits",
        "MIPS: registradores, syscall e endereços em bytes",
        {
            {K::Page, "Do computador de 16 bits ao MIPS"},
            {K::Text, "O computador de 16 bits tinha 8 registradores e instruções inventadas para este jogo. O de 32 bits segue o "
             "MIPS, um conjunto de instruções de verdade, o mesmo de muitas faculdades e do simulador MARS: os programas "
             "das aulas rodam aqui como estão escritos. Ele tem 32 registradores de 32 bits, e cada instrução é uma palavra "
             "de 32 bits. Os registradores têm número e nome, e o nome diz para que cada um costuma servir."},
            {K::Table, "Nome|Número|Para quê\n$zero|0|Vale sempre 0\n$at|1|Do montador, nas pseudoinstruções\n$v0 e $v1|2 e 3|O "
             "serviço do syscall e a resposta de uma função\n$a0 a $a3|4 a 7|Os argumentos de uma função e do syscall\n$t0 "
             "a $t7|8 a 15|Temporários, para as contas\n$s0 a $s7|16 a 23|Valores que uma função preserva\n$t8 e $t9|24 e "
             "25|Mais temporários\n$sp|29|O topo da pilha\n$ra|31|O endereço de volta de uma função"},
            {K::Question, "Depois de addi $zero, $zero, 5, quanto vale $zero?\n*0\n5\n1\n=$zero vale sempre 0: o que se grava nele se "
             "perde. Por isso ele serve de zero em qualquer conta.\n?Veja a primeira linha da tabela."},
            {K::Mistake, "Usar qualquer registrador para qualquer coisa\nO circuito aceitaria, mas os nomes são um combinado: $a0 leva "
             "o argumento, $v0 traz a resposta, $ra guarda a volta. Seguindo o combinado, uma função escrita por uma pessoa "
             "funciona no programa de outra."},
            {K::Page, "Um programa em MIPS"},
            {K::Text, "Cada linha tem uma instrução, com o destino primeiro: addi $t0, $t0, 2 soma 2 a $t0 e guarda em $t0. Para "
             "falar com o mundo, o programa usa syscall: põe em $v0 o número do serviço, e em $a0 o que vai junto. Este "
             "computador tem quatro serviços."},
            {K::Table, "$v0|O que o syscall faz\n1|Mostra o número que está em $a0\n11|Mostra o caractere cujo código está em $a0\n"
             "5|Lê as chaves e põe o número em $v0\n10|Termina o programa"},
            {K::Code, "        li   $t0, 40       # $t0 = 40\n        addi $t0, $t0, 2   # $t0 = 42\n        move $a0, $t0      # o "
             "que mostrar\n        li   $v0, 1        # serviço 1: mostra\n        syscall\n        li   $v0, 10       # "
             "serviço 10: termina\n        syscall"},
            {K::Example, "O programa, linha a linha\nli põe 40 em $t0, e addi soma 2: $t0 fica 42.\nmove copia $t0 para $a0, onde o "
             "syscall procura o número.\nCom $v0 em 1, o syscall mostra 42 na saída.\nCom $v0 em 10, o syscall termina o "
             "programa."},
            {K::Question, "Que número vai em $v0 para terminar o programa?\n*10\n1\n0\n=O serviço 10 termina. O 1 mostra um número.\n"
             "?Veja a tabela de serviços."},
            {K::Tip, "li e move são pseudoinstruções: o montador as escreve com instruções de verdade. li $t0, 40 vira addiu $t0, "
             "$zero, 40, e move $a0, $t0 vira addu $a0, $zero, $t0."},
            {K::Mistake, "Esquecer o syscall 10\nSem ele, a CPU continua lendo a ROM depois do programa. As palavras vazias valem 0, "
             "que é nop, a instrução que não faz nada, e o computador roda para sempre. Todo programa termina com li $v0, "
             "10 e syscall."},
            {K::Page, "Endereços contam bytes"},
            {K::Text, "No MIPS, cada endereço aponta um byte, e uma palavra tem 4 bytes. Por isso as palavras ficam de 4 em 4: a "
             "primeira do .data em 0x10010000, a segunda em 0x10010004, a terceira em 0x10010008. lw $t1, 4($t0) lê a "
             "palavra no endereço $t0 + 4: a que vem depois da que $t0 aponta. sw grava do mesmo jeito."},
            {K::Code, "        .data\nvetor:  .word 3, 5, 7, 9\n        .text\n        la   $t0, vetor    # $t0 = 0x10010000\n       "
             " lw   $t1, 0($t0)   # $t1 = 3\n        lw   $t2, 8($t0)   # $t2 = 7, a terceira"},
            {K::Example, "Percorrendo um vetor\n$t0 começa no endereço do vetor, 0x10010000.\nlw $t1, 0($t0) lê a primeira palavra: 3.\n"
             "addi $t0, $t0, 4 passa para a próxima, em 0x10010004.\nRepetindo, lw lê 5, depois 7, depois 9."},
            {K::Question, "vetor começa em 0x10010000. Em que endereço está a quarta palavra?\n*0x1001000C\n0x10010003\n0x10010010\n"
             "=Antes dela há três palavras de 4 bytes: 0x10010000 + 12 = 0x1001000C.\n?Quantos bytes ocupam as três "
             "palavras antes dela?"},
            {K::Mistake, "Somar 1 para ir à próxima palavra\naddi $t0, $t0, 1 aponta para o meio da palavra, não para a próxima. Para "
             "andar uma palavra, some 4. Para achar a palavra de número i, desloque i duas casas para a esquerda: é "
             "multiplicar por 4."},
            {K::Page, "RISC: poucas instruções, simples e rápidas"},
            {K::Text, "O MIPS é o exemplo clássico de RISC, computador de conjunto reduzido de instruções. A ideia, dos anos 1980, "
             "era o contrário do que se fazia: em vez de muitas instruções complexas, poucas e simples, todas do mesmo "
             "tamanho, cada uma rápida o bastante para terminar num ciclo."},
            {K::Table, "RISC, como o MIPS|CISC, como o x86\ninstruções de um tamanho só, 32 bits|instruções de 1 a 15 bytes\nsó lw e "
             "sw vão à memória|contas podem usar a memória direto\nmuitos registradores, 32|poucos registradores\npoucos "
             "modos de endereçamento|muitos modos"},
            {K::Text, "Instruções simples e iguais são fáceis de decodificar e de pôr em linha de montagem, o pipeline. Os celulares "
             "usam ARM, outro RISC; e até os processadores x86 de hoje traduzem as suas instruções complexas, por dentro, "
             "em operações simples como as do RISC."},
            {K::Question, "No MIPS, uma conta como add pode pegar um valor direto da memória?\nSim, como qualquer instrução\n*Não: só lw "
             "e sw falam com a memória\nSó se o endereço for pequeno\n=No RISC, as contas usam só registradores; trazer da "
             "memória é trabalho do lw.\n?Qual é a ideia de load e store?"},
            {K::Summary, "32 registradores de 32 bits; $zero vale sempre 0.\nsyscall pede um serviço pelo número em $v0: 1 mostra $a0, "
             "10 termina.\nOs endereços contam bytes: as palavras ficam de 4 em 4.\nlw e sw leem e gravam no endereço "
             "registrador + número."},
        },
        {"mips-perguntas", "mips-primeiro", "mips-vetor"},
    });

    all.push_back(Topic{
        "formatos-mips",
        11,
        "Os formatos R, I e J",
        "Como cada instrução cabe em 32 bits",
        {
            {K::Page, "Três formatos de instrução"},
            {K::Text, "Toda instrução MIPS tem 32 bits, e os 6 de cima são o op. Depois dele, há três jeitos de dividir o resto. O "
             "formato R é o das contas entre registradores: três registradores, o shamt, que diz quantas casas deslocar, e "
             "o funct, que diz qual conta fazer; o op é 0. O formato I tem dois registradores e um número de 16 bits, o "
             "imediato. O formato J é o dos saltos: 26 bits de endereço."},
            {K::Table, "Formato|Campos e seus bits|Exemplos\nR|op 6, rs 5, rt 5, rd 5, shamt 5, funct 6|add, sub, and, slt, sll, jr, "
             "syscall\nI|op 6, rs 5, rt 5, imediato 16|addi, andi, lw, sw, beq, lui\nJ|op 6, endereço 26|j, jal"},
            {K::Text, "Os registradores entram pelo número, e 5 bits bastam para os 32. Em add $t2, $t0, $t1, rd é o destino, $t2, "
             "que é 10; rs é $t0, 8; rt é $t1, 9. O funct de add é 32."},
            {K::Example, "Montando add $t2, $t0, $t1\nop 0: 000000. rs = $t0 = 8: 01000. rt = $t1 = 9: 01001.\nrd = $t2 = 10: 01010. "
             "shamt 0: 00000. funct 32: 100000.\nJuntando: 0000 0001 0000 1001 0101 0000 0010 0000.\nEm hexadecimal, "
             "0x01095020."},
            {K::Question, "Quantos registradores um campo de 5 bits consegue escolher?\n*32\n16\n5\n=2 elevado a 5 é 32: um número para "
             "cada registrador.\n?Quantos valores diferentes 5 bits formam?"},
            {K::Mistake, "Pôr os registradores na ordem do texto\nNo texto, o destino vem primeiro: add $t2, $t0, $t1. Na palavra, a "
             "ordem é rs, rt, rd: o destino vem por último. O montador troca sozinho; quem monta à mão precisa lembrar."},
            {K::Page, "Imediatos de 16 bits"},
            {K::Text, "No formato I, o número tem só 16 bits, e a CPU completa os outros 16 antes de usar. Em addi, lw, sw e beq, "
             "ela estende o sinal, repetindo o bit de cima: o número vai de −32768 a 32767. Em andi, ori e xori, ela "
             "completa com zeros: vai de 0 a 65535. É o extensor de bits, com Sinal ligado ou não."},
            {K::Example, "addi $t0, $zero, -1\nO imediato −1 em 16 bits é 0xFFFF.\nA CPU estende o sinal: 0xFFFFFFFF, que é −1 em 32 "
             "bits.\n$t0 fica −1. Com ori no lugar de addi, 0xFFFF viraria 0x0000FFFF, que é 65535."},
            {K::Text, "E um número que não cabe em 16 bits? li $t0, 0x12345678 vira duas instruções: lui $at, 0x1234 põe 0x1234 na "
             "metade de cima, e ori $t0, $at, 0x5678 completa a de baixo. la faz o mesmo com um endereço."},
            {K::Question, "Qual destes números addi aceita no imediato?\n*−5000\n40000\n0x12345\n=addi estende o sinal de 16 bits: vai "
             "de −32768 a 32767. −5000 cabe; 40000 e 0x12345 não.\n?16 bits com sinal vão de −32768 a 32767."},
            {K::Mistake, "Somar um número grande com addi\naddi $t0, $t0, 40000 não monta: o imediato de addi vai só até 32767. Ponha o "
             "número num registrador com li, que usa lui e ori quando precisa, e some com add."},
            {K::Page, "Máscaras e deslocamentos"},
            {K::Text, "andi é uma máscara: guarda os bits onde o número tem 1 e zera o resto. srl desloca para a direita, trazendo "
             "bits de cima para baixo. Juntos, recortam qualquer pedaço de uma palavra: os 4 bits de baixo com andi 0xF, e "
             "os 4 seguintes com srl 4 e depois andi 0xF. E sll 2 multiplica por 4, o jeito de passar do número de uma "
             "palavra ao seu endereço."},
            {K::Code, "        li   $t0, 0x5A\n        andi $t1, $t0, 0xF  # $t1 = 0xA, os 4 de baixo\n        srl  $t2, $t0, 4    # "
             "$t2 = 0x5, os de cima\n        sll  $t3, $t2, 2    # $t3 = 20, 5 vezes 4"},
            {K::Question, "Com $t0 = 0xC3, quanto vale $t2 depois de srl $t2, $t0, 4?\n*0xC\n0x3\n0xC30\n=Cada dígito hexadecimal são 4 "
             "bits: deslocar 4 para a direita joga fora o 3 e desce o C.\n?srl 4 desce um dígito hexadecimal."},
            {K::Page, "A ordem dos bytes numa palavra"},
            {K::Text, "Uma palavra de 32 bits ocupa 4 bytes, em 4 endereços seguidos. Mas qual byte vai no primeiro? No big-endian, "
             "o byte mais significativo vem primeiro, como se escreve um número. No little-endian, o menos significativo "
             "vem primeiro. O MIPS pode funcionar dos dois jeitos; os processadores x86 e quase todos os ARM usam "
             "little-endian."},
            {K::Table, "Endereço|Big-endian|Little-endian\n100|12|78\n101|34|56\n102|56|34\n103|78|12"},
            {K::Text, "A tabela mostra a palavra 0x12345678 guardada a partir do endereço 100. O computador deste jogo só lê e grava "
             "palavras inteiras, então a ordem não aparece aqui; ela importa quando dois computadores trocam dados por um "
             "arquivo ou pela rede."},
            {K::Question, "Num computador little-endian, a palavra 0x0A0B0C0D está no endereço 200. Que byte está no endereço 200?\n0A\n"
             "*0D\n0C\n=No little-endian o byte menos significativo vem primeiro: 0D.\n?Little-endian começa pela ponta "
             "pequena."},
            {K::Summary, "R: três registradores, o shamt e o funct, com op 0. I: dois registradores e um imediato de 16 bits. J: 26 "
             "bits de endereço.\nNa palavra, o destino de uma instrução R é rd, o terceiro campo de registrador.\naddi, lw, "
             "sw e beq estendem o sinal do imediato; andi, ori e xori completam com zeros.\nandi recorta bits, srl os traz "
             "para baixo, sll 2 multiplica por 4."},
        },
        {"formatos-mips-perguntas", "mips-mascara"},
    });

    all.push_back(Topic{
        "desvios-mips",
        11,
        "Desvios, saltos e funções",
        "beq, j, jal e a pilha",
        {
            {K::Page, "beq e bne"},
            {K::Text, "beq $s, $t, rótulo desvia quando os dois registradores são iguais; bne, quando são diferentes. O rótulo vira "
             "um imediato de 16 bits que conta instruções a partir da seguinte: o destino é PC + 4 + imediato × 4. Um "
             "imediato negativo volta, e é assim que um laço repete."},
            {K::Example, "Um beq que pula duas instruções\nO beq está em 0x00400008, com imediato 2.\nA instrução seguinte está em "
             "0x0040000C.\n2 instruções são 8 bytes: o destino é 0x0040000C + 8 = 0x00400014."},
            {K::Text, "blt, bgt, ble e bge são pseudoinstruções: o montador escreve slt, que põe 1 em $at quando o primeiro é menor, "
             "e um beq ou bne que olha $at."},
            {K::Question, "Um bne em 0x00400010 tem imediato −3. Se desviar, para onde vai?\n*0x00400008\n0x00400004\n0x0040000D\n=A "
             "seguinte está em 0x00400014; voltando 3 instruções, 12 bytes, chega em 0x00400008.\n?Comece da instrução "
             "seguinte e conte de 4 em 4."},
            {K::Mistake, "Contar a partir do próprio beq\nO imediato conta a partir da instrução seguinte, PC + 4, porque o PC + 4 já "
             "está calculado quando o desvio é decidido. Um beq com imediato 0 não volta para si: segue para a próxima, "
             "desviando ou não."},
            {K::Page, "j, jal e jr"},
            {K::Text, "j rótulo salta sempre. O endereço tem 32 bits, mas o campo tem 26: como toda instrução está num múltiplo de "
             "4, os 2 bits de baixo são sempre 0 e não vão na palavra, e os 4 de cima vêm do PC. jal faz o mesmo e, antes, "
             "guarda em $ra o endereço da instrução seguinte. No fim da função, jr $ra volta para lá."},
            {K::Code, "main:   li   $a0, 5\n        jal  dobro          # $ra = o endereço do move\n        move $a0, $v0       # "
             "$a0 = 10\n        li   $v0, 1\n        syscall\n        li   $v0, 10\n        syscall\ndobro:  add  $v0, $a0, "
             "$a0  # a resposta vai em $v0\n        jr   $ra            # volta para depois do jal"},
            {K::Example, "Chamando dobro\nO jal está em 0x00400004: guarda 0x00400008 em $ra e salta para dobro.\ndobro põe 5 + 5 = 10 "
             "em $v0.\njr $ra volta para 0x00400008, o move.\nO programa mostra 10."},
            {K::Question, "Um jal está em 0x00400020. Que valor ele põe em $ra?\n*0x00400024\n0x00400020\n0x00400028\n=$ra recebe o "
             "endereço da instrução seguinte: 0x00400020 + 4.\n?Para onde a função deve voltar?"},
            {K::Page, "A pilha"},
            {K::Text, "Uma função que chama outra perde o seu $ra: o segundo jal escreve por cima. A saída é guardar $ra na pilha, "
             "um pedaço da RAM que cresce para baixo a partir de 0x7FFFEFFC, com o topo em $sp. Para guardar, a função "
             "desce $sp 4 bytes e grava; antes de voltar, lê de volta e sobe $sp."},
            {K::Code, "f:      addi $sp, $sp, -4  # abre espaço na pilha\n        sw   $ra, 0($sp)   # guarda a volta\n        jal  "
             "g             # muda o $ra\n        lw   $ra, 0($sp)   # recupera a volta\n        addi $sp, $sp, 4   # fecha "
             "o espaço\n        jr   $ra"},
            {K::Question, "Para guardar dois registradores na pilha, quanto se soma a $sp?\n*−8\n−2\n8\n=Dois registradores são duas "
             "palavras, 8 bytes, e a pilha cresce para baixo: −8.\n?Cada palavra são 4 bytes, e a pilha desce."},
            {K::Mistake, "Desequilibrar a pilha\nCada addi $sp, $sp, -4 precisa do seu addi $sp, $sp, 4 antes do jr. Se faltar um, quem "
             "chamou a função encontra $sp no lugar errado e lê o valor de outro."},
            {K::Page, "Sem sinalizadores"},
            {K::Text, "Muitos processadores comparam numa instrução e desviam em outra, com os sinalizadores da ULA no meio: "
             "compara, e depois salta se deu menor. O MIPS não tem sinalizadores. O beq e o bne comparam dois registradores "
             "dentro da própria instrução, e para saber se um é menor que o outro existe o slt, que põe 1 ou 0 num "
             "registrador."},
            {K::Text, "Sem sinalizadores, nenhuma instrução muda escondido um estado que a próxima vai ler. Isso simplifica o "
             "pipeline: cada instrução depende só dos registradores que cita."},
            {K::Example, "Desviar se $t0 < $t1\nslt $t2, $t0, $t1 põe 1 em $t2 se $t0 for menor que $t1, e 0 se não for.\nbne $t2, "
             "$zero, menor salta se $t2 não for 0, ou seja, se $t0 < $t1.\nÉ o que a pseudoinstrução blt $t0, $t1, menor "
             "faz: o montador a escreve com essas duas."},
            {K::Question, "Depois de slt $t2, $t0, $t1 com $t0 = 7 e $t1 = 3, quanto vale $t2?\n*0\n1\n4\n=7 não é menor que 3: slt põe "
             "0.\n?slt pergunta: o primeiro é menor que o segundo?"},
            {K::Summary, "beq e bne desviam para PC + 4 + imediato × 4.\nj e jal levam 26 bits do endereço; jal guarda a volta em $ra.\n"
             "jr $ra volta de uma função; o argumento vai em $a0 e a resposta em $v0.\nUma função que chama outra guarda "
             "$ra na pilha, descendo $sp."},
        },
        {"desvios-mips-perguntas", "mips-soma-ate", "mips-quadrado"},
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

QuestionParts questionParts(std::string_view text) {
    QuestionParts parts;
    std::size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (first) {
            parts.prompt = std::string(line);
            first = false;
        } else if (!line.empty() && line.front() == '=') {
            parts.explain = std::string(line.substr(1));
        } else if (!line.empty() && line.front() == '?') {
            parts.hint = std::string(line.substr(1));
        } else if (!line.empty() && line.front() == '*') {
            parts.right = static_cast<int>(parts.options.size());
            parts.options.emplace_back(line.substr(1));
        } else if (!line.empty()) {
            parts.options.emplace_back(line);
        }
    }
    return parts;
}

std::vector<std::pair<std::size_t, std::size_t>> readingPages(const Topic& topic) {
    std::vector<std::pair<std::size_t, std::size_t>> pages;
    std::size_t first = 0;
    for (std::size_t b = 0; b < topic.blocks.size(); ++b) {
        if (topic.blocks[b].kind != TheoryBlockKind::Page || b == 0) continue;
        pages.push_back({first, b});
        first = b;
    }
    pages.push_back({first, topic.blocks.size()});
    return pages;
}

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
