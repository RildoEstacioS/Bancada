// Every example in the gallery opens, does what its card says, and every program runs to its end on the
// computer's own circuit with the result it promises.
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/computer_view.hpp"
#include "editor/gallery.hpp"
#include "editor/truth_table.hpp"

using namespace bancada;
using namespace bancada::editor;

namespace {

const GalleryItem& item(const char* id) {
    for (const GalleryItem& each : galleryItems()) {
        if (std::string(each.id) == id) return each;
    }
    throw std::logic_error(std::string("no example ") + id);
}

struct Run {
    bool halted = false;
    std::uint16_t output = 0;
    std::vector<std::uint16_t> shown;  // every value the output took, in order
    int instructions = 0;
};

// The program runs on the circuit, one instruction at a time, as the clock would run it.
Run run(const GalleryProject& project, int most) {
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    BoardCircuit circuit(project.board, &chips);
    for (const auto& [id, value] : project.toggles) circuit.setToggle(id, value);
    circuit.settle();
    Run out;
    for (; out.instructions < most; ++out.instructions) {
        const std::optional<ComputerState> state = readComputer(project.board, circuit, chips);
        if (!state) break;
        if (state->output != out.output) {
            out.shown.push_back(state->output);
            out.output = state->output;
        }
        if (state->halted) {
            out.halted = true;
            break;
        }
        circuit.setClocks(true);
        circuit.settle();
        circuit.setClocks(false);
        circuit.settle();
    }
    return out;
}

}  // namespace

TEST(every_example_opens) {
    CHECK(galleryItems().size() >= 18);
    for (const GalleryItem& each : galleryItems()) {
        try {
            const GalleryProject project = buildGalleryItem(each);
            CHECK(!project.board.parts().empty());
            CHECK(!project.note.empty());
            CHECK(each.group >= 0 && each.group < static_cast<int>(galleryGroups().size()));
        } catch (const std::exception& error) {
            std::printf("  %s: %s\n", each.id, error.what());
            CHECK(false);
        }
    }
}

TEST(every_program_assembles) {
    for (const ComputerProgram& program : computerPrograms()) {
        const assembly::Result result = assembly::assemble(program.source);
        for (const assembly::Error& error : result.errors) std::printf("  %s, line %d: %s\n", program.id, error.line, error.message.c_str());
        CHECK(result.errors.empty());
    }
}

TEST(every_program_ends_with_its_result) {
    struct Expected {
        const char* id;
        std::uint16_t output;
    };
    for (const Expected& expected : {Expected{"soma", 55}, Expected{"multiplica", 42}, Expected{"fibonacci", 144}, Expected{"potencias", 32768},
                                     Expected{"regressiva", 0}, Expected{"lista", 100}, Expected{"subrotina", 81}}) {
        const Run result = run(buildGalleryItem(item(expected.id)), 400);
        if (!result.halted || result.output != expected.output) {
            std::printf("  %s: halted %d, output %u after %d instructions\n", expected.id, result.halted, result.output, result.instructions);
        }
        CHECK(result.halted);
        CHECK_EQ(result.output, expected.output);
    }
}

