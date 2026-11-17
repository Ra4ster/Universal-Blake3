#include <cstdio>
#include <cstring>

#include "blake3.hpp"

#ifndef ASSERT
#define ASSERT(cmp, msg)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(cmp))                                                                                    \
    {                                                                                              \
      fprintf(stderr, "%s\n", msg);                                                                \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0);
#endif

int main(int argc, char* argv[])
{
  if (argc > 3)
    return 1;

  Blake3 hasher;

  if (argc == 3)
  {
    ASSERT(strcmp(argv[1], "--file") == 0, "2nd arg must be --file")
    ASSERT(hasher.UpdateFile(argv[2]), "Cannot read file.")
  }
  else
  {
    const char* input = (argc == 2) ? argv[1] : "abc";
    hasher.UpdateParallel(input, strlen(input));
  }

  printf("%s\n", hasher.Hex().c_str());
  return 0;
}
