/*
 * syscall_fmt.cpp - System call string formatting
 *
 * This module formats system call information into human-readable strings
 * for logging, debugging, and display purposes.
 */

#include "guest.h"
#include "state.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>

/* Raft message parser */
#include "raft_msg_parser.h"
#define DISPLAY_MAX_LEN 128

/* External syscall name table */
extern const char *syscalls[450];

namespace syscall_fmt
{

/* ======================================================================
 * Helper Functions
 * ====================================================================== */

/* Format memory content with printable characters escaped */
static int format_mem_content(char *dest, const char *src, size_t len)
{
  int pos = 0;
  for (size_t i = 0; i < len; i++)
  {
    uint8_t val = src[i];
    if (isprint(src[i]))
      pos += sprintf(dest + pos, "%c", src[i]);
    else
      pos += sprintf(dest + pos, "\\%03u", val);
  }
  return pos;
}

/* Helper: Format return value with errno if negative */
static int format_ret(char *dest, long rval)
{
  if (rval < 0)
    return sprintf(dest, "= %ld (%s)", rval, strerror(-rval));
  else
    return sprintf(dest, "= %ld", rval);
}

/* Format sockaddr_in structure */
static int format_sockaddr(char *dest, struct sockaddr_in *addr)
{
  char ipbuf[INET_ADDRSTRLEN];
  const char *ipstr =
      inet_ntop(AF_INET, &(addr->sin_addr), ipbuf, sizeof(ipbuf));
  if (!ipstr)
    ipstr = "invalid";

  return sprintf(
      dest,
      "{sin_family=AF_INET, sin_port=htons(%d), sin_addr=inet_addr(\"%s\")}",
      ntohs(addr->sin_port), ipstr);
}

/* ======================================================================
 * Individual Syscall Formatters
 * ====================================================================== */

/* Format: write - input buffer, print by requested size */
void format_write(char *buf, int pid, hash_type ts_hash,
                  const syscall_info &info)
{
  size_t requested_len = info.args[2];
  size_t display_len = requested_len;
  if (display_len > DISPLAY_MAX_LEN)
    display_len = DISPLAY_MAX_LEN;

  char *mem = (char *)read_mem(pid, ts_hash, info.args[1], display_len);

  int pos = sprintf(buf, "write(%ld, ", info.args[0]);

  if (mem)
  {
    pos += sprintf(buf + pos, "%p\"", (void *)info.args[1]);
    pos += format_mem_content(buf + pos, mem, display_len);
    if (requested_len > DISPLAY_MAX_LEN)
      pos += sprintf(buf + pos, "...(%ld more bytes)",
                     requested_len - DISPLAY_MAX_LEN);
    pos += sprintf(buf + pos, "\"");
    free(mem);
  }
  else
  {
    pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
  }

  pos += sprintf(buf + pos, ", %ld) ", info.args[2]);
  format_ret(buf + pos, info.rval);
}

/* Format: read - output buffer, print by return value */
void format_read(char *buf, int pid, hash_type ts_hash,
                 const syscall_info &info)
{
  int pos = sprintf(buf, "read(%ld, ", info.args[0]);

  ssize_t ret = info.rval;
  if (ret < 0)
  {
    /* Failed: just show pointer */
    pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
  }
  else if (ret == 0)
  {
    /* EOF: show empty string */
    pos += sprintf(buf + pos, "%p\"\"", (void *)info.args[1]);
  }
  else
  {
    /* Success: show content by return value */
    size_t display_len = ret;
    if (display_len > DISPLAY_MAX_LEN)
      display_len = DISPLAY_MAX_LEN;

    char *mem = (char *)read_mem(pid, ts_hash, info.args[1], display_len);
    if (mem)
    {
      pos += sprintf(buf + pos, "%p\"", (void *)info.args[1]);
      pos += format_mem_content(buf + pos, mem, display_len);
      if (ret > DISPLAY_MAX_LEN)
        pos += sprintf(buf + pos, "...");
      pos += sprintf(buf + pos, "\"");
      free(mem);
    }
    else
    {
      pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
    }
  }

  pos += sprintf(buf + pos, ", %ld) ", info.args[2]);
  format_ret(buf + pos, info.rval);
}

/* Format: int fd only (close style) */
void format_fd_only(char *buf, const syscall_info &info)
{
  int pos = sprintf(buf, "%s(%ld) ", syscalls[info.nr], info.args[0]);
  format_ret(buf + pos, info.rval);
}

/* Format: sendto - input buffer, print by requested size */
void format_sendto(char *buf, int pid, hash_type ts_hash,
                   const syscall_info &info)
{
  /* Limit buffer size to prevent overflow - cap at 4KB for display */
  size_t requested_len = info.args[2];
  size_t display_len = requested_len;
  if (display_len > DISPLAY_MAX_LEN)
    display_len = DISPLAY_MAX_LEN;

  int pos = sprintf(buf, "sendto(%ld, ", info.args[0]);

  char *mem = (char *)read_mem(pid, ts_hash, info.args[1], display_len);
  if (mem)
  {
    /* Check if this looks like a Raft message (starts with valid type 0-7) */
    bool is_raft = false;
    if (display_len >= 4)
    {
      int msg_type;
      memcpy(&msg_type, mem, sizeof(int));
      if (msg_type >= 0 && msg_type <= 7)
      {
        /* Likely a Raft message, try to parse it */
        std::string raft_desc = raft::parse_raft_message(mem, display_len);
        if (!raft_desc.empty() &&
            raft_desc.find("malformed") == std::string::npos)
        {
          pos += sprintf(buf + pos, "%p\"%s\"", (void *)info.args[1],
                         raft_desc.c_str());
          is_raft = true;
        }
      }
    }

    if (!is_raft)
    {
      /* Regular binary content */
      pos += sprintf(buf + pos, "%p\"", (void *)info.args[1]);
      pos += format_mem_content(buf + pos, mem, display_len);
      if (requested_len > DISPLAY_MAX_LEN)
        pos += sprintf(buf + pos, "...(%ld more bytes)",
                       requested_len - DISPLAY_MAX_LEN);
      pos += sprintf(buf + pos, "\"");
    }
    free(mem);
  }
  else
  {
    pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
  }

  pos += sprintf(buf + pos, ", %ld, %ld, ", info.args[2], info.args[3]);

  struct sockaddr_in *addr =
      (struct sockaddr_in *)read_mem(pid, ts_hash, info.args[4], info.args[5]);
  if (addr)
  {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  }
  else
  {
    pos += sprintf(buf + pos, "<invalid addr>");
  }

  pos += sprintf(buf + pos, ", %ld) ", info.args[5]);
  format_ret(buf + pos, info.rval);
}

/* Format: recvfrom - output buffer, print by return value */
void format_recvfrom(char *buf, int pid, hash_type ts_hash,
                     const syscall_info &info)
{
  int pos = 0;
  ssize_t ret = info.rval;

  /* Start: recvfrom(fd, buf, len, flags, addr, addrlen) */
  pos += sprintf(buf + pos, "recvfrom(%ld, ", info.args[0]);

  /* buf argument - output buffer, print by return value */
  if (ret < 0)
  {
    /* Failed: just show pointer */
    pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
  }
  else if (ret == 0)
  {
    /* EOF: show empty string */
    pos += sprintf(buf + pos, "%p\"\"", (void *)info.args[1]);
  }
  else
  {
    /* Success: show pointer + content by return value */
    size_t display_len = ret;
    if (display_len > DISPLAY_MAX_LEN)
      display_len = DISPLAY_MAX_LEN;

    char *mem = (char *)read_mem(pid, ts_hash, info.args[1], display_len);
    if (mem)
    {
      /* Check if this looks like a Raft message */
      bool is_raft = false;
      if (display_len >= 4)
      {
        int msg_type;
        memcpy(&msg_type, mem, sizeof(int));
        if (msg_type >= 0 && msg_type <= 7)
        {
          std::string raft_desc = raft::parse_raft_message(mem, display_len);
          if (!raft_desc.empty() &&
              raft_desc.find("malformed") == std::string::npos)
          {
            pos += sprintf(buf + pos, "%p\"%s\"", (void *)info.args[1],
                           raft_desc.c_str());
            is_raft = true;
          }
        }
      }

      if (!is_raft)
      {
        /* Regular binary content */
        pos += sprintf(buf + pos, "%p\"", (void *)info.args[1]);
        pos += format_mem_content(buf + pos, mem, display_len);
        pos += sprintf(buf + pos, "\"");
      }
      if (ret > DISPLAY_MAX_LEN)
        pos += sprintf(buf + pos, "...");
      free(mem);
    }
    else
    {
      pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
    }
  }

  /* len and flags */
  pos += sprintf(buf + pos, ", %ld, %ld, ", info.args[2], info.args[3]);

  /* addr and addrlen - output arguments */
  if (info.args[5] != 0)
  {
    /* Read addrlen from output pointer */
    socklen_t *plen =
        (socklen_t *)read_mem(pid, ts_hash, info.args[5], sizeof(socklen_t));
    if (plen && *plen > 0 && ret >= 0)
    {
      socklen_t addrlen = *plen;
      /* addr pointer */
      pos += sprintf(buf + pos, "%p", (void *)info.args[4]);
      /* addr content */
      if (addrlen >= sizeof(struct sockaddr_in))
      {
        struct sockaddr_in *addr =
            (struct sockaddr_in *)read_mem(pid, ts_hash, info.args[4], addrlen);
        if (addr)
        {
          pos += sprintf(buf + pos, " ");
          pos += format_sockaddr(buf + pos, addr);
          free(addr);
        }
      }
      pos += sprintf(buf + pos, ", [%d]", addrlen);
      free(plen);
    }
    else
    {
      /* No address returned or failed */
      pos += sprintf(buf + pos, "%p", (void *)info.args[4]);
      if (plen)
      {
        pos += sprintf(buf + pos, ", [%d]", (int)*plen);
        free(plen);
      }
      else
      {
        pos += sprintf(buf + pos, ", [0]");
      }
    }
  }
  else
  {
    /* NULL addrlen pointer */
    pos += sprintf(buf + pos, "%p, NULL", (void *)info.args[4]);
  }

  pos += sprintf(buf + pos, ") ");
  format_ret(buf + pos, info.rval);
}

/* Format: void *addr (brk/mmap style) */
void format_addr(char *buf, const syscall_info &info)
{
  int pos = sprintf(buf, "%s(0x%lx) ", syscalls[info.nr], info.args[0]);
  format_ret(buf + pos, info.rval);
}

/* Format: mmap - detailed like strace */
void format_mmap(char *buf, const syscall_info &info)
{
  /* mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
   */
  int pos = sprintf(buf, "mmap(");

  /* addr - NULL or address */
  if (info.args[0] == 0)
    pos += sprintf(buf + pos, "NULL, ");
  else
    pos += sprintf(buf + pos, "0x%lx, ", info.args[0]);

  /* length */
  pos += sprintf(buf + pos, "%lu, ", info.args[1]);

  /* prot - combination of PROT_* flags */
  int prot = info.args[2];
  if (prot == PROT_NONE)
  {
    pos += sprintf(buf + pos, "PROT_NONE");
  }
  else
  {
    int first = 1;
    if (prot & PROT_READ)
    {
      pos += sprintf(buf + pos, "PROT_READ");
      first = 0;
    }
    if (prot & PROT_WRITE)
    {
      pos += sprintf(buf + pos, "%sPROT_WRITE", first ? "" : "|");
      first = 0;
    }
    if (prot & PROT_EXEC)
    {
      pos += sprintf(buf + pos, "%sPROT_EXEC", first ? "" : "|");
      first = 0;
    }
    /* Handle unknown prot bits */
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE))
    {
      pos += sprintf(buf + pos, "%s0x%x", first ? "" : "|",
                     prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC));
    }
  }
  pos += sprintf(buf + pos, ", ");

  /* flags - combination of MAP_* flags */
  int flags = info.args[3];
  int first_flag = 1;

  /* Sharing type */
  if (flags & MAP_SHARED)
  {
    pos += sprintf(buf + pos, "MAP_SHARED");
    first_flag = 0;
  }
  else if (flags & MAP_PRIVATE)
  {
    pos += sprintf(buf + pos, "MAP_PRIVATE");
    first_flag = 0;
  }

  /* Other common flags */
  if (flags & MAP_ANONYMOUS)
  {
    pos += sprintf(buf + pos, "%sMAP_ANONYMOUS", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_FIXED)
  {
    pos += sprintf(buf + pos, "%sMAP_FIXED", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_FIXED_NOREPLACE)
  {
    pos += sprintf(buf + pos, "%sMAP_FIXED_NOREPLACE", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_DENYWRITE)
  {
    pos += sprintf(buf + pos, "%sMAP_DENYWRITE", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_EXECUTABLE)
  {
    pos += sprintf(buf + pos, "%sMAP_EXECUTABLE", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_LOCKED)
  {
    pos += sprintf(buf + pos, "%sMAP_LOCKED", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_NORESERVE)
  {
    pos += sprintf(buf + pos, "%sMAP_NORESERVE", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_POPULATE)
  {
    pos += sprintf(buf + pos, "%sMAP_POPULATE", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_STACK)
  {
    pos += sprintf(buf + pos, "%sMAP_STACK", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_HUGETLB)
  {
    pos += sprintf(buf + pos, "%sMAP_HUGETLB", first_flag ? "" : "|");
    first_flag = 0;
  }
  if (flags & MAP_SYNC)
  {
    pos += sprintf(buf + pos, "%sMAP_SYNC", first_flag ? "" : "|");
    first_flag = 0;
  }

  /* Handle unknown flags */
  int known_flags = MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED |
                    MAP_FIXED_NOREPLACE | MAP_DENYWRITE | MAP_EXECUTABLE |
                    MAP_LOCKED | MAP_NORESERVE | MAP_POPULATE | MAP_STACK |
                    MAP_HUGETLB | MAP_SYNC;
  if (flags & ~known_flags)
  {
    pos += sprintf(buf + pos, "%s0x%x", first_flag ? "" : "|",
                   flags & ~known_flags);
  }

  /* fd - show as signed integer (can be -1 for anonymous mappings) */
  pos += sprintf(buf + pos, ", %ld, ", (long)info.args[4]);

  /* offset */
  pos += sprintf(buf + pos, "%lu) ", info.args[5]);

  format_ret(buf + pos, info.rval);
}

void format_tgkill(char *buf, const syscall_info &info)
{
  sprintf(buf, "%s(%ld, %ld, %ld(%s)) = ?", syscalls[info.nr], info.args[0],
          info.args[1], info.args[2], strsignal(info.args[2]));
}

/* Format: int status (exit style) */
void format_exit(char *buf, const syscall_info &info)
{
  sprintf(buf, "%s(%ld) = ?", syscalls[info.nr], info.args[0]);
}

/* Format: openat */
void format_openat(char *buf, int pid, hash_type ts_hash,
                   const syscall_info &info)
{
  /* Use a reasonable size for path display - protect against huge paths */
  size_t path_len = 256;
  char *path = (char *)read_mem(pid, ts_hash, info.args[1], path_len);

  int pos = sprintf(buf, "openat(%ld, ", info.args[0]);

  if (path)
  {
    /* Ensure null termination within our buffer */
    path[path_len - 1] = '\0';

    const char *flag_str = "";
    int flags = info.args[2];
    if ((flags & O_RDWR) == O_RDWR)
      flag_str = "O_RDWR";
    else if (flags & O_WRONLY)
      flag_str = "O_WRONLY";
    else
      flag_str = "O_RDONLY";

    pos += sprintf(buf + pos, "\"%s\", %s|0x%x, 0%03lo) ", path, flag_str,
                   flags & ~O_ACCMODE, info.args[3]);
    free(path);
  }
  else
  {
    pos += sprintf(buf + pos, "<error reading path>, %ld, %ld) ", info.args[2],
                   info.args[3]);
  }

  format_ret(buf + pos, info.rval);
}

/* Format: gettimeofday */
void format_gettimeofday(char *buf, int pid, hash_type ts_hash,
                         const syscall_info &info)
{
  int pos = sprintf(buf, "gettimeofday(");

  struct timeval *tv = (struct timeval *)read_mem(pid, ts_hash, info.args[0],
                                                  sizeof(struct timeval));
  if (tv && info.rval >= 0)
  {
    pos += sprintf(buf + pos, "{tv_sec=%ld, tv_usec=%ld}", tv->tv_sec,
                   tv->tv_usec);
    free(tv);
  }
  else
  {
    pos += sprintf(buf + pos, "%p", (void *)info.args[0]);
    if (tv)
      free(tv);
  }

  pos += sprintf(buf + pos, ", %p) ", (void *)info.args[1]);
  format_ret(buf + pos, info.rval);
}

/* Format: clock_nanosleep */
void format_clock_nanosleep(char *buf, int pid, hash_type ts_hash,
                            const syscall_info &info)
{
  int pos =
      sprintf(buf, "clock_nanosleep(%ld, %ld, ", info.args[0], info.args[1]);

  struct timespec *ts = (struct timespec *)read_mem(pid, ts_hash, info.args[2],
                                                    sizeof(struct timespec));
  if (ts && info.rval >= 0)
    pos += sprintf(buf + pos, "{tv_sec=%ld, tv_nsec=%ld}", ts->tv_sec,
                   ts->tv_nsec);
  else
    pos += sprintf(buf + pos, "%p", (void *)info.args[2]);

  free(ts);
  pos += sprintf(buf + pos, ", %p) ", (void *)info.args[3]);
  format_ret(buf + pos, info.rval);
}

/* Format: socket */
void format_socket(char *buf, const syscall_info &info)
{
  const char *domain_str = "AF_???";
  if (info.args[0] == AF_INET)
    domain_str = "AF_INET";
  else if (info.args[0] == AF_UNIX)
    domain_str = "AF_UNIX";

  const char *type_str = "SOCK_???";
  if (info.args[1] == SOCK_STREAM)
    type_str = "SOCK_STREAM";
  else if (info.args[1] == SOCK_DGRAM)
    type_str = "SOCK_DGRAM";

  int pos =
      sprintf(buf, "socket(%s, %s, %ld) ", domain_str, type_str, info.args[2]);
  format_ret(buf + pos, info.rval);
}

/* Format: bind with sockaddr details */
void format_bind(char *buf, int pid, hash_type ts_hash,
                 const syscall_info &info)
{
  int pos = sprintf(buf, "bind(%ld, ", info.args[0]);

  /* Read sockaddr */
  struct sockaddr_in *addr =
      (struct sockaddr_in *)read_mem(pid, ts_hash, info.args[1], info.args[2]);
  if (addr && info.rval >= 0)
  {
    pos += format_sockaddr(buf + pos, addr);
    free(addr);
  }
  else
  {
    pos += sprintf(buf + pos, "%p", (void *)info.args[1]);
    if (addr)
      free(addr);
  }

  pos += sprintf(buf + pos, ", %ld) ", info.args[2]);
  format_ret(buf + pos, info.rval);
}

/* Format: listen/accept/connect */
void format_socket_fd(char *buf, const syscall_info &info)
{
  int pos = sprintf(buf, "%s(%ld, ...) ", syscalls[info.nr], info.args[0]);
  format_ret(buf + pos, info.rval);
}

void format_chdir(char *buf, int pid, hash_type ts_hash,
                  const syscall_info &info)
{
  (void)pid;
  (void)ts_hash;
  char path[256] = {};
  memcpy_guest2host(path, (void *)info.args[0], 255);
  path[255] = '\0';
  int pos = sprintf(buf, "chdir(\"%s\") ", path);
  format_ret(buf + pos, info.rval);
}

/* Format: default (6 arguments hex) */
void format_default(char *buf, const syscall_info &info)
{
  int pos = sprintf(buf, "%s(0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx, 0x%lx) ",
                    syscalls[info.nr], info.args[0], info.args[1], info.args[2],
                    info.args[3], info.args[4], info.args[5]);
  format_ret(buf + pos, info.rval);
}

/* ======================================================================
 * Main Formatting Function
 * ====================================================================== */

void format(char *buf, int pid, hash_type ts_hash)
{
  tracee_state ts(ts_hash);
  syscall_info info = ts.si;
  switch (info.nr)
  {
    /* Network */
    case SYS_sendto:
      format_sendto(buf, pid, ts_hash, info);
      break;
    case SYS_recvfrom:
      format_recvfrom(buf, pid, ts_hash, info);
      break;
    case SYS_socket:
      format_socket(buf, info);
      break;
    case SYS_bind:
      format_bind(buf, pid, ts_hash, info);
      break;
    case SYS_listen:
    case SYS_accept:
    case SYS_accept4:
    case SYS_connect:
      format_socket_fd(buf, info);
      break;

    case SYS_epoll_create:
      snprintf(buf, 1024, "epoll_create(%ld)", info.args[0]);
      break;
    case SYS_epoll_create1:
      snprintf(buf, 1024, "epoll_create1(%ld)", info.args[0]);
      break;
    case SYS_epoll_ctl:
      snprintf(buf, 1024, "epoll_ctl(epfd=%ld, op=%ld, fd=%ld, event=%p)",
               info.args[0], info.args[1], info.args[2], (void *)info.args[3]);
      break;
    case SYS_epoll_wait:
      snprintf(buf, 1024,
               "epoll_wait(epfd=%ld, events=%p, maxevents=%ld, timeout=%ld)",
               info.args[0], (void *)info.args[1], info.args[2], info.args[3]);
      break;

    case SYS_setsockopt:
      snprintf(buf, 1024, "setsockopt(fd=%ld, level=%ld, optname=%ld)",
               info.args[0], info.args[1], info.args[2]);
      break;
    case SYS_getsockopt:
      snprintf(buf, 1024, "getsockopt(fd=%ld, level=%ld, optname=%ld)",
               info.args[0], info.args[1], info.args[2]);
      break;

    case SYS_fsync:
      snprintf(buf, 1024, "fsync(%ld)", info.args[0]);
      break;
    case SYS_fdatasync:
      snprintf(buf, 1024, "fdatasync(%ld)", info.args[0]);
      break;

    case SYS_fcntl:
      snprintf(buf, 1024, "fcntl(fd=%ld, cmd=%ld, arg=%ld)", info.args[0],
               info.args[1], info.args[2]);
      break;

    /* File I/O */
    case SYS_read:
      format_read(buf, pid, ts_hash, info);
      break;
    case SYS_write:
      format_write(buf, pid, ts_hash, info);
      break;
    case SYS_openat:
      format_openat(buf, pid, ts_hash, info);
      break;
    case SYS_close:
      format_fd_only(buf, info);
      break;
    case SYS_chdir:
      format_chdir(buf, pid, ts_hash, info);
      break;

    /* Memory */
    case SYS_brk:
      format_addr(buf, info);
      break;
    case SYS_mmap:
      format_mmap(buf, info);
      break;

    /* Time */
    case SYS_gettimeofday:
      format_gettimeofday(buf, pid, ts_hash, info);
      break;
    case SYS_clock_nanosleep:
      format_clock_nanosleep(buf, pid, ts_hash, info);
      break;

    /* Exception */
    case SYS_tgkill:
      format_tgkill(buf, info);
      break;

    /* Process */
    case SYS_exit:
    case SYS_exit_group:
      format_exit(buf, info);
      break;

    /* Simple fd-only */
    case SYS_sched_yield:
    case SYS_set_tid_address:
      format_fd_only(buf, info);
      break;

    /* Default */
    default:
      format_default(buf, info);
      break;
  }
}

} /* namespace syscall_fmt */
