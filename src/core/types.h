/*
 * types.h - Core type definitions used across the project
 * 
 * This header contains:
 * - Basic integer type aliases (__s8, __u8, etc.)
 * - ptrace_syscall_info struct (from kernel headers)
 * - Global constants (HASH_LEN, KiB/MiB)
 * - Common typedefs (bytes, hash_type)
 * 
 * This is a minimal header designed to be included by almost all other headers
 * without causing circular dependencies.
 */

#ifndef __TYPES_H
#define __TYPES_H

#include <stdint.h>
#include <unistd.h>

/* ========================================================================
 * Basic Integer Types (compatible with kernel style)
 * ======================================================================== */

typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

#ifdef __GNUC__
__extension__ typedef __signed__ long long __s64;
__extension__ typedef unsigned long long __u64;
#else
typedef __signed__ long long __s64;
typedef unsigned long long __u64;
#endif

/* ========================================================================
 * ptrace syscall info (from Linux kernel headers)
 * Copied here to ensure compatibility and avoid kernel header dependencies
 * ======================================================================== */

struct ptrace_syscall_info
{
  __u8 op; /* PTRACE_SYSCALL_INFO_* */
  __u8 pad[3];
  __u32 arch;
  __u64 instruction_pointer;
  __u64 stack_pointer;
  union
  {
    struct
    {
      __u64 nr;
      __u64 args[6];
    } entry;
    struct
    {
      __s64 rval;
      __u8 is_error;
    } exit;
    struct
    {
      __u64 nr;
      __u64 args[6];
      __u32 ret_data;
    } seccomp;
  };
};

/* ========================================================================
 * Global Constants
 * ======================================================================== */

#ifndef NP
  #define NP 0
#endif
static_assert(NP > 0);

#define HASH_LEN 16
#define HASH_FORMAT "%016lx"

#define KiB *1024
#define MiB KiB * 1024

/* ========================================================================
 * Common Type Aliases
 * ======================================================================== */

typedef uint8_t bytes;
typedef uint64_t hash_type;

/* ========================================================================
 * Memory Mapping Item
 * ======================================================================== */

/* Represents a single entry from /proc/pid/maps */
typedef struct maps_item
{
  uint64_t start, end;
  char flags[5];
  uint32_t offset;
  int a, b, inode;
  char name[512];
  maps_item() : start(0), end(0), offset(0), a(0), b(0), inode(0)
  {
    name[0] = '\0';
  }
} maps_item;

/* ========================================================================
 * Feature Flags
 * ======================================================================== */

/* Define this to enable detailed file metadata (timestamps, owner, etc.)
 * This will cause state explosion due to timestamp changes on every write.
 * When undefined, only file size is tracked. */
// #define FSSTATE_DETAILED_METADATA

#endif /* __TYPES_H */
