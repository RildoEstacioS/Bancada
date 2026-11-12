#include "editor/session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "editor/router.hpp"

namespace bancada::editor {

namespace {

constexpr std::size_t kMaxUndoSteps = 100;

// Cycles a second. The last one, 0, means as fast as the computer goes.
constexpr std::uint64_t kSpeeds[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, 1048576, 0};
constexpr int kSpeedCount = static_cast<int>(std::size(kSpeeds));
constexpr double kFrameBudgetSeconds = 0.010;   // how long running the clock may take out of each frame

Point nearestGridPoint(float x, float y) { return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}; }

Point cellUnder(float x, float y) { return {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))}; }

template <typename Id>
bool contains(const std::vector<Id>& ids, Id id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

Session::Session() { rebuildCircuit(); }

void Session::adopt(Board board, std::span<const PartId> togglesOn, bool saved) {
    board_ = std::move(board);
    undo_.clear();
    redo_.clear();
    selectedParts_.clear();
    selectedWires_.clear();
    toggles_.clear();
    for (PartId toggle : togglesOn) toggles_[toggle] = 1;
    current_ = 0;
    currentName_ = "principal";
    stored_.erase(0);
    restart();
    ++revision_;
    savedRevision_ = saved ? revision_ : 0;
}

std::vector<PartId> Session::togglesOn() const {
    std::vector<PartId> on;
    for (const auto& [toggle, value] : toggles_) {
        if (value != 0 && board_.part(toggle) != nullptr) on.push_back(toggle);
    }
    std::sort(on.begin(), on.end());
    return on;
}

void Session::setToggleValues(std::span<const std::pair<PartId, std::uint64_t>> values) {
    for (const auto& [toggle, value] : values) toggles_[toggle] = value;
    restart();
}

std::vector<std::pair<PartId, std::uint64_t>> Session::toggleValues() const {
    std::vector<std::pair<PartId, std::uint64_t>> out;
    for (const auto& [toggle, value] : toggles_) {
        if (value != 0 && board_.part(toggle) != nullptr) out.emplace_back(toggle, value);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Session::adoptProjectChips(std::vector<ChipData> chips) {
    stored_.clear();
    nextChip_ = 1;
    for (ChipData& chip : chips) {
        nextChip_ = std::max(nextChip_, chip.id + 1);
        Stored stored;
        stored.name = std::move(chip.name);
        stored.board = std::move(chip.board);
        stored.toggles = std::move(chip.toggles);
        stored_[chip.id] = std::move(stored);
    }
}

void Session::addChips(std::vector<ChipData> chips) {
    for (ChipData& chip : chips) {
        nextChip_ = std::max(nextChip_, chip.id + 1);
        Stored stored;
        stored.name = std::move(chip.name);
        stored.board = std::move(chip.board);
        stored.toggles = std::move(chip.toggles);
        stored_[chip.id] = std::move(stored);
    }
    ++revision_;
}

std::vector<ChipData> Session::projectChips() const {
    std::vector<ChipData> out;
    for (const auto& [id, stored] : stored_) {
        if (id != 0) out.push_back({id, stored.name, stored.board, stored.toggles});
    }
    if (current_ != 0) out.push_back({current_, currentName_, board_, toggles_});
    std::sort(out.begin(), out.end(), [](const ChipData& a, const ChipData& b) { return a.id < b.id; });
    return out;
}

ChipData Session::mainCircuit() const {
    if (current_ == 0) return {0, "principal", board_, toggles_};
    const Stored& stored = stored_.at(0);
    return {0, "principal", stored.board, stored.toggles};
}

std::vector<CircuitEntry> Session::circuits() const {
    std::vector<CircuitEntry> out;
    out.push_back({0, "principal"});
    for (const ChipData& chip : projectChips()) out.push_back({chip.id, chip.name});
    return out;
}

const Board* Session::boardOf(ChipId id) const {
    if (id == current_) return &board_;
    const auto found = stored_.find(id);
    return found == stored_.end() ? nullptr : &found->second.board;
}

Board* Session::mutableBoardOf(ChipId id) {
    if (id == current_) return &board_;
    const auto found = stored_.find(id);
    return found == stored_.end() ? nullptr : &found->second.board;
}

void Session::rebuildChipBoards() {
    chipBoards_.clear();
    for (const auto& [id, stored] : stored_) {
        if (id != 0) chipBoards_[id] = &stored.board;
    }
    if (current_ != 0) chipBoards_[current_] = &board_;
}

bool Session::chipContains(ChipId outer, ChipId inner) const {
    if (outer == inner) return true;
    const Board* board = boardOf(outer);
    if (board == nullptr) return false;
    for (const Part& part : board->parts()) {
        if (part.kind == PartKind::Chip && chipContains(part.chip, inner)) return true;
    }
    return false;
}

bool Session::canPlaceChip(ChipId chip) const { return boardOf(chip) != nullptr && chip != 0 && !chipContains(chip, current_); }

ChipId Session::createChip(std::string name) {
    const ChipId id = nextChip_++;
    Stored stored;
    stored.name = std::move(name);
    stored_[id] = std::move(stored);
    ++revision_;
    openCircuit(id);
    return id;
}

void Session::openCircuit(ChipId id) {
    if (id == current_ || boardOf(id) == nullptr) return;
    cancelTool();
    const ChipId left = current_;

    Stored parked;
    parked.name = std::move(currentName_);
    parked.board = std::move(board_);
    parked.undo = std::move(undo_);
    parked.redo = std::move(redo_);
    parked.toggles = std::move(toggles_);
    stored_[left] = std::move(parked);

    Stored opened = std::move(stored_.at(id));
    stored_.erase(id);
    board_ = std::move(opened.board);
    undo_ = std::move(opened.undo);
    redo_ = std::move(opened.redo);
    toggles_ = std::move(opened.toggles);
    currentName_ = std::move(opened.name);
    current_ = id;

    selectedParts_.clear();
    selectedWires_.clear();
    if (left != 0) refreshChip(left);
    running_ = false;
    clockLevel_ = false;
    restart();
}

// A chip's pins come from its switches and outputs, so after editing it every use of it has to follow.
void Session::refreshChip(ChipId chip) {
    const Board* board = boardOf(chip);
    if (board == nullptr) return;
    const auto found = stored_.find(chip);
    const InterfacePtr interface = makeChipInterface(*board, chip == current_ ? currentName_ : found->second.name);
    for (auto& [id, stored] : stored_) {
        if (id != chip) stored.board.updateChip(chip, interface);
    }
    if (current_ != chip) board_.updateChip(chip, interface);
}

void Session::openChipUnderCursor() {
    if (mode_ != Mode::Idle) return;
    const Part* part = board_.partAtCell(cellUnder(worldX_, worldY_));
    if (part != nullptr && part->kind == PartKind::Chip) openCircuit(part->chip);
}

void Session::setRunning(bool running) { running_ = running && circuit_->hasClock(); }

void Session::stepClock() {
    clockLevel_ = !clockLevel_;
    circuit_->setClocks(clockLevel_);
    circuit_->settle();
}

void Session::resetSimulation() {
    circuit_->reset();
    clockLevel_ = false;
    cycleDebt_ = 0;
    for (const auto& [toggle, value] : toggles_) circuit_->setToggle(toggle, value);
    circuit_->settle();
}

void Session::changeSpeed(int delta) { speedIndex_ = std::clamp(speedIndex_ + delta, 0, kSpeedCount - 1); }

std::uint64_t Session::speed() const { return kSpeeds[speedIndex_]; }

void Session::advance(double seconds) {
    // Capacitors charge in real time, clock or no clock. A long pause isn't caught up in one go.
    if (circuit_->analogStepping() && seconds > 0.0) circuit_->advanceAnalog(std::min(seconds, 0.05));
    if (!running_ || !circuit_->hasClock()) {
        measuredSpeed_ = 0;
        return;
    }
    const std::uint64_t hz = speed();
    cycleDebt_ = hz == 0 ? 1e18 : std::min(cycleDebt_ + static_cast<double>(hz) * seconds, static_cast<double>(hz) + 1.0);

    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    std::uint64_t cycles = 0;
    while (cycleDebt_ >= 1.0) {
        stepClock();
        stepClock();
        cycleDebt_ -= 1.0;
        ++cycles;
        if ((cycles & 15) == 0 && std::chrono::duration<double>(Clock::now() - start).count() > kFrameBudgetSeconds) break;
    }
    if (hz == 0) cycleDebt_ = 0;
    if (seconds > 0) measuredSpeed_ = measuredSpeed_ * 0.9 + static_cast<double>(cycles) / seconds * 0.1;
}

void Session::restart() {
    preview_.reset();
    mode_ = Mode::Idle;
    wirePoints_.clear();
    wireRoute_.clear();
    forgetMissing();
    rebuildCircuit();
}

void Session::rebuildCircuit() {
    rebuildChipBoards();
    circuit_ = std::make_unique<BoardCircuit>(board_, &chipBoards_);
    for (const auto& [toggle, value] : toggles_) circuit_->setToggle(toggle, value);
    clockLevel_ = false;
    if (!circuit_->hasClock()) running_ = false;
    circuit_->settle();
    rebuildTable();
}

void Session::rebuildTable() { table_ = computeTruthTable(board_, &chipBoards_); }

int Session::currentRow() const {
    if (table_.status != TruthTable::Status::Ready) return -1;
    std::uint64_t row = 0;
    for (std::size_t i = 0; i < table_.inputs.size(); ++i) row = (row << table_.inputBits[i]) | toggleValue(table_.inputs[i]);
    return static_cast<int>(row);
}

void Session::forgetMissing() {
    std::erase_if(selectedParts_, [&](PartId id) { return board_.part(id) == nullptr; });
    std::erase_if(selectedWires_, [&](WireId id) { return board_.wire(id) == nullptr; });
}

void Session::commit(Board next) {
    undo_.push_back(std::move(board_));
    if (undo_.size() > kMaxUndoSteps) undo_.erase(undo_.begin());
    redo_.clear();
    board_ = std::move(next);
    ++revision_;
    restart();
}

void Session::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(board_));
    board_ = std::move(undo_.back());
    undo_.pop_back();
    ++revision_;
    restart();
}

void Session::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(board_));
    board_ = std::move(redo_.back());
    redo_.pop_back();
    ++revision_;
    restart();
}

