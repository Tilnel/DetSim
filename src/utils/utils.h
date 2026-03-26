#ifndef __UTILS_H
#define __UTILS_H

#include "common.h"
#include "state.h"
#include <filesystem>
#include <fmt/printf.h>
#include <fstream>
#include <optional>
#include <string>

#include <xxhash.h>
#include <cstddef>
#include <cstdint>

class XXHash64
{
  public:
  static uint64_t hash(const void *input, size_t len)
  {
    return XXH3_64bits(input, len);
  }

  static uint32_t hash32(const void *input, size_t len)
  {
    return static_cast<uint32_t>(hash(input, len));
  }
};

using hash_type = uint64_t;
int is_dynamically_linked(const char *filename);
void fcopy(char *source_filename, char *destination_filename);
int filecmp(const char *file1, const char *file2);
FILE *create_anonymous_tmp(const char *id, const char *mode);

namespace fileutils
{
struct file_closer
{
  void operator()(FILE *fp) const;
};

bool file_exists(const std::string &path);
std::optional<std::ifstream>
open_ifstream(const std::string &path,
              std::ios::openmode mode = std::ios::binary);
std::optional<std::ofstream>
open_ofstream(const std::string &path,
              std::ios::openmode mode = std::ios::binary);
bool read_file(const std::string &path, std::vector<char> &buffer);
bool write_file(const std::string &path, const std::vector<char> &buffer);
std::string temp_filename(const std::string &prefix,
                          const std::string &ext = ".tmp");
bool copy_file(const std::string &src, const std::string &dst);
bool remove_file(const std::string &path);
std::unique_ptr<FILE, file_closer> open_cfile(const std::string &path,
                                              const char *mode);
std::string format_hash_filename(const std::string &dir, const std::string &ext,
                                 hash_type hash);
void ensure_directory_for_file(const std::string &path);
std::unique_ptr<FILE, file_closer> open_map_file(hash_type hash,
                                                 const char *mode = "r");
std::unique_ptr<FILE, file_closer>
open_state_file(hash_type hash, const std::string &prefix,
                const std::string &ext = ".ss");

} // namespace fileutils
#endif
