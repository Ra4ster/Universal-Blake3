import blake3
import random
import re
import string
import subprocess

rng = random.Random(42)
alphabet = string.ascii_letters + string.digits

lengths = [
    0, 1, 3, 63, 64, 65,
    1023, 1024, 1025,
    2047, 2048, 2049,
    4095, 4096, 4097,
    8192, 16384, 32768,
]

lengths += [rng.randint(1, 32768) for _ in range(100)]

for n in lengths:
    data = "".join(rng.choices(alphabet, k=n))
    expected = blake3.blake3(data.encode()).hexdigest()

    result = subprocess.run(
        ["./Blake3", data],
        capture_output=True,
        text=True,
        check=True,
    )

    hashes = re.findall(r"\b[0-9a-f]{64}\b", result.stdout)
    actual = hashes[-1] if hashes else None

    if actual != expected:
        print(f"FAIL: {n} bytes")
        print(f"Expected: {expected}")
        print(f"Actual:   {actual}")
        raise SystemExit(1)

    print(f"PASS: {n} bytes")

print(f"\nAll {len(lengths)} tests passed!")
