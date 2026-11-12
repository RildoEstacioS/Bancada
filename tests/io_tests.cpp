#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "asm/assembler.hpp"
#include "check.hpp"
#include "editor/circuit.hpp"
#include "editor/computer16.hpp"
#include "editor/router.hpp"
#include "editor/session.hpp"
#include "io/container.hpp"
#include "io/files.hpp"
#include "io/profiles.hpp"
#include "io/project.hpp"

using namespace bancada;
using namespace bancada::editor;
using namespace bancada::io;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr ChunkTag kMeta{'M', 'E', 'T', 'A'};
constexpr ChunkTag kBoard{'B', 'O', 'R', 'D'};

// A folder with a space and accents, because real user folders have them.
fs::path testFolder(const char* name) {
    const fs::path folder = fs::temp_directory_path() / pathFromUtf8("bancada testes é ç") / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

Drop pinAt(const Board& board, PartId part, std::uint8_t pin) {
    return {Drop::Kind::Pin, pinPosition(*board.part(part), pin), part, pin};
}

void link(Board& board, const Drop& from, const Drop& to) { board.connect(from, to, elbowPath(from.at, to.at)); }

struct XorBoard {
    Board board;
    PartId a = 0;
    PartId b = 0;
    PartId lamp = 0;
};

XorBoard xorBoard() {
    XorBoard x;
    Board& board = x.board;
    x.a = *board.addPart(PartKind::Toggle, {2, 4});
    x.b = *board.addPart(PartKind::Toggle, {2, 12});
    const PartId n1 = *board.addPart(PartKind::Nand, {10, 8});
    const PartId n2 = *board.addPart(PartKind::Nand, {18, 3});
    const PartId n3 = *board.addPart(PartKind::Nand, {18, 13});
    const PartId n4 = *board.addPart(PartKind::Nand, {27, 8});
    x.lamp = *board.addPart(PartKind::Led, {35, 9});

    link(board, pinAt(board, x.a, 0), pinAt(board, n2, 0));
    link(board, pinAt(board, x.b, 0), pinAt(board, n3, 1));
    link(board, pinAt(board, n1, 2), pinAt(board, n2, 1));
    link(board, pinAt(board, n2, 2), pinAt(board, n4, 0));
    link(board, pinAt(board, n3, 2), pinAt(board, n4, 1));
    link(board, pinAt(board, n4, 2), pinAt(board, x.lamp, 0));
    link(board, board.dropAt({6, 5}), pinAt(board, n1, 0));
    link(board, board.dropAt({6, 13}), pinAt(board, n1, 1));
    link(board, board.dropAt({15, 10}), pinAt(board, n3, 0));
    return x;
}

void checkIsXor(const Board& board, PartId a, PartId b, PartId lamp) {
    BoardCircuit circuit(board);
    for (int i = 0; i < 4; ++i) {
        circuit.setToggle(a, (i & 1) != 0);
        circuit.setToggle(b, (i & 2) != 0);
        circuit.settle();
        CHECK_EQ(circuit.outputOn(lamp), ((i & 1) != 0) != ((i & 2) != 0));
    }
}

Project sampleProject() {
    XorBoard x = xorBoard();
    Project project;
    project.name = "meu ou exclusivo";
    project.board = std::move(x.board);
    project.togglesOn = {x.a};
    project.view = {3.5f, -2.25f, 21.0f};
    return project;
}

// A well-formed file around whatever JSON a test wants to feed the parser. Most of these say version 1,
// so they also go through the migration, which has to survive anything a file can hold.
constexpr const char* kPlainMeta = R"({"name":"t","view":{"x":0,"y":0,"zoom":16}})";

Bytes fileWith(const std::string& board, const std::string& meta = kPlainMeta, std::uint32_t version = 1) {
    const std::array chunks{Chunk{kMeta, Bytes(meta.begin(), meta.end())}, Chunk{kBoard, Bytes(board.begin(), board.end())}};
    return packContainer(version, chunks);
}

// One of everything version 3 added: chips, one inside another, many-bit parts, a filled ROM, a clock.
Project thirdFormatProject() {
    Project project;
    project.name = "terceira versao";

    ChipData inner;
    inner.id = 3;
    inner.name = "mascara";
    {
        Board& board = inner.board;
        Part in = makePart(PartKind::Toggle, {0, 0});
        in.width = 8;
        in.label = "ENT";
        const PartId input = *board.addPart(in);
        Part constant = makePart(PartKind::Constant, {0, 10});
        constant.width = 8;
        constant.value = 0x0F;
        const PartId mask = *board.addPart(constant);
        Part gate = makePart(PartKind::And, {20, 2});
        gate.width = 8;
        const PartId gatePart = *board.addPart(gate);
        Part out = makePart(PartKind::Probe, {40, 3});
        out.width = 8;
        const PartId output = *board.addPart(out);
        link(board, pinAt(board, input, 0), pinAt(board, gatePart, 0));
        link(board, pinAt(board, mask, 0), pinAt(board, gatePart, 1));
        link(board, pinAt(board, gatePart, 2), pinAt(board, output, 0));
    }
    const InterfacePtr innerPins = makeChipInterface(inner.board, inner.name);

    ChipData outer;
    outer.id = 5;
    outer.name = "embrulho";
    {
        Board& board = outer.board;
        Part in = makePart(PartKind::Toggle, {0, 0});
        in.width = 8;
        const PartId input = *board.addPart(in);
        Part chip = makePart(PartKind::Chip, {20, 0});
        chip.chip = 3;
        chip.interface = innerPins;
        const PartId chipPart = *board.addPart(chip);
        Part out = makePart(PartKind::Probe, {40, 0});
        out.width = 8;
        const PartId output = *board.addPart(out);
        link(board, pinAt(board, input, 0), pinAt(board, chipPart, 0));
        link(board, pinAt(board, chipPart, 1), pinAt(board, output, 0));
    }
    const InterfacePtr outerPins = makeChipInterface(outer.board, outer.name);

    Board& board = project.board;
    Part in = makePart(PartKind::Toggle, {0, 0});
    in.width = 8;
    const PartId input = *board.addPart(in);
    Part chip = makePart(PartKind::Chip, {20, 0});
    chip.chip = 5;
    chip.interface = outerPins;
    const PartId chipPart = *board.addPart(chip);
    Part out = makePart(PartKind::Probe, {40, 0});
    out.width = 8;
    const PartId output = *board.addPart(out);
    link(board, pinAt(board, input, 0), pinAt(board, chipPart, 0));
    link(board, pinAt(board, chipPart, 1), pinAt(board, output, 0));
    board.addPart(PartKind::Clock, {0, 20});
    Part splitter = makePart(PartKind::Splitter, {20, 20}, 0, 3);
    splitter.width = 16;
    board.addPart(splitter);
    Part joiner = makePart(PartKind::Joiner, {30, 20}, 1, 4);
    joiner.width = 12;
    board.addPart(joiner);
    Part rom = makePart(PartKind::Box, {0, 40}, 0, 0, 2, boxFromKey("rom"));
    rom.rom = std::make_shared<const std::vector<std::uint64_t>>(std::vector<std::uint64_t>{1, 2, 3, 0xFF});
    board.addPart(rom);
    board.addPart(makePart(PartKind::Box, {40, 40}, 0, 0, 1, boxFromKey("ram")));

    project.toggleValues = {{input, 0xA5}};
    project.chips = {std::move(inner), std::move(outer)};
    return project;
}

// What version 4 added, on top of everything the third had.
Project fourthFormatProject() {
    Project project = thirdFormatProject();
    project.name = "quarta versao";
    Part wide = makePart(PartKind::Splitter, {60, 20}, 0, 32);
    wide.width = 32;
    project.board.addPart(wide);
    Part rom = makePart(PartKind::Box, {60, 90}, 0, 0, 1, boxFromKey("rom"));
    rom.width = 16;
    rom.rom = std::make_shared<const std::vector<std::uint64_t>>(std::vector<std::uint64_t>{0x2207, 0xB040, 0xF000});
    rom.source = "LI R1, 7 ; la\xC3\xA7o\nOUT R1\nHLT\n";
    project.board.addPart(rom);
    return project;
}

// What version 5 added: labels that connect by name.
Project fifthFormatProject() {
    Project project = fourthFormatProject();
    project.name = "quinta versao";
    Part flag = makePart(PartKind::Tunnel, {60, 120});
    flag.label = "Dados";
    flag.width = 8;
    project.board.addPart(flag);
    return project;
}

// What version 6 added: the electric parts, and a switch that's on.
Project sixthFormatProject() {
    Project project = fifthFormatProject();
    project.name = "sexta versao";
    int x = 0;
    PartId lastSwitch = 0;
    for (PartKind kind : {PartKind::Battery, PartKind::Ground, PartKind::Resistor, PartKind::Diode, PartKind::Light, PartKind::Switch,
                          PartKind::Npn, PartKind::Pnp, PartKind::Nmos, PartKind::Pmos, PartKind::Capacitor}) {
        const PartId id = *project.board.addPart(makePart(kind, {x, 140}, static_cast<std::uint8_t>(x / 6 % 4)));
        if (kind == PartKind::Switch) lastSwitch = id;
        x += 6;
    }
    project.togglesOn.push_back(lastSwitch);
    return project;
}

// What version 7 added: the LED drawn as it looks, turned every way.
Project seventhFormatProject() {
    Project project = sixthFormatProject();
    project.name = "setima versao";
    for (std::uint8_t turn = 0; turn < 4; ++turn) project.board.addPart(makePart(PartKind::LedBulb, {turn * 6, 150}, turn));
    return project;
}

// What version 8 added: the battery drawn as it looks, at 9 V.
Project eighthFormatProject() {
    Project project = seventhFormatProject();
    project.name = "oitava versao";
    Part cell = makePart(PartKind::BatteryCell, {0, 160});
    cell.value = 9'000;
    project.board.addPart(cell);
    project.board.addPart(makePart(PartKind::BatteryCell, {8, 160}, 1));
    return project;
}

// What version 9 added: the button and the seven-segment display.
Project ninthFormatProject() {
    Project project = eighthFormatProject();
    project.name = "nona versao";
    project.board.addPart(makePart(PartKind::Button, {0, 170}));
    project.board.addPart(makePart(PartKind::Display7, {8, 170}));
    project.board.addPart(makePart(PartKind::Display7, {20, 170}, 1));
    return project;
}

// What version 10 added: the keyboard.
Project tenthFormatProject() {
    Project project = ninthFormatProject();
    project.name = "decima versao";
    project.board.addPart(makePart(PartKind::Keyboard, {0, 190}));
    return project;
}

// The board's JSON inside a saved file.
std::string boardChunk(const Bytes& file) {
    const Container container = unpackContainer(file, kFormatVersion);
    for (const Chunk& chunk : container.chunks) {
        if (chunk.tag == kBoard) return std::string(chunk.data.begin(), chunk.data.end());
    }
    return {};
}

// One of everything version 2 added: a wider gate, a bigger gate, a 0/1 output and a box.
Project secondFormatProject() {
    Project project;
    project.name = "segunda versao";
    Board& board = project.board;
    const PartId a = *board.addPart(PartKind::Toggle, {0, 0});
    const PartId gate = *board.addPart(PartKind::Or, {10, 0}, 0, 3);
    const PartId big = *board.addPart(PartKind::Xnor, {10, 12}, 1, 2, 2);
    const PartId led = *board.addPart(PartKind::Led, {24, 2});
    const PartId probe = *board.addPart(PartKind::Probe, {24, 8});
    const PartId box = *board.addPart(PartKind::Box, {0, 30}, 0, 0, 1, boxFromKey("full_adder"));
    link(board, pinAt(board, a, 0), pinAt(board, gate, 0));
    link(board, pinAt(board, gate, 3), pinAt(board, led, 0));
    link(board, pinAt(board, a, 0), pinAt(board, box, 0));
    link(board, pinAt(board, box, 3), pinAt(board, probe, 0));
    (void)big;
    project.togglesOn = {a};
    return project;
}

std::optional<LoadError> errorOf(const std::string& board) { return decodeProject(fileWith(board)).error; }

std::string withEmptyBoardExcept(const std::string& key, const std::string& value) {
    json document = {{"parts", json::array()}, {"joints", json::array()}, {"wires", json::array()},
                     {"togglesOn", json::array()}, {"chips", json::array()}};
    document[key] = json::parse(value);
    return document.dump();
}

std::uint32_t u32At(const Bytes& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= std::uint32_t{bytes[at + static_cast<std::size_t>(i)]} << (i * 8);
    return value;
}

void putU64(Bytes& bytes, std::size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
}

}  // namespace

