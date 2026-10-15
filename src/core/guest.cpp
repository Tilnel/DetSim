/*
 * guest.cpp - Low-level ptrace operations
 *
 * This module provides direct interaction with tracee processes via ptrace:
 * - Register read/write
 * - Memory read/write (via process_vm_readv/process_vm_writev)
 * - System call injection
 * - Memory mapping operations
 * - Stack backtrace (libunwind)
 * - DWARF debug info parsing (moved to dwarf.cpp)
 */

#include "guest.h"
#include <climits>
#include <stdexcept>
#include "monitor.h"
#include "dwarf_info.h"
#include <cstdint>
#include <fcntl.h>
#include <fmt/format.h>
#include <libunwind-ptrace.h>
#include <libunwind.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/reg.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <elf.h>
#include <unistd.h>
#include <vector>

const uintptr_t scratch_page = 0x0000600000000000;
const uintptr_t syscall_instr = 0x0000600000000ff0;

/* ======================================================================
 * Section 1: Backtrace (Local Unwinding)
 * ====================================================================== */

__attribute__((unused)) void print_call_stack()
{
  unw_cursor_t cursor;
  unw_context_t context;

  // Initialize cursor to current frame for local unwinding.
  unw_getcontext(&context);
  unw_init_local(&cursor, &context);

  // Unwind frames one by one, going up the frame stack.
  while (unw_step(&cursor) > 0)
  {
    unw_word_t offset, pc;
    unw_get_reg(&cursor, UNW_REG_IP, &pc);
    if (pc == 0)
      break;
    detsim::ui::ui_printf("0x%lx:", pc);

    char sym[256];
    if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0)
      detsim::ui::ui_printf(" (%s+0x%lx)\n", sym, offset);
    else
      detsim::ui::ui_printf(" -- error: unable to obtain symbol name for this frame\n");
  }
}

/* ======================================================================
 * Section 2: Register Access
 * ====================================================================== */

#define def_tracee_set(reg)                                                    \
  void tracee_set_##reg(int pid, uint64_t val)                                 \
  { \
    struct user_regs_struct uregs; \
    ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs); \
    uregs.reg = val; \
    ptrace_right(PTRACE_SETREGS, pid, NULL, &uregs); \
  } 
def_tracee_set(rax); 
def_tracee_set(orig_rax);
def_tracee_set(rdi);

#define def_tracee_get(reg)                                                    \
  uint64_t tracee_get_##reg(int pid)                                           \
  { \
    struct user_regs_struct uregs; \
    ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs); \
    return uregs.reg; \
  }

__attribute__((unused)) def_tracee_get(rip);
def_tracee_get(rbp);
def_tracee_get(rsp);
def_tracee_get(rdi);

/* ======================================================================
 * Section 3: VDSO Removal
 * ====================================================================== */

#define AT_NULL 0
#define AT_IGNORE 1
#define AT_SYSINFO_EHDR 33
#define DETERMINISTIC_RANDOM_0 0xdeadbeefcafebabeULL
#define DETERMINISTIC_RANDOM_1 0x1122334455667788ULL

void remove_vdso(int pid)
{
  user_regs_struct regs{};
  if (ptrace(PTRACE_GETREGS, pid, nullptr, &regs) < 0)
    throw std::runtime_error("Cannot read initial registers for auxv normalization");
  auto read_word = [pid](uintptr_t address) {
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, address, nullptr);
    if (errno) throw std::runtime_error("Cannot read initial stack auxiliary vector");
    return uintptr_t(word);
  };
  uintptr_t argc = read_word(regs.rsp);
  if (argc > 1048576) throw std::runtime_error("Invalid initial stack argument count");
  uintptr_t pos = regs.rsp + (argc + 2) * sizeof(uintptr_t);
  size_t environment_count = 0;
  while (read_word(pos)) {
    if (++environment_count > 1048576)
      throw std::runtime_error("Unterminated initial stack environment");
    pos += sizeof(uintptr_t);
  }
  pos += sizeof(uintptr_t);
  for (size_t count = 0; count < 256; ++count, pos += sizeof(Elf64_auxv_t)) {
    auto type = read_word(pos);
    if (type == AT_NULL) return; // A static executable need not expose a vDSO.
    if (type == AT_SYSINFO_EHDR) {
      if (ptrace(PTRACE_POKEDATA, pid, pos, AT_IGNORE) < 0)
        throw std::runtime_error("Cannot disable initial stack vDSO entry");
      return;
    }
  }
  throw std::runtime_error("Unterminated initial stack auxiliary vector");
}

