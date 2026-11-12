#include "editor/electric_examples.hpp"

#include "editor/layout.hpp"

namespace bancada::editor {

namespace {

constexpr std::uint64_t kOhm = 1000;              // values are kept in milliohms
constexpr std::uint64_t kMicrofarad = 1'000'000;  // and in picofarads
constexpr std::uint8_t kUpright = 1;              // a quarter turn: first pin at the top

// A battery whose minus goes to ground and whose plus is the named supply.
void supply(Layout& l, Point at, const char* name) {
    const PartId battery = l.part(PartKind::Battery, at);
    l.name(battery, 0, name, true);
    const PartId ground = l.part(PartKind::Ground, {at.x + 6, at.y + 3});
    l.wire(battery, 1u, ground, 0u);
}

}  // namespace

LedExample buildLedExample() {
    LedExample out;
    Layout l(out.board);
    const PartId battery = l.part(PartKind::BatteryCell, {0, 10});  // the battery as it looks, here too
    out.toggle = l.part(PartKind::Switch, {0, 0});
    out.resistor = l.part(PartKind::Resistor, {8, 0}, 0, 330 * kOhm);
    out.light = l.part(PartKind::LedBulb, {16, 0});  // the LED as it looks: this is the beginners' example
    const PartId ground = l.part(PartKind::Ground, {9, 13});
    l.wire(battery, 0u, out.toggle, 0u);
    l.wire(out.toggle, 1u, out.resistor, 0u);
    l.wire(out.resistor, 1u, out.light, 0u);
    l.wire(out.light, 1u, battery, 1u);
    l.wire(battery, 1u, ground, 0u);
    return out;
}

NandExample buildTransistorNand() {
    NandExample out;
    Layout l(out.board);
    out.a = l.input("A", {0, 6});
    out.b = l.input("B", {0, 16});
    const PartId baseA = l.part(PartKind::Resistor, {8, 6}, 0, 10'000 * kOhm);
    const PartId baseB = l.part(PartKind::Resistor, {8, 16}, 0, 10'000 * kOhm);
    const PartId top = l.part(PartKind::Npn, {18, 5});
    const PartId bottom = l.part(PartKind::Npn, {18, 15});
    const PartId pullUp = l.part(PartKind::Resistor, {20, -6}, kUpright, 1'000 * kOhm);
    const PartId ground = l.part(PartKind::Ground, {20, 22});
    out.output = l.output("S", {32, 3});

    l.wire(out.a, 0u, baseA, 0u);
    l.wire(baseA, 1u, top, 0u);
    l.wire(out.b, 0u, baseB, 0u);
    l.wire(baseB, 1u, bottom, 0u);
    l.wire(top, 2u, bottom, 1u);
    l.wire(bottom, 2u, ground, 0u);
    l.wire(pullUp, 1u, top, 1u);
    l.name(pullUp, 0, "5V", true);
    l.wire(top, 1u, out.output, 0u);
    supply(l, {34, 18}, "5V");
    return out;
}

BlinkerExample buildBlinker() {
    BlinkerExample out;
    Layout l(out.board);
    const auto side = [&](int x, std::uint64_t baseOhms) {
        struct Side {
            PartId transistor, light;
        };
        const PartId transistor = l.part(PartKind::Npn, {x, 22});
        const PartId light = l.part(PartKind::Light, {x + 2, 12}, kUpright);
        const PartId load = l.part(PartKind::Resistor, {x + 2, 2}, kUpright, 330 * kOhm);
        const PartId base = l.part(PartKind::Resistor, {x - 5, 12}, kUpright, baseOhms);
        const PartId ground = l.part(PartKind::Ground, {x + 2, 28});
        l.name(load, 0, "5V", true);
        l.name(base, 0, "5V", true);
        l.wire(load, 1u, light, 0u);
        l.wire(light, 1u, transistor, 1u);
        l.wire(base, 1u, transistor, 0u);
        l.wire(transistor, 2u, ground, 0u);
        return Side{transistor, light};
    };
    // Slightly different, so that one side starts first: a perfectly even pair would wait forever.
    const auto first = side(12, 47'000 * kOhm);
    const auto second = side(42, 51'000 * kOhm);
    out.left = first.light;
    out.right = second.light;
    const PartId toSecond = l.part(PartKind::Capacitor, {24, 16}, 0, 10 * kMicrofarad);
    const PartId toFirst = l.part(PartKind::Capacitor, {24, 30}, 0, 10 * kMicrofarad);
    l.wire(first.transistor, 1u, toSecond, 0u);
    l.wire(toSecond, 1u, second.transistor, 0u);
    l.wire(second.transistor, 1u, toFirst, 1u);
    l.wire(toFirst, 0u, first.transistor, 0u);
    supply(l, {56, 2}, "5V");
    return out;
}

}  // namespace bancada::editor