// --- container -------------------------------------------------------------------------------------

TEST(chunks_come_back_exactly) {
    const std::string text(5000, 'x');  // compresses
    const Bytes noise = [] {
        Bytes out(5000);
        std::mt19937 random(1);
        for (std::uint8_t& byte : out) byte = static_cast<std::uint8_t>(random());
        return out;
    }();                                // doesn't
    const std::array chunks{Chunk{{'A', 'A', 'A', 'A'}, Bytes(text.begin(), text.end())},
                            Chunk{{'B', 'B', 'B', 'B'}, noise}, Chunk{{'C', 'C', 'C', 'C'}, {}}};
    const Bytes file = packContainer(1, chunks);
    CHECK(file.size() < 5000 + 5000 + 200);
    const Container read = unpackContainer(file, 1);
    CHECK(!read.error.has_value());
    CHECK_EQ(read.chunks.size(), 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(read.chunks[i].tag == chunks[i].tag);
        CHECK(read.chunks[i].data == chunks[i].data);
    }
}

TEST(every_single_byte_of_damage_is_noticed) {
    const Bytes file = encodeProject(sampleProject());
    for (std::size_t i = 0; i < file.size(); ++i) {
        for (std::uint8_t mask : {std::uint8_t{0x01}, std::uint8_t{0x80}}) {
            Bytes damaged = file;
            damaged[i] ^= mask;
            CHECK(decodeProject(damaged).error.has_value());
        }
    }
}

