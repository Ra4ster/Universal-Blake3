for n in 64 65 1024 2048 2049 3072 4096 8192 16384; do
    input=$(python3 -c "print('a' * $n)")
    actual=$(./Blake3 "$input" | grep -oE '[0-9a-f]{64}' | tail -1)
    expected=$(python3 -c "import blake3; print(blake3.blake3(b'a' * $n).hexdigest())")

    if [ "$actual" = "$expected" ]; then
        echo "$n bytes: PASS"
    else
        echo "$n bytes: FAIL"
        echo "  Got:      $actual"
        echo "  Expected: $expected"
    fi
done