bool Session::toggleOn(PartId toggle) const { return toggleValue(toggle) != 0; }

std::uint64_t Session::toggleValue(PartId toggle) const {
    const auto found = toggles_.find(toggle);
    return found == toggles_.end() ? 0 : found->second & sim::widthMask(board_.part(toggle) == nullptr ? 64 : board_.part(toggle)->width);
}

// A button is 1 while it's held, and back to 0 when it's let go, wherever the mouse went meanwhile.
void Session::holdButton(PartId button, std::uint64_t value) {
    heldButton_ = value != 0 ? button : 0;
    toggles_[button] = value;
    ++revision_;
    circuit_->setToggle(button, value);
    circuit_->settle();
}

// A key from the real keyboard, for the keyboard part: its code stays after it's let go.
void Session::pressKey(PartId keyboard, std::uint8_t code, bool down) {
    const std::uint64_t value = down ? (code & 0x7Fu) | 0x80u : toggleValue(keyboard) & 0x7Fu;
    toggles_[keyboard] = value;
    ++revision_;
    circuit_->setToggle(keyboard, value);
    circuit_->settle();
}

void Session::flipToggle(const Part& toggle) {
    std::uint64_t value = toggleValue(toggle.id);
    if (toggle.width == 1) {
        value ^= 1;
    } else {
        const int bit = toggleBitAt(toggle, worldX_, worldY_);
        if (bit < 0) return;
        value ^= std::uint64_t{1} << bit;
    }
    toggles_[toggle.id] = value;
    ++revision_;
    circuit_->setToggle(toggle.id, value);
    circuit_->settle();
}

