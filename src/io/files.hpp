#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bancada::io {

// Paths reach us as UTF-8, and user folders can have accents, on every system.
std::filesystem::path pathFromUtf8(std::string_view text);
std::string pathToUtf8(const std::filesystem::path& path);

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& path, std::uint64_t maxSize);

// Writes to a temporary file beside the target, flushes it to disk and renames it over the target, so a
// crash or power cut leaves the old file or the new one, and never half of either.
bool writeFileAtomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

}  // namespace bancada::io