int patch_at_random(pid_t pid) {
  char auxv_path[64];
  snprintf(auxv_path, sizeof(auxv_path), "/proc/%d/auxv", pid);
  FILE* input = fopen(auxv_path, "rb");
  if (!input) return -1;
  Elf64_auxv_t entry{};
  uintptr_t address = 0;
  while (fread(&entry, sizeof(entry), 1, input) == 1) {
    if (entry.a_type == AT_RANDOM) { address = entry.a_un.a_val; break; }
    if (entry.a_type == AT_NULL) break;
  }
  fclose(input);
  if (!address) return -1;
  const uint64_t value[2] = {DETERMINISTIC_RANDOM_0, DETERMINISTIC_RANDOM_1};
  return guest_write_exact(pid, reinterpret_cast<void*>(address), value, sizeof(value)) ? 0 : -1;
}

/* ======================================================================
 * Section 4: Memory Access (via process_vm_readv/process_vm_writev)
 * ====================================================================== */

#include <sys/uio.h>
#include <sys/syscall.h>

static inline struct iovec make_iovec(void *base, size_t len)
{
  struct iovec iov = { base, len };
  return iov;
}

static bool guest_transfer_exact(bool write, pid_t pid, void* guest,
                                 void* host, size_t size)
{
  if (!size) return true;
  const auto address = reinterpret_cast<uintptr_t>(guest);
  if (pid <= 0 || !guest || !host || size > size_t(SSIZE_MAX) ||
      size > UINTPTR_MAX - address) return false;
  iovec local{host, size}, remote{guest, size};
  ssize_t transferred;
  do {
    transferred = write ? process_vm_writev(pid, &local, 1, &remote, 1, 0)
                        : process_vm_readv(pid, &local, 1, &remote, 1, 0);
  } while (transferred < 0 && errno == EINTR);
  return transferred >= 0 && size_t(transferred) == size;
}

bool guest_read_exact(pid_t pid, const void* guest, void* host, size_t size)
{
  return guest_transfer_exact(false, pid, const_cast<void*>(guest), host, size);
}
bool guest_write_exact(pid_t pid, void* guest, const void* host, size_t size)
{
  return guest_transfer_exact(true, pid, guest, const_cast<void*>(host), size);
}

void tracee_write_mem(int pid, void *addr, const void *data, int len)
{
  struct iovec local = make_iovec((void *)data, len);
  struct iovec remote = make_iovec(addr, len);

  ssize_t n = syscall(SYS_process_vm_writev, pid, &local, 1, &remote, 1, 0);
  if (n != len)
  {
    LOG_CRIT("process_vm_writev failed for pid %d at %p: %s", pid, addr, strerror(errno));
  }
}

void tracee_read_mem(int pid, const void *addr, void *data, int len)
{
  struct iovec local = make_iovec(data, len);
  struct iovec remote = make_iovec((void *)addr, len);

  ssize_t n = syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0);
  if (n != len)
  {
    LOG_ERROR("process_vm_readv failed for pid %d at %p: %s", pid, addr, strerror(errno));
    memset(data, 0, len);
  }
}

__attribute__((unused)) uint8_t tracee_read_byte(int pid, void *addr)
{
  uint8_t data[8];
  *((uint64_t *)data) = ptrace_right(PTRACE_PEEKDATA, pid, addr, NULL);
  return data[0];
}

