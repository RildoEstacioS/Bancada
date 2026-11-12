// The editor's behaviour, with no screen attached: it takes mouse and key events in board coordinates
// and keeps the board, the tool state, undo history and a running circuit. The window only draws what's
// here and translates events, so everything a player can do can be tested without one.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/board.hpp"
#include "editor/circuit.hpp"
#include "editor/truth_table.hpp"

namespace bancada::editor {

enum class Key : std::uint8_t { Escape, Delete, Enter, Rotate, Undo, Redo, SelectAll, Copy, Cut, Paste };
enum class Mode : std::uint8_t { Idle, Placing, PressedPart, MovingParts, PressedWire, BoxSelect, Wiring, Pasting };

// What the properties panel shows: the part about to be placed, or the one selected part.
struct Inspected {
    Part part;
    const char* name = "";
    bool placing = false;
};

// Another circuit of the project, as it's kept and as it's saved.
struct ChipData {
    ChipId id = 0;
    std::string name;
    Board board;
    std::unordered_map<PartId, std::uint64_t> toggles;
};

struct CircuitEntry {
    ChipId id = 0;
    std::string name;
};

// What's under the cursor, for the status line.
struct HoverInfo {
    unsigned bits = 1;
    std::uint64_t value = 0;
};

class Session {
public:
    Session();

    // Replaces the board, clearing undo history. saved says whether it matches what's on disk.
    void adopt(Board board, std::span<const PartId> togglesOn = {}, bool saved = true);
    std::vector<PartId> togglesOn() const;

    // Values of many-bit switches, and every other circuit of the project, for saving and loading. Call
    // adoptProjectChips before adopt, since the main board may use them.
    void setToggleValues(std::span<const std::pair<PartId, std::uint64_t>> values);
    void pressKey(PartId keyboard, std::uint8_t code, bool down);
    std::vector<std::pair<PartId, std::uint64_t>> toggleValues() const;
    void adoptProjectChips(std::vector<ChipData> chips);

    // Adds ready-made circuits to the project. Their ids must start at nextChipId().
    ChipId nextChipId() const { return nextChip_; }
    const ChipBoards& chipBoards() const { return chipBoards_; }
    void addChips(std::vector<ChipData> chips);
    std::vector<ChipData> projectChips() const;
    ChipData mainCircuit() const;  // id 0, whichever circuit is open

    // The circuits of a project: 0 is the main board, and any other can be placed as a chip.
    std::vector<CircuitEntry> circuits() const;
    ChipId currentCircuit() const { return current_; }
    const std::string& currentName() const { return currentName_; }
    ChipId createChip(std::string name);
    void openCircuit(ChipId id);
    void openChipUnderCursor();
    bool canPlaceChip(ChipId chip) const;   // false if it would end up inside itself

    // Clock: a tick is one full cycle, low to high and back.
    bool hasClock() const { return circuit_->hasClock(); }
    bool running() const { return running_; }
    void setRunning(bool running);
    void stepClock();                       // one half cycle, by hand
    bool clockLevel() const { return clockLevel_; }
    void resetSimulation();
    void changeSpeed(int delta);
    std::uint64_t speed() const;            // cycles a second, or 0 for as fast as it goes
    void advance(double seconds);
    double measuredSpeed() const { return measuredSpeed_; }

    // Goes up with every change, including switches.
    std::uint64_t revision() const { return revision_; }
    bool dirty() const { return revision_ != savedRevision_; }
    void markSaved() { savedRevision_ = revision_; }

    void mouseMove(float x, float y, bool straight = false);
    void leftDown(bool adding = false);
    void leftUp();
    void rightDown();
    void key(Key key);
    void beginPlacing(PartKind kind, BoxId box = kNoBox);
    void beginPlacingChip(ChipId chip);

    std::optional<Inspected> inspected() const;
    void changeInputs(int delta);
    void changeScale(int delta);
    void changeBits(int delta);
    void changeAddress(int delta);
    void setValue(std::uint64_t value);
    void setLabel(std::string label);
    void setRomWords(std::vector<std::uint64_t> words, std::string source = {});
    // A program into its ROM and its data into its RAM at once, so that one undo takes both back.
    void setMemories(PartId rom, std::vector<std::uint64_t> words, std::string source, PartId ram, std::vector<std::uint64_t> data);
    // Called just before each rising edge of the clock, with the circuit settled: what's about to happen.
    void watchClock(std::function<void()> watch) { watchClock_ = std::move(watch); }

