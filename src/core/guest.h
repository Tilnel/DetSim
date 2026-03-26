#ifndef __GUEST_H
#define __GUEST_H
#include "debug.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <unistd.h>

extern const char *syscalls[450];
extern const uintptr_t scratch_page;
extern int running_process;

void tracee_set_rax(int pid, uint64_t val);
void tracee_set_orig_rax(int pid, uint64_t val);
void tracee_set_ret(int pid, uint64_t val);
uint64_t tracee_get_rip(int pid);
uint64_t tracee_get_rbp(int pid);
uint64_t tracee_get_rsp(int pid);

void print_call_stack();
void tracee_reserve_temp_page(int pid);
uint64_t tracee_read_word(int pid, void *addr);
uint8_t tracee_read_byte(int pid, void *addr);
int tracee_write_word(int pid, void *addr, long data);
void tracee_write_mem(int pid, void *addr, const void *data, int len);
void tracee_read_mem(int pid, const void *addr, void *data, int len);
void remove_vdso(int pid);
int patch_at_random(pid_t pid);
void patch_cpu_features_elf(pid_t pid);

void show_regs(struct user_regs_struct *regs);
void tracee_switch_syscall(int pid, int SYS_which, uint64_t rdi, uint64_t rsi,
                           uint64_t rdx, uint64_t r10, uint64_t r8,
                           uint64_t r9);

uint64_t tracee_do_syscall(int pid, int SYS_which, uint64_t rdi, uint64_t rsi,
                           uint64_t rdx, uint64_t r10, uint64_t r8,
                           uint64_t r9);

__attribute__((unused)) static inline long
Ptrace_right(enum __ptrace_request op, pid_t pid, void *addr, void *data)
{
  /* ptrace(2): On success, the PTRACE_PEEK* operations return the requested *
   * data ... and other operations return zero. On error, all operations     *
   * return -1, and errno is set to indicate the error. Since the value      *
   * returned by a successful PTRACE_PEEK* operation may be -1, the caller   *
   * must clear errno before the call, and then check it ...                 */
  errno = 0;
  long result = ptrace(op, pid, addr, data);
  if (errno)
    panic("ptrace(%d, %d, %p, %p): %s", op, pid, addr, data, strerror(errno));
  return result;
}

#define ptrace_right(a, b, c, d) Ptrace_right((enum __ptrace_request)a, (pid_t)b, (void *)(c), (void *)(d))
void tracee_do_munmap(int pid, uint64_t start, uint64_t end);
void *tracee_do_mmap(int pid, uint64_t start, uint64_t end, int prot = PROT_READ | PROT_WRITE | PROT_EXEC);
void *tracee_do_mmap_in_place(int pid, uint64_t start, uint64_t end, int prot = PROT_READ | PROT_WRITE | PROT_EXEC);
void *tracee_do_mmap_back(int pid, uint64_t start, uint64_t end, int prot = PROT_READ | PROT_WRITE | PROT_EXEC);
int tracee_do_open(int pid, const char *filename, uint64_t flags);

void tracee_backtrace(int pid);
void tracee_show_regs(int pid);
uintptr_t get_var_addr(const char *varname);
std::string get_var_type(const char *varname);
void init_dwarf();
void cleanup_dwarf();

void *memcpy_guest2host(void *dest, const void *src, size_t n);
void *memcpy_host2guest(void *dest, const void *src, size_t n);

#endif /* __GUEST_H */
