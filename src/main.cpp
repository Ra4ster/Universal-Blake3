#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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
#ifdef _WIN32
  HANDLE hFile = INVALID_HANDLE_VALUE;
  HANDLE hMap = NULL;
  LARGE_INTEGER fileSize;
#else
  struct stat st;
  int fd = -1;
#endif

  if (argc == 3)
  {
#ifdef _WIN32
    hFile = CreateFileA(
        argv[2], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ASSERT(hFile != INVALID_HANDLEVALUE, "Cannot open file.")
    ASSERT(GetFileSizeEx(hFile, &fileSize) != 0, "Cannot fetch file metadata.")

    if (fileSize.QuadPart == 0)
    {
      input = NULL;
      input_size = 0;
    }
    else
    {
      hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
      ASSERT(hMap != NULL, "Failed to create file mapping.")

      input = (char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
      ASSERT(input != NULL, "Failed to map file to memory.")

      input_size = (size_t)fileSize.QuadPart;
      is_file = 1;
    }
#else
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
#endif
  }

  Blake3 hasher;
  hasher.UpdateParallel(input, input_size);
  printf("%s\n", hasher.Hex().c_str());
  if (is_file)
  {
#ifdef _WIN32
    UnmapViewOfFile(input);
    CloseHandle(hMap);
    CloseHandle(hFile);
#else
    ASSERT(munmap(input, st.st_size) == 0, "Could not unmap memory back to file.");
    ASSERT(close(fd) == 0, "Error in closing file.");
#endif
  }
  return 0;
}
