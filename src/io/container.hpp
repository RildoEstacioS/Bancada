// The outside of a save file: a header and a set of named chunks, each compressed and hashed on its
// own. It knows nothing about what's in the chunks. Readers skip chunks they don't know, so a file can
// grow new ones without breaking older copies of the game.
//
//   0   8  magic: 0x89 "BCD" CR LF 0x1A LF, which also catches a file mangled as text
//   8   4  format version
//   12  4  chunk count
//   16  8  hash of the chunk table, seeded with the format version from version 3 on
//   24     chunk table, 32 bytes each: tag, flags, stored size, raw size, hash of the stored bytes
//   ...    the chunks' stored bytes, one after another, and nothing after them
//
// All numbers are little-endian. The hashes catch damage, not tampering: this is not security.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bancada::io {

using Bytes = std::vector<std::uint8_t>;
using ChunkTag = std::array<char, 4>;

enum class LoadError : std::uint8_t {
    CannotRead,    // the file couldn't be opened or read
    NotOurFile,    // it isn't one of our files
    NewerVersion,  // made by a newer version of the game
    Damaged,       // it's ours, but its bytes don't add up
    Invalid,       // the bytes are intact, but what they say can't be a project
};

struct Chunk {
    ChunkTag tag{};
    Bytes data;
};

struct Container {
    std::uint32_t version = 0;
    std::vector<Chunk> chunks;
    std::optional<LoadError> error;
};

Bytes packContainer(std::uint32_t version, std::span<const Chunk> chunks);

// Never trusts the input: sizes are checked before anything is allocated. Versions above maxVersion
// are refused before anything else is read, since the layout itself could have changed.
Container unpackContainer(std::span<const std::uint8_t> file, std::uint32_t maxVersion);

const Chunk* findChunk(const Container& container, ChunkTag tag);

}  // namespace bancada::io
