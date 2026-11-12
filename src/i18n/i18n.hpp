// The game's languages. Portuguese is the original: every text in the code is written in it, and the
// other languages look that Portuguese up in a catalog, the way gettext does. A text missing from a
// catalog shows in Portuguese, so a forgotten line is never a blank on screen.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bancada::i18n {

enum class Language : std::uint8_t { Portuguese, English };
inline constexpr int kLanguageCount = 2;

void setLanguage(Language language);
Language language();

// "pt" and "en", for saving the choice; and back, nothing for a code it doesn't know.
std::string_view codeOf(Language language);
std::optional<Language> languageFromCode(std::string_view code);
// Each language's name in itself: "Português", "English".
std::string_view nameOf(Language language);

// The text in the chosen language. The result lives as long as the text passed in.
std::string_view tr(std::string_view portuguese);
const char* tr(const char* portuguese);

// tr, then each "{}" in it replaced by the next argument, in order.
std::string format(std::string_view portuguese, std::initializer_list<std::string_view> arguments);

// The English catalog: Portuguese text and its English. For the tests, which check it covers the lessons.
struct Entry {
    std::string_view portuguese;
    std::string_view english;
};
std::span<const Entry> englishCatalog();

}  // namespace bancada::i18n