TEST(a_file_cut_short_at_any_point_is_refused) {
    const Bytes file = encodeProject(sampleProject());
    for (std::size_t size = 0; size < file.size(); ++size) {
        CHECK(decodeProject(std::span(file).first(size)).error.has_value());
    }
}

TEST(extra_bytes_after_the_end_are_refused) {
    Bytes file = encodeProject(sampleProject());
    file.push_back(0);
    CHECK(decodeProject(file).error == LoadError::Damaged);
}

TEST(other_files_are_recognised_as_other_files) {
    const std::string text = "just some text file, long enough to have a header's worth of bytes in it";
    CHECK(decodeProject(Bytes(text.begin(), text.end())).error == LoadError::NotOurFile);
    CHECK(decodeProject(Bytes{}).error == LoadError::NotOurFile);
}

TEST(a_newer_file_says_so_instead_of_looking_damaged) {
    const std::array chunks{Chunk{kMeta, {}}};
    const Bytes file = packContainer(kFormatVersion + 1, chunks);
    CHECK(decodeProject(file).error == LoadError::NewerVersion);
}

TEST(a_huge_size_in_a_correctly_hashed_table_is_refused_before_allocating) {
    Bytes file = encodeProject(sampleProject());
    const std::uint32_t count = u32At(file, 12);
    putU64(file, 24 + 16, std::uint64_t{1} << 62);  // the first chunk's raw size
    putU64(file, 16, XXH3_64bits_withSeed(file.data() + 24, std::size_t{count} * 32, kFormatVersion));
    CHECK(decodeProject(file).error == LoadError::Damaged);
}

TEST(chunks_the_game_does_not_know_are_skipped) {
    const std::string meta = R"({"name":"t","view":{"x":0,"y":0,"zoom":16}})";
    const std::string board = withEmptyBoardExcept("parts", "[]");
    const std::array chunks{Chunk{{'N', 'E', 'W', '!'}, Bytes(100, 7)}, Chunk{kMeta, Bytes(meta.begin(), meta.end())},
                            Chunk{kBoard, Bytes(board.begin(), board.end())}};
    CHECK(decodeProject(packContainer(kFormatVersion, chunks)).project.has_value());
}

// --- projects --------------------------------------------------------------------------------------

TEST(a_project_comes_back_identical) {
    const Project saved = sampleProject();
    const LoadedProject loaded = decodeProject(encodeProject(saved));
    CHECK(loaded.project.has_value());
    const Project& back = *loaded.project;
    CHECK(back.board.sameContent(saved.board));
    CHECK(back.name == saved.name);
    CHECK(back.togglesOn == saved.togglesOn);
    CHECK(back.view.x == saved.view.x);
    CHECK(back.view.y == saved.view.y);
    CHECK(back.view.cellPixels == saved.view.cellPixels);
}

TEST(every_kind_of_part_survives_a_save) {
    Project project;
    for (std::size_t k = 0; k < kPartKindCount; ++k) {
        const auto kind = static_cast<PartKind>(k);
        if (kind == PartKind::Chip) continue;  // needs a chip to be made from: see the third format's tests
        project.board.addPart(kind, {static_cast<int>(k) * 20, 0}, static_cast<std::uint8_t>(k % 4), 0, 1,
                              kind == PartKind::Box ? BoxId{1} : kNoBox);
    }
    const LoadedProject loaded = decodeProject(encodeProject(project));
    CHECK(loaded.project.has_value());
    CHECK_EQ(loaded.project->board.parts().size(), kPartKindCount - 1);
    CHECK(loaded.project->board.sameContent(project.board));
}

TEST(everything_the_second_format_added_comes_back) {
    const Project saved = secondFormatProject();
    const LoadedProject loaded = decodeProject(encodeProject(saved));
    CHECK(loaded.project.has_value());
    CHECK(loaded.project->board.sameContent(saved.board));
}

TEST(saving_what_was_loaded_gives_the_same_bytes) {
    const Bytes first = encodeProject(sampleProject());
    const LoadedProject loaded = decodeProject(first);
    CHECK(encodeProject(*loaded.project) == first);
}

