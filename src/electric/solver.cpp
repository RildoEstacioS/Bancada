#include "electric/solver.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace bancada::electric {

namespace {

constexpr int kMaxIterations = 200;
constexpr double kAbsoluteTolerance = 1e-7;
constexpr double kRelativeTolerance = 1e-7;
constexpr double kExponentLimit = 200.0;
// What every junction carries in parallel when solved, as in SPICE. Reported currents include it, so that what
// they say adds up to what was solved.
constexpr double kJunctionLeak = 1e-12;
constexpr double kSmallestStep = 1e-11;
constexpr double kStrayCapacitance = 1e-10;

// e^x that keeps going in a straight line instead of overflowing.
double boundedExp(double x) {
    if (x <= kExponentLimit) return std::exp(x);
    return std::exp(kExponentLimit) * (1.0 + x - kExponentLimit);
}

// An exponential's next guess can be absurdly far from the last one. This keeps the step to a size the
// exponential can be trusted over, which is the difference between converging and blowing up.
double limitJunction(double wanted, double last, double thermal, double critical, bool& limited) {
    if (wanted <= critical || std::abs(wanted - last) <= 2.0 * thermal) return wanted;
    limited = true;
    if (last <= 0.0) return thermal * std::log(wanted / thermal);
    const double scaled = (wanted - last) / thermal;
    return scaled > 0.0 ? last + thermal * (2.0 + std::log(scaled - 2.0)) : last - thermal * (2.0 + std::log(2.0 - scaled));
}

double criticalVoltage(double thermal, double saturation) { return thermal * std::log(thermal / (std::sqrt(2.0) * saturation)); }

// A MOSFET's equations are piecewise and tame, so a plain cap on the step is enough.
double limitStep(double wanted, double last, double most, bool& limited) {
    if (std::abs(wanted - last) <= most) return wanted;
    limited = true;
    return last + std::copysign(most, wanted - last);
}

struct Junction {
    double current;
    double conductance;
};

Junction diodeAt(double volts, double saturation, double thermal) {
    const double power = boundedExp(volts / thermal);
    return {saturation * (power - 1.0), saturation / thermal * power};
}

struct MosPoint {
    double current;
    double gm;
    double gds;
};

// Vgs and Vds here are already the right way round: Vds is never negative.
MosPoint mosAt(double vgs, double vds, const MosModel& model) {
    const double drive = vgs - model.threshold;
    if (drive <= 0.0) return {0.0, 0.0, 0.0};
    const double channel = 1.0 + model.lambda * vds;
    if (vds < drive) {
        const double shape = drive * vds - vds * vds / 2.0;
        return {model.gain * shape * channel, model.gain * vds * channel, model.gain * ((drive - vds) * channel + shape * model.lambda)};
    }
    const double shape = drive * drive / 2.0;
    return {model.gain * shape * channel, model.gain * drive * channel, model.gain * shape * model.lambda};
}

double polarity(Kind kind) { return kind == Kind::Pnp || kind == Kind::Pmos ? -1.0 : 1.0; }

}  // namespace

std::size_t Circuit::add(Element element) {
    elements_.push_back(element);
    return elements_.size() - 1;
}

std::size_t Circuit::addResistor(Node a, Node b, double ohms) {
    Element e;
    e.kind = Kind::Resistor;
    e.a = a;
    e.b = b;
    e.value = ohms;
    return add(e);
}

std::size_t Circuit::addSource(Node plus, Node minus, double volts, double series) {
    Element e;
    e.kind = Kind::Source;
    e.a = plus;
    e.b = minus;
    e.value = volts;
    e.series = series;
    return add(e);
}

std::size_t Circuit::addDiode(Node anode, Node cathode, DiodeModel model) {
    Element e;
    e.kind = Kind::Diode;
    e.a = anode;
    e.b = cathode;
    e.diode = model;
    return add(e);
}

