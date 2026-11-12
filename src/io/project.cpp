#include "io/project.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <utility>

#include "io/files.hpp"

namespace bancada::io {

namespace {

using nlohmann::json;
using namespace bancada::editor;

constexpr ChunkTag kMetaTag{'M', 'E', 'T', 'A'};
constexpr ChunkTag kBoardTag{'B', 'O', 'R', 'D'};
constexpr std::uint64_t kMaxFileSize = std::uint64_t{1} << 31;

// Version 2: parts got a number of inputs and a size, and "lamp" became "led".
void upgradeToVersion2(json& document) {
    for (json& part : document.at("board").at("parts")) {
        std::string kind = part.at("kind").get<std::string>();
        if (kind == "lamp") kind = "led";
        part["kind"] = kind;
        part["inputs"] = kind == "not" ? 1 : kind == "toggle" || kind == "led" ? 0 : 2;
        part["scale"] = 1;
    }
}

// Version 3: projects got chips. Everything new in a part is optional, so nothing else changes.
void upgradeToVersion3(json& document) { document.at("board")["chips"] = json::array(); }

// Nothing to convert: version 4 only adds things older files don't have, a ROM's source text and splitters
// of up to 32 branches. It's a new version because an older game would lose the one and refuse the other.
void upgradeToVersion4(json&) {}

// Nothing to convert either: version 5 only adds a kind of part.
void upgradeToVersion5(json&) {}

// Nor here: version 6 adds the electric parts.
void upgradeToVersion6(json&) {}

// Version 7 added the LED drawn as it looks, and version 8 the battery drawn as it looks. Nothing older changes.
void upgradeToVersion7(json&) {}
void upgradeToVersion8(json&) {}
// Version 9 added the button and the seven-segment display.
void upgradeToVersion9(json&) {}
// Version 10 added the keyboard.
void upgradeToVersion10(json&) {}

const std::vector<MigrationStep>& migrationSteps() {
    static const std::vector<MigrationStep> steps{upgradeToVersion2, upgradeToVersion3, upgradeToVersion4, upgradeToVersion5,
                                                  upgradeToVersion6, upgradeToVersion7, upgradeToVersion8, upgradeToVersion9, upgradeToVersion10};
    return steps;
}

// Names, not numbers, so reordering an enum can never change what an old file means.
constexpr std::array<const char*, kPartKindCount> kKindNames{
    "nand", "toggle", "led", "not", "and", "or", "nor", "xor", "xnor", "probe", "box", "splitter", "joiner", "constant", "clock", "chip", "tunnel",
    "battery", "ground", "resistor", "diode", "light", "switch", "npn", "pnp", "nmos", "pmos", "capacitor", "led_real", "battery_real", "button", "display7", "keyboard"};

const char* kindName(PartKind kind) { return kKindNames[static_cast<std::size_t>(kind)]; }

std::optional<PartKind> kindFromName(const std::string& name) {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (name == kKindNames[i]) return static_cast<PartKind>(i);
    }
    return std::nullopt;
}

json pointToJson(Point point) { return json::array({point.x, point.y}); }

json terminalToJson(const Terminal& terminal) {
    if (terminal.kind == Terminal::Kind::Pin) return {{"part", terminal.id}, {"pin", terminal.pin}};
    return {{"joint", terminal.id}};
}

json partToJson(const Part& part) {
    json out = {{"id", part.id},
                {"kind", kindName(part.kind)},
                {"at", pointToJson(part.position)},
                {"rot", part.rotation},
                {"inputs", part.inputs},
                {"scale", part.scale}};
    if (part.kind == PartKind::Box) out["box"] = boxSpec(part.box).key;
    if (part.width != 1) out["width"] = part.width;
    if (part.address != 0) out["address"] = part.address;
    if (part.value != 0) out["value"] = part.value;
    if (part.chip != 0) out["chip"] = part.chip;
    if (!part.label.empty()) out["label"] = part.label;
    if (part.rom != nullptr) out["rom"] = *part.rom;
    if (!part.source.empty()) out["source"] = part.source;
    return out;
}

json circuitToJson(const Board& board, const std::vector<PartId>& togglesOn, const std::vector<std::pair<PartId, std::uint64_t>>& values) {
    json parts = json::array();
    for (const Part& part : board.parts()) parts.push_back(partToJson(part));
    json joints = json::array();
    for (const Joint& joint : board.joints()) {
        joints.push_back({{"id", joint.id}, {"at", pointToJson(joint.position)}});
    }
    json wires = json::array();
    for (const Wire& wire : board.wires()) {
        json path = json::array();
        for (const Point& point : wire.path) path.push_back(pointToJson(point));
        wires.push_back({{"id", wire.id}, {"from", terminalToJson(wire.from)}, {"to", terminalToJson(wire.to)}, {"path", std::move(path)}});
    }
    json out = {{"parts", std::move(parts)}, {"joints", std::move(joints)}, {"wires", std::move(wires)}, {"togglesOn", togglesOn}};
    if (!values.empty()) {
        json valueList = json::array();
        for (const auto& [id, value] : values) valueList.push_back(json::array({id, value}));
        out["toggleValues"] = std::move(valueList);
    }
    return out;
}

json boardToJson(const Project& project) {
    json out = circuitToJson(project.board, project.togglesOn, project.toggleValues);
    json chips = json::array();
    for (const ChipData& chip : project.chips) {
        std::vector<std::pair<PartId, std::uint64_t>> values(chip.toggles.begin(), chip.toggles.end());
        std::sort(values.begin(), values.end());
        json item = circuitToJson(chip.board, {}, values);
        item["id"] = chip.id;
        item["name"] = chip.name;
        chips.push_back(std::move(item));
    }
    out["chips"] = std::move(chips);
    return out;
}

json metaToJson(const Project& project) {
    json meta = {{"name", project.name},
                 {"view", {{"x", project.view.x}, {"y", project.view.y}, {"zoom", project.view.cellPixels}}}};
    if (project.origin) meta["origin"] = *project.origin;
    if (project.hasUnsavedChanges) meta["unsaved"] = true;
    return meta;
}

Bytes textBytes(const json& document) {
    // A name with broken UTF-8 must not stop a save.
    const std::string text = document.dump(-1, ' ', false, json::error_handler_t::replace);
    return Bytes(text.begin(), text.end());
}

// All of these throw on anything unexpected, and decodeProject turns that into LoadError::Invalid.
std::int64_t integer(const json& value) {
    // Decimals don't count: converting a huge one to an integer would be undefined behaviour.
    if (!value.is_number_integer()) throw std::out_of_range("not an integer");
    // Above this, reading it as signed would wrap around into an innocent-looking small number.
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        throw std::out_of_range("integer too large");
    }
    return value.get<std::int64_t>();
}