TEST(a_loaded_circuit_still_works) {
    const XorBoard original = xorBoard();
    Project project;
    project.board = original.board;
    const LoadedProject loaded = decodeProject(encodeProject(project));
    checkIsXor(loaded.project->board, original.a, original.b, original.lamp);
}

TEST(new_things_on_a_loaded_board_get_unused_ids) {
    Board board;
    const PartId first = *board.addPart(PartKind::Nand, {0, 0});
    const PartId second = *board.addPart(PartKind::Nand, {10, 0});
    Project project;
    project.board = board;
    LoadedProject loaded = decodeProject(encodeProject(project));
    const PartId third = *loaded.project->board.addPart(PartKind::Nand, {20, 0});
    CHECK(third != first);
    CHECK(third != second);
}

TEST(an_empty_project_is_a_valid_project) {
    const LoadedProject loaded = decodeProject(encodeProject(Project{}));
    CHECK(loaded.project.has_value());
    CHECK(loaded.project->board.parts().empty());
}

TEST(a_name_with_accents_survives_and_broken_text_does_not_stop_a_save) {
    Project project;
    project.name = "Calculadora da Conceição";
    CHECK(decodeProject(encodeProject(project)).project->name == project.name);
    project.name = "bad \xC3";
    CHECK(decodeProject(encodeProject(project)).project.has_value());
}

TEST(a_normal_file_never_records_where_the_players_files_are) {
    Project project = sampleProject();
    const std::string secret = "C:/Users/alguem/projetos/x.bancada";
    const Bytes normal = encodeProject(project);
    const Container read = unpackContainer(normal, kFormatVersion);
    const Chunk* meta = findChunk(read, kMeta);
    const std::string text(meta->data.begin(), meta->data.end());
    CHECK(text.find("origin") == std::string::npos);

    project.origin = secret;
    project.hasUnsavedChanges = true;
    const LoadedProject loaded = decodeProject(encodeProject(project));
    CHECK(loaded.project->origin == secret);
    CHECK(loaded.project->hasUnsavedChanges);
}

// A file saved by this version, kept forever. If this ever fails, old players' files just stopped
// opening: fix the code, or add a migration. Never regenerate the file to make it pass.
TEST(the_first_format_keeps_opening) {
    const fs::path path = pathFromUtf8(BANCADA_FIXTURES) / "xor-v1.bancada";
    const LoadedProject loaded = loadProject(path);
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const XorBoard original = xorBoard();
    CHECK(loaded.project->board.sameContent(original.board));
    CHECK(loaded.project->name == "meu ou exclusivo");
    checkIsXor(loaded.project->board, original.a, original.b, original.lamp);
}

TEST(the_third_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v3.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const Project expected = thirdFormatProject();
    CHECK(loaded.project->board.sameContent(expected.board));
    CHECK_EQ(loaded.project->chips.size(), 2);
    CHECK(loaded.project->chips[1].board.sameContent(expected.chips[1].board));
}

TEST(the_fourth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v4.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const Project expected = fourthFormatProject();
    CHECK(loaded.project->board.sameContent(expected.board));
    const Part& rom = loaded.project->board.parts().back();
    CHECK(rom.source == "LI R1, 7 ; la\xC3\xA7o\nOUT R1\nHLT\n");
    CHECK_EQ(rom.rom->size(), 3);
}

TEST(the_fifth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v5.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(fifthFormatProject().board));
    CHECK(loaded.project->board.parts().back().label == "Dados");
}

TEST(the_sixth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v6.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const Project expected = sixthFormatProject();
    CHECK(loaded.project->board.sameContent(expected.board));
    CHECK(loaded.project->togglesOn == expected.togglesOn);
}

TEST(the_seventh_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v7.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const Project expected = seventhFormatProject();
    CHECK(loaded.project->board.sameContent(expected.board));
    CHECK(loaded.project->togglesOn == expected.togglesOn);
}

TEST(the_eighth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v8.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(eighthFormatProject().board));
}

TEST(the_ninth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v9.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(ninthFormatProject().board));
}

TEST(the_tenth_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v10.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(tenthFormatProject().board));
}

TEST(the_parts_drawn_as_they_look_save_and_load) {
    const LoadedProject loaded = decodeProject(encodeProject(eighthFormatProject()));
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(eighthFormatProject().board));
}