std::size_t Circuit::addNpn(Node base, Node collector, Node emitter, BjtModel model) {
    Element e;
    e.kind = Kind::Npn;
    e.a = base;
    e.b = collector;
    e.c = emitter;
    e.bjt = model;
    return add(e);
}

std::size_t Circuit::addPnp(Node base, Node collector, Node emitter, BjtModel model) {
    const std::size_t index = addNpn(base, collector, emitter, model);
    elements_[index].kind = Kind::Pnp;
    return index;
}

std::size_t Circuit::addNmos(Node gate, Node drain, Node source, MosModel model) {
    Element e;
    e.kind = Kind::Nmos;
    e.a = gate;
    e.b = drain;
    e.c = source;
    e.mos = model;
    return add(e);
}

std::size_t Circuit::addPmos(Node gate, Node drain, Node source, MosModel model) {
    const std::size_t index = addNmos(gate, drain, source, model);
    elements_[index].kind = Kind::Pmos;
    return index;
}

std::size_t Circuit::addCapacitor(Node a, Node b, double farads) {
    Element e;
    e.kind = Kind::Capacitor;
    e.a = a;
    e.b = b;
    e.value = farads;
    return add(e);
}

Solver::Solver(Circuit circuit) : circuit_(std::move(circuit)) {
    const std::size_t count = circuit_.elements().size();
    sourceRow_.assign(count, -1);
    size_ = circuit_.nodeCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (circuit_.elements()[i].kind == Kind::Source) sourceRow_[i] = size_++;
    }
    const auto cells = static_cast<std::size_t>(size_);
    matrix_.assign(cells * cells, 0.0);
    rhs_.assign(cells, 0.0);
    x_.assign(cells, 0.0);
    previous_.assign(cells, 0.0);
    pivots_.assign(cells, 0);
    firstJunction_.assign(count, 0.0);
    secondJunction_.assign(count, 0.0);
    capacitorVolts_.assign(count, 0.0);
    capacitorAmps_.assign(count, 0.0);
}

double Solver::nodeVolts(const std::vector<double>& values, Node node) const {
    return node == kGround ? 0.0 : values[static_cast<std::size_t>(node - 1)];
}

double Solver::volts(Node node) const { return nodeVolts(x_, node); }

void Solver::stampMatrix(int row, int column, double value) {
    matrix_[static_cast<std::size_t>(row) * static_cast<std::size_t>(size_) + static_cast<std::size_t>(column)] += value;
}

void Solver::stampNode(Node row, Node column, double value) {
    if (row != kGround && column != kGround) stampMatrix(row - 1, column - 1, value);
}

void Solver::stampRhs(Node row, double value) {
    if (row != kGround) rhs_[static_cast<std::size_t>(row - 1)] += value;
}

void Solver::stampConductance(Node a, Node b, double conductance) {
    stampNode(a, a, conductance);
    stampNode(b, b, conductance);
    stampNode(a, b, -conductance);
    stampNode(b, a, -conductance);
}

void Solver::stampDiode(std::size_t index, const Conditions& when) {
    const Element& e = circuit_.elements()[index];
    const double thermal = e.diode.emission * kThermalVoltage;
    const double applied = limitJunction(volts(e.a) - volts(e.b), firstJunction_[index], thermal, criticalVoltage(thermal, e.diode.saturation), limited_);
    firstJunction_[index] = applied;
    const Junction j = diodeAt(applied, e.diode.saturation, thermal);
    const double g = j.conductance + when.minConductance;
    const double level = j.current + when.minConductance * applied - g * applied;
    stampConductance(e.a, e.b, g);
    stampRhs(e.a, -level);
    stampRhs(e.b, level);
}

