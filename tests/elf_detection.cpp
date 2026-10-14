#include "ui/log_wrapper.h"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <dirent.h>

// Replace only output. Detection links the real utils.cpp and libelf.
extern "C" void ui_log_error_c(const char *, int, const char *, ...) {}

static void require(bool condition, const char *message)
{
  if (!condition)
  {
    std::fprintf(stderr, "ELF regression: %s\n", message);
    std::exit(1);
  }
}

static int open_descriptor_count()
{
  DIR *dir = opendir("/proc/self/fd");
  require(dir != nullptr, "open descriptor directory");
  int count = 0;
  while (readdir(dir))
    ++count;
  closedir(dir);
  return count;
}

int main(int argc, char **argv)
{
  require(argc == 4, "expected ordinary and two Go-section fixtures");
  const int before = open_descriptor_count();
  for (int pass = 0; pass != 16; ++pass)
  {
    require(is_go_program(argv[1]) == 0, "ordinary ELF must not be Go");
    require(is_go_program(argv[2]) == 1,
            "Go buildinfo section must be detected");
    require(is_go_program(argv[3]) == 1, "Go pclntab section must be detected");
    require(is_go_program("/dev/null") == 0, "non-ELF input");
    require(is_go_program("/dev/null/nonexistent") == 0, "unopenable input");
  }
  require(open_descriptor_count() == before, "detector must close descriptors");
  std::puts(
      "ELF Go-section detection and descriptor lifetime regression passed");
}
