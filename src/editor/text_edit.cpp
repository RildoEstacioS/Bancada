#include "editor/text_edit.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace bancada::editor {

namespace {

constexpr std::size_t kUndoSteps = 200;
constexpr char kTabSpaces[] = "    ";

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// The length of a valid UTF-8 character here, or 0. Stricter than it looks because saved files are JSON,
// which refuses overlong forms and surrogates.
std::size_t validCharacter(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned first = byte(at);
    std::size_t length = 0;
    unsigned low = 0x80;
    unsigned high = 0xBF;
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
        if (first == 0xE0) low = 0xA0;
        if (first == 0xED) high = 0x9F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
        if (first == 0xF0) low = 0x90;
        if (first == 0xF4) high = 0x8F;
    } else {
        return 0;
    }
    if (at + length > text.size()) return 0;
    if (byte(at + 1) < low || byte(at + 1) > high) return 0;
    for (std::size_t i = 2; i < length; ++i) {
        if (!isContinuation(text[at + i])) return 0;
    }
    return length;
}

std::string cleaned(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0x80) {
            const std::size_t length = validCharacter(text, i);
            if (length == 0) {
                out += '?';
                ++i;
            } else {
                out.append(text.substr(i, length));
                i += length;
            }
            continue;
        }
        if (byte == '\t') out += kTabSpaces;
        if (byte == '\n' || (byte >= 0x20 && byte != 0x7F)) out += static_cast<char>(byte);
        ++i;
    }
    return out;
}

}  // namespace

TextEdit::TextEdit(std::string_view text) {
    const std::string safe = cleaned(text);
    if (safe.size() <= kMaxBytes) insertClean(safe);
    cursor_ = {};
}

std::string TextEdit::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i != 0) out += '\n';
        out += lines_[i];
    }
    return out;
}

TextPosition TextEdit::selectionBegin() const { return hasSelection() ? std::min(*anchor_, cursor_) : cursor_; }
TextPosition TextEdit::selectionEnd() const { return hasSelection() ? std::max(*anchor_, cursor_) : cursor_; }

std::string TextEdit::selectedText() const {
    if (!hasSelection()) return {};
    const TextPosition begin = selectionBegin();
    const TextPosition end = selectionEnd();
    const auto at = [&](int index) -> const std::string& { return lines_[static_cast<std::size_t>(index)]; };
    const auto column = [](int value) { return static_cast<std::size_t>(value); };
    if (begin.line == end.line) return at(begin.line).substr(column(begin.column), column(end.column - begin.column));
    std::string out = at(begin.line).substr(column(begin.column));
    for (int index = begin.line + 1; index < end.line; ++index) out += '\n' + at(index);
    out += '\n' + at(end.line).substr(0, column(end.column));
    return out;
}

int TextEdit::displayColumn(TextPosition position) const {
    const std::string& text = line(position.line);
    const auto end = text.begin() + position.column;
    return static_cast<int>(std::count_if(text.begin(), end, [](char c) { return !isContinuation(c); }));
}

TextPosition TextEdit::atDisplayColumn(int index, int column) const {
    const std::string& text = line(index);
    std::size_t at = 0;
    for (int passed = 0; passed < column && at < text.size(); ++passed) {
        ++at;
        while (at < text.size() && isContinuation(text[at])) ++at;
    }
    return {index, static_cast<int>(at)};
}

std::size_t TextEdit::byteCount() const {
    std::size_t total = lines_.size() - 1;
    for (const std::string& text : lines_) total += text.size();
    return total;
}

// Typing a word is one step to undo, however many keys it took.
void TextEdit::remember(Change change) {
    ++revision_;
    if (change != Change::None && change == lastChange_) return;
    undo_.push_back({lines_, cursor_});
    if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
    redo_.clear();
    lastChange_ = change;
}

void TextEdit::eraseRange(TextPosition begin, TextPosition end) {
    const auto first = static_cast<std::size_t>(begin.line);
    const auto last = static_cast<std::size_t>(end.line);
    std::string joined = lines_[first].substr(0, static_cast<std::size_t>(begin.column)) + lines_[last].substr(static_cast<std::size_t>(end.column));
    lines_[first] = std::move(joined);
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(first) + 1, lines_.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    cursor_ = begin;
    anchor_.reset();
}

void TextEdit::insertClean(std::string_view text) {
    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        pieces.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    const auto index = static_cast<std::size_t>(cursor_.line);
    const auto column = static_cast<std::size_t>(cursor_.column);
    const std::string tail = lines_[index].substr(column);
    pieces.front().insert(0, lines_[index], 0, column);
    const int newColumn = static_cast<int>(pieces.back().size());
    pieces.back() += tail;
    cursor_ = {cursor_.line + static_cast<int>(pieces.size()) - 1, newColumn};
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(index), std::make_move_iterator(pieces.begin()), std::make_move_iterator(pieces.end()));
}

