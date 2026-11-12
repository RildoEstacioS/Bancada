#include "io/container.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bancada::io {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{0x89, 'B', 'C', 'D', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kEntrySize = 32;
constexpr std::uint32_t kMaxChunks = 256;
constexpr std::uint64_t kMaxRawSize = std::uint64_t{1} << 31;
constexpr std::uint32_t kFlagCompressed = 1;
constexpr int kCompressionLevel = 6;

void putU32(Bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void putU64(Bytes& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

std::uint32_t getU32(const std::uint8_t* in) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= std::uint32_t{in[i]} << (i * 8);
    return value;
}

std::uint64_t getU64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= std::uint64_t{in[i]} << (i * 8);
    return value;
}

std::uint64_t hashOf(const std::uint8_t* data, std::size_t size) { return XXH3_64bits(data, size); }

// From version 3 on, the version number goes into the table's hash, so that a flipped bit there can't
// make a file read as a different format. Older files were written without it.
std::uint64_t tableHash(const std::uint8_t* table, std::size_t size, std::uint32_t version) {
    return XXH3_64bits_withSeed(table, size, version >= 3 ? version : 0);
}

Bytes compressed(const Bytes& raw) {
    Bytes out(ZSTD_compressBound(raw.size()));
    const std::size_t size = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), kCompressionLevel);
    out.resize(ZSTD_isError(size) != 0 ? 0 : size);
    return out;
}

}  // namespace

Bytes packContainer(std::uint32_t version, std::span<const Chunk> chunks) {
    struct Stored {
        Bytes bytes;
        std::uint32_t flags = 0;
    };
    std::vector<Stored> stored;
    stored.reserve(chunks.size());
    for (const Chunk& chunk : chunks) {
        Bytes packed = compressed(chunk.data);
        if (!packed.empty() && packed.size() < chunk.data.size()) {
            stored.push_back({std::move(packed), kFlagCompressed});
        } else {
            stored.push_back({chunk.data, 0});
        }
    }

    Bytes table;
    table.reserve(chunks.size() * kEntrySize);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        table.insert(table.end(), chunks[i].tag.begin(), chunks[i].tag.end());
        putU32(table, stored[i].flags);
        putU64(table, stored[i].bytes.size());
        putU64(table, chunks[i].data.size());
        putU64(table, hashOf(stored[i].bytes.data(), stored[i].bytes.size()));
    }

    Bytes out(kMagic.begin(), kMagic.end());
    putU32(out, version);
    putU32(out, static_cast<std::uint32_t>(chunks.size()));
    putU64(out, tableHash(table.data(), table.size(), version));
    out.insert(out.end(), table.begin(), table.end());
    for (const Stored& item : stored) out.insert(out.end(), item.bytes.begin(), item.bytes.end());
    return out;
}

Container unpackContainer(std::span<const std::uint8_t> file, std::uint32_t maxVersion) {
    Container result;
    auto fail = [&](LoadError error) {
        result.chunks.clear();
        result.error = error;
        return result;
    };

    if (file.size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), file.begin())) {
        return fail(LoadError::NotOurFile);
    }
    if (file.size() < kHeaderSize) return fail(LoadError::Damaged);

    result.version = getU32(file.data() + 8);
    if (result.version > maxVersion) return fail(LoadError::NewerVersion);
    if (result.version == 0) return fail(LoadError::Damaged);

    const std::uint32_t count = getU32(file.data() + 12);
    if (count > kMaxChunks) return fail(LoadError::Damaged);
    const std::size_t tableSize = std::size_t{count} * kEntrySize;
    if (file.size() - kHeaderSize < tableSize) return fail(LoadError::Damaged);

    const std::uint8_t* const table = file.data() + kHeaderSize;
    if (tableHash(table, tableSize, result.version) != getU64(file.data() + 16)) return fail(LoadError::Damaged);

    std::uint64_t offset = kHeaderSize + tableSize;
    result.chunks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* const entry = table + std::size_t{i} * kEntrySize;
        const std::uint32_t flags = getU32(entry + 4);
        const std::uint64_t storedSize = getU64(entry + 8);
        const std::uint64_t rawSize = getU64(entry + 16);

        if (storedSize > file.size() - offset || rawSize > kMaxRawSize) return fail(LoadError::Damaged);
        const std::uint8_t* const stored = file.data() + offset;
        const auto storedBytes = static_cast<std::size_t>(storedSize);
        if (hashOf(stored, storedBytes) != getU64(entry + 24)) return fail(LoadError::Damaged);

        Chunk chunk;
        std::memcpy(chunk.tag.data(), entry, chunk.tag.size());
        if ((flags & kFlagCompressed) != 0) {
            if (ZSTD_getFrameContentSize(stored, storedBytes) != rawSize) return fail(LoadError::Damaged);
            chunk.data.resize(static_cast<std::size_t>(rawSize));
            const std::size_t size = ZSTD_decompress(chunk.data.data(), chunk.data.size(), stored, storedBytes);
            if (ZSTD_isError(size) != 0 || size != rawSize) return fail(LoadError::Damaged);
        } else {
            if (rawSize != storedSize) return fail(LoadError::Damaged);
            chunk.data.assign(stored, stored + storedBytes);
        }
        result.chunks.push_back(std::move(chunk));
        offset += storedSize;
    }
    if (offset != file.size()) return fail(LoadError::Damaged);
    return result;
}

const Chunk* findChunk(const Container& container, ChunkTag tag) {
    for (const Chunk& chunk : container.chunks) {
        if (chunk.tag == tag) return &chunk;
    }
    return nullptr;
}

}  // namespace bancada::io
