#include "utils.h"
#include "common.h"
#include "log_wrapper.h"
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fmt/printf.h>
#include <fmt/printf.h>
#include <fstream>
#include <gelf.h>
#include <memory>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

#define CHUNK_SIZE 128 KiB
#define BUFFER_SIZE 8 KiB

int is_dynamically_linked(const char *filename)
{
  if (elf_version(EV_CURRENT) == EV_NONE)
  {
    LOG_INFO("ELF library initialization failed\n");
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
    LOG_INFO("elf_begin failed: %s\n", elf_errmsg(-1));
    close(fd);
    return -1;
  }

  size_t phnum;
  if (elf_getphdrnum(e, &phnum) != 0)
  {
    LOG_INFO("elf_getphdrnum failed\n");
    elf_end(e);
    close(fd);
    return -1;
  }

  for (size_t i = 0; i < phnum; i++)
  {
    GElf_Phdr phdr;
    if (gelf_getphdr(e, i, &phdr) != &phdr)
    {
      LOG_INFO("gelf_getphdr failed\n");
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

// Detect if binary is a Go program by checking for Go-specific ELF sections
int is_go_program(const char *filename)
{
  LOG_INFO("is_go_program: checking %s", filename);
  if (elf_version(EV_CURRENT) == EV_NONE)
    return 0;
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    LOG_INFO("is_go_program: open failed: %s", strerror(errno));
    return 0;
  }
  LOG_INFO("is_go_program: opened fd=%d", fd);

  Elf *e = elf_begin(fd, ELF_C_READ, NULL);
  if (!e) {
    LOG_INFO("is_go_program: elf_begin failed");
    close(fd);
    return 0;
  }

  // Check for Go-specific sections
  Elf_Scn *scn = NULL;
  size_t shstrndx;
  if (elf_getshdrstrndx(e, &shstrndx) != 0) {
    elf_end(e);
    close(fd);
    return 0;
  }
  int is_go = 0;
  while ((scn = elf_nextscn(e, scn)) != NULL) {
    GElf_Shdr shdr;
    if (gelf_getshdr(scn, &shdr) != NULL) {
      const char *name = elf_strptr(e, shstrndx, shdr.sh_name);
      if (name && (strcmp(name, ".go.buildinfo") == 0 ||
                     strcmp(name, ".gopclntab") == 0 ||
                     strncmp(name, ".go.", 4) == 0)) {
        is_go = 1;
        break;
      }
    }
  }

  elf_end(e);
  close(fd);
  return is_go;
}

/* ======================================================================
 * Multi-threading support functions
 * ====================================================================== */

#include <dirent.h>
#include <algorithm>

// Get list of all thread TIDs in a process
std::vector<pid_t> get_thread_list(pid_t pid)
{
  std::vector<pid_t> threads;
  std::string task_dir = fmt::format("/proc/{}/task", pid);

  DIR *dir = opendir(task_dir.c_str());
  if (!dir) {
    LOG_ERROR("Failed to open %s: %s", task_dir.c_str(), strerror(errno));
    return threads;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;

    pid_t tid = atoi(entry->d_name);
    if (tid > 0) {
      threads.push_back(tid);
    }
  }

  closedir(dir);
  std::sort(threads.begin(), threads.end());
  return threads;
}

// Get thread count for a process
int get_thread_count(pid_t pid)
{
  auto threads = get_thread_list(pid);
  return static_cast<int>(threads.size());
}