TEST(an_electric_value_out_of_range_is_refused) {
    json document = json::parse(boardChunk(encodeProject(sixthFormatProject())));
    for (json& part : document.at("parts")) {
        if (part.at("kind") == "resistor") part["value"] = 0;
    }
    CHECK(decodeProject(fileWith(document.dump(), kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
}

TEST(a_label_needs_a_name) {
    json document = json::parse(boardChunk(encodeProject(fifthFormatProject())));
    document.at("parts").back().erase("label");
    CHECK(decodeProject(fileWith(document.dump(), kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
}

TEST(a_rom_keeps_its_source_only_along_with_its_words) {
    Project project = fourthFormatProject();
    CHECK(decodeProject(encodeProject(project)).project.has_value());

    json document = json::parse(boardChunk(encodeProject(project)));
    document.at("parts").back().erase("rom");
    CHECK(decodeProject(fileWith(document.dump(), kPlainMeta, kFormatVersion)).error == LoadError::Invalid);

    document = json::parse(boardChunk(encodeProject(project)));
    document.at("parts").back()["source"] = 7;
    CHECK(decodeProject(fileWith(document.dump(), kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
}

TEST(the_computer_saved_and_opened_again_still_runs_its_program) {
    const assembly::Result program = assembly::assemble(exampleProgram());
    ComputerProject computer = buildComputer16(program.words, exampleProgram());
    Project project;
    project.name = "computador";
    project.board = computer.board;
    project.chips = computer.chips;

    const LoadedProject loaded = decodeProject(encodeProject(project));
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(computer.board));
    CHECK(loaded.project->board.part(computer.rom)->source == exampleProgram());

    ChipBoards chips;
    for (const ChipData& chip : loaded.project->chips) chips[chip.id] = &chip.board;
    BoardCircuit circuit(loaded.project->board, &chips);
    for (int cycle = 0; cycle < 100 && !circuit.outputOn(computer.haltLed); ++cycle) {
        circuit.setClocks(true);
        circuit.settle();
        circuit.setClocks(false);
        circuit.settle();
    }
    CHECK_EQ(circuit.outputValue(computer.outputProbe), 55);
}

TEST(a_project_with_chips_buses_and_memory_comes_back_identical) {
    const Project saved = thirdFormatProject();
    const Bytes file = encodeProject(saved);
    const LoadedProject loaded = decodeProject(file);
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    const Project& back = *loaded.project;
    CHECK(back.board.sameContent(saved.board));
    CHECK(back.toggleValues == saved.toggleValues);
    CHECK_EQ(back.chips.size(), saved.chips.size());
    for (std::size_t i = 0; i < saved.chips.size(); ++i) {
        CHECK_EQ(back.chips[i].id, saved.chips[i].id);
        CHECK(back.chips[i].name == saved.chips[i].name);
        CHECK(back.chips[i].board.sameContent(saved.chips[i].board));
    }
    CHECK(encodeProject(back) == file);
}

TEST(a_loaded_chip_still_works) {
    const Project project = thirdFormatProject();
    LoadedProject loaded = decodeProject(encodeProject(project));
    Session session;
    session.adoptProjectChips(std::move(loaded.project->chips));
    session.adopt(std::move(loaded.project->board), loaded.project->togglesOn);
    session.setToggleValues(loaded.project->toggleValues);
    PartId probe = 0;
    for (const Part& part : session.board().parts()) {
        if (part.kind == PartKind::Probe) probe = part.id;
    }
    CHECK_EQ(session.circuit().outputValue(probe), 0xA5u & 0x0Fu);
}

TEST(chips_that_contain_each_other_are_refused) {
    const auto chip = [](int id, int other) {
        return R"({"id":)" + std::to_string(id) + R"(,"name":"c","parts":[{"id":1,"kind":"chip","at":[0,0],"rot":0,"inputs":0,"scale":1,"chip":)" +
               std::to_string(other) + R"(}],"joints":[],"wires":[],"togglesOn":[]})";
    };
    const std::string looped = R"({"parts":[],"joints":[],"wires":[],"togglesOn":[],"chips":[)" + chip(1, 2) + "," + chip(2, 1) + "]}";
    CHECK(decodeProject(fileWith(looped, kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
    const std::string self = R"({"parts":[],"joints":[],"wires":[],"togglesOn":[],"chips":[)" + chip(1, 1) + "]}";
    CHECK(decodeProject(fileWith(self, kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
    const std::string missing = R"({"parts":[],"joints":[],"wires":[],"togglesOn":[],"chips":[)" + chip(1, 9) + "]}";
    CHECK(decodeProject(fileWith(missing, kPlainMeta, kFormatVersion)).error == LoadError::Invalid);
}

TEST(impossible_parts_in_the_third_format_are_refused) {
    const auto errorIn = [](const std::string& part) {
        const std::string board = R"({"parts":[)" + part + R"(],"joints":[],"wires":[],"togglesOn":[],"chips":[]})";
        return decodeProject(fileWith(board, kPlainMeta, kFormatVersion)).error;
    };
    CHECK(!errorIn(R"({"id":1,"kind":"toggle","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":64})").has_value());  // the control
    CHECK(!errorIn(R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"rom","width":8,"address":2,"rom":[1,2,3,4]})").has_value());
    const std::vector<std::string> bad{
        R"({"id":1,"kind":"toggle","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":65})",
        R"({"id":1,"kind":"toggle","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":0})",
        R"({"id":1,"kind":"led","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":8})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"mux2","width":8})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"ram","width":8,"address":21})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"ram","width":8})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"rom","width":8,"address":1,"rom":[1,2,3]})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"register","width":8,"rom":[1]})",
        R"({"id":1,"kind":"splitter","at":[0,0],"rot":0,"inputs":9,"scale":1,"width":8})",
        R"({"id":1,"kind":"chip","at":[0,0],"rot":0,"inputs":0,"scale":1})",
        R"({"id":1,"kind":"nand","at":[0,0],"rot":0,"inputs":2,"scale":1,"chip":1})",
        R"({"id":1,"kind":"toggle","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":300})",
    };
    for (const std::string& part : bad) {
        const std::optional<LoadError> error = errorIn(part);
        CHECK(error == LoadError::Invalid);
        if (error != LoadError::Invalid) std::printf("  accepted: %s\n", part.c_str());
    }
}

TEST(a_many_bit_switch_value_is_cut_to_its_width_when_read) {
    const std::string board =
        R"({"parts":[{"id":1,"kind":"toggle","at":[0,0],"rot":0,"inputs":0,"scale":1,"width":4}],"joints":[],"wires":[],"togglesOn":[],"toggleValues":[[1,255]],"chips":[]})";
    const LoadedProject loaded = decodeProject(fileWith(board, kPlainMeta, kFormatVersion));
    CHECK(loaded.project.has_value());
    CHECK_EQ(loaded.project->toggleValues[0].second, 15);
}

TEST(the_second_format_keeps_opening) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "bancada-v2.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    CHECK(loaded.project->board.sameContent(secondFormatProject().board));
}

TEST(a_first_format_lamp_becomes_an_led_and_parts_get_their_inputs) {
    const LoadedProject loaded = loadProject(pathFromUtf8(BANCADA_FIXTURES) / "xor-v1.bancada");
    CHECK(loaded.project.has_value());
    if (!loaded.project) return;
    int leds = 0;
    for (const Part& part : loaded.project->board.parts()) {
        CHECK_EQ(part.scale, 1);
        if (part.kind == PartKind::Led) ++leds;
        if (part.kind == PartKind::Nand) CHECK_EQ(part.inputs, 2);
        if (part.kind == PartKind::Toggle) CHECK_EQ(part.inputs, 0);
    }
    CHECK_EQ(leds, 1);
    CHECK(encodeProject(*loaded.project) != encodeProject(Project{}));
    CHECK(decodeProject(encodeProject(*loaded.project)).project.has_value());  // saved again as version 2
}

// --- what a file may not say -----------------------------------------------------------------------

TEST(impossible_shapes_in_the_second_format_are_refused) {
    const auto board = [](const std::string& part) {
        return R"({"parts":[)" + part + R"(],"joints":[],"wires":[],"togglesOn":[]})";
    };
    const auto errorIn = [](const std::string& text) { return decodeProject(fileWith(text, kPlainMeta, 2)).error; };

    CHECK(!errorIn(board(R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":8,"scale":3})")).has_value());  // the control
    CHECK(!errorIn(board(R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"mux2"})")).has_value());

    const std::vector<std::string> bad{
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":9,"scale":1})",
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":1,"scale":1})",
        R"({"id":1,"kind":"not","at":[0,0],"rot":0,"inputs":2,"scale":1})",
        R"({"id":1,"kind":"led","at":[0,0],"rot":0,"inputs":1,"scale":1})",
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":2,"scale":0})",
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":2,"scale":4})",
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"inputs":2})",
        R"({"id":1,"kind":"and","at":[0,0],"rot":0,"scale":1})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1})",
        R"({"id":1,"kind":"box","at":[0,0],"rot":0,"inputs":0,"scale":1,"box":"nope"})",
        R"({"id":1,"kind":"lamp","at":[0,0],"rot":0,"inputs":0,"scale":1})",
    };
    for (const std::string& part : bad) {
        const std::optional<LoadError> error = errorIn(board(part));
        CHECK(error == LoadError::Invalid);
        if (error != LoadError::Invalid) std::printf("  accepted: %s\n", part.c_str());
    }
}

TEST(impossible_boards_are_refused) {
    const std::string part = R"({"id":1,"kind":"nand","at":[0,0],"rot":0})";
    const std::string lamp = R"({"id":2,"kind":"lamp","at":[20,0],"rot":0})";
    const std::string toggle = R"({"id":1,"kind":"toggle","at":[0,0],"rot":0})";
    const std::string wire = R"({"id":1,"from":{"part":1,"pin":0},"to":{"part":2,"pin":0},"path":[[3,1],[20,1]]})";
    const auto board = [](const std::string& parts, const std::string& joints, const std::string& wires) {
        return R"({"parts":[)" + parts + R"(],"joints":[)" + joints + R"(],"wires":[)" + wires + R"(],"togglesOn":[]})";
    };

    CHECK(!errorOf(board(toggle + "," + lamp, "", wire)).has_value());  // the control: this one is fine

    const std::vector<std::string> bad{
        board(part + "," + R"({"id":2,"kind":"nand","at":[1,1],"rot":0})", "", ""),          // parts overlap
        board(R"({"id":2,"kind":"nand","at":[0,0],"rot":0},{"id":1,"kind":"nand","at":[9,0],"rot":0})", "", ""),  // ids out of order
        board(part + "," + R"({"id":1,"kind":"nand","at":[9,0],"rot":0})", "", ""),          // repeated id
        board(R"({"id":0,"kind":"nand","at":[0,0],"rot":0})", "", ""),                       // id zero
        board(R"({"id":1,"kind":"robot","at":[0,0],"rot":0})", "", ""),                      // unknown part
        board(R"({"id":1,"kind":"nand","at":[0,0],"rot":4})", "", ""),                       // bad rotation
        board(R"({"id":1,"kind":"nand","at":[999999999,0],"rot":0})", "", ""),               // too far away
        board(R"({"id":1,"kind":"nand","at":[-9223372036854775808,0],"rot":0})", "", ""),    // the most negative number
        board(R"({"id":1,"kind":"nand","at":[18446744073709551615,0],"rot":0})", "", ""),    // the largest number
        board(R"({"id":1,"kind":"nand","at":[1.5,0],"rot":0})", "", ""),                     // a decimal
        board(R"({"id":1,"kind":"nand","at":[1e308,0],"rot":0})", "", ""),                   // a huge decimal
        board(R"({"id":1.5,"kind":"nand","at":[0,0],"rot":0})", "", ""),                     // a decimal id
        board(R"({"id":1,"kind":"nand","at":[0,0],"rot":1.5})", "", ""),                     // a decimal rotation
        board(R"({"id":1,"kind":"nand","at":[0.5,0],"rot":0})", "", "") + "x",               // not even JSON
        board(R"({"id":1,"kind":"nand","at":[0],"rot":0})", "", ""),                         // a point with one number
        board(R"({"id":"1","kind":"nand","at":[0,0],"rot":0})", "", ""),                     // id of the wrong type
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"part":1,"pin":0},"to":{"part":2,"pin":0},"path":[[3,1],[20,2]]})"),  // diagonal
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"part":1,"pin":0},"to":{"part":2,"pin":0},"path":[[3,1],[19,1]]})"),  // misses the pin
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"part":1,"pin":7},"to":{"part":2,"pin":0},"path":[[3,1],[20,1]]})"),  // no such pin
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"part":9,"pin":0},"to":{"part":2,"pin":0},"path":[[3,1],[20,1]]})"),  // no such part
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"joint":1},"to":{"part":2,"pin":0},"path":[[3,1],[20,1]]})"),         // no such joint
        board(toggle + "," + lamp, "", R"({"id":1,"from":{"part":1,"pin":0},"to":{"part":2,"pin":0},"path":[[3,1]]})"),         // one point
        board(toggle + "," + lamp, "", R"({"id":1,"from":{},"to":{"part":2,"pin":0},"path":[[3,1],[20,1]]})"),                  // empty terminal
        board("", R"({"id":1,"at":[0,0]})", ""),                                             // a joint nothing uses
        R"({"parts":[]})",                                                                   // missing lists
        R"([1,2,3])",                                                                        // not an object
        R"(null)",
        withEmptyBoardExcept("togglesOn", "[5]"),                                            // a switch that isn't there
        withEmptyBoardExcept("parts", R"({"a":1})"),                                         // an object where a list goes
    };
    for (const std::string& text : bad) {
        const std::optional<LoadError> error = errorOf(text);
        CHECK(error == LoadError::Invalid);
        if (error != LoadError::Invalid) std::printf("  accepted: %s\n", text.c_str());
    }
}