TEST(the_programs_show_what_they_promise_on_the_way) {
    CHECK(run(buildGalleryItem(item("fibonacci")), 400).shown == std::vector<std::uint16_t>({1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144}));
    CHECK(run(buildGalleryItem(item("regressiva")), 400).shown == std::vector<std::uint16_t>({10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));
    CHECK(run(buildGalleryItem(item("subrotina")), 400).shown == std::vector<std::uint16_t>({3, 9, 27, 81}));
    CHECK(run(buildGalleryItem(item("chamadas")), 400).shown == std::vector<std::uint16_t>({6, 12}));
    const Run tasks = run(buildGalleryItem(item("tarefas")), 600);
    CHECK(tasks.halted);
    CHECK(tasks.shown == std::vector<std::uint16_t>({1, 10, 2, 20, 3, 30}));
    const Run forever = run(buildGalleryItem(item("contador")), 200);
    CHECK(!forever.halted);
    CHECK(forever.output > 50);
}

namespace {

// What a circuit does, by name: its named switches, and what each named output shows for every
// combination of them, or for a fixed sample of combinations when there are too many to try.
struct Behaviour {
    std::vector<std::string> inputs;
    std::map<std::string, std::vector<std::uint64_t>> outputs;
};

Behaviour behaviourOf(const GalleryProject& project) {
    ChipBoards chips;
    for (const ChipData& chip : project.chips) chips[chip.id] = &chip.board;
    BoardCircuit circuit(project.board, &chips);
    std::vector<const Part*> ins;
    std::vector<const Part*> outs;
    for (const Part& part : project.board.parts()) {
        if (part.label.empty()) continue;
        if (part.kind == PartKind::Toggle) ins.push_back(&part);
        if (isOutputPart(part.kind) || isLed(part.kind)) outs.push_back(&part);
    }
    const auto byName = [](const Part* a, const Part* b) { return a->label < b->label; };
    std::sort(ins.begin(), ins.end(), byName);
    std::sort(outs.begin(), outs.end(), byName);
    Behaviour out;
    unsigned bits = 0;
    for (const Part* part : ins) {
        out.inputs.push_back(part->label);
        bits += part->width;
    }
    const std::uint64_t all = std::uint64_t{1} << bits;
    const std::uint64_t tries = bits <= 10 ? all : 300;
    std::uint64_t seed = 12345;
    for (std::uint64_t t = 0; t < tries; ++t) {
        std::uint64_t combination = t;
        if (bits > 10) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            combination = (seed >> 20) % all;
        }
        unsigned shift = 0;
        for (const Part* part : ins) {
            circuit.setToggle(part->id, (combination >> shift) & ((std::uint64_t{1} << part->width) - 1));
            shift += part->width;
        }
        circuit.settle();
        for (const Part* part : outs) out.outputs[part->label].push_back(outputOf(project.board, circuit, part->id));
    }
    return out;
}

// Circuits that remember, and the display, whose versions take different inputs: compared otherwise.
bool remembers(const GalleryItem& item) {
    const std::string_view id = item.id;
    return id == "latch" || id == "contador4" || id == "registrador" || id == "aleatorio" || id == "display7" || id == "botao" || id == "matriz" ||
           id == "teclado" || id == "latch-d" || id == "flip-flop";
}

}  // namespace

// The point of the versions: the gates and the transistors do exactly what the ready part does.
TEST(every_version_of_an_example_does_the_same) {
    int compared = 0;
    for (const GalleryItem& each : galleryItems()) {
        if (each.versions.size() < 2 || remembers(each)) continue;
        const Behaviour first = behaviourOf(buildGalleryItem(each, 0));
        CHECK(!first.inputs.empty());
        CHECK(!first.outputs.empty());
        for (std::size_t v = 1; v < each.versions.size(); ++v) {
            const Behaviour other = behaviourOf(buildGalleryItem(each, v));
            const bool same = other.inputs == first.inputs && other.outputs == first.outputs;
            if (!same) std::printf("  %s: %s does something else than %s\n", each.id, each.versions[v].name, each.versions[0].name);
            CHECK(same);
            ++compared;
        }
    }
    CHECK(compared >= 20);
}

TEST(every_version_builds_and_says_how_it_works) {
    for (const GalleryItem& each : galleryItems()) {
        CHECK(std::string_view(each.about).size() > 40);
        CHECK(std::string_view(each.tryIt).size() > 10);
        for (std::size_t v = 0; v < each.versions.size(); ++v) {
            CHECK(std::string_view(each.versions[v].how).size() > 40);
            try {
                CHECK(!buildGalleryItem(each, v).board.parts().empty());
            } catch (const std::exception& error) {
                std::printf("  %s, %s: %s\n", each.id, each.versions[v].name, error.what());
                CHECK(false);
            }
        }
    }
}

TEST(the_logic_examples_follow_their_tables) {
    CHECK(behaviourOf(buildGalleryItem(item("xor"), 1)).outputs.at("S") == std::vector<std::uint64_t>({0, 1, 1, 0}));
    CHECK(behaviourOf(buildGalleryItem(item("nao"), 1)).outputs.at("S") == std::vector<std::uint64_t>({1, 0}));
    CHECK(behaviourOf(buildGalleryItem(item("nand"), 1)).outputs.at("S") == std::vector<std::uint64_t>({1, 1, 1, 0}));
    const TruthTable decoderTable = computeTruthTable(buildGalleryItem(item("decodificador"), 1).board);
    CHECK(decoderTable.status == TruthTable::Status::Ready);
    CHECK_EQ(decoderTable.rows.size(), 4);
    for (const std::string& row : decoderTable.rows) CHECK_EQ(std::count(row.begin(), row.end(), '1'), 1);
    // The four-bit adder adds: A, B and Ce, the switches in name order, the first in the lowest bits.
    const Behaviour adder = behaviourOf(buildGalleryItem(item("somador4"), 2));
    bool sums = adder.outputs.at("S").size() == 512;
    for (unsigned row = 0; row < 512 && sums; ++row) {
        const unsigned total = (row & 15) + ((row >> 4) & 15) + (row >> 8);
        sums = adder.outputs.at("S")[row] == (total & 15) && adder.outputs.at("Cs")[row] == (total >> 4);
    }
    CHECK(sums);
}