Drop Session::hovered() const { return board().dropAt(cursor_); }

std::optional<HoverInfo> Session::hoverInfo() const {
    const Drop drop = board_.dropAt(cursor_);
    if (drop.kind == Drop::Kind::Pin) {
        const Part* part = board_.part(drop.id);
        return HoverInfo{pinBits(*part, drop.pin), circuit_->pinValue(drop.id, drop.pin)};
    }
    if (drop.kind == Drop::Kind::Wire) return HoverInfo{circuit_->wireBits(drop.id), circuit_->wireValue(drop.id)};
    return std::nullopt;
}

Part Session::placingPart() const {
    Part part = makePart(placingKind_, {}, placingRotation_, placingInputs_, placingScale_, placingBox_);
    if (placingKind_ == PartKind::Chip) {
        part.chip = placingChip_;
        const auto found = stored_.find(placingChip_);
        if (found != stored_.end()) part.interface = makeChipInterface(found->second.board, found->second.name);
    }
    if (placingWidth_ != 0 && hasWidth(part)) part.width = placingWidth_;
    if (placingAddress_ != 0 && hasAddress(part)) part.address = placingAddress_;
    if (part.kind == PartKind::Box) part.interface = boxInterface(part.box, part.width, part.address);
    const Rect bounds = boundsOf(part);
    part.position = cursor_ - Point{bounds.right / 2, bounds.bottom / 2};
    return part;
}