TEST(a_switch_on_a_part_that_is_not_a_switch_is_refused) {
    const std::string board =
        R"({"parts":[{"id":1,"kind":"nand","at":[0,0],"rot":0}],"joints":[],"wires":[],"togglesOn":[1]})";
    CHECK(errorOf(board) == LoadError::Invalid);
}

TEST(an_impossible_view_falls_back_to_a_sensible_one) {
    const std::string board = withEmptyBoardExcept("parts", "[]");
    const LoadedProject loaded = decodeProject(fileWith(board, R"({"name":"t","view":{"x":1e308,"y":-1e308,"zoom":1e308}})"));
    CHECK(loaded.project.has_value());
    const ProjectView standard;
    CHECK(loaded.project->view.x == standard.x);
    CHECK(loaded.project->view.y == standard.y);
    CHECK(loaded.project->view.cellPixels == standard.cellPixels);
}

TEST(a_file_with_a_missing_chunk_is_refused) {
    const std::array chunks{Chunk{kMeta, Bytes{'{', '}'}}};
    CHECK(decodeProject(packContainer(kFormatVersion, chunks)).error == LoadError::Invalid);
}

// Hashes stop random damage long before the parser sees it, so this goes around them, re-hashing
// mangled JSON into valid files: the part of a load that a deliberately crafted file reaches.
TEST(mangled_content_never_crashes_and_never_gives_a_broken_board) {
    const Container original = unpackContainer(encodeProject(thirdFormatProject()), kFormatVersion);
    const std::string board(findChunk(original, kBoard)->data.begin(), findChunk(original, kBoard)->data.end());
    int accepted = 0;
    constexpr const char* kFragments[] = {"0", "-1", "1", "99999999999999999999", "null", "[]", "{}", "\"\"", ",", "]", "}", "true", "1.5", "1e308"};
    for (std::uint32_t seed = 1; seed <= 4000; ++seed) {
        std::mt19937 random(seed);
        std::string text = board;
        const auto edits = static_cast<std::uint32_t>(1 + random() % 3);
        for (std::uint32_t i = 0; i < edits; ++i) {
            const std::size_t at = random() % text.size();
            switch (random() % 3) {
                case 0:
                    text[at] = static_cast<char>(random());
                    break;
                case 1:
                    text.erase(at, 1 + random() % 8);
                    break;
                default:
                    text.insert(at, kFragments[random() % std::size(kFragments)]);
                    break;
            }
        }
        const LoadedProject loaded = decodeProject(fileWith(text, kPlainMeta, kFormatVersion));
        if (!loaded.project) {
            CHECK(loaded.error == LoadError::Invalid);
            continue;
        }
        ++accepted;
        for (const Wire& wire : loaded.project->board.wires()) {
            CHECK(wire.path.front() == loaded.project->board.terminalPosition(wire.from));
            CHECK(wire.path.back() == loaded.project->board.terminalPosition(wire.to));
        }
        const BoardCircuit circuit(loaded.project->board);
        (void)circuit;
    }
    std::printf("  (%d of 4000 mangled files were still valid projects)\n", accepted);
}