// The new parts do the sums they promise, on every input: the switches in name order, the first lowest.
TEST(the_new_parts_do_their_sums) {
    const Behaviour multiply = behaviourOf(buildGalleryItem(item("multiplicador")));
    const Behaviour divide = behaviourOf(buildGalleryItem(item("divisor")));
    bool products = true;
    bool quotients = true;
    for (unsigned row = 0; row < 256; ++row) {
        const unsigned a = row & 15, b = row >> 4;
        products = products && multiply.outputs.at("Baixo")[row] == ((a * b) & 15) && multiply.outputs.at("Alto")[row] == (a * b) >> 4;
        if (b != 0) quotients = quotients && divide.outputs.at("Q")[row] == a / b && divide.outputs.at("R")[row] == a % b;
    }
    CHECK(products);
    CHECK(quotients);
    const Behaviour negate = behaviourOf(buildGalleryItem(item("negador")));
    for (unsigned a = 0; a < 16; ++a) CHECK_EQ(negate.outputs.at("N")[a], (16 - a) & 15);
    const Behaviour count = behaviourOf(buildGalleryItem(item("conta-bits")));
    const Behaviour find = behaviourOf(buildGalleryItem(item("acha-bit")));
    for (unsigned d = 0; d < 256; ++d) {
        CHECK_EQ(count.outputs.at("C")[d], static_cast<std::uint64_t>(std::popcount(d)));
        CHECK_EQ(find.outputs.at("V")[d], d != 0 ? 1u : 0u);
        if (d != 0) CHECK_EQ(find.outputs.at("I")[d], static_cast<std::uint64_t>(std::countr_zero(d)));
    }
    const Behaviour odd = behaviourOf(buildGalleryItem(item("paridade-impar")));
    for (unsigned d = 0; d < 16; ++d) CHECK_EQ(odd.outputs.at("S")[d], static_cast<std::uint64_t>(std::popcount(d) & 1));
    // D0..D3 in name order: the highest one on wins.
    const Behaviour priority = behaviourOf(buildGalleryItem(item("prioridade")));
    for (unsigned d = 1; d < 16; ++d) {
        const unsigned highest = 31u - static_cast<unsigned>(std::countl_zero(d));
        CHECK_EQ(priority.outputs.at("A0")[d] + 2 * priority.outputs.at("A1")[d], highest);
    }
}

// The random generator built of flip-flops goes through the same numbers as the ready one.
TEST(both_random_generators_draw_the_same_numbers) {
    std::vector<std::uint64_t> drawn[2];
    for (std::size_t v = 0; v < 2; ++v) {
        const GalleryProject project = buildGalleryItem(item("aleatorio"), v);
        BoardCircuit circuit(project.board);
        circuit.reset();
        circuit.settle();
        PartId q = 0;
        for (const Part& part : project.board.parts()) {
            if (part.kind == PartKind::Probe) q = part.id;
        }
        for (int pulse = 0; pulse < 20; ++pulse) {
            circuit.setClocks(true);
            circuit.settle();
            circuit.setClocks(false);
            circuit.settle();
            drawn[v].push_back(outputOf(project.board, circuit, q));
        }
    }
    CHECK(drawn[0] == drawn[1]);
    CHECK(drawn[0][0] == 1);
    CHECK(std::set<std::uint64_t>(drawn[0].begin(), drawn[0].end()).size() == 20);
}

// Typing on the keyboard writes the codes into the terminal, one place each.
TEST(the_terminal_keeps_what_was_typed) {
    const GalleryProject project = buildGalleryItem(item("teclado"));
    BoardCircuit circuit(project.board);
    circuit.reset();
    PartId keyboard = 0;
    PartId screen = 0;
    for (const Part& part : project.board.parts()) {
        if (part.kind == PartKind::Keyboard) keyboard = part.id;
        if (part.kind == PartKind::Box) screen = part.id;
    }
    for (const char c : std::string("Oi!")) {
        circuit.setToggle(keyboard, 0x80u | static_cast<unsigned>(c));
        circuit.settle();
        circuit.setToggle(keyboard, static_cast<unsigned>(c));
        circuit.settle();
    }
    const std::array<PartId, 1> path{screen};
    CHECK_EQ(circuit.memoryWord(path, 0), static_cast<std::uint64_t>('O'));
    CHECK_EQ(circuit.memoryWord(path, 1), static_cast<std::uint64_t>('i'));
    CHECK_EQ(circuit.memoryWord(path, 2), static_cast<std::uint64_t>('!'));
}

int main() { return check::runAll(); }