uint64_t tracee_read_word(int pid, void *addr)
{
  return ptrace_right(PTRACE_PEEKDATA, pid, addr, NULL);
}

int tracee_write_word(int pid, void *addr, long data)
{
  return ptrace_right(PTRACE_POKEDATA, pid, addr, data);
}

/* ======================================================================
 * Section 5: Syscall Injection
 * ====================================================================== */

void tracee_switch_syscall(int pid, int SYS_which, uint64_t rdi, uint64_t rsi,
                           uint64_t rdx, uint64_t r10, uint64_t r8, uint64_t r9)
{
  struct user_regs_struct syscall_regs;
  ptrace_right(PTRACE_GETREGS, pid, NULL, &syscall_regs);
  LOG_TRACE("rip syscall enter: %p", (void *)syscall_regs.rip);
  syscall_regs.orig_rax = SYS_which;
  syscall_regs.rdi = rdi;
  syscall_regs.rsi = rsi;
  syscall_regs.rdx = rdx;
  syscall_regs.r10 = r10;
  syscall_regs.r8 = r8;
  syscall_regs.r9 = r9;
  ptrace_right(PTRACE_SETREGS, pid, NULL, &syscall_regs);
}

/* ======================================================================
 * Section 6: Register Logging & Display
 * ====================================================================== */

void log_regs(struct user_regs_struct *regs)
{
  LOG_TRACE("rax = %016llx rbx = %016llx rcx = %016llx rdx = %016llx\n"
            "rsp = %016llx rbp = %016llx rdi = %016llx rsi = %016llx\n"
            "r8  = %016llx r9  = %016llx r10 = %016llx rip = %016llx\n",
            regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsp, regs->rbp,
            regs->rdi, regs->rsi, regs->r8, regs->r9, regs->r10, regs->rip);
  LOG_TRACE("orig_rax = %016lx", regs->orig_rax);
}

void show_regs(struct user_regs_struct *regs)
{
  detsim::ui::ui_printf("rax = %016llx rbx = %016llx rcx = %016llx rdx = %016llx\n"
         "rsp = %016llx rbp = %016llx rdi = %016llx rsi = %016llx\n"
         "r8  = %016llx r9  = %016llx r10 = %016llx rip = %016llx\n",
         regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsp, regs->rbp,
         regs->rdi, regs->rsi, regs->r8, regs->r9, regs->r10, regs->rip);
  detsim::ui::ui_printf("orig_rax = %016llx\n", regs->orig_rax);
}

/* ======================================================================
 * Section 7: Syscall Execution
 * ====================================================================== */