Point Session::placingPosition() const { return placingPart().position; }

bool Session::placingFits() const { return board_.canPlace(placingPart()); }

std::optional<Inspected> Session::inspected() const {
    if (mode_ == Mode::Placing) {
        Inspected shown;
        shown.part = placingPart();
        shown.name = partName(shown.part);
        shown.placing = true;
        return shown;
    }
    if (mode_ == Mode::Idle && selectedParts_.size() == 1) {
        if (const Part* part = board_.part(selectedParts_[0])) {
            Inspected shown;
            shown.part = *part;
            shown.name = partName(*part);
            return shown;
        }
    }
    return std::nullopt;
}

void Session::reconfigure(Part shape) {
    const PartId id = selectedParts_[0];
    Board next = board_;
    if (next.reconfigurePart(id, std::move(shape))) commit(std::move(next));
}

void Session::changeInputs(int delta) {
    const std::optional<Inspected> now = inspected();
    if (!now) return;
    const int wanted = static_cast<int>(now->part.inputs) + delta;
    if (wanted < minInputs(now->part.kind) || wanted > maxInputs(now->part.kind)) return;
    if (isBundle(now->part.kind) && wanted > now->part.width) return;
    if (mode_ == Mode::Placing) {
        placingInputs_ = static_cast<std::uint8_t>(wanted);
        return;
    }
    Part shape = now->part;
    shape.inputs = static_cast<std::uint8_t>(wanted);
    reconfigure(std::move(shape));
}

void Session::changeScale(int delta) {
    const std::optional<Inspected> now = inspected();
    if (!now) return;
    const int wanted = static_cast<int>(now->part.scale) + delta;
    if (wanted < 1 || wanted > kMaxScale) return;
    if (mode_ == Mode::Placing) {
        placingScale_ = static_cast<std::uint8_t>(wanted);
        return;
    }
    Part shape = now->part;
    shape.scale = static_cast<std::uint8_t>(wanted);
    reconfigure(std::move(shape));
}

void Session::changeBits(int delta) {
    const std::optional<Inspected> now = inspected();
    if (!now || !hasWidth(now->part)) return;
    const int wanted = static_cast<int>(now->part.width) + delta;
    const int lowest = isBundle(now->part.kind) ? std::max<int>(2, now->part.inputs) : 1;
    const int lowestForBox = now->part.kind == PartKind::Box ? 2 : lowest;
    if (wanted < lowestForBox || wanted > kMaxBits) return;
    if (mode_ == Mode::Placing) {
        placingWidth_ = static_cast<std::uint8_t>(wanted);
        return;
    }
    Part shape = now->part;
    shape.width = static_cast<std::uint8_t>(wanted);
    reconfigure(std::move(shape));
}

void Session::changeAddress(int delta) {
    const std::optional<Inspected> now = inspected();
    if (!now || !hasAddress(now->part)) return;
    const int wanted = static_cast<int>(now->part.address) + delta;
    if (wanted < 1 || wanted > kMaxPartAddress) return;
    if (mode_ == Mode::Placing) {
        placingAddress_ = static_cast<std::uint8_t>(wanted);
        return;
    }
    Part shape = now->part;
    shape.address = static_cast<std::uint8_t>(wanted);
    if (shape.rom != nullptr && shape.rom->size() > (std::size_t{1} << wanted)) {
        auto words = *shape.rom;
        words.resize(std::size_t{1} << wanted);
        shape.rom = std::make_shared<const std::vector<std::uint64_t>>(std::move(words));
    }
    reconfigure(std::move(shape));
}

