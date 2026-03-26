#include "utils.h"
#include "common.h"
#include "state.h"
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fmt/printf.h>
#include <fstream>
#include <gelf.h>
#include <libgen.h>
#include <memory>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include <zstd.h>

namespace fs = std::filesystem;

#define CHUNK_SIZE 128 KiB
#define BUFFER_SIZE 8 KiB

FILE *create_anonymous_tmp(const char *id, const char *mode)
{
  int fd = syscall(SYS_memfd_create, id, 0);
  if (fd < 0)
    return NULL;
  return fdopen(fd, mode);
}

hash_type crc32(FILE *fp);

int filecmp(const char *file1, const char *file2)
{
  FILE *fp1 = fopen(file1, "rb");
  FILE *fp2 = fopen(file2, "rb");

  if (!fp1 || !fp2)
  {
    fprintf(stderr, "Error opening files.\n");
    if (fp1)
      fclose(fp1);
    if (fp2)
      fclose(fp2);
    return -1;
  }

  unsigned char buf1[BUFFER_SIZE];
  unsigned char buf2[BUFFER_SIZE];

  size_t bytes_read1, bytes_read2;
  int result = 0;

  while (1)
  {
    bytes_read1 = fread(buf1, 1, BUFFER_SIZE, fp1);
    bytes_read2 = fread(buf2, 1, BUFFER_SIZE, fp2);

    if (bytes_read1 != bytes_read2)
    {
      result = 1; // size mismatch
      break;
    }

    if (bytes_read1 == 0)
    {
      // both files reached EOF
      break;
    }

    if (memcmp(buf1, buf2, bytes_read1) != 0)
    {
      result = 1; // content mismatch
      break;
    }
  }

  fclose(fp1);
  fclose(fp2);
  return result;
}

