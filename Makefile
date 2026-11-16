CXX = icpx
OPS = -DNDEBUG -mllvm -enable-misched=false -march=native # Tune this to your own CPU and compiler!

CXXFLAGS = -O3 -Wall -Wextra $(OPS) -std=c++20 -pthread
CPPFLAGS = -Iinclude -Iexternal/highway

all: Blake3

Blake3: src/main.cpp include/blake3.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

Blake3_bench: bench.cpp include/blake3.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

bench: Blake3_bench
	./Blake3_bench

clean:
	rm -f Blake3 Blake3_bench

.PHONY: all bench clean