void Session::setValue(std::uint64_t value) {
    const std::optional<Inspected> now = inspected();
    if (!now || now->placing) return;
    Part shape = now->part;
    if (shape.kind == PartKind::Constant) {
        shape.value = value & sim::widthMask(shape.width);
    } else if (const std::optional<electric::Quantity> quantity = valueQuantity(shape.kind)) {
        const electric::ValueRange range = electric::rangeOf(*quantity);
        if (value < range.least || value > range.most) return;
        shape.value = value;
    } else {
        return;
    }
    reconfigure(std::move(shape));
}

void Session::setLabel(std::string label) {
    const std::optional<Inspected> now = inspected();
    if (!now || now->placing) return;
    if (now->part.kind == PartKind::Tunnel && label.empty()) return;  // a label is its name
    Part shape = now->part;
    shape.label = std::move(label);
    reconfigure(std::move(shape));
}

void Session::setRomWords(std::vector<std::uint64_t> words, std::string source) {
    const std::optional<Inspected> now = inspected();
    if (!now || now->placing || now->part.kind != PartKind::Box || !boxSpec(now->part.box).hasContents) return;
    Part shape = now->part;
    words.resize(std::min(words.size(), std::size_t{1} << shape.address));
    for (std::uint64_t& word : words) word &= sim::widthMask(shape.width);
    shape.rom = std::make_shared<const std::vector<std::uint64_t>>(std::move(words));
    shape.source = std::move(source);
    reconfigure(std::move(shape));
}

void Session::selectOnly(PartId part) {
    if (board_.part(part) == nullptr) return;
    selectedParts_.assign(1, part);
    selectedWires_.clear();
}

void Session::copySelection() {
    if (selectedParts_.empty() && selectedWires_.empty()) return;
    Board copied = board_.fragment(selectedParts_, selectedWires_);
    if (copied.empty()) return;
    clipboardToggles_.clear();
    for (const Part& part : copied.parts()) {
        if (toggleOn(part.id)) clipboardToggles_[part.id] = toggleValue(part.id);
    }
    clipboard_ = std::move(copied);
}

Point Session::pasteDelta() const {
    const Rect box = clipboard_->extent();
    return cursor_ - Point{(box.left + box.right) / 2, (box.top + box.bottom) / 2};
}

Rect Session::pasteBox() const {
    Rect box = clipboard_->extent();
    const Point delta = pasteDelta();
    box.left += delta.x;
    box.right += delta.x;
    box.top += delta.y;
    box.bottom += delta.y;
    return box;
}

void Session::startPaste() {
    if (!clipboard_) return;
    // Copied from another circuit, a chip could end up inside itself.
    for (const Part& part : clipboard_->parts()) {
        if (part.kind == PartKind::Chip && !canPlaceChip(part.chip)) return;
    }
    cancelTool();
    mode_ = Mode::Pasting;
    updatePaste();
}

void Session::updatePaste() {
    Board next = board_;
    pasted_ = next.paste(*clipboard_, pasteDelta());
    if (pasted_) preview_ = std::move(next);
}

void Session::finishPaste() {
    if (!pasted_ || !preview_) return;
    const Pasted pasted = std::move(*pasted_);
    Board next = std::move(*preview_);
    pasted_.reset();
    for (const auto& [from, to] : pasted.parts) {
        const auto found = clipboardToggles_.find(from);
        if (found != clipboardToggles_.end()) toggles_[to] = found->second;
    }
    commit(std::move(next));
    selectedParts_.clear();
    for (const auto& pair : pasted.parts) selectedParts_.push_back(pair.second);
    selectedWires_ = pasted.wires;
}

std::vector<Point> Session::wirePreview() const {
    std::vector<Point> path = wirePoints_;
    if (!wireRoute_.empty()) path.insert(path.end(), wireRoute_.begin() + 1, wireRoute_.end());
    return path;
}

Rect Session::selectionBox() const {
    return {std::min(pressCell_.x, cursor_.x), std::min(pressCell_.y, cursor_.y), std::max(pressCell_.x, cursor_.x),
            std::max(pressCell_.y, cursor_.y)};
}

void Session::beginPlacing(PartKind kind, BoxId box) {
    if ((kind == PartKind::Box) != (box != kNoBox) || box > boxCount() || kind == PartKind::Chip) return;
    cancelTool();
    placingBox_ = box;
    placingChip_ = 0;
    mode_ = Mode::Placing;
    placingKind_ = kind;
    placingRotation_ = 0;
    placingInputs_ = minInputs(kind);
    placingWidth_ = isBundle(kind) ? 8 : 0;
    placingAddress_ = 0;
}

