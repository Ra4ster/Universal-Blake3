#!/usr/bin/env python3
"""Checks the Blake3 CLI against the official BLAKE3 implementation.

Usage: python test.py path/to/Blake3

Requires the reference package: pip install blake3
"""

import os
import subprocess
import sys
import tempfile

import blake3

# Sizes around the block, chunk, parallel-task and stream-buffer boundaries
FILE_SIZES = [
    0, 1, 63, 64, 65, 1023, 1024, 1025, 2048, 8192, 65536,
    262143, 262144, 262145, 1048576,
    1310719, 1310720, 1310721, 1572864, 1572865,
    5000000, 16777215, 16777216, 16777217, 50000001,
]

STRINGS = ["", "abc", "Hello, World"]


def pattern(size):
    """The same repeating 0..250 byte pattern the official BLAKE3 test vectors use."""
    block = bytes(range(251))
    return (block * (size // 251 + 1))[:size]


def run(exe, *args, stdin=None):
    result = subprocess.run([exe, *args], stdin=stdin, capture_output=True, text=True)
    if result.returncode != 0:
        return f"<exit {result.returncode}: {result.stderr.strip()}>"
    return result.stdout.strip()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    exe = os.path.abspath(sys.argv[1])
    failures = 0

    def check(label, actual, expected):
        nonlocal failures
        if actual == expected:
            print(f"PASS  {label}")
        else:
            failures += 1
            print(f"FAIL  {label}\n      expected {expected}\n      actual   {actual}")

    for text in STRINGS:
        expected = blake3.blake3(text.encode()).hexdigest()
        check(f"string {text!r}", run(exe, text), expected)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.bin")

        for size in FILE_SIZES:
            data = pattern(size)
            expected = blake3.blake3(data).hexdigest()

            with open(path, "wb") as f:
                f.write(data)

            check(f"file  {size:>9} bytes", run(exe, "--file", path), expected)

            # A pipe exercises the non-seekable path
            if os.name == "posix":
                with open(path, "rb") as f:
                    cat = subprocess.Popen(["cat"], stdin=f, stdout=subprocess.PIPE)
                    actual = run(exe, "--file", "/dev/stdin", stdin=cat.stdout)
                    cat.stdout.close()
                    cat.wait()
                check(f"pipe  {size:>9} bytes", actual, expected)

        missing = subprocess.run([exe, "--file", os.path.join(tmp, "missing.bin")],
                                 capture_output=True, text=True)
        check("missing file exits non-zero", missing.returncode != 0, True)

    if failures:
        sys.exit(f"\n{failures} check(s) failed")

    print("\nAll checks passed")


if __name__ == "__main__":
    main()
