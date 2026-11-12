#include "electric/units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace bancada::electric {

namespace {

bool endsWith(std::string_view text, std::string_view tail) { return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail; }

double prefixValue(char prefix) {
    switch (prefix) {
        case 'p': return 1e-12;
        case 'n': return 1e-9;
        case 'u': return 1e-6;
        case 'm': return 1e-3;
        case 'k':
        case 'K': return 1e3;
        case 'M': return 1e6;
        case 'G': return 1e9;
        case 'R':
        case 'r': return 1.0;  // 4R7 is how 4.7 ohms is marked on a part
        default: return 0.0;
    }
}

bool allDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

}  // namespace

double storedUnit(Quantity quantity) {
    switch (quantity) {
        case Quantity::Volts:
        case Quantity::Ohms: return 1e-3;
        case Quantity::Farads: return 1e-12;
    }
    return 1.0;
}

ValueRange rangeOf(Quantity quantity) {
    switch (quantity) {
        case Quantity::Volts: return {1, 1'000'000};
        case Quantity::Ohms: return {1, 1'000'000'000'000};
        case Quantity::Farads: return {1, 10'000'000'000'000};
    }
    return {1, 1};
}

double toSi(std::uint64_t stored, Quantity quantity) { return static_cast<double>(stored) * storedUnit(quantity); }

std::optional<std::uint64_t> parseQuantity(std::string_view input, Quantity quantity) {
    std::string text;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (std::isspace(c) != 0) continue;
        if (c == ',') {
            text += '.';
        } else if (c == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xB5) {
            text += 'u';  // the micro sign
            ++i;
        } else if (c == 0xCE && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xBC) {
            text += 'u';  // the Greek letter mu, which looks the same
            ++i;
        } else {
            text += static_cast<char>(c);
        }
    }
    // The unit is optional: "5", "5V" and "5 V" are the same. Longer spellings first, so "ohms" isn't read as "ohm".
    static constexpr std::string_view kVolts[] = {"V", "v"};
    static constexpr std::string_view kFarads[] = {"F", "f"};
    static constexpr std::string_view kOhms[] = {"\xCE\xA9", "ohms", "Ohms", "ohm", "Ohm"};
    const std::span<const std::string_view> units = quantity == Quantity::Volts    ? std::span<const std::string_view>(kVolts)
                                                    : quantity == Quantity::Farads ? std::span<const std::string_view>(kFarads)
                                                                                   : std::span<const std::string_view>(kOhms);
    for (const std::string_view unit : units) {
        if (endsWith(text, unit)) {
            text.resize(text.size() - unit.size());
            break;
        }
    }
    if (text.empty()) return std::nullopt;

    const std::size_t letter = text.find_first_not_of("0123456789.-+");
    double mantissa = 0.0;
    double scale = 1.0;
    char* end = nullptr;
    if (letter == std::string::npos) {
        mantissa = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) return std::nullopt;
    } else {
        scale = prefixValue(text[letter]);
        if (scale == 0.0 || (text[letter] == 'R' && quantity != Quantity::Ohms)) return std::nullopt;
        const std::string before = text.substr(0, letter);
        const std::string after = text.substr(letter + 1);
        if (!after.empty()) {
            // 4k7: the letter is also the decimal point.
            if (!allDigits(before) || !allDigits(after)) return std::nullopt;
            mantissa = std::strtod((before + "." + after).c_str(), &end);
        } else {
            mantissa = std::strtod(before.c_str(), &end);
            if (before.empty() || end != before.c_str() + before.size()) return std::nullopt;
        }
    }
    const double stored = std::round(mantissa * scale / storedUnit(quantity));
    const ValueRange range = rangeOf(quantity);
    if (!std::isfinite(stored) || stored < static_cast<double>(range.least) || stored > static_cast<double>(range.most)) return std::nullopt;
    return static_cast<std::uint64_t>(stored);
}

std::string formatSi(double value, const char* unit) {
    if (!std::isfinite(value)) return std::string("? ") + unit;
    if (std::abs(value) < 1e-15) return std::string("0 ") + unit;
    static constexpr struct {
        double scale;
        const char* prefix;
    } kPrefixes[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""}, {1e-3, "m"}, {1e-6, "\xC2\xB5"}, {1e-9, "n"}, {1e-12, "p"}};
    std::size_t chosen = std::size(kPrefixes) - 1;
    for (std::size_t i = 0; i < std::size(kPrefixes); ++i) {
        if (std::abs(value) >= kPrefixes[i].scale * 0.9995) {
            chosen = i;
            break;
        }
    }
    const double mantissa = value / kPrefixes[chosen].scale;
    char digits[32];
    const double size = std::abs(mantissa);
    std::snprintf(digits, sizeof digits, size >= 99.95 ? "%.0f" : size >= 9.995 ? "%.1f" : "%.2f", mantissa);
    std::string number = digits;
    if (number.find('.') != std::string::npos) {
        while (number.back() == '0') number.pop_back();
        if (number.back() == '.') number.pop_back();
    }
    std::replace(number.begin(), number.end(), '.', ',');
    return number + " " + kPrefixes[chosen].prefix + unit;
}

const char* unitSymbol(Quantity quantity) { return quantity == Quantity::Volts ? "V" : quantity == Quantity::Ohms ? "\xCE\xA9" : "F"; }

std::string formatQuantity(std::uint64_t stored, Quantity quantity) { return formatSi(toSi(stored, quantity), unitSymbol(quantity)); }

std::string formatNumber(std::uint64_t stored, Quantity quantity) {
    std::string text = formatSi(toSi(stored, quantity), "");
    std::erase(text, ' ');
    return text;
}

}  // namespace bancada::electric
