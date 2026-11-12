#include "i18n/i18n.hpp"

#include <unordered_map>

namespace bancada::i18n {

namespace {

Language& chosen() {
    static Language language = Language::Portuguese;
    return language;
}

const std::unordered_map<std::string_view, std::string_view>& english() {
    static const std::unordered_map<std::string_view, std::string_view> map = [] {
        std::unordered_map<std::string_view, std::string_view> built;
        for (const Entry& entry : englishCatalog()) built.emplace(entry.portuguese, entry.english);
        return built;
    }();
    return map;
}

}  // namespace

void setLanguage(Language language) { chosen() = language; }

Language language() { return chosen(); }

std::string_view codeOf(Language language) { return language == Language::English ? "en" : "pt"; }

std::optional<Language> languageFromCode(std::string_view code) {
    if (code == "pt") return Language::Portuguese;
    if (code == "en") return Language::English;
    return std::nullopt;
}

std::string_view nameOf(Language language) { return language == Language::English ? "English" : "Português"; }

std::string_view tr(std::string_view portuguese) {
    if (chosen() == Language::Portuguese || portuguese.empty()) return portuguese;
    const auto found = english().find(portuguese);
    return found == english().end() ? portuguese : found->second;
}

// The catalog's texts are string literals, so they end in a zero like the text passed in.
const char* tr(const char* portuguese) { return tr(std::string_view(portuguese)).data(); }

std::string format(std::string_view portuguese, std::initializer_list<std::string_view> arguments) {
    const std::string_view pattern = tr(portuguese);
    std::string out;
    out.reserve(pattern.size() + 16);
    const std::string_view* next = arguments.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}' && next != arguments.end()) {
            out += *next++;
            ++i;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

}  // namespace bancada::i18n