#define PTRACE_TRAP_SIG (SIGTRAP | 0x80)
uint64_t tracee_do_syscall_in_place(int pid, int SYS_which, uint64_t rdi, uint64_t rsi,
                           uint64_t rdx, uint64_t r10, uint64_t r8, uint64_t r9)
{
  /* after last syscall exits */
  /* I don't care */
  /* scanf("%*c"); */

  /* detsim::ui::ui_printf("%d\n", pid); */
  struct user_regs_struct restore;
  struct user_regs_struct syscall_regs;

  ptrace_right(PTRACE_GETREGS, pid, NULL, &restore);
  syscall_regs = restore;

  syscall_regs.rip -= 2;
  LOG_TRACE("rip syscall enter: %p", (void *)syscall_regs.rip);
  syscall_regs.orig_rax = SYS_which;
  syscall_regs.rax = SYS_which;
  syscall_regs.rdi = rdi;
  syscall_regs.rsi = rsi;
  syscall_regs.rdx = rdx;
  syscall_regs.r10 = r10;
  syscall_regs.r8 = r8;
  syscall_regs.r9 = r9;
  ptrace_right(PTRACE_SETREGS, pid, NULL, &syscall_regs);

  int wstatus = 0;
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != PTRACE_TRAP_SIG)
    throw std::runtime_error("Unexpected signal or exit during bootstrap syscall injection");

  /* before syscall enter */
  /* syscall modified already */
  LOG_TRACE("To do syscall:");
  log_regs(&syscall_regs);

  /* push syscall to exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != PTRACE_TRAP_SIG)
    throw std::runtime_error("Unexpected signal or exit during syscall injection");

  ptrace_right(PTRACE_GETREGS, pid, NULL, &syscall_regs);
  LOG_TRACE("Syscall returns %p", (void *)syscall_regs.rax);
  LOG_TRACE("rip syscall return: %p", (void *)syscall_regs.rip);

  /* restore for another syscall */
  ptrace_right(PTRACE_SETREGS, pid, NULL, &restore);
  /* it may stop */

  return syscall_regs.rax;
}
uint64_t tracee_do_syscall(int pid, int SYS_which, uint64_t rdi, uint64_t rsi,
                           uint64_t rdx, uint64_t r10, uint64_t r8, uint64_t r9)
{
  /* after last syscall exits */
  /* I don't care */
  /* scanf("%*c"); */

  /* detsim::ui::ui_printf("%d\n", pid); */
  struct user_regs_struct restore;
  struct user_regs_struct syscall_regs;

  ptrace_right(PTRACE_GETREGS, pid, NULL, &restore);
  syscall_regs = restore;

  syscall_regs.rip = syscall_instr;
  LOG_TRACE("rip syscall enter: %p", (void *)syscall_regs.rip);
  syscall_regs.orig_rax = SYS_which;
  syscall_regs.rax = SYS_which;
  syscall_regs.rdi = rdi;
  syscall_regs.rsi = rsi;
  syscall_regs.rdx = rdx;
  syscall_regs.r10 = r10;
  syscall_regs.r8 = r8;
  syscall_regs.r9 = r9;
  ptrace_right(PTRACE_SETREGS, pid, NULL, &syscall_regs);

  int wstatus = 0;
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != PTRACE_TRAP_SIG)
    throw std::runtime_error("Unexpected signal or exit during injected syscall entry");

  /* before syscall enter */
  /* syscall modified already */
  LOG_TRACE("To do syscall:");
  log_regs(&syscall_regs);

  /* push syscall to exit */
  ptrace_right(PTRACE_SYSCALL, pid, NULL, NULL);
  waitpid(pid, &wstatus, 0);
  if (!WIFSTOPPED(wstatus) || WSTOPSIG(wstatus) != PTRACE_TRAP_SIG)
    throw std::runtime_error("Unexpected signal or exit during injected syscall exit");

  ptrace_right(PTRACE_GETREGS, pid, NULL, &syscall_regs);
  LOG_TRACE("Syscall returns %p", (void *)syscall_regs.rax);
  LOG_TRACE("rip syscall return: %p", (void *)syscall_regs.rip);

  /* restore for another syscall */
  ptrace_right(PTRACE_SETREGS, pid, NULL, &restore);
  /* it may stop */

  return syscall_regs.rax;
}

/* ======================================================================
 * Section 8: Memory Mapping Operations
 * ====================================================================== */

void tracee_do_munmap(int pid, uint64_t start, uint64_t end)
{
  tracee_do_syscall(pid, SYS_munmap, start, end - start, 0, 0, 0, 0);
}

