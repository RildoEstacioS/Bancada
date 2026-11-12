"""Holds the game's electric solver against ngspice, on random circuits with every kind of part.

Needs ngspice and a built electric_cli. Nothing in the game depends on this: it's how the solver gets
checked against a program nobody here wrote.

    python3 compare_spice.py path/to/electric_cli [rounds]

A circuit with feedback can have more than one correct answer, and two solvers may well land on different
ones. So when the answers differ, the game's solver is started from ngspice's: if that's a solution of the
game's equations too, it stays put, and the two only chose differently. If it moves, one of them is wrong.
"""
import os
import random
import re
import subprocess
import sys
import tempfile

IS, N = 1e-14, 1.0
LED_IS, LED_N = 1e-19, 2.0
BF, BR = 100.0, 1.0
VT, KP, LAMBDA = 1.0, 2e-3, 0.01
TOLERANCE = 2e-3  # volts


def make_circuit(seed):
    rng = random.Random(seed)
    nodes = rng.randint(3, 14)
    ours, spice = [f'nodes {nodes}'], ['* random', '.options reltol=1e-6 vntol=1e-9 abstol=1e-13 gmin=1e-12 itl1=500 temp=27 tnom=27']
    spice += [
        f'.model DD D(IS={IS} N={N})',
        f'.model LL D(IS={LED_IS} N={LED_N})',
        f'.model QN NPN(IS={IS} BF={BF} BR={BR})',
        f'.model QP PNP(IS={IS} BF={BF} BR={BR})',
        f'.model MN NMOS(LEVEL=1 VTO={VT} KP={KP} LAMBDA={LAMBDA} IS=1e-30)',
        f'.model MP PMOS(LEVEL=1 VTO={-VT} KP={KP} LAMBDA={LAMBDA} IS=1e-30)',
    ]
    ours.append('V 1 0 5')
    spice.append('V1 1 0 5')
    count = 0

    def resistor(a, b):
        nonlocal count
        count += 1
        ohms = 10 ** rng.uniform(1, 6)
        ours.append(f'R {a} {b} {ohms!r}')
        spice.append(f'R{count} {a} {b} {ohms!r}')

    for node in range(2, nodes + 1):
        resistor(node, rng.randrange(0, node))
    for _ in range(rng.randint(2, 14)):
        count += 1
        a, b, c = rng.randint(0, nodes), rng.randint(0, nodes), rng.randint(0, nodes)
        kind = rng.randrange(7)
        if kind == 0 and a != b:
            ours.append(f'D {a} {b} {IS} {N}')
            spice.append(f'D{count} {a} {b} DD')
        elif kind == 1 and a != b:
            ours.append(f'D {a} {b} {LED_IS} {LED_N}')
            spice.append(f'D{count} {a} {b} LL')
        elif kind == 2:
            ours.append(f'Q npn {a} {b} {c} {IS} {BF} {BR}')
            spice.append(f'Q{count} {b} {a} {c} QN')
        elif kind == 3:
            ours.append(f'Q pnp {a} {b} {c} {IS} {BF} {BR}')
            spice.append(f'Q{count} {b} {a} {c} QP')
        elif kind == 4:
            ours.append(f'M nmos {a} {b} {c} {VT} {KP} {LAMBDA}')
            spice.append(f'M{count} {b} {a} {c} {c} MN W=1 L=1')
        elif kind == 5:
            ours.append(f'M pmos {a} {b} {c} {VT} {KP} {LAMBDA}')
            spice.append(f'M{count} {b} {a} {c} {c} MP W=1 L=1')
        elif a != b:
            resistor(a, b)
    probes = ' '.join(f'v({n})' for n in range(1, nodes + 1))
    spice += ['.control', 'op', f'print {probes}', '.endc', '.end']
    return nodes, '\n'.join(ours) + '\n', '\n'.join(spice) + '\n'


def run_spice(path, nodes):
    result = subprocess.run(['ngspice', '-b', path], capture_output=True, text=True, timeout=60)
    found = dict(re.findall(r'v\((\d+)\)\s*=\s*([-+0-9.eE]+)', result.stdout))
    if len(found) != nodes:
        return None
    return [float(found[str(n)]) for n in range(1, nodes + 1)]


def run_ours(cli, path, start=None):
    command = [cli, path] + (['--from', start] if start else [])
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        return None
    return [float(line) for line in result.stdout.split()]


def main():
    cli = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    same = chose_differently = wrong = spice_failed = ours_failed = both_failed = 0
    worst = 0.0
    with tempfile.TemporaryDirectory() as folder:
        ours_path, spice_path, guess_path = (os.path.join(folder, name) for name in ('c.txt', 'c.cir', 'g.txt'))
        for seed in range(rounds):
            nodes, ours_text, spice_text = make_circuit(seed)
            open(ours_path, 'w').write(ours_text)
            open(spice_path, 'w').write(spice_text)
            spice = run_spice(spice_path, nodes)
            ours = run_ours(cli, ours_path)
            if spice is None and ours is None:
                both_failed += 1
                continue
            if spice is None:
                spice_failed += 1
                continue
            if ours is None:
                ours_failed += 1
                print(f'seed {seed}: the game could not solve what ngspice did')
                continue
            error = max(abs(a - b) for a, b in zip(ours, spice))
            if error <= TOLERANCE:
                same += 1
                worst = max(worst, error)
                continue
            open(guess_path, 'w').write('\n'.join(repr(v) for v in spice))
            polished = run_ours(cli, ours_path, guess_path)
            if polished is not None and max(abs(a - b) for a, b in zip(polished, spice)) <= TOLERANCE:
                chose_differently += 1
            else:
                wrong += 1
                print(f'seed {seed}: DISAGREES by {error:.4f} V')
    print(f'{rounds} circuits: {same} agree (worst {worst * 1000:.3f} mV), '
          f'{chose_differently} are both right with different answers, {wrong} DISAGREE; '
          f'only ngspice failed on {spice_failed}, only the game on {ours_failed}, both on {both_failed}')
    return 1 if wrong else 0


if __name__ == '__main__':
    sys.exit(main())