void Solver::stampBjt(std::size_t index, const Conditions& when) {
    const Element& e = circuit_.elements()[index];
    const double p = polarity(e.kind);
    const double thermal = kThermalVoltage;
    const double critical = criticalVoltage(thermal, e.bjt.saturation);
    const double vbe = limitJunction(p * (volts(e.a) - volts(e.c)), firstJunction_[index], thermal, critical, limited_);
    const double vbc = limitJunction(p * (volts(e.a) - volts(e.b)), secondJunction_[index], thermal, critical, limited_);
    firstJunction_[index] = vbe;
    secondJunction_[index] = vbc;

    const Junction forward = diodeAt(vbe, e.bjt.saturation, thermal);
    const Junction reverse = diodeAt(vbc, e.bjt.saturation, thermal);
    const double reverseToBase = 1.0 / e.bjt.betaReverse;
    const double gmin = when.minConductance;

    const double ic = forward.current - reverse.current * (1.0 + reverseToBase) - gmin * vbc;
    const double ib = forward.current / e.bjt.betaForward + reverse.current * reverseToBase + gmin * (vbe + vbc);
    const double icByVbe = forward.conductance;
    const double icByVbc = -reverse.conductance * (1.0 + reverseToBase) - gmin;
    const double ibByVbe = forward.conductance / e.bjt.betaForward + gmin;
    const double ibByVbc = reverse.conductance * reverseToBase + gmin;

    const Node base = e.a;
    const Node collector = e.b;
    const Node emitter = e.c;
    // The polarity cancels on the conductances: it appears once in the voltages and once in the currents.
    const auto into = [&](Node row, double sign, double byVbe, double byVbc, double level) {
        stampNode(row, base, sign * (byVbe + byVbc));
        stampNode(row, emitter, -sign * byVbe);
        stampNode(row, collector, -sign * byVbc);
        stampRhs(row, -sign * p * level);
    };
    const double collectorLevel = ic - icByVbe * vbe - icByVbc * vbc;
    const double baseLevel = ib - ibByVbe * vbe - ibByVbc * vbc;
    into(collector, 1.0, icByVbe, icByVbc, collectorLevel);
    into(base, 1.0, ibByVbe, ibByVbc, baseLevel);
    into(emitter, -1.0, icByVbe + ibByVbe, icByVbc + ibByVbc, collectorLevel + baseLevel);
}

void Solver::stampMos(std::size_t index, const Conditions& when) {
    const Element& e = circuit_.elements()[index];
    const double p = polarity(e.kind);
    const double rawVgs = p * (volts(e.a) - volts(e.c));
    const double rawVds = p * (volts(e.b) - volts(e.c));
    const double vgsApplied = limitStep(rawVgs, firstJunction_[index], 1.0, limited_);
    const double vdsApplied = limitStep(rawVds, secondJunction_[index], 2.0, limited_);
    firstJunction_[index] = vgsApplied;
    secondJunction_[index] = vdsApplied;

    // A MOSFET has no fixed drain: whichever side is further along acts as one.
    const bool swapped = vdsApplied < 0.0;
    const Node drain = swapped ? e.c : e.b;
    const Node source = swapped ? e.b : e.c;
    const double vds = swapped ? -vdsApplied : vdsApplied;
    const double vgs = swapped ? vgsApplied - vdsApplied : vgsApplied;
    const MosPoint point = mosAt(vgs, vds, e.mos);

    stampConductance(drain, source, when.minConductance);
    stampNode(drain, e.a, point.gm);
    stampNode(drain, source, -point.gm - point.gds);
    stampNode(drain, drain, point.gds);
    stampNode(source, e.a, -point.gm);
    stampNode(source, source, point.gm + point.gds);
    stampNode(source, drain, -point.gds);
    const double level = p * (point.current - point.gm * vgs - point.gds * vds);
    stampRhs(drain, -level);
    stampRhs(source, level);
}