void *tracee_do_mmap(int pid, uint64_t start, uint64_t end, int prot)
{
  void *ret = (void *)tracee_do_syscall(pid, SYS_mmap, start, end - start,
                                        prot,
                                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  if (ret == MAP_FAILED) {
    LOG_ERROR("mmap failed for %p-%p (prot=%x): %s", 
              (void*)start, (void*)end, prot, strerror(errno));
  }
  return ret;
}

void *tracee_do_mmap_in_place(int pid, uint64_t start, uint64_t end, int prot)
{
  void *ret = (void *)tracee_do_syscall_in_place(pid, SYS_mmap, start, end - start,
                                        prot,
                                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
  if (ret == MAP_FAILED) {
    LOG_ERROR("mmap failed for %p-%p (prot=%x): %s", 
              (void*)start, (void*)end, prot, strerror(errno));
  }
  return ret;
}

__attribute__((unused)) static bool
addr_executable(uint64_t addr, const std::vector<maps_item> &items)
{
  for (auto &item : items)
  {
    if (item.start <= addr && addr < item.end)
    {
      if (item.flags[2] == 'x')
        return true;
      else
        return false;
    }
  }
  return false;
}

__attribute__((unused)) static bool
addr_on_stack(uint64_t addr, const std::vector<maps_item> &items)
{
  for (auto &item : items)
  {
    if (strcmp(item.name, "[stack]"))
      continue;
    if (addr >= item.start && addr < item.end)
      return true;
  }
  return false;
}

/* ======================================================================
 * Section 9: Stack Backtrace (Remote Unwinding)
 * ====================================================================== */

int resolve_rip_func(const char *exefile, uintptr_t rip);

void tracee_backtrace(int pid)
{
  unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
  void *ui = _UPT_create(pid);
  if (!ui)
  {
    perror("_UPT_create failed");
    return;
  }
  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, as, ui) < 0)
  {
    LOG_ERROR("unw_init_remote failed\n");
  }
  int frame = 0;

  do
  {
    detsim::ui::ui_printf("#%d ", frame++);
    unw_word_t rip, rsp;
    char func[256];
    unw_get_reg(&cursor, UNW_REG_IP, &rip);
    unw_get_reg(&cursor, UNW_REG_SP, &rsp);

    if (unw_get_proc_name(&cursor, func, sizeof(func), NULL) == 0)
    {
      detsim::ui::ui_printf("0x%lx in %s()", (uintptr_t)rip, func);
      resolve_rip_func(ptmc_state.tracee[ptmc_state.cursor].executable, rip);
    }
    else
      detsim::ui::ui_printf("0x%lx in ??\n", (uintptr_t)rip);

  } while (unw_step(&cursor) > 0);
  _UPT_destroy(ui);
  unw_destroy_addr_space(as);
}

/* ======================================================================
 * Section 10: Temporary Memory Page
 * ====================================================================== */

/* Only once for each process */
void tracee_reserve_temp_page(int pid)
{
  tracee_do_mmap_in_place(pid, scratch_page, scratch_page + PAGE_SIZE);
}

int tracee_do_open(int pid, const char *filename, uint64_t flags)
{
  /* need tracee memory to place filename */
  /* mappings has been restored */
  /* LOG_TRACE("open filename, flags = %s, %o", filename, flags); */
  assert(scratch_page);
  tracee_write_mem(pid, (void *)scratch_page, filename,
                   strlen(filename) + 1);
  return tracee_do_syscall(pid, SYS_open, (uint64_t)scratch_page, flags, 0,
                           0, 0, 0);
}

/* ======================================================================
 * Section 11: DWARF Debug Info Wrapper
 * These functions delegate to the new dwarf.cpp module.
 * ====================================================================== */

#include "dwarf_info.h"

uintptr_t get_var_addr(const char *varname)
{
  return dwarf_get_global_addr(varname);
}

std::string get_var_type(const char *varname)
{
  return dwarf_get_global_type(varname);
}

void *memcpy_guest2host(void *dest, const void *src, size_t n)
{
  int cursor = ptmc_state.cursor;
  if (cursor < 0 || cursor >= NP) cursor = 0;
  int pid = ptmc_state.pids[cursor];
  tracee_read_mem(pid, src, dest, n);
  return dest;
}

void *memcpy_host2guest(void *dest, const void *src, size_t n)
{
  int pid = ptmc_state.pids[ptmc_state.cursor];
  tracee_write_mem(pid, dest, src, n);
  return dest;
}

void tracee_show_regs(int pid)
{
  struct user_regs_struct uregs;
  ptrace_right(PTRACE_GETREGS, pid, NULL, &uregs);
  show_regs(&uregs);
}