int mkdir_p(const char *path)
{
  char tmp[256];
  char *p = NULL;
  size_t len;

  if (!path) return -1;
  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (len > 0 && tmp[len - 1] == '/')
    tmp[len - 1] = '\0';

  for (p = tmp + 1; *p; p++)
  {
    if (*p == '/')
    {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  return mkdir(tmp, 0755);
}

void fcopy(char *source_filename, char *destination_filename)
{
  int source_fd = open(source_filename, O_RDONLY);
  if (source_fd == -1)
  {
    perror("Error opening source file");
    return;
  }

  char *dup = strdup(destination_filename);
  char *dir = dirname(dup);
  mkdir_p(dir);
  free(dup);

  int dest_fd = open(destination_filename, O_WRONLY | O_CREAT | O_TRUNC,
                     S_IRUSR | S_IWUSR);

  if (dest_fd == -1)
  {
    perror("Error opening destination file");
    close(source_fd);
    return;
  }

  char buffer[BUFFER_SIZE];
  ssize_t bytes_read, bytes_written;

  while ((bytes_read = read(source_fd, buffer, BUFFER_SIZE)) > 0)
  {
    bytes_written = write(dest_fd, buffer, bytes_read);
    if (bytes_written != bytes_read)
    {
      perror("Error writing to destination file");
      close(source_fd);
      close(dest_fd);
      return;
    }
  }

  if (bytes_read == -1)
  {
    perror("Error reading from source file");
    close(source_fd);
    close(dest_fd);
    return;
  }

  close(source_fd);
  close(dest_fd);
}

int is_dynamically_linked(const char *filename)
{
  if (elf_version(EV_CURRENT) == EV_NONE)
  {
    fprintf(stderr, "ELF library initialization failed\n");
    return -1;
  }

  int fd = open(filename, O_RDONLY);
  if (fd < 0)
  {
    perror("open");
    return -1;
  }

  Elf *e = elf_begin(fd, ELF_C_READ, NULL);
  if (!e)
  {
    fprintf(stderr, "elf_begin failed: %s\n", elf_errmsg(-1));
    close(fd);
    return -1;
  }

  size_t phnum;
  if (elf_getphdrnum(e, &phnum) != 0)
  {
    fprintf(stderr, "elf_getphdrnum failed\n");
    elf_end(e);
    close(fd);
    return -1;
  }

  for (size_t i = 0; i < phnum; i++)
  {
    GElf_Phdr phdr;
    if (gelf_getphdr(e, i, &phdr) != &phdr)
    {
      fprintf(stderr, "gelf_getphdr failed\n");
      continue;
    }

    if (phdr.p_type == PT_INTERP)
    {
      char interp[256] = {0};
      lseek(fd, phdr.p_offset, SEEK_SET);
      read(fd, interp, sizeof(interp) - 1);
      printf("Dynamic linker: %s\n", interp);
      elf_end(e);
      close(fd);
      return 1; // dynamically linked
    }
  }

  elf_end(e);
  close(fd);
  return 0; // statically linked
}

namespace fileutils
{
void file_closer::operator()(FILE *fp) const
{
  if (fp)
    fclose(fp);
}

bool file_exists(const std::string &path) { return fs::exists(path); }

std::optional<std::ifstream> open_ifstream(const std::string &path,
                                           std::ios::openmode mode)
{
  std::ifstream ifs(path, mode);
  if (!ifs.is_open())
    return std::nullopt;
  return std::move(ifs);
}

std::optional<std::ofstream> open_ofstream(const std::string &path,
                                           std::ios::openmode mode)
{
  std::ofstream ofs(path, mode);
  if (!ofs.is_open())
    return std::nullopt;
  return std::move(ofs);
}

bool read_file(const std::string &path, std::vector<char> &buffer)
{
  auto ifs_opt = open_ifstream(path);
  if (!ifs_opt)
    return false;

  std::ifstream &ifs = *ifs_opt;
  ifs.seekg(0, std::ios::end);
  size_t size = ifs.tellg();
  ifs.seekg(0);
  buffer.resize(size);
  ifs.read(buffer.data(), size);
  return true;
}

bool write_file(const std::string &path, const std::vector<char> &buffer)
{
  auto ofs_opt = open_ofstream(path);
  if (!ofs_opt)
    return false;

  std::ofstream &ofs = *ofs_opt;
  ofs.write(buffer.data(), buffer.size());
  return true;
}

std::string temp_filename(const std::string &prefix, const std::string &ext)
{
  char tmpl[64];
  snprintf(tmpl, sizeof(tmpl), "/tmp/%sXXXXXX", prefix.c_str());
  int fd = mkstemp(tmpl);
  if (fd < 0)
    return "";
  close(fd);
  return std::string(tmpl) + ext;
}

bool copy_file(const std::string &src, const std::string &dst)
{
  std::error_code ec;
  fs::create_directories(fs::path(dst).parent_path(), ec);
  return fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
}

bool remove_file(const std::string &path)
{
  std::error_code ec;
  return fs::remove(path, ec);
}

std::unique_ptr<FILE, file_closer> open_cfile(const std::string &path,
                                              const char *mode)
{
  FILE *fp = fopen(path.c_str(), mode);
  return std::unique_ptr<FILE, file_closer>(fp);
}

std::string format_hash_filename(const std::string &dir, const std::string &ext,
                                 hash_type hash)
{
  return dir + fmt::sprintf("/" HASH_FORMAT, hash) + ext;
}

std::unique_ptr<FILE, file_closer> open_map_file(hash_type hash,
                                                 const char *mode)
{
  std::string filename = format_hash_filename("mappings", ".maps", hash);
  return open_cfile(filename, mode);
}

std::unique_ptr<FILE, file_closer> open_state_file(hash_type hash,
                                                   const std::string &prefix,
                                                   const std::string &ext)
{
  std::string filename = format_hash_filename(prefix, ext, hash);
  return open_cfile(filename, "rb");
}

void ensure_directory_for_file(const std::string &path)
{
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }
}

} // namespace fileutils