void Solver::stamp(std::size_t index, const Conditions& when) {
    const Element& e = circuit_.elements()[index];
    switch (e.kind) {
        case Kind::Resistor:
            stampConductance(e.a, e.b, 1.0 / e.value);
            break;
        case Kind::Source: {
            const int row = sourceRow_[index];
            if (e.a != kGround) {
                stampMatrix(e.a - 1, row, 1.0);
                stampMatrix(row, e.a - 1, 1.0);
            }
            if (e.b != kGround) {
                stampMatrix(e.b - 1, row, -1.0);
                stampMatrix(row, e.b - 1, -1.0);
            }
            stampMatrix(row, row, -e.series);
            rhs_[static_cast<std::size_t>(row)] += e.value * when.sourceScale;
            break;
        }
        case Kind::Capacitor:
            if (when.step > 0.0 && when.capacitors) {
                const double g = e.value / when.step;
                stampConductance(e.a, e.b, g);
                stampRhs(e.a, g * capacitorVolts_[index]);
                stampRhs(e.b, -g * capacitorVolts_[index]);
            }
            break;
        case Kind::Diode: stampDiode(index, when); break;
        case Kind::Npn:
        case Kind::Pnp: stampBjt(index, when); break;
        case Kind::Nmos:
        case Kind::Pmos: stampMos(index, when); break;
    }
}

// Gaussian elimination with partial pivoting, leaving the answer in rhs_.
bool Solver::solveLinear() {
    const auto n = static_cast<std::size_t>(size_);
    const auto at = [&](std::size_t r, std::size_t c) -> double& { return matrix_[r * n + c]; };
    for (std::size_t column = 0; column < n; ++column) {
        std::size_t best = column;
        for (std::size_t row = column + 1; row < n; ++row) {
            if (std::abs(at(row, column)) > std::abs(at(best, column))) best = row;
        }
        if (std::abs(at(best, column)) < 1e-300 || !std::isfinite(at(best, column))) return false;
        if (best != column) {
            for (std::size_t c = column; c < n; ++c) std::swap(at(best, c), at(column, c));
            std::swap(rhs_[best], rhs_[column]);
        }
        const double pivot = at(column, column);
        for (std::size_t row = column + 1; row < n; ++row) {
            const double factor = at(row, column) / pivot;
            if (factor == 0.0) continue;
            for (std::size_t c = column; c < n; ++c) at(row, c) -= factor * at(column, c);
            rhs_[row] -= factor * rhs_[column];
        }
    }
    for (std::size_t column = n; column-- > 0;) {
        double value = rhs_[column];
        for (std::size_t c = column + 1; c < n; ++c) value -= at(column, c) * rhs_[c];
        rhs_[column] = value / at(column, column);
    }
    return std::all_of(rhs_.begin(), rhs_.end(), [](double v) { return std::isfinite(v); });
}

void Solver::loadJunctions() {
    for (std::size_t i = 0; i < circuit_.elements().size(); ++i) {
        const Element& e = circuit_.elements()[i];
        const double p = polarity(e.kind);
        switch (e.kind) {
            case Kind::Diode:
                firstJunction_[i] = volts(e.a) - volts(e.b);
                break;
            case Kind::Npn:
            case Kind::Pnp:
                firstJunction_[i] = p * (volts(e.a) - volts(e.c));
                secondJunction_[i] = p * (volts(e.a) - volts(e.b));
                break;
            case Kind::Nmos:
            case Kind::Pmos:
                firstJunction_[i] = p * (volts(e.a) - volts(e.c));
                secondJunction_[i] = p * (volts(e.b) - volts(e.c));
                break;
            default:
                break;
        }
    }
}