    // Selects just this part on the current circuit, the way a click on it would.
    void selectOnly(PartId part);

    const TruthTable& truthTable() const { return table_; }
    // The table's row for the switches as they are now, or -1 if the table has no rows.
    int currentRow() const;

    Mode mode() const { return mode_; }
    const Board& board() const { return preview_ ? *preview_ : board_; }
    const BoardCircuit& circuit() const { return *circuit_; }
    bool toggleOn(PartId toggle) const;
    std::uint64_t toggleValue(PartId toggle) const;

    Point cursor() const { return cursor_; }
    const std::vector<PartId>& selectedParts() const { return selectedParts_; }
    const std::vector<WireId>& selectedWires() const { return selectedWires_; }
    Drop hovered() const;
    std::optional<HoverInfo> hoverInfo() const;

    PartKind placingKind() const { return placingKind_; }
    std::uint8_t placingRotation() const { return placingRotation_; }
    std::uint8_t placingInputs() const { return placingInputs_; }
    std::uint8_t placingScale() const { return placingScale_; }
    Point placingPosition() const;
    bool placingFits() const;

    Part placingPart() const;

    bool canPaste() const { return clipboard_.has_value(); }
    bool pasteFits() const { return pasted_.has_value(); }
    Rect pasteBox() const;

    std::vector<Point> wirePreview() const;
    Rect selectionBox() const;

    std::size_t undoSteps() const { return undo_.size(); }
    std::size_t redoSteps() const { return redo_.size(); }

private:
    void commit(Board next);
    void restart();
    void rebuildCircuit();
    void forgetMissing();
    void cancelTool();
    void holdButton(PartId button, std::uint64_t value);
    void flipToggle(const Part& toggle);
    void reconfigure(Part shape);
    void refreshChip(ChipId chip);
    bool chipContains(ChipId outer, ChipId inner) const;
    const Board* boardOf(ChipId id) const;
    Board* mutableBoardOf(ChipId id);
    void rebuildChipBoards();
    void startWire(const Drop& from);
    void addWaypoint();
    void finishWire(const Drop& to);
    void updateRoute();
    void updateDrag();
    void undo();
    void redo();
    void copySelection();
    void startPaste();
    void updatePaste();
    void finishPaste();
    void rebuildTable();
    Point pasteDelta() const;

    Board board_;
    std::optional<Board> preview_;
    std::unique_ptr<BoardCircuit> circuit_;
    std::unordered_map<PartId, std::uint64_t> toggles_;
    std::vector<Board> undo_;
    std::vector<Board> redo_;
    std::uint64_t revision_ = 1;
    std::uint64_t savedRevision_ = 1;

    Mode mode_ = Mode::Idle;
    float worldX_ = 0;
    float worldY_ = 0;
    Point cursor_;
    Point pressCell_;
    bool straight_ = false;
    bool adding_ = false;

    std::vector<PartId> selectedParts_;
    std::vector<WireId> selectedWires_;
    PartId pressedPart_ = 0;
    PartId heldButton_ = 0;  // a button the mouse is holding down

    PartKind placingKind_ = PartKind::Nand;
    std::uint8_t placingRotation_ = 0;
    std::uint8_t placingInputs_ = 2;
    std::uint8_t placingScale_ = 1;
    BoxId placingBox_ = kNoBox;
    std::uint8_t placingWidth_ = 0;    // 0: whatever the part starts with
    std::uint8_t placingAddress_ = 0;

    Drop wireStart_;
    Drop pressedDrop_;
    std::vector<Point> wirePoints_;   // fixed so far
    std::vector<Point> wireRoute_;    // from the last fixed point to the cursor
    bool wiringByDrag_ = false;

    TruthTable table_;

    struct Stored {
        std::string name;
        Board board;
        std::vector<Board> undo;
        std::vector<Board> redo;
        std::unordered_map<PartId, std::uint64_t> toggles;
    };
    std::map<ChipId, Stored> stored_;  // every circuit but the open one
    ChipId current_ = 0;
    std::string currentName_ = "principal";
    ChipId nextChip_ = 1;
    ChipBoards chipBoards_;

    bool running_ = false;
    bool clockLevel_ = false;
    std::function<void()> watchClock_;
    int speedIndex_ = 3;
    double cycleDebt_ = 0;
    double measuredSpeed_ = 0;
    ChipId placingChip_ = 0;

    std::optional<Board> clipboard_;
    std::unordered_map<PartId, std::uint64_t> clipboardToggles_;
    std::optional<Pasted> pasted_;
};

}  // namespace bancada::editor
