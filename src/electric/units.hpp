// Values as people write them: 4k7, 220, 10u, 4,7 kΩ. Kept on parts as whole numbers of a small unit, so
// that a saved file never holds a rounding error.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bancada::electric {

enum class Quantity : std::uint8_t { Volts, Ohms, Farads };

// Millivolts, milliohms and picofarads.
double storedUnit(Quantity quantity);

struct ValueRange {
    std::uint64_t least;
    std::uint64_t most;
};

// From 1 mV to 1000 V, from 1 mΩ to 1 GΩ, and from 1 pF to 10 F.
ValueRange rangeOf(Quantity quantity);

// Nothing if it isn't a value of this kind, or is outside the range.
std::optional<std::uint64_t> parseQuantity(std::string_view text, Quantity quantity);

std::string formatQuantity(std::uint64_t stored, Quantity quantity);

// The unit alone, "Ω", and the value without it, "4,7k", for a field where only the number is typed.
const char* unitSymbol(Quantity quantity);
std::string formatNumber(std::uint64_t stored, Quantity quantity);
double toSi(std::uint64_t stored, Quantity quantity);

// Any reading, with its unit: "4,32 mA", "0,72 V". Three figures, and a decimal comma.
std::string formatSi(double value, const char* unit);

}  // namespace bancada::electric
