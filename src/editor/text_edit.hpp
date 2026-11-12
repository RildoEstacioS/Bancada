// Editing text with no window attached: lines, a cursor, a selection and undo. The window only sends it
// keys and draws what it says, so all of this can be tested without one.
#pragma once

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bancada::editor {

struct TextPosition {
    int line = 0;
    int column = 0;  // in bytes, and always at the start of a character

    friend auto operator<=>(const TextPosition&, const TextPosition&) = default;
};

class TextEdit {
public:
    static constexpr std::size_t kMaxBytes = std::size_t{4} << 20;

    explicit TextEdit(std::string_view text = {});

    std::string text() const;
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int index) const { return lines_[static_cast<std::size_t>(index)]; }

    TextPosition cursor() const { return cursor_; }
    bool hasSelection() const { return anchor_.has_value() && *anchor_ != cursor_; }
    TextPosition selectionBegin() const;
    TextPosition selectionEnd() const;
    std::string selectedText() const;

    // Where a position shows on screen: characters from the start of its line, not bytes.
    int displayColumn(TextPosition position) const;

    // Replaces the selection, if any. Anything pasted is made safe first: line ends become plain ones, tabs
    // become spaces, control characters go, and bytes that aren't valid UTF-8 become '?'. Text that would
    // take the total past kMaxBytes is refused whole, and false is returned.
    bool insert(std::string_view text);
    void newline();  // keeps the current line's indentation
    void backspace();
    void deleteForward();
    void deleteSelection();

    void moveLeft(bool extend = false);
    void moveRight(bool extend = false);
    void moveVertically(int lines, bool extend = false);  // keeps its column across shorter lines
    void moveToLineStart(bool extend = false);            // the first non-space, then the very start
    void moveToLineEnd(bool extend = false);
    void moveToStart(bool extend = false);
    void moveToEnd(bool extend = false);
    void place(int line, int displayColumn, bool extend = false);
    void selectAll();

    bool undo();
    bool redo();

    // Changes whenever the text does, so that anything derived from it knows when to look again.
    std::size_t revision() const { return revision_; }

private:
    enum class Change { None, Typing, Erasing };

    struct Snapshot {
        std::vector<std::string> lines;
        TextPosition cursor;
    };

    std::size_t byteCount() const;
    void remember(Change change);
    void insertClean(std::string_view text);
    void eraseRange(TextPosition begin, TextPosition end);
    void prepareMove(bool extend);
    void restore(Snapshot& from);
    TextPosition atDisplayColumn(int line, int column) const;

    std::vector<std::string> lines_{std::string{}};
    TextPosition cursor_;
    std::optional<TextPosition> anchor_;
    std::optional<int> wantedColumn_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    Change lastChange_ = Change::None;
    std::size_t revision_ = 0;
};

}  // namespace bancada::editor