bool isCoordinate(std::int64_t value) { return value >= -kMaxCoordinate && value <= kMaxCoordinate; }

std::uint64_t word(const json& value) {
    if (!value.is_number_integer()) throw std::out_of_range("not an integer");
    return value.get<std::uint64_t>();
}

std::uint8_t smallField(const json& item, const char* key, std::uint8_t fallback) {
    const auto found = item.find(key);
    if (found == item.end()) return fallback;
    const std::int64_t value = integer(*found);
    if (value < 0 || value > 255) throw std::out_of_range("field out of range");
    return static_cast<std::uint8_t>(value);
}

std::uint32_t idFrom(const json& value) {
    const std::int64_t id = integer(value);
    if (id < 1 || id > INT32_MAX) throw std::out_of_range("bad id");
    return static_cast<std::uint32_t>(id);
}

Point pointFrom(const json& value) {
    if (!value.is_array() || value.size() != 2) throw std::out_of_range("bad point");
    const std::int64_t x = integer(value[0]);
    const std::int64_t y = integer(value[1]);
    if (!isCoordinate(x) || !isCoordinate(y)) throw std::out_of_range("point too far");
    return {static_cast<int>(x), static_cast<int>(y)};
}

Terminal terminalFrom(const json& value) {
    if (value.contains("joint")) return {Terminal::Kind::Joint, idFrom(value.at("joint")), 0};
    const std::int64_t pin = integer(value.at("pin"));
    if (pin < 0 || pin > 255) throw std::out_of_range("bad pin");
    return {Terminal::Kind::Pin, idFrom(value.at("part")), static_cast<std::uint8_t>(pin)};
}

struct RawCircuit {
    std::vector<Part> parts;
    std::vector<Joint> joints;
    std::vector<Wire> wires;
};

