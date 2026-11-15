CXX = icpx
OPS = -mavx2 -mfma -DNDEBUG -march=native # Tune this to your own CPU!

CXXFLAGS = -O3 -Wall -Wextra $(OPS) -std=c++20
CPPFLAGS = -Iinclude -Iexternal/highway -MMD -MP

SRCS = $(wildcard src/*.cpp)
OBJS = $(patsubst src/%.cpp, build/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

HWY_LIB = build/highway/libhwy.a

all: Blake3

Blake3: $(OBJS) $(HWY_LIB)
	$(CXX) $^ -o $@

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(HWY_LIB):
	cmake --build build/highway -j

build:
	mkdir -p build

clean:
	rm -rf build/*.o build/*.d Blake3

-include $(DEPS)
