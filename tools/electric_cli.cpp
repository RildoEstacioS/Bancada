// Solves a circuit read from a text file and prints every node's voltage. Only tools/compare_spice.py uses
// this, to hold the game's solver against ngspice.
//
//   nodes N
//   R a b ohms | V a b volts | D a b Is n | Q npn|pnp b c e Is betaF betaR | M nmos|pmos g d s Vt gain lambda
//
// With --from FILE, starts from those voltages instead, and doesn't try any other way of solving: this asks
// "is that an answer?", for circuits with more than one.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "electric/solver.hpp"

using namespace bancada::electric;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream in(argv[1]);
    Circuit circuit;
    std::string word;
    int nodes = 0;
    while (in >> word) {
        if (word == "nodes") {
            in >> nodes;
            for (int i = 0; i < nodes; ++i) circuit.addNode();
        } else if (word == "R") {
            int a = 0, b = 0;
            double v = 0;
            in >> a >> b >> v;
            circuit.addResistor(a, b, v);
        } else if (word == "V") {
            int a = 0, b = 0;
            double v = 0;
            in >> a >> b >> v;
            circuit.addSource(a, b, v);
        } else if (word == "D") {
            int a = 0, b = 0;
            DiodeModel m;
            in >> a >> b >> m.saturation >> m.emission;
            circuit.addDiode(a, b, m);
        } else if (word == "Q") {
            std::string type;
            int b = 0, c = 0, e = 0;
            BjtModel m;
            in >> type >> b >> c >> e >> m.saturation >> m.betaForward >> m.betaReverse;
            if (type == "npn") {
                circuit.addNpn(b, c, e, m);
            } else {
                circuit.addPnp(b, c, e, m);
            }
        } else if (word == "M") {
            std::string type;
            int g = 0, d = 0, s = 0;
            MosModel m;
            in >> type >> g >> d >> s >> m.threshold >> m.gain >> m.lambda;
            if (type == "nmos") {
                circuit.addNmos(g, d, s, m);
            } else {
                circuit.addPmos(g, d, s, m);
            }
        } else {
            return 2;
        }
    }
    Solver solver(std::move(circuit));
    bool solved = false;
    if (argc == 4 && std::strcmp(argv[2], "--from") == 0) {
        std::ifstream guessFile(argv[3]);
        std::vector<double> guess;
        double value = 0;
        while (guessFile >> value) guess.push_back(value);
        solved = solver.solveFrom(guess);
    } else {
        solved = solver.solve();
    }
    if (!solved) {
        std::puts("fail");
        return 1;
    }
    for (int node = 1; node <= nodes; ++node) std::printf("%.10e\n", solver.volts(node));
    return 0;
}