RawCircuit rawCircuitFrom(const json& document) {
    RawCircuit raw;
    for (const json& item : document.at("parts")) {
        const std::optional<PartKind> kind = kindFromName(item.at("kind").get<std::string>());
        const std::int64_t rotation = integer(item.at("rot"));
        const std::int64_t inputs = integer(item.at("inputs"));
        const std::int64_t scale = integer(item.at("scale"));
        if (!kind || rotation < 0 || rotation > 3 || inputs < 0 || inputs > 255 || scale < 0 || scale > 255) {
            throw std::out_of_range("bad part");
        }
        BoxId box = kNoBox;
        if (*kind == PartKind::Box) {
            box = boxFromKey(item.at("box").get<std::string>());
            if (box == kNoBox) throw std::out_of_range("unknown box");
        }
        Part part;
        part.id = idFrom(item.at("id"));
        part.kind = *kind;
        part.position = pointFrom(item.at("at"));
        part.rotation = static_cast<std::uint8_t>(rotation);
        part.inputs = static_cast<std::uint8_t>(inputs);
        part.scale = static_cast<std::uint8_t>(scale);
        part.box = box;
        part.width = smallField(item, "width", 1);
        part.address = smallField(item, "address", 0);
        part.value = item.contains("value") ? word(item.at("value")) : 0;
        part.chip = item.contains("chip") ? idFrom(item.at("chip")) : 0;
        part.label = item.value("label", std::string{});
        part.source = item.value("source", std::string{});
        if (const auto rom = item.find("rom"); rom != item.end()) {
            std::vector<std::uint64_t> words;
            for (const json& value : *rom) words.push_back(word(value));
            part.rom = std::make_shared<const std::vector<std::uint64_t>>(std::move(words));
        }
        raw.parts.push_back(std::move(part));
    }
    for (const json& item : document.at("joints")) raw.joints.push_back({idFrom(item.at("id")), pointFrom(item.at("at"))});
    for (const json& item : document.at("wires")) {
        Wire wire;
        wire.id = idFrom(item.at("id"));
        wire.from = terminalFrom(item.at("from"));
        wire.to = terminalFrom(item.at("to"));
        for (const json& point : item.at("path")) wire.path.push_back(pointFrom(point));
        raw.wires.push_back(std::move(wire));
    }
    return raw;
}

// Boards of a project, built chips first, since a board needs the pins of the chips on it.
class ChipResolver {
public:
    explicit ChipResolver(const json& chips) {
        for (const json& item : chips) {
            const ChipId id = idFrom(item.at("id"));
            Entry entry;
            entry.raw = rawCircuitFrom(item);
            entry.name = item.at("name").get<std::string>();
            if (!raw_.emplace(id, std::move(entry)).second) throw std::out_of_range("repeated chip");
        }
    }

    std::vector<ChipId> ids() const {
        std::vector<ChipId> out;
        for (const auto& [id, entry] : raw_) out.push_back(id);
        return out;
    }

    const std::string& name(ChipId id) const { return raw_.at(id).name; }

    Board& board(ChipId id) {
        Entry& entry = raw_.at(id);
        if (entry.done) return entry.board;
        if (entry.visiting) throw std::out_of_range("a chip inside itself");
        entry.visiting = true;
        std::optional<Board> built = restored(entry.raw);
        if (!built) throw std::out_of_range("bad chip");
        entry.board = std::move(*built);
        entry.interface = makeChipInterface(entry.board, entry.name);
        entry.done = true;
        entry.visiting = false;
        return entry.board;
    }

    std::optional<Board> restored(RawCircuit& raw) {
        for (Part& part : raw.parts) {
            if (part.kind != PartKind::Chip) continue;
            const auto found = raw_.find(part.chip);
            if (found == raw_.end()) return std::nullopt;
            board(part.chip);
            part.interface = found->second.interface;
        }
        return Board::restore(std::move(raw.parts), std::move(raw.joints), std::move(raw.wires));
    }

private:
    struct Entry {
        RawCircuit raw;
        std::string name;
        Board board;
        InterfacePtr interface;
        bool visiting = false;
        bool done = false;
    };
    std::map<ChipId, Entry> raw_;
};

float finiteOr(const json& value, float fallback) {
    const double number = value.get<double>();
    return std::isfinite(number) && std::abs(number) <= kMaxCoordinate ? static_cast<float>(number) : fallback;
}

std::uint64_t maskFor(const Part& part) {
    return part.width >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << part.width) - 1;
}

void readToggleValues(const json& circuit, const Board& board, std::vector<std::pair<PartId, std::uint64_t>>& out) {
    const auto list = circuit.find("toggleValues");
    if (list == circuit.end()) return;
    for (const json& item : *list) {
        const PartId id = idFrom(item.at(0));
        const Part* part = board.part(id);
        if (part == nullptr || (part->kind != PartKind::Toggle && part->kind != PartKind::Switch && part->kind != PartKind::Button && part->kind != PartKind::Keyboard)) throw std::out_of_range("not a toggle");
        out.emplace_back(id, word(item.at(1)) & maskFor(*part));
    }
}