bool TextEdit::insert(std::string_view text) {
    const std::string safe = cleaned(text);
    if (safe.empty()) return true;
    const std::size_t selected = selectedText().size();
    if (byteCount() - selected + safe.size() > kMaxBytes) return false;
    const bool typing = safe.size() <= 4 && safe.find('\n') == std::string::npos && !hasSelection();
    remember(typing ? Change::Typing : Change::None);
    if (hasSelection()) eraseRange(selectionBegin(), selectionEnd());
    anchor_.reset();
    wantedColumn_.reset();
    insertClean(safe);
    return true;
}

void TextEdit::newline() {
    const std::string& current = line(cursor_.line);
    const std::size_t indent = std::min(current.find_first_not_of(' '), current.size());
    insert("\n" + current.substr(0, std::min(indent, static_cast<std::size_t>(cursor_.column))));
}

void TextEdit::deleteSelection() {
    if (!hasSelection()) return;
    remember(Change::None);
    eraseRange(selectionBegin(), selectionEnd());
    wantedColumn_.reset();
}

void TextEdit::backspace() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    if (cursor_ == TextPosition{}) return;
    remember(Change::Erasing);
    const TextPosition end = cursor_;
    moveLeft();
    lastChange_ = Change::Erasing;
    eraseRange(cursor_, end);
}

void TextEdit::deleteForward() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    if (cursor_.line == lineCount() - 1 && static_cast<std::size_t>(cursor_.column) == line(cursor_.line).size()) return;
    remember(Change::Erasing);
    const TextPosition begin = cursor_;
    moveRight();
    lastChange_ = Change::Erasing;
    eraseRange(begin, cursor_);
}

void TextEdit::prepareMove(bool extend) {
    if (extend && !anchor_) anchor_ = cursor_;
    if (!extend) anchor_.reset();
    lastChange_ = Change::None;
}

void TextEdit::moveLeft(bool extend) {
    const bool collapse = !extend && hasSelection();
    const TextPosition begin = selectionBegin();
    prepareMove(extend);
    wantedColumn_.reset();
    if (collapse) {
        cursor_ = begin;
    } else if (cursor_.column > 0) {
        const std::string& text = line(cursor_.line);
        do {
            --cursor_.column;
        } while (cursor_.column > 0 && isContinuation(text[static_cast<std::size_t>(cursor_.column)]));
    } else if (cursor_.line > 0) {
        --cursor_.line;
        cursor_.column = static_cast<int>(line(cursor_.line).size());
    }
}

void TextEdit::moveRight(bool extend) {
    const bool collapse = !extend && hasSelection();
    const TextPosition end = selectionEnd();
    prepareMove(extend);
    wantedColumn_.reset();
    const std::string& text = line(cursor_.line);
    if (collapse) {
        cursor_ = end;
    } else if (static_cast<std::size_t>(cursor_.column) < text.size()) {
        ++cursor_.column;
        while (static_cast<std::size_t>(cursor_.column) < text.size() && isContinuation(text[static_cast<std::size_t>(cursor_.column)])) ++cursor_.column;
    } else if (cursor_.line + 1 < lineCount()) {
        ++cursor_.line;
        cursor_.column = 0;
    }
}

void TextEdit::moveVertically(int lines, bool extend) {
    if (!wantedColumn_) wantedColumn_ = displayColumn(cursor_);
    prepareMove(extend);
    cursor_ = atDisplayColumn(std::clamp(cursor_.line + lines, 0, lineCount() - 1), *wantedColumn_);
}

void TextEdit::moveToLineStart(bool extend) {
    prepareMove(extend);
    wantedColumn_.reset();
    const std::string& text = line(cursor_.line);
    const int firstWord = static_cast<int>(std::min(text.find_first_not_of(' '), text.size()));
    cursor_.column = cursor_.column == firstWord ? 0 : firstWord;
}

void TextEdit::moveToLineEnd(bool extend) {
    prepareMove(extend);
    wantedColumn_.reset();
    cursor_.column = static_cast<int>(line(cursor_.line).size());
}

void TextEdit::moveToStart(bool extend) {
    prepareMove(extend);
    wantedColumn_.reset();
    cursor_ = {};
}

void TextEdit::moveToEnd(bool extend) {
    prepareMove(extend);
    wantedColumn_.reset();
    const int last = lineCount() - 1;
    cursor_ = {last, static_cast<int>(line(last).size())};
}

void TextEdit::place(int index, int column, bool extend) {
    prepareMove(extend);
    wantedColumn_.reset();
    cursor_ = atDisplayColumn(std::clamp(index, 0, lineCount() - 1), std::max(0, column));
}

void TextEdit::selectAll() {
    moveToStart();
    moveToEnd(true);
}

void TextEdit::restore(Snapshot& from) {
    lines_ = std::move(from.lines);
    cursor_ = from.cursor;
    anchor_.reset();
    wantedColumn_.reset();
    lastChange_ = Change::None;
    ++revision_;
}

bool TextEdit::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({lines_, cursor_});
    restore(undo_.back());
    undo_.pop_back();
    return true;
}

bool TextEdit::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({lines_, cursor_});
    restore(redo_.back());
    redo_.pop_back();
    return true;
}

}  // namespace bancada::editor