void Session::beginPlacingChip(ChipId chip) {
    if (!canPlaceChip(chip)) return;
    cancelTool();
    placingBox_ = kNoBox;
    placingChip_ = chip;
    mode_ = Mode::Placing;
    placingKind_ = PartKind::Chip;
    placingRotation_ = 0;
    placingInputs_ = 0;
    placingWidth_ = 0;
    placingAddress_ = 0;
}

void Session::cancelTool() {
    preview_.reset();
    pasted_.reset();
    wirePoints_.clear();
    wireRoute_.clear();
    wiringByDrag_ = false;
    mode_ = Mode::Idle;
}

void Session::startWire(const Drop& from) {
    mode_ = Mode::Wiring;
    wireStart_ = from;
    wirePoints_ = {from.at};
    wireRoute_.clear();
    updateRoute();
}

void Session::updateRoute() {
    const Point last = wirePoints_.back();
    std::optional<Point> arrivedFrom;
    if (wirePoints_.size() >= 2) arrivedFrom = wirePoints_[wirePoints_.size() - 2];
    wireRoute_ = straight_ ? elbowPath(last, cursor_) : routePath(board_, last, cursor_, arrivedFrom);
}

void Session::addWaypoint() {
    if (cursor_ == wirePoints_.back()) return;
    updateRoute();
    wirePoints_ = wirePreview();
    wireRoute_.clear();
}

void Session::finishWire(const Drop& to) {
    updateRoute();
    std::vector<Point> path = wirePreview();
    Board next = board_;
    const bool made = next.connect(wireStart_, to, std::move(path)).has_value();
    cancelTool();
    if (made) commit(std::move(next));
}

void Session::updateDrag() {
    const Point delta = cursor_ - pressCell_;
    Board moved = board_;
    if (moved.moveParts(selectedParts_, delta)) preview_ = std::move(moved);
}

void Session::mouseMove(float x, float y, bool straight) {
    worldX_ = x;
    worldY_ = y;
    straight_ = straight;
    const Point previous = cursor_;
    cursor_ = nearestGridPoint(x, y);
    const bool moved = !(cursor_ == previous);

    switch (mode_) {
        case Mode::PressedPart:
            if (!(cursor_ == pressCell_)) {
                mode_ = Mode::MovingParts;
                updateDrag();
            }
            break;
        case Mode::MovingParts:
            if (moved) updateDrag();
            break;
        case Mode::PressedWire:
            if (!(cursor_ == pressCell_)) {
                wiringByDrag_ = true;
                startWire(pressedDrop_);
            }
            break;
        case Mode::Wiring:
            updateRoute();
            break;
        case Mode::Pasting:
            if (moved) updatePaste();
            break;
        default:
            break;
    }
}

void Session::leftDown(bool adding) {
    adding_ = adding;
    switch (mode_) {
        case Mode::Placing:
            if (placingFits()) {
                Board next = board_;
                next.addPart(placingPart());
                const PartKind kind = placingKind_;
                const std::uint8_t rotation = placingRotation_;
                commit(std::move(next));
                mode_ = Mode::Placing;  // keep placing, for rows of the same part
                placingKind_ = kind;
                placingRotation_ = rotation;
            }
            return;
        case Mode::Wiring: {
            const Drop drop = board_.dropAt(cursor_);
            if (cursor_ == wirePoints_.back()) return;
            if (drop.kind == Drop::Kind::Empty) {
                addWaypoint();
            } else {
                finishWire(drop);
            }
            return;
        }
        case Mode::Pasting:
            finishPaste();
            return;
        default:
            break;
    }

    pressCell_ = cursor_;
    const Drop drop = board_.dropAt(cursor_);
    if (drop.kind == Drop::Kind::Pin || drop.kind == Drop::Kind::Joint) {
        wiringByDrag_ = true;
        startWire(drop);
        return;
    }
    if (const Part* part = board_.partAtCell(cellUnder(worldX_, worldY_))) {
        pressedPart_ = part->id;
        if (!contains(selectedParts_, part->id)) {
            if (!adding_) {
                selectedParts_.clear();
                selectedWires_.clear();
            }
            selectedParts_.push_back(part->id);
        }
        mode_ = Mode::PressedPart;
        if (part->kind == PartKind::Button) holdButton(part->id, 1);
        return;
    }
    if (drop.kind == Drop::Kind::Wire) {
        pressedDrop_ = drop;
        mode_ = Mode::PressedWire;
        return;
    }
    if (!adding_) {
        selectedParts_.clear();
        selectedWires_.clear();
    }
    mode_ = Mode::BoxSelect;
}

