CXX = icpx
CXXFLAGS = -O3 -Wall -Wextra -march=native -std=c++17
CPPFLAGS = -Iinclude -Iexternal/highway -MMD -MP

SRCS = $(wildcard src/*.cpp)
OBJS = $(patsubst src/%.cpp, build/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

HWY_LIB = build/highway/libhwy.a

all: Blake3

Blake3: $(OBJS)
	$(CXX) $(OBJS) -o $@

build/%.o: src/%.cpp | build
	$(CXX) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(HWY_LIB):
	cmake --build build/highway -j

build:
	mkdir -p build

clean:
	rm -rf build

-include $(DEPS)
