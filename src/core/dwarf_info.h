/*
 * dwarf.h - DWARF debug information parsing
 *
 * This module provides debugging capabilities similar to GDB:
 * - Stack frame analysis
 * - Variable inspection (global and local)
 * - Type information (structs, unions, arrays, pointers)
 * - Expression evaluation for member access
 */

#ifndef __DWARF_H
#define __DWARF_H

#include "types.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* ======================================================================
 * Type Information
 * ====================================================================== */

struct type_info
{
  std::string name;
  size_t size = 0;
  bool is_pointer = false;
  bool is_struct = false;
  bool is_array = false;

  /* For structs */
  struct member
  {
    std::string name;
    size_t offset = 0;
    size_t size = 0;
    std::string type_name;
  };
  std::vector<member> members;

  /* For arrays */
  size_t array_elements = 0;
  std::string element_type;
};

/* ======================================================================
 * Variable Information
 * ====================================================================== */

struct var_info
{
  std::string name;
  std::string type_name;
  uintptr_t address = 0; /* For global variables */
  int stack_offset = 0;  /* For local variables (from $rbp) */
  bool is_global = false;
  bool is_local = false;
};

/* ======================================================================
 * Stack Frame Information
 * ====================================================================== */

struct stack_frame
{
  int frame_id;
  std::string function_name;
  uintptr_t pc;
  uintptr_t sp;
  uintptr_t bp;
  std::vector<var_info> local_vars;
};

/* ======================================================================
 * Initialization
 * ====================================================================== */

/* Initialize DWARF parsing for the executable */
void dwarf_init(const char *executable);

/* Cleanup DWARF resources */
void dwarf_cleanup(void);

/* Legacy compatibility wrapper - uses tracee[0].executable */
void init_dwarf(void);

/* Load shared library symbols for a specific PID (for dynamic linking) */
void dwarf_load_shared_library_symbols(int pid);

/* ======================================================================
 * Variable Lookup
 * ====================================================================== */

/* Get global variable address */
uintptr_t dwarf_get_global_addr(const char *varname);

/* Get global variable type */
std::string dwarf_get_global_type(const char *varname);

const type_info &dwarf_get_type_info(const char *type_name);

/* ======================================================================
 * Stack Analysis
 * ====================================================================== */

/* Get current stack trace with frame info */
std::vector<stack_frame> dwarf_get_stack_trace(int pid);

/* Print stack trace with parameters */
void dwarf_print_stack_trace(int pid);

/* Switch to a specific frame */
bool dwarf_set_frame(int frame_id);

/* Get current frame ID */
int dwarf_get_current_frame(void);

/* Get local variables for a specific frame */
std::vector<var_info> dwarf_get_local_vars(int pid, int frame_id);

/* Print local variables for current frame */
void dwarf_print_local_vars(int pid);

/* Get frame base address (BP) for frame_id */
uintptr_t dwarf_get_frame_base(int frame_id);

/* Look up local variable in current frame */
bool dwarf_lookup_local(const char *varname, uintptr_t *out_addr,
                        std::string *out_type);

/* Resolve PC to function name and source location */
bool dwarf_resolve_pc(uintptr_t pc, std::string &func_name,
                      std::string &file_name, int &line_num);

/* ======================================================================
 * Expression Evaluation
 * ====================================================================== */

/* Calculate address of an expression like "a.b.c[5].d" */
uintptr_t dwarf_eval_expr(int pid, const char *expr);

/* Read variable value by name (simple types only) */
template <typename T>
bool dwarf_read_var(int pid, const char *varname, T *out);

/* Read member from struct by path like "struct_ptr->member.submember" */
bool dwarf_read_member(int pid, const char *base_var,
                       const std::vector<std::string> &members, void *out_buf,
                       size_t out_size);

/* ======================================================================
 * Type Operations
 * ====================================================================== */

/* Get size of a type */
size_t dwarf_type_size(const char *type_name);

/* Get member offset within struct */
ptrdiff_t dwarf_member_offset(const char *struct_type, const char *member);

/* Get member size and type name */
bool dwarf_member_info(const char *struct_type, const char *member,
                       ptrdiff_t *offset, size_t *size, std::string *type_name);

/* Get element type after one level of array/pointer indexing
 * e.g., "rect * *" -> "rect *", "point[]" -> "point", "int*" -> "int"
 */
std::string dwarf_get_element_type(const char *type_name);

/* Dump type information for debugging */
void dwarf_dump_type(const char *type_name);

/* ======================================================================
 * Raw Access (for advanced use)
 * ====================================================================== */

/* Direct DWARF die lookup */
void *dwarf_lookup_die(uintptr_t pc);

/* Per-frame local variable cache */
class DwarfLocalCache
{
  public:
  static DwarfLocalCache &instance();

  struct VarInfo
  {
    uintptr_t address;
    std::string type_name;
  };

  void cache_local(int frame_id, const std::string &varname,
                   const VarInfo &info);
  bool lookup_local(int frame_id, const std::string &varname, VarInfo &out);
  void clear();
  void clear_frame(int frame_id);

  private:
  DwarfLocalCache() = default;
  std::unordered_map<std::string, VarInfo> cache_;
  mutable std::mutex mutex_;
  int current_frame_id_ = -1;
};

#endif /* __DWARF_H */