// --- migrations ------------------------------------------------------------------------------------

TEST(migrations_run_in_order_from_the_files_version) {
    std::vector<MigrationStep> steps;
    steps.emplace_back([](json& d) { d["log"].push_back("1to2"); });
    steps.emplace_back([](json& d) { d["log"].push_back("2to3"); });
    steps.emplace_back([](json& d) { d["log"].push_back("3to4"); });
    json fromOne = {{"log", json::array()}};
    CHECK(migrateDocument(fromOne, 1, 4, steps));
    CHECK(fromOne["log"] == json::array({"1to2", "2to3", "3to4"}));

    json fromThree = {{"log", json::array()}};
    CHECK(migrateDocument(fromThree, 3, 4, steps));
    CHECK(fromThree["log"] == json::array({"3to4"}));

    json current = {{"log", json::array()}};
    CHECK(migrateDocument(current, 4, 4, steps));
    CHECK(current["log"].empty());
}

TEST(a_migration_path_with_a_missing_step_is_refused) {
    std::vector<MigrationStep> steps;
    steps.emplace_back([](json&) {});
    json document = json::object();
    CHECK(!migrateDocument(document, 1, 3, steps));
    CHECK(!migrateDocument(document, 0, 1, steps));
    CHECK(!migrateDocument(document, 2, 1, steps));
}

TEST(migration_steps_exist_for_every_version_up_to_the_current_one) {
    // Bumping kFormatVersion without adding a step would make every older file unopenable.
    const Bytes current = encodeProject(sampleProject());
    CHECK(decodeProject(current).project.has_value());
}

// --- files on disk ---------------------------------------------------------------------------------

TEST(a_project_saves_and_loads_through_a_folder_with_accents) {
    const fs::path path = testFolder("salvar") / pathFromUtf8("projeto é.bancada");
    const Project project = sampleProject();
    CHECK(saveProject(path, project));
    const LoadedProject loaded = loadProject(path);
    CHECK(loaded.project.has_value());
    CHECK(loaded.project->board.sameContent(project.board));
}

TEST(saving_again_replaces_the_file_and_leaves_nothing_else_behind) {
    const fs::path folder = testFolder("substituir");
    const fs::path path = folder / "p.bancada";
    Project project;
    project.name = "um";
    CHECK(saveProject(path, project));
    project.name = "dois";
    CHECK(saveProject(path, project));
    CHECK(loadProject(path).project->name == "dois");
    int files = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder)) ++files;
    CHECK_EQ(files, 1);
}

TEST(a_failed_save_leaves_the_old_file_alone) {
    const fs::path folder = testFolder("falha");
    const fs::path path = folder / "p.bancada";
    Project project;
    project.name = "original";
    CHECK(saveProject(path, project));

    // A folder sitting where the temporary file has to go: the write can't happen.
    fs::create_directory(fs::path(path) += ".tmp");
    project.name = "novo";
    CHECK(!saveProject(path, project));
    CHECK(loadProject(path).project->name == "original");
}

TEST(saving_into_a_folder_that_does_not_exist_fails_cleanly) {
    CHECK(!saveProject(testFolder("sem") / "nao" / "existe" / "p.bancada", Project{}));
}

