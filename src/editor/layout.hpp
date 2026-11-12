// Lays out a board from code: the ready-made examples and the tests use it. A mistake throws rather than
// leaving a part or a wire off, and tests build every example, so mistakes don't reach the player.
#pragma once

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "editor/board.hpp"
#include "editor/router.hpp"
#include "editor/session.hpp"

namespace bancada::editor {

class Layout {
public:
    explicit Layout(Board& board) : board_(board) {}

    PartId add(Part part) {
        const std::optional<PartId> id = board_.addPart(part);
        if (!id) {
            throw std::logic_error("no room for " + std::string(partName(part)) + " " + part.label + " at " + std::to_string(part.position.x) + "," +
                                   std::to_string(part.position.y));
        }
        return *id;
    }

    PartId part(PartKind kind, Point at, std::uint8_t rotation = 0, std::uint64_t value = 0) {
        Part p = makePart(kind, at, rotation);
        if (value != 0) p.value = value;
        return add(std::move(p));
    }

    PartId box(const char* key, Point at, std::uint8_t width = 0, std::uint8_t address = 0) {
        Part part = makePart(PartKind::Box, at, 0, 0, 1, boxFromKey(key));
        if (width != 0) part.width = width;
        if (address != 0) part.address = address;
        return add(std::move(part));
    }

    PartId input(const char* label, Point at, std::uint8_t width = 1) {
        Part part = makePart(PartKind::Toggle, at);
        part.width = width;
        part.label = label;
        return add(std::move(part));
    }

    PartId output(const char* label, Point at, std::uint8_t width = 1) {
        Part part = makePart(width == 1 ? PartKind::Led : PartKind::Probe, at);
        part.width = width;
        part.label = label;
        return add(std::move(part));
    }

    PartId constant(std::uint64_t value, Point at, std::uint8_t width) {
        Part part = makePart(PartKind::Constant, at);
        part.width = width;
        part.value = value;
        return add(std::move(part));
    }

    PartId gate(PartKind kind, Point at) { return add(makePart(kind, at)); }

    PartId bundle(PartKind kind, Point at, std::uint8_t width, std::uint8_t branches) {
        Part part = makePart(kind, at, 0, branches);
        part.width = width;
        return add(std::move(part));
    }

    PartId chip(const ChipData& data, Point at) {
        Part part = makePart(PartKind::Chip, at);
        part.chip = data.id;
        part.interface = makeChipInterface(data.board, data.name);
        return add(std::move(part));
    }

    // A pin by the name it has on the box or chip.
    unsigned pin(PartId id, const char* name) const {
        const Part& part = *board_.part(id);
        const unsigned count = pinCount(part);
        for (unsigned i = 0; i < count; ++i) {
            if (std::strcmp(pinName(part, i), name) == 0) return i;
        }
        throw std::logic_error(std::string("no pin called ") + name);
    }

    int pinY(PartId id, unsigned index) const { return pinPosition(*board_.part(id), index).y; }

    void wire(PartId from, unsigned fromPin, PartId to, unsigned toPin) {
        const Drop start = pinDrop(from, fromPin);
        const Drop end = pinDrop(to, toPin);
        if (!board_.connect(start, end, routePath(board_, start.at, end.at))) {
            throw std::logic_error("no wire from " + std::to_string(start.at.x) + "," + std::to_string(start.at.y) + " to " +
                                   std::to_string(end.at.x) + "," + std::to_string(end.at.y));
        }
    }

    void wire(PartId from, const char* fromPin, PartId to, const char* toPin) { wire(from, pin(from, fromPin), to, pin(to, toPin)); }
    void wire(PartId from, unsigned fromPin, PartId to, const char* toPin) { wire(from, fromPin, to, pin(to, toPin)); }
    void wire(PartId from, const char* fromPin, PartId to, unsigned toPin) { wire(from, pin(from, fromPin), to, toPin); }

    // A name on any pin, to its left or its right, in place of a wire across the whole board.
    void name(PartId id, unsigned index, const char* net, bool onLeft, std::uint8_t width = 1) {
        const Point at = pinPosition(*board_.part(id), index);
        Part flag = makePart(PartKind::Tunnel, {0, 0}, onLeft ? 0 : 2);
        flag.label = net;
        flag.width = width;
        flag.position = onLeft ? Point{at.x - 2 - baseWidth(flag), at.y - 1} : Point{at.x + 2, at.y - 1};
        const PartId placed = add(std::move(flag));
        if (onLeft) {
            wire(placed, 0u, id, index);
        } else {
            wire(id, index, placed, 0u);
        }
    }

    void nameOutput(PartId id, const char* pinName_, const char* net, std::uint8_t width = 1) { name(id, pin(id, pinName_), net, false, width); }
    void nameInput(PartId id, const char* pinName_, const char* net, std::uint8_t width = 1) { name(id, pin(id, pinName_), net, true, width); }
    void nameInput(PartId id, const char* pinName_, std::uint8_t width = 1) { nameInput(id, pinName_, pinName_, width); }

private:
    Drop pinDrop(PartId id, unsigned index) const {
        return {Drop::Kind::Pin, pinPosition(*board_.part(id), index), id, static_cast<std::uint8_t>(index)};
    }

    Board& board_;
};

}  // namespace bancada::editor
