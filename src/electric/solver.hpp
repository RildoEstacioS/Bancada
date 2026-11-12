// Real circuits: voltages, currents, and parts that aren't straight lines on a graph. Knows nothing about
// boards or windows. It takes elements between numbered nodes and finds what every node's voltage is.
//
// This is modified nodal analysis, the method SPICE uses, and the device equations are SPICE's too, so the
// tests can hold it against the real thing. Non-linear parts are solved by Newton's method, one straight-line
// approximation after another. Capacitors step through time with backward Euler, which never rings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bancada::electric {

// kT/q at 27 degrees C, which is the temperature SPICE assumes.
inline constexpr double kThermalVoltage = 0.025864918;

struct DiodeModel {
    double saturation = 1e-14;
    double emission = 1.0;
};

// A red LED: about 1.9 V at 1 mA and 2.0 V at 10 mA.
inline constexpr DiodeModel kLedModel{1e-19, 2.0};

struct BjtModel {
    double saturation = 1e-14;
    double betaForward = 100.0;
    double betaReverse = 1.0;
};

// The square-law MOSFET. Gain is KP * W / L, in A/V^2.
struct MosModel {
    double threshold = 1.0;
    double gain = 2e-3;
    double lambda = 0.01;
};

using Node = int;
inline constexpr Node kGround = 0;

enum class Kind : std::uint8_t { Resistor, Source, Diode, Npn, Pnp, Nmos, Pmos, Capacitor };

// The nodes are, by kind: resistor, capacitor and diode a-b (a is the anode); a source +, -;
// a bipolar transistor base, collector, emitter; a MOSFET gate, drain, source.
struct Element {
    Kind kind = Kind::Resistor;
    Node a = kGround;
    Node b = kGround;
    Node c = kGround;
    double value = 0.0;  // ohms, volts or farads
    double series = 0.0;  // a source's own resistance
    DiodeModel diode{};
    BjtModel bjt{};
    MosModel mos{};
};

class Circuit {
public:
    Node addNode() { return ++nodes_; }
    int nodeCount() const { return nodes_; }  // not counting ground

    std::size_t addResistor(Node a, Node b, double ohms);
    std::size_t addSource(Node plus, Node minus, double volts, double series = 0.0);
    std::size_t addDiode(Node anode, Node cathode, DiodeModel model = {});
    std::size_t addNpn(Node base, Node collector, Node emitter, BjtModel model = {});
    std::size_t addPnp(Node base, Node collector, Node emitter, BjtModel model = {});
    std::size_t addNmos(Node gate, Node drain, Node source, MosModel model = {});
    std::size_t addPmos(Node gate, Node drain, Node source, MosModel model = {});
    std::size_t addCapacitor(Node a, Node b, double farads);

    // Changes a source's volts or a resistor's ohms. The shape of the circuit never changes after it's built.
    void setValue(std::size_t element, double value) { elements_[element].value = value; }

    const std::vector<Element>& elements() const { return elements_; }

private:
    std::size_t add(Element element);

    int nodes_ = 0;
    std::vector<Element> elements_;
};

// The current going into each of an element's terminals. They always add up to zero.
struct Currents {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
};

class Solver {
public:
    explicit Solver(Circuit circuit);

    Circuit& circuit() { return circuit_; }
    const Circuit& circuit() const { return circuit_; }

    // Finds the steady state, starting from the last answer. Capacitors count as open. False if it can't.
    bool solve();

    // The same from these starting voltages, with no help: the nearest answer Newton's method can reach.
    bool solveFrom(std::span<const double> nodeVolts);

    // Lets the circuit settle from these voltages, as if each node had a little capacitance. Unlike solve(),
    // this can only end in a stable state, which is what makes it the way to read a circuit that remembers.
    bool relax(std::span<const double> nodeVolts);

    // Moves time forward. False, with nothing changed, if no step this small can be solved.
    bool step(double seconds);

    // Every capacitor empty.
    void dischargeCapacitors();

    double volts(Node node) const;
    std::vector<double> allVolts() const { return {x_.begin(), x_.begin() + circuit_.nodeCount()}; }
    Currents currents(std::size_t element) const;

private:
    struct Conditions {
        double step = 0.0;             // 0 is the steady state
        double minConductance = 1e-12;
        double sourceScale = 1.0;
        double nodeCapacitance = 0.0;  // on every node: for relaxing, and for steps too sudden to solve without it
        bool capacitors = true;        // the circuit's own, while time is passing
    };

    bool newton(const Conditions& when);
    bool settle(std::span<const double> nodeVolts);
    bool solveLinear();
    bool solveHarder();
    bool stepOnce(double seconds, double nodeCapacitance);
    void stamp(std::size_t index, const Conditions& when);
    void stampDiode(std::size_t index, const Conditions& when);
    void stampBjt(std::size_t index, const Conditions& when);
    void stampMos(std::size_t index, const Conditions& when);
    void stampMatrix(int row, int column, double value);
    void stampNode(Node row, Node column, double value);
    void stampRhs(Node row, double value);
    void stampConductance(Node a, Node b, double conductance);
    void adoptGuess(std::span<const double> nodeVolts);
    void loadJunctions();
    double nodeVolts(const std::vector<double>& values, Node node) const;

    Circuit circuit_;
    int size_ = 0;
    std::vector<int> sourceRow_;          // by element: its row in the matrix, or -1
    std::vector<double> matrix_;
    std::vector<double> rhs_;
    std::vector<double> x_;
    std::vector<double> previous_;
    std::vector<double> firstJunction_;   // by element: the junction voltage last used
    std::vector<double> secondJunction_;
    std::vector<double> capacitorVolts_;  // by element
    std::vector<double> capacitorAmps_;
    std::vector<int> pivots_;
    bool limited_ = false;
    bool solvedOnce_ = false;
};

}  // namespace bancada::electric