TEST(a_missing_file_says_it_could_not_be_read) {
    CHECK(loadProject(testFolder("nada") / "p.bancada").error == LoadError::CannotRead);
}

// --- profiles --------------------------------------------------------------------------------------

TEST(the_first_run_makes_one_profile) {
    const ProfileStore store(testFolder("perfil1"));
    CHECK_EQ(store.profiles().size(), 1);
    CHECK(store.current().name == "Jogador 1");
    CHECK(fs::is_directory(store.projectsFolder()));
}

TEST(profiles_and_the_last_choice_survive_a_restart) {
    const fs::path data = testFolder("perfil2");
    std::string secondId;
    {
        ProfileStore store(data);
        CHECK(store.create("Conceição"));
        secondId = store.current().id;
        CHECK(store.create("Filho"));
        CHECK(store.select(secondId));
    }
    const ProfileStore again(data);
    CHECK_EQ(again.profiles().size(), 3);
    CHECK(again.current().id == secondId);
    CHECK(again.current().name == "Conceição");
}

TEST(each_profile_has_its_own_files) {
    ProfileStore store(testFolder("perfil3"));
    const fs::path first = store.projectsFolder();
    const fs::path firstAutosave = store.autosavePath();
    CHECK(store.create("Segundo"));
    CHECK(store.projectsFolder() != first);
    CHECK(store.autosavePath() != firstAutosave);
}

TEST(a_name_never_decides_where_files_go) {
    const fs::path data = testFolder("perfil4");
    ProfileStore store(data);
    CHECK(store.create("../../fora"));
    CHECK(store.current().name == "../../fora");
    const std::string inside = pathToUtf8(fs::relative(store.projectsFolder(), data));
    CHECK(inside.find("..") == std::string::npos);
}

TEST(lesson_progress_is_kept_per_profile_and_can_be_reset) {
    const fs::path data = testFolder("perfil-aulas");
    ProfileStore store(data);
    CHECK(store.finishedLessons().empty());
    CHECK(store.finishLesson("acender-led"));
    CHECK(store.finishLesson("chave"));
    CHECK(store.finishLesson("chave"));  // twice is once
    CHECK(store.finishedLessons() == std::vector<std::string>({"acender-led", "chave"}));
    fs::create_directories(store.lessonPath("chave").parent_path());
    CHECK(saveProject(store.lessonPath("chave"), sampleProject()));
    CHECK(store.create("Outra pessoa"));
    CHECK(store.finishedLessons().empty());  // another profile starts from nothing
    CHECK(store.select(store.profiles().front().id));
    CHECK(store.resetLessons());
    CHECK(store.finishedLessons().empty());
    CHECK(!fs::exists(store.lessonPath("chave")));
}

TEST(the_language_is_kept_for_every_profile) {
    const fs::path data = testFolder("perfil-idioma");
    {
        ProfileStore store(data);
        CHECK(store.language().empty());  // none chosen yet: the game goes by the computer's
        CHECK(store.setLanguage("en"));
        CHECK(store.create("Outra pessoa"));
    }
    ProfileStore again(data);
    CHECK(again.language() == "en");
}

TEST(an_empty_name_and_an_unknown_profile_are_refused) {
    ProfileStore store(testFolder("perfil5"));
    CHECK(!store.create(""));
    CHECK(!store.select("p99"));
    CHECK_EQ(store.profiles().size(), 1);
}

TEST(damaged_profile_files_do_not_stop_the_game) {
    const fs::path data = testFolder("perfil6");
    {
        ProfileStore store(data);
        store.create("Bom");
    }
    const Bytes junk{'n', 'o', 't', ' ', 'j', 's', 'o', 'n'};
    CHECK(writeFileAtomically(data / "perfis" / "p1" / "perfil.json", junk));
    CHECK(writeFileAtomically(data / "estado.json", junk));
    const ProfileStore store(data);
    CHECK_EQ(store.profiles().size(), 1);
    CHECK(store.current().name == "Bom");
}

// --- session ---------------------------------------------------------------------------------------

TEST(a_session_knows_when_it_has_unsaved_changes) {
    Session session;
    CHECK(!session.dirty());
    session.beginPlacing(PartKind::Toggle);
    session.mouseMove(4, 4);
    session.leftDown();
    session.leftUp();
    session.key(Key::Escape);
    CHECK(session.dirty());
    session.markSaved();
    CHECK(!session.dirty());
    session.key(Key::Undo);
    CHECK(session.dirty());
}

TEST(flipping_a_switch_counts_as_a_change_worth_saving) {
    Project project = sampleProject();
    Session session;
    session.adopt(project.board, project.togglesOn);
    CHECK(!session.dirty());
    const PartId toggle = project.board.parts()[1].id;
    const Rect bounds = boundsOf(*session.board().part(toggle));
    session.mouseMove(static_cast<float>(bounds.left + bounds.right) / 2.0f, static_cast<float>(bounds.top + bounds.bottom) / 2.0f);
    session.leftDown();
    session.leftUp();
    CHECK(session.dirty());
    CHECK_EQ(session.togglesOn().size(), 2);
}

TEST(a_session_adopting_a_project_shows_its_switches_and_forgets_undo) {
    const XorBoard original = xorBoard();
    const std::array on{original.a};
    Session session;
    session.adopt(original.board, on);
    CHECK(session.circuit().outputOn(original.lamp));
    CHECK(session.togglesOn() == std::vector<PartId>{original.a});
    CHECK_EQ(session.undoSteps(), 0);
    session.adopt(original.board, on, false);
    CHECK(session.dirty());
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--write-fixture") == 0) {
        const fs::path folder = pathFromUtf8(BANCADA_FIXTURES);
        fs::create_directories(folder);
        // Only the current version's file: older ones were written by older code, and are kept as they are.
        // Refuses to replace one that exists, so that no build, however old, can ever rewrite a fixture.
        const fs::path file = folder / "bancada-v10.bancada";
        if (fs::exists(file)) {
            std::fprintf(stderr, "%s already exists: fixtures are never replaced\n", pathToUtf8(file).c_str());
            return 1;
        }
        return saveProject(file, tenthFormatProject()) ? 0 : 1;
    }
    return check::runAll();
}