Project projectFrom(const json& document) {
    const json& boardDocument = document.at("board");
    ChipResolver chips(boardDocument.at("chips"));

    RawCircuit mainRaw = rawCircuitFrom(boardDocument);
    std::optional<Board> mainBoard = chips.restored(mainRaw);
    if (!mainBoard) throw std::out_of_range("bad board");

    Project project;
    project.board = std::move(*mainBoard);

    const json& meta = document.at("meta");
    project.name = meta.at("name").get<std::string>();
    const json& view = meta.at("view");
    project.view.x = finiteOr(view.at("x"), project.view.x);
    project.view.y = finiteOr(view.at("y"), project.view.y);
    project.view.cellPixels = finiteOr(view.at("zoom"), project.view.cellPixels);
    if (const auto origin = meta.find("origin"); origin != meta.end()) project.origin = origin->get<std::string>();
    project.hasUnsavedChanges = meta.value("unsaved", false);

    for (const json& id : boardDocument.at("togglesOn")) {
        const std::uint32_t part = idFrom(id);
        const Part* found = project.board.part(part);
        if (found == nullptr || (found->kind != PartKind::Toggle && found->kind != PartKind::Switch && found->kind != PartKind::Button && found->kind != PartKind::Keyboard)) throw std::out_of_range("not a toggle");
        project.togglesOn.push_back(part);
    }
    readToggleValues(boardDocument, project.board, project.toggleValues);

    for (const json& item : boardDocument.at("chips")) {
        const ChipId id = idFrom(item.at("id"));
        ChipData chip;
        chip.id = id;
        chip.name = chips.name(id);
        chip.board = chips.board(id);
        std::vector<std::pair<PartId, std::uint64_t>> values;
        readToggleValues(item, chip.board, values);
        for (const auto& [part, value] : values) chip.toggles[part] = value;
        project.chips.push_back(std::move(chip));
    }
    return project;
}

}  // namespace

bool migrateDocument(json& document, std::uint32_t fromVersion, std::uint32_t toVersion,
                     std::span<const MigrationStep> steps) {
    if (fromVersion == 0 || toVersion < fromVersion) return false;
    if (toVersion - fromVersion > steps.size() || toVersion - 1 > steps.size()) return false;
    for (std::uint32_t version = fromVersion; version < toVersion; ++version) steps[version - 1](document);
    return true;
}

Bytes encodeProject(const Project& project) {
    const std::array chunks{Chunk{kMetaTag, textBytes(metaToJson(project))}, Chunk{kBoardTag, textBytes(boardToJson(project))}};
    return packContainer(kFormatVersion, chunks);
}

LoadedProject decodeProject(std::span<const std::uint8_t> file) {
    const Container container = unpackContainer(file, kFormatVersion);
    if (container.error) return {std::nullopt, container.error};

    const Chunk* meta = findChunk(container, kMetaTag);
    const Chunk* board = findChunk(container, kBoardTag);
    if (meta == nullptr || board == nullptr) return {std::nullopt, LoadError::Invalid};

    try {
        json document;
        document["meta"] = json::parse(meta->data.begin(), meta->data.end());
        document["board"] = json::parse(board->data.begin(), board->data.end());
        if (!migrateDocument(document, container.version, kFormatVersion, migrationSteps())) {
            return {std::nullopt, LoadError::Invalid};
        }
        return {projectFrom(document), std::nullopt};
    } catch (const std::exception&) {
        return {std::nullopt, LoadError::Invalid};
    }
}

bool saveProject(const std::filesystem::path& path, const Project& project) {
    const Bytes bytes = encodeProject(project);
    return writeFileAtomically(path, bytes);
}

LoadedProject loadProject(const std::filesystem::path& path) {
    const std::optional<Bytes> bytes = readWholeFile(path, kMaxFileSize);
    if (!bytes) return {std::nullopt, LoadError::CannotRead};
    return decodeProject(*bytes);
}

const char* describe(LoadError error) {
    switch (error) {
        case LoadError::CannotRead: return "Nao deu para abrir o arquivo.";
        case LoadError::NotOurFile: return "Esse arquivo nao e um projeto.";
        case LoadError::NewerVersion: return "Feito por uma versao mais nova do jogo.";
        case LoadError::Damaged: return "O arquivo esta danificado.";
        default: return "O conteudo do arquivo nao e valido.";
    }
}

}  // namespace bancada::io
