#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

  char default_arg[] = "abc";
  char* input = (argc == 2) ? argv[1] : default_arg;
  size_t input_size = strlen(input);

  int is_file = 0;
  int fd = -1;
#ifdef __unix__
  struct stat st;
#else

#endif

  if (argc == 3)
  {
#ifdef __unix__
    ASSERT(strcmp(argv[1], "--file") == 0, "2nd arg must be --file")

    fd = open(argv[2], O_RDONLY);
    ASSERT(fd != -1, "Cannot open file.")
    ASSERT(fstat(fd, &st) == 0, "Cannot fetch file metadata.")

    if (st.st_size == 0)
    {
      input = NULL;
      input_size = 0;
    }
    else
    {
      input = (char*)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      ASSERT(input != MAP_FAILED, "Failed to map file to memory.")
      input_size = st.st_size;
      is_file = 1;
    }
#else

#endif
  }

  Blake3 hasher;
  hasher.UpdateParallel(input, input_size);
  printf("%s\n", hasher.Hex().c_str());
  if (is_file)
  {
#ifdef __unix__
    ASSERT(munmap(input, st.st_size) == 0, "Could not unmap memory back to file.");
    ASSERT(close(fd) == 0, "Error in closing file.");
#else

#endif
  }

  return 0;
}
