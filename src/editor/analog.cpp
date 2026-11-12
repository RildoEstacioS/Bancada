#include "editor/analog.hpp"

#include <algorithm>
#include <cmath>

namespace bancada::editor {

namespace {

constexpr double kLongestStep = 0.002;

double polarity(PartKind kind) { return kind == PartKind::Pnp || kind == PartKind::Pmos ? -1.0 : 1.0; }

}  // namespace

AnalogBoard::AnalogBoard(ElectricModel model) : model_(std::move(model)), solver_(model_.circuit) {}

void AnalogBoard::update(const std::vector<bool>& sourcesHigh, const std::vector<bool>& switchesClosed) {
    for (std::size_t i = 0; i < model_.sources.size() && i < sourcesHigh.size(); ++i) {
        solver_.circuit().setValue(model_.sources[i].element, sourcesHigh[i] ? kLogicHigh : 0.0);
    }
    for (std::size_t i = 0; i < model_.switches.size() && i < switchesClosed.size(); ++i) {
        solver_.circuit().setValue(model_.switches[i].element, switchesClosed[i] ? kSwitchClosedOhms : kSwitchOpenOhms);
    }
    if (!stepping()) {
        solver_.solve();
        return;
    }
    if (!started_) {
        // Switched on with every capacitor empty, from where the rest of the circuit would rest.
        solver_.solve();
        solver_.dischargeCapacitors();
        started_ = true;
    }
}

void AnalogBoard::advance(double seconds) {
    if (!stepping() || seconds <= 0.0) return;
    const int steps = std::max(1, static_cast<int>(std::ceil(seconds / kLongestStep)));
    for (int i = 0; i < steps; ++i) {
        if (!solver_.step(seconds / steps)) break;
    }
}

std::optional<double> AnalogBoard::groupVolts(std::uint32_t group) const {
    if (group >= model_.nodeOfGroup.size() || model_.nodeOfGroup[group] < 0) return std::nullopt;
    return solver_.volts(model_.nodeOfGroup[group]);
}

double AnalogBoard::pinAmps(PartId part, unsigned pin) const {
    const auto found = model_.parts.find(part);
    if (found != model_.parts.end()) {
        if (found->second.kind == PartKind::Ground) return 0.0;
        const electric::Currents c = solver_.currents(found->second.element);
        const double into = pin == 0 ? c.a : pin == 1 ? c.b : c.c;
        return -into;
    }
    for (const ElectricModel::Source& source : model_.sources) {
        if (source.driver == part && source.driverPin == pin) return -solver_.currents(source.element).a;
    }
    return 0.0;
}

std::optional<PartReading> AnalogBoard::reading(PartId part) const {
    const auto found = model_.parts.find(part);
    if (found == model_.parts.end()) return std::nullopt;
    PartReading out;
    out.kind = found->second.kind;
    if (out.kind == PartKind::Ground) return out;
    const electric::Element& e = solver_.circuit().elements()[found->second.element];
    const electric::Currents c = solver_.currents(found->second.element);
    const double va = solver_.volts(e.a);
    const double vb = solver_.volts(e.b);
    const double vc = solver_.volts(e.c);
    switch (out.kind) {
        case PartKind::Battery:
        case PartKind::BatteryCell:
            out.volts = va - vb;
            out.amps = -c.a;  // what it sends out of its + terminal
            out.power = out.volts * out.amps;
            out.burnt = out.amps > 3.0;
            out.state = out.burnt ? "em curto" : "";
            break;
        case PartKind::Npn:
        case PartKind::Pnp: {
            const double p = polarity(out.kind);
            out.volts = p * (va - vc);
            out.amps = p * c.b;
            out.control = p * c.a;
            out.power = std::abs((vb - vc) * c.b) + std::abs((va - vc) * c.a);
            const double vce = p * (vb - vc);
            out.state = out.volts < 0.5 ? "desligado" : vce < 0.25 ? "ligado, como chave fechada" : "amplificando";
            out.burnt = std::abs(out.amps) > 0.5;
            break;
        }
        case PartKind::Nmos:
        case PartKind::Pmos: {
            const double p = polarity(out.kind);
            out.volts = p * (va - vc);
            out.amps = p * c.b;
            out.power = std::abs((vb - vc) * c.b);
            const double drive = out.volts - e.mos.threshold;
            out.state = drive <= 0.0 ? "desligado" : p * (vb - vc) < drive ? "ligado, como chave fechada" : "saturado";
            out.burnt = std::abs(out.amps) > 0.5;
            break;
        }
        default:
            out.volts = va - vb;
            out.amps = c.a;
            out.power = std::abs(out.volts * out.amps);
            break;
    }
    switch (out.kind) {
        case PartKind::Light:
        case PartKind::LedBulb:
            out.light = std::clamp(out.amps / 0.01, 0.0, 1.0);
            out.burnt = out.amps > 0.05;
            break;
        case PartKind::Diode: out.burnt = out.amps > 1.0; break;
        case PartKind::Resistor: out.burnt = out.power > 0.5; break;
        case PartKind::Switch: out.state = e.value <= kSwitchClosedOhms * 2.0 ? "fechada" : "aberta"; break;
        default: break;
    }
    return out;
}

}  // namespace bancada::editor
