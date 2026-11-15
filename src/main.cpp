#include "blake3.hpp"
#include <iostream>

int main(int argc, char* argv[])
{
  if (argc > 2)
    return 1;
  const char* input = (argc == 2) ? argv[1] : "abc";

  Deep::Blake3 hasher;
  hasher.Update(input, strlen(input));
  std::string b3 = hasher.Hex();

  std::cout << input << " hashed:\n\n" << b3 << std::endl;
  return 0;
}