bool Solver::newton(const Conditions& when) {
    const auto n = static_cast<std::size_t>(size_);
    const int nodes = circuit_.nodeCount();
    loadJunctions();
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        limited_ = false;
        std::fill(matrix_.begin(), matrix_.end(), 0.0);
        std::fill(rhs_.begin(), rhs_.end(), 0.0);
        for (std::size_t i = 0; i < circuit_.elements().size(); ++i) stamp(i, when);
        // A little path to ground everywhere, so that a node with nothing on it still has a voltage.
        for (int node = 0; node < nodes; ++node) {
            stampMatrix(node, node, when.minConductance);
            if (when.nodeCapacitance > 0.0) {
                const double g = when.nodeCapacitance / when.step;
                stampMatrix(node, node, g);
                rhs_[static_cast<std::size_t>(node)] += g * previous_[static_cast<std::size_t>(node)];
            }
        }
        if (n == 0) return true;
        if (!solveLinear()) return false;

        bool settled = !limited_ && iteration > 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(nodes); ++i) {
            if (std::abs(rhs_[i] - x_[i]) > kAbsoluteTolerance + kRelativeTolerance * std::abs(x_[i])) settled = false;
        }
        x_ = rhs_;
        if (settled) return true;
    }
    return false;
}

// When the plain attempt fails: first with every node leaning on ground and the lean slowly taken away, then
// with the batteries turned up from nothing.
bool Solver::solveHarder() {
    const std::vector<double> start = x_;
    Conditions when;
    bool ok = true;
    for (double g = 1e-2; g > 0.9e-12 && ok; g /= 10.0) {
        when.minConductance = g;
        ok = newton(when);
    }
    if (ok) return true;

    x_.assign(x_.size(), 0.0);
    when = Conditions{};
    for (int stage = 1; stage <= 40; ++stage) {
        when.sourceScale = static_cast<double>(stage) / 40.0;
        if (!newton(when)) {
            x_ = start;
            return false;
        }
    }
    return true;
}

bool Solver::solve() {
    const Conditions when;
    const std::vector<double> start = x_;
    if (newton(when)) {
        solvedOnce_ = true;
        return true;
    }
    x_ = start;
    if (solveHarder()) {
        solvedOnce_ = true;
        return true;
    }
    // The last resort, as in SPICE: let the circuit settle in time from nothing, the way it would if it were
    // switched on, and finish from wherever that ends.
    const std::vector<double> nothing(static_cast<std::size_t>(circuit_.nodeCount()), 0.0);
    if (settle(nothing) && newton(when)) {
        solvedOnce_ = true;
        return true;
    }
    x_ = start;
    return false;
}

void Solver::adoptGuess(std::span<const double> nodeVolts) {
    std::fill(x_.begin(), x_.end(), 0.0);
    const std::size_t count = std::min(nodeVolts.size(), static_cast<std::size_t>(circuit_.nodeCount()));
    std::copy_n(nodeVolts.begin(), count, x_.begin());
}

bool Solver::solveFrom(std::span<const double> nodeVolts) {
    adoptGuess(nodeVolts);
    return newton(Conditions{});
}

bool Solver::settle(std::span<const double> nodeVolts) {
    adoptGuess(nodeVolts);
    Conditions when;
    when.nodeCapacitance = 1e-9;
    when.step = 1e-9;
    when.capacitors = false;
    // A circuit with no resting state, such as one that oscillates, would keep this going forever.
    for (int budget = 4000; when.step < 1.0; --budget) {
        if (budget == 0) return false;
        previous_ = x_;
        if (!newton(when)) {
            // Too big a step for how fast things are moving: take a smaller one.
            x_ = previous_;
            when.step /= 4.0;
            if (when.step < 1e-13) return false;
            continue;
        }
        double moved = 0.0;
        for (int node = 0; node < circuit_.nodeCount(); ++node) {
            moved = std::max(moved, std::abs(x_[static_cast<std::size_t>(node)] - previous_[static_cast<std::size_t>(node)]));
        }
        // Moving a lot means there's more to come. Moving little in a large step means it's nearly there.
        when.step *= moved < 0.05 ? 2.0 : 1.2;
    }
    return true;
}

bool Solver::relax(std::span<const double> nodeVolts) { return settle(nodeVolts) && newton(Conditions{}); }

void Solver::dischargeCapacitors() {
    std::fill(capacitorVolts_.begin(), capacitorVolts_.end(), 0.0);
    std::fill(capacitorAmps_.begin(), capacitorAmps_.end(), 0.0);
}