void Session::leftUp() {
    if (heldButton_ != 0) holdButton(heldButton_, 0);
    switch (mode_) {
        case Mode::PressedPart: {
            const Part* part = board_.part(pressedPart_);
            mode_ = Mode::Idle;
            if (part != nullptr && (part->kind == PartKind::Toggle || part->kind == PartKind::Switch)) flipToggle(*part);
            break;
        }
        case Mode::MovingParts: {
            mode_ = Mode::Idle;
            if (preview_) {
                Board next = std::move(*preview_);
                preview_.reset();
                commit(std::move(next));
            }
            break;
        }
        case Mode::PressedWire:
            if (!adding_) {
                selectedParts_.clear();
                selectedWires_.clear();
            }
            if (!contains(selectedWires_, pressedDrop_.id)) selectedWires_.push_back(pressedDrop_.id);
            mode_ = Mode::Idle;
            break;
        case Mode::BoxSelect: {
            const Rect box = selectionBox();
            for (const Part& part : board_.parts()) {
                if (box.holds(boundsOf(part)) && !contains(selectedParts_, part.id)) selectedParts_.push_back(part.id);
            }
            for (const Wire& wire : board_.wires()) {
                const bool inside = std::all_of(wire.path.begin(), wire.path.end(), [&](Point p) { return box.touches(p); });
                if (inside && !contains(selectedWires_, wire.id)) selectedWires_.push_back(wire.id);
            }
            mode_ = Mode::Idle;
            break;
        }
        case Mode::Wiring:
            // A press on a pin that doesn't move is a click: the wire stays on the cursor.
            if (wiringByDrag_) {
                wiringByDrag_ = false;
                if (cursor_ == wirePoints_.back()) break;
                const Drop drop = board_.dropAt(cursor_);
                if (drop.kind == Drop::Kind::Empty) {
                    addWaypoint();
                } else {
                    finishWire(drop);
                }
            }
            break;
        default:
            break;
    }
}

void Session::rightDown() { cancelTool(); }

void Session::key(Key key) {
    switch (key) {
        case Key::Escape:
            if (mode_ != Mode::Idle) {
                cancelTool();
            } else {
                selectedParts_.clear();
                selectedWires_.clear();
            }
            break;
        case Key::Enter:
            if (mode_ == Mode::Wiring && !(cursor_ == wirePoints_.back())) {
                const Drop drop = board_.dropAt(cursor_);
                finishWire(drop);
            }
            break;
        case Key::Delete:
            if (mode_ == Mode::Idle && (!selectedParts_.empty() || !selectedWires_.empty())) {
                Board next = board_;
                next.remove(selectedParts_, selectedWires_);
                commit(std::move(next));
            }
            break;
        case Key::Rotate:
            if (mode_ == Mode::Placing) {
                placingRotation_ = static_cast<std::uint8_t>((placingRotation_ + 1) & 3);
            } else if (mode_ == Mode::Idle && !selectedParts_.empty()) {
                Board next = board_;
                bool turned = false;
                for (PartId id : selectedParts_) turned = next.rotatePart(id) || turned;
                if (turned) commit(std::move(next));
            }
            break;
        case Key::Undo:
            cancelTool();
            undo();
            break;
        case Key::Redo:
            cancelTool();
            redo();
            break;
        case Key::SelectAll:
            if (mode_ == Mode::Idle) {
                selectedParts_.clear();
                selectedWires_.clear();
                for (const Part& part : board_.parts()) selectedParts_.push_back(part.id);
                for (const Wire& wire : board_.wires()) selectedWires_.push_back(wire.id);
            }
            break;
        case Key::Copy:
            if (mode_ == Mode::Idle) copySelection();
            break;
        case Key::Cut:
            if (mode_ == Mode::Idle && (!selectedParts_.empty() || !selectedWires_.empty())) {
                copySelection();
                Board next = board_;
                next.remove(selectedParts_, selectedWires_);
                commit(std::move(next));
            }
            break;
        case Key::Paste:
            startPaste();
            break;
    }
}

}  // namespace bancada::editor
