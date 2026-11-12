#include "io/files.hpp"

#include <cstdio>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bancada::io {

namespace {

std::FILE* openFile(const std::filesystem::path& path, bool writing) {
#ifdef _WIN32
    return _wfopen(path.c_str(), writing ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), writing ? "wb" : "rb");
#endif
}

bool flushToDisk(std::FILE* file) {
    if (std::fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

}  // namespace

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& path, std::uint64_t maxSize) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > maxSize) return std::nullopt;

    std::FILE* file = openFile(path, false);
    if (file == nullptr) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    const std::size_t got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (got != bytes.size()) return std::nullopt;
    return bytes;
}

bool writeFileAtomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";

    std::FILE* file = openFile(temporary, true);
    if (file == nullptr) return false;
    bool good = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    good = flushToDisk(file) && good;
    good = std::fclose(file) == 0 && good;

    std::error_code error;
    if (good) {
        std::filesystem::rename(temporary, path, error);
        good = !error;
    }
    if (!good) std::filesystem::remove(temporary, error);
    return good;
}

}  // namespace bancada::io