bool Solver::stepOnce(double seconds, double nodeCapacitance) {
    Conditions when;
    when.step = seconds;
    when.nodeCapacitance = nodeCapacitance;
    previous_ = x_;
    if (!newton(when)) return false;
    for (std::size_t i = 0; i < circuit_.elements().size(); ++i) {
        const Element& e = circuit_.elements()[i];
        if (e.kind != Kind::Capacitor) continue;
        const double now = volts(e.a) - volts(e.b);
        capacitorAmps_[i] = e.value / seconds * (now - capacitorVolts_[i]);
        capacitorVolts_[i] = now;
    }
    return true;
}

// Where positive feedback makes a circuit snap from one state to another, real parts are saved by their own
// stray capacitance, and this model has none. So when a step can't be solved, every node gets a little of it
// and time goes by in smaller steps until the jump is over.
bool Solver::step(double seconds) {
    const std::vector<double> startX = x_;
    const std::vector<double> startVolts = capacitorVolts_;
    double done = 0.0;
    double size = seconds;
    double stray = 0.0;
    while (done < seconds * (1.0 - 1e-12)) {
        const double now = std::min(size, seconds - done);
        const std::vector<double> before = x_;
        if (stepOnce(now, stray)) {
            done += now;
            if (stray > 0.0) size = std::min(size * 1.3, seconds);
            continue;
        }
        x_ = before;
        if (stray == 0.0) {
            stray = kStrayCapacitance;
            continue;
        }
        size /= 2.0;
        if (size < kSmallestStep) {
            x_ = startX;
            capacitorVolts_ = startVolts;
            return false;
        }
    }
    return true;
}

Currents Solver::currents(std::size_t index) const {
    const Element& e = circuit_.elements()[index];
    const double va = volts(e.a);
    const double vb = volts(e.b);
    const double vc = volts(e.c);
    switch (e.kind) {
        case Kind::Resistor: {
            const double i = (va - vb) / e.value;
            return {i, -i, 0.0};
        }
        case Kind::Source: {
            const double i = x_[static_cast<std::size_t>(sourceRow_[index])];
            return {i, -i, 0.0};
        }
        case Kind::Capacitor:
            return {capacitorAmps_[index], -capacitorAmps_[index], 0.0};
        case Kind::Diode: {
            const double thermal = e.diode.emission * kThermalVoltage;
            const double i = diodeAt(va - vb, e.diode.saturation, thermal).current + kJunctionLeak * (va - vb);
            return {i, -i, 0.0};
        }
        case Kind::Npn:
        case Kind::Pnp: {
            const double p = polarity(e.kind);
            const Junction forward = diodeAt(p * (va - vc), e.bjt.saturation, kThermalVoltage);
            const Junction reverse = diodeAt(p * (va - vb), e.bjt.saturation, kThermalVoltage);
            const double reverseToBase = 1.0 / e.bjt.betaReverse;
            const double vbe = p * (va - vc);
            const double vbc = p * (va - vb);
            const double ic = forward.current - reverse.current * (1.0 + reverseToBase) - kJunctionLeak * vbc;
            const double ib = forward.current / e.bjt.betaForward + reverse.current * reverseToBase + kJunctionLeak * (vbe + vbc);
            return {p * ib, p * ic, -p * (ib + ic)};
        }
        case Kind::Nmos:
        case Kind::Pmos: {
            const double p = polarity(e.kind);
            const double rawVgs = p * (va - vc);
            const double rawVds = p * (vb - vc);
            const bool swapped = rawVds < 0.0;
            const double i = mosAt(swapped ? rawVgs - rawVds : rawVgs, swapped ? -rawVds : rawVds, e.mos).current;
            const double intoDrain = p * (swapped ? -i : i) + kJunctionLeak * (vb - vc);
            return {0.0, intoDrain, -intoDrain};
        }
    }
    return {};
}

}  // namespace bancada::electric
