#!/usr/bin/env python3
""".cpmx conformance test: firmware reader vs Python reference reader.

The firmware's CpmxReader (C++) is an independent implementation of the
format; this test asserts it agrees with the Python reference on every
test piece, and that both reject the same corrupt inputs. Motivated by the
RECT skip-table bug (commit a99691cd) that only a cross-implementation
diff would have caught.

For each input file (valid pieces + a corruption matrix of truncations and
bit flips), both dumpers run; the test fails if their exit codes differ,
or if both accept but their decoded output is not structurally identical.

Usage (from the repo root; converter venv must exist):
    python3 test/conformance/run_conformance.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONVERTER = REPO / "converter"
PYTHON = CONVERTER / ".venv" / "bin" / "python"
HERE = REPO / "test" / "conformance"
TOOL = HERE / "build" / "cpmx_dump"

TEST_PIECES = ["simple-melody", "march-excerpt", "repeat-endings"]

# Deterministic corruption matrix applied to the first valid file:
# truncation points (fractions of file size) and single-bit flips.
TRUNCATE_AT = [0.02, 0.1, 0.3, 0.6, 0.9, 0.99]
BIT_FLIPS = [4, 5, 6, 9, 17, 33, 65, 129, 257, 513]  # byte offsets, bit 0x40


def build_tool():
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "c++", "-std=c++2a", "-O1", "-fno-exceptions",
        "-I", str(HERE / "stubs"),
        "-I", str(REPO / "lib" / "Cpmx"),
        "-I", str(REPO / "lib" / "Memory"),
        str(HERE / "cpmx_dump_main.cpp"),
        str(REPO / "lib" / "Cpmx" / "CpmxReader.cpp"),
        "-o", str(TOOL),
    ]
    subprocess.run(cmd, check=True)


def ensure_test_files():
    """(Re)build .cpmx files from the checked-in MusicXML test pieces."""
    files = []
    for piece in TEST_PIECES:
        subprocess.run(
            [str(PYTHON), "extract.py", f"testdata/{piece}.musicxml"],
            cwd=CONVERTER, check=True, capture_output=True)
        subprocess.run(
            [str(PYTHON), "cpmx_emit.py", f"out/{piece}.primitives.json"],
            cwd=CONVERTER, check=True, capture_output=True)
        files.append(CONVERTER / "out" / f"{piece}.cpmx")
    return files


def dump_cpp(path):
    r = subprocess.run([str(TOOL), str(path)], capture_output=True, text=True)
    return r.returncode, r.stdout


def dump_py(path):
    r = subprocess.run([str(PYTHON), "cpmx_dump.py", str(path)],
                       cwd=CONVERTER, capture_output=True, text=True)
    return r.returncode, r.stdout


def check_agreement(path, label):
    cpp_rc, cpp_out = dump_cpp(path)
    py_rc, py_out = dump_py(path)
    accepted = cpp_rc == 0
    if (cpp_rc == 0) != (py_rc == 0):
        print(f"FAIL {label}: exit codes differ (C++ {cpp_rc}, Python {py_rc})")
        return False, accepted
    if cpp_rc == 0:
        try:
            if json.loads(cpp_out) != json.loads(py_out):
                print(f"FAIL {label}: decoded output differs")
                return False, accepted
        except json.JSONDecodeError as e:
            print(f"FAIL {label}: dump is not valid JSON ({e})")
            return False, accepted
    return True, accepted


def main():
    build_tool()
    files = ensure_test_files()

    failures = 0
    for f in files:
        ok, accepted = check_agreement(f, f.name)
        if not ok:
            failures += 1
        elif not accepted:
            print(f"FAIL {f.name}: valid file rejected by both readers")
            failures += 1
        else:
            print(f"ok   {f.name}")

    # Corruption matrix on the richest piece (repeats + splits + texts).
    base = (CONVERTER / "out" / f"{TEST_PIECES[-1]}.cpmx").read_bytes()
    cases = []
    for frac in TRUNCATE_AT:
        cases.append((f"truncate@{frac}", base[: int(len(base) * frac)]))
    for off in BIT_FLIPS:
        if off < len(base):
            mutated = bytearray(base)
            mutated[off] ^= 0x40
            cases.append((f"bitflip@{off}", bytes(mutated)))

    with tempfile.TemporaryDirectory() as td:
        for label, data in cases:
            p = Path(td) / "case.cpmx"
            p.write_bytes(data)
            ok, accepted = check_agreement(p, label)
            if not ok:
                failures += 1
            else:
                print(f"ok   {label} ({'accepted' if accepted else 'rejected'} by both)")

    if failures:
        print(f"\n{failures} conformance failure(s)")
        return 1
    print("\nAll conformance checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
