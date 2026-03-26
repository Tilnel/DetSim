/*
 * dwarf.cpp - DWARF debug information parsing
 */

#include "debug.h"
#include "dwarf_info.h"
#include <cctype>
#include <cstring>
#include <dwarf.h>
#include <elf.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <gelf.h>
#include <libdwarf/libdwarf.h>
#include <stack>
#include <unistd.h>
#include <unordered_set>

/* libunwind for stack trace */
#include <libunwind-ptrace.h>
#include <libunwind.h>

/* Defined in guest.cpp */
extern void *memcpy_guest2host(void *dest, const void *src, size_t n);

/* Current frame context for expression evaluation */
static int current_frame_id = 0;
static std::vector<stack_frame> current_stack_trace;

/* Function parameter info from DWARF */
struct param_info
{
  std::string name;
  std::string type_name;
  int reg_num;            /* Register number for parameter passing */
  ptrdiff_t stack_offset; /* Stack offset if passed on stack */
  bool in_reg;
};

/* Function info cache */
struct func_info
{
  std::string name;
  uintptr_t low_pc;
  uintptr_t high_pc;
  std::vector<param_info> params;
  std::vector<var_info> local_vars;
};

/* ======================================================================
 * Section 1: Global State
 * ====================================================================== */

static Dwarf_Debug dbg = nullptr;
static int dwarf_fd = -1;

/* Global variables cache */
std::unordered_map<std::string, var_info> global_vars;

/* Type cache */
std::unordered_map<std::string, type_info> type_cache;

/* Type size cache */
std::unordered_map<std::string, size_t> type_size_cache;

/* Member offset cache: struct_type -> member_name -> offset */
std::unordered_map<std::string, std::unordered_map<std::string, ptrdiff_t>>
    member_offset_cache;

/* ELF symbol cache for symbols not in DWARF (e.g., _dl_x86_cpu_features) */
struct elf_symbol
{
  std::string name;
  uintptr_t address = 0; /* Runtime address (base + offset) */
  size_t size = 0;
  int type = 0;         /* STT_OBJECT, STT_FUNC, etc. */
  int bind = 0;         /* STB_GLOBAL, STB_LOCAL, etc. */
  std::string lib_path; /* Which library this symbol comes from */
};

/* Per-library symbol cache */
struct library_symbols
{
  std::string path;
  uintptr_t base_addr = 0; /* Runtime load address */
  std::unordered_map<std::string, elf_symbol> symbols;
};

/* Global library cache */
static std::unordered_map<std::string, library_symbols> elf_library_cache;
static std::string current_elf_path;
static int elf_symbols_loaded_for_pid = -1;

/* Forward declarations for ELF symbol handling */
static void load_elf_symbols(const char *executable);
static void load_shared_library_symbols(int pid);
static uintptr_t lookup_elf_symbol(const char *symname);
static std::string get_elf_symbol_type(const char *symname);

/* ======================================================================
 * Section 2: Type Parsing
 * ====================================================================== */

static std::string get_type_name_internal(Dwarf_Debug dbg, Dwarf_Die type_die);
static std::string get_type_name_simple(Dwarf_Debug dbg, Dwarf_Die type_die);
static void parse_type_info(Dwarf_Debug dbg, Dwarf_Die type_die,
                            type_info &info);

static std::string get_array_type_name(Dwarf_Debug dbg, Dwarf_Die type_die,
                                       type_info &info)
{
  Dwarf_Error err;

  /* Get element type */
  Dwarf_Attribute type_attr;
  if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) != DW_DLV_OK)
    return "unknown[]";

  Dwarf_Off type_offset;
  if (dwarf_global_formref(type_attr, &type_offset, &err) != DW_DLV_OK)
    return "unknown[]";

  Dwarf_Die elem_die;
  if (dwarf_offdie(dbg, type_offset, &elem_die, &err) != DW_DLV_OK)
    return "unknown[]";

  /* Use simple name lookup to avoid recursion into struct members */
  std::string elem_name = get_type_name_simple(dbg, elem_die);
  info.element_type = elem_name;
  info.is_array = true;

  /* Get array size from subrange child */
  Dwarf_Die child_die;
  if (dwarf_child(type_die, &child_die, &err) == DW_DLV_OK)
  {
    do
    {
      Dwarf_Half child_tag;
      if (dwarf_tag(child_die, &child_tag, &err) != DW_DLV_OK)
        continue;

      if (child_tag == DW_TAG_subrange_type)
      {
        Dwarf_Attribute count_attr;
        if (dwarf_attr(child_die, DW_AT_count, &count_attr, &err) == DW_DLV_OK)
        {
          Dwarf_Unsigned count;
          if (dwarf_formudata(count_attr, &count, &err) == DW_DLV_OK)
          {
            info.array_elements = count;
          }
        }
        else if (dwarf_attr(child_die, DW_AT_upper_bound, &count_attr, &err) ==
                 DW_DLV_OK)
        {
          Dwarf_Signed bound;
          if (dwarf_formsdata(count_attr, &bound, &err) == DW_DLV_OK)
          {
            info.array_elements = bound + 1; /* upper_bound + 1 = count */
          }
        }
      }
    } while (dwarf_siblingof(dbg, child_die, &child_die, &err) == DW_DLV_OK);
  }

  if (info.array_elements > 0)
  {
    return fmt::format("{}[{}]", elem_name, info.array_elements);
  }
  return elem_name + "[]";
}

/* Forward declaration */
static std::string get_type_name_simple(Dwarf_Debug dbg, Dwarf_Die type_die);

static void parse_struct_members(Dwarf_Debug dbg, Dwarf_Die struct_die,
                                 type_info &info)
{
  Dwarf_Error err;
  info.is_struct = true;

  /* Get struct size */
  Dwarf_Attribute size_attr;
  if (dwarf_attr(struct_die, DW_AT_byte_size, &size_attr, &err) == DW_DLV_OK)
  {
    Dwarf_Unsigned size;
    if (dwarf_formudata(size_attr, &size, &err) == DW_DLV_OK)
    {
      info.size = size;
    }
  }

  /* Parse members */
  Dwarf_Die child_die;
  if (dwarf_child(struct_die, &child_die, &err) != DW_DLV_OK)
    return;

  do
  {
    Dwarf_Half tag;
    if (dwarf_tag(child_die, &tag, &err) != DW_DLV_OK)
      continue;

    if (tag == DW_TAG_member)
    {
      type_info::member m;

      /* Member name */
      char *name = nullptr;
      if (dwarf_diename(child_die, &name, &err) == DW_DLV_OK && name)
      {
        m.name = name;
      }

      /* Member offset */
      Dwarf_Attribute loc_attr;
      if (dwarf_attr(child_die, DW_AT_data_member_location, &loc_attr, &err) ==
          DW_DLV_OK)
      {
        Dwarf_Unsigned offset;
        if (dwarf_formudata(loc_attr, &offset, &err) == DW_DLV_OK)
        {
          m.offset = offset;
        }
      }

      /* Member type - just get the name and size */
      Dwarf_Attribute type_attr;
      if (dwarf_attr(child_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Off type_offset;
        if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
        {
          Dwarf_Die type_die;
          if (dwarf_offdie(dbg, type_offset, &type_die, &err) == DW_DLV_OK)
          {
            /* Get type name without full recursion */
            m.type_name = get_type_name_simple(dbg, type_die);

            /* Get size - for pointers use sizeof(void*), for others parse
             * normally */
            Dwarf_Half member_tag;
            if (dwarf_tag(type_die, &member_tag, &err) == DW_DLV_OK &&
                member_tag == DW_TAG_pointer_type)
            {
              m.size = sizeof(void *);
            }
            else
            {
              type_info member_info;
              parse_type_info(dbg, type_die, member_info);
              m.size = member_info.size;
            }
          }
        }
      }

      info.members.push_back(m);
    }
  } while (dwarf_siblingof(dbg, child_die, &child_die, &err) == DW_DLV_OK);
}

static void parse_type_info(Dwarf_Debug dbg, Dwarf_Die type_die,
                            type_info &info);

static void cache_type_by_name(Dwarf_Debug dbg, const std::string &name);

static void cache_type_info(Dwarf_Debug dbg, const std::string &name,
                            Dwarf_Die type_die)
{
  if (name.empty() || type_cache.find(name) != type_cache.end())
    return;

  type_info info;
  parse_type_info(dbg, type_die, info);
  if (!info.name.empty())
  {
    type_cache[info.name] = info;
  }
}

static void parse_type_info(Dwarf_Debug dbg, Dwarf_Die type_die,
                            type_info &info)
{
  Dwarf_Error err;
  Dwarf_Half tag;

  if (dwarf_tag(type_die, &tag, &err) != DW_DLV_OK)
  {
    return;
  }

  char *type_name = nullptr;
  dwarf_diename(type_die, &type_name, &err);
  if (type_name)
  {
    info.name = type_name;
  }

  /* Get size for base types */
  Dwarf_Attribute size_attr;
  if (dwarf_attr(type_die, DW_AT_byte_size, &size_attr, &err) == DW_DLV_OK)
  {
    Dwarf_Unsigned size;
    if (dwarf_formudata(size_attr, &size, &err) == DW_DLV_OK)
    {
      info.size = size;
    }
  }

  switch (tag)
  {
    case DW_TAG_base_type:
    case DW_TAG_enumeration_type:
      /* Size already retrieved */
      break;

    case DW_TAG_structure_type:
    case DW_TAG_union_type:
      parse_struct_members(dbg, type_die, info);
      /* Cache the struct type for later lookup */
      if (!info.name.empty())
      {
        type_cache[info.name] = info;
      }
      break;

    case DW_TAG_pointer_type:
    {
      info.is_pointer = true;
      info.size = sizeof(void *); /* Pointer size */

      /* Get pointed-to type name using simple lookup (no recursion) */
      Dwarf_Attribute type_attr;
      if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Off type_offset;
        if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
        {
          Dwarf_Die pointed_die;
          if (dwarf_offdie(dbg, type_offset, &pointed_die, &err) == DW_DLV_OK)
          {
            std::string pointed_name = get_type_name_simple(dbg, pointed_die);
            if (!pointed_name.empty())
            {
              info.name = pointed_name + " *";
            }
            else
            {
              info.name = "void *";
            }
          }
        }
      }
      else
      {
        /* No type attribute - void pointer */
        info.name = "void *";
      }
      break;
    }

    case DW_TAG_array_type:
      info.name = get_array_type_name(dbg, type_die, info);
      /* Calculate total size if possible */
      if (info.array_elements > 0)
      {
        type_info elem_info;
        Dwarf_Attribute type_attr;
        if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
        {
          Dwarf_Off type_offset;
          if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
          {
            Dwarf_Die elem_die;
            if (dwarf_offdie(dbg, type_offset, &elem_die, &err) == DW_DLV_OK)
            {
              parse_type_info(dbg, elem_die, elem_info);
              info.size = elem_info.size * info.array_elements;
            }
          }
        }
      }
      break;

    case DW_TAG_typedef:
    {
      /* Follow typedef to actual type */
      std::string alias_name = info.name;

      Dwarf_Attribute type_attr;
      if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Off type_offset;
        if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
        {
          Dwarf_Die real_die;
          if (dwarf_offdie(dbg, type_offset, &real_die, &err) == DW_DLV_OK)
          {
            /* Check if underlying type is a forward declaration */
            Dwarf_Bool is_decl = 0;
            Dwarf_Attribute decl_attr;
            if (dwarf_attr(real_die, DW_AT_declaration, &decl_attr, &err) ==
                DW_DLV_OK)
            {
              dwarf_formflag(decl_attr, &is_decl, &err);
            }

            if (is_decl)
            {
              /* Underlying type is a forward declaration, need to find the
               * real definition */
              char *underlying_name = nullptr;
              if (dwarf_diename(real_die, &underlying_name, &err) ==
                      DW_DLV_OK &&
                  underlying_name)
              {
                /* Cache the underlying type by name to find the full
                 * definition */
                cache_type_by_name(dbg, underlying_name);
                /* Now get it from cache */
                auto it = type_cache.find(underlying_name);
                if (it != type_cache.end())
                {
                  info = it->second;
                  info.name = alias_name;
                  break;
                }
              }
            }

            type_info underlying;
            parse_type_info(dbg, real_die, underlying);
            info = underlying;
            info.name = alias_name;
          }
        }
      }
      break;
    }

    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    {
      /* Follow to actual type */
      Dwarf_Attribute type_attr;
      if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Off type_offset;
        if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
        {
          Dwarf_Die real_die;
          if (dwarf_offdie(dbg, type_offset, &real_die, &err) == DW_DLV_OK)
          {
            parse_type_info(dbg, real_die, info);
          }
        }
      }
      break;
    }
  }
}

/* Get type name without full recursion - used for member type names */
static std::string get_type_name_simple(Dwarf_Debug dbg, Dwarf_Die type_die)
{
  Dwarf_Error err;
  Dwarf_Half tag;

  if (dwarf_tag(type_die, &tag, &err) != DW_DLV_OK)
  {
    return "";
  }

  /* For named types (structs, typedefs, base types), just use the name */
  char *name = nullptr;
  if (dwarf_diename(type_die, &name, &err) == DW_DLV_OK && name)
  {
    return std::string(name);
  }

  /* For pointers, get pointed-to type name and append " *" */
  if (tag == DW_TAG_pointer_type)
  {
    Dwarf_Attribute type_attr;
    if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
    {
      Dwarf_Off type_offset;
      if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
      {
        Dwarf_Die pointed_die;
        if (dwarf_offdie(dbg, type_offset, &pointed_die, &err) == DW_DLV_OK)
        {
          std::string pointed_name = get_type_name_simple(dbg, pointed_die);
          if (!pointed_name.empty())
          {
            return pointed_name + " *";
          }
        }
      }
    }
    return "void *";
  }

  /* For arrays, get element type and append "[]" */
  if (tag == DW_TAG_array_type)
  {
    Dwarf_Attribute type_attr;
    if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
    {
      Dwarf_Off type_offset;
      if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
      {
        Dwarf_Die elem_die;
        if (dwarf_offdie(dbg, type_offset, &elem_die, &err) == DW_DLV_OK)
        {
          std::string elem_name = get_type_name_simple(dbg, elem_die);
          if (!elem_name.empty())
          {
            return elem_name + "[]";
          }
        }
      }
    }
    return "array";
  }

  /* For typedef/const/volatile, follow to base type */
  if (tag == DW_TAG_typedef || tag == DW_TAG_const_type ||
      tag == DW_TAG_volatile_type)
  {
    Dwarf_Attribute type_attr;
    if (dwarf_attr(type_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
    {
      Dwarf_Off type_offset;
      if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
      {
        Dwarf_Die real_die;
        if (dwarf_offdie(dbg, type_offset, &real_die, &err) == DW_DLV_OK)
        {
          return get_type_name_simple(dbg, real_die);
        }
      }
    }
  }

  return "";
}

static std::string get_type_name_internal(Dwarf_Debug dbg, Dwarf_Die type_die)
{
  type_info info;
  parse_type_info(dbg, type_die, info);
  return info.name;
}

/* ======================================================================
 * Section 3: Type Cache Building
 * ====================================================================== */

static void cache_type_by_name(Dwarf_Debug dbg, const std::string &name)
{
  if (type_cache.find(name) != type_cache.end())
    return; /* Already cached */

  Dwarf_Error err;
  Dwarf_Unsigned cu_header_length, abbrev_offset, next_cu_header;
  Dwarf_Half version_stamp, address_size;

  /* Reset to start */
  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                              &abbrev_offset, &address_size, &next_cu_header,
                              &err) == DW_DLV_OK)
  {
    /* Skip to end to reset */
  }

  /* Search all CUs for type */
  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                              &abbrev_offset, &address_size, &next_cu_header,
                              &err) == DW_DLV_OK)
  {
    Dwarf_Die no_die = 0;
    Dwarf_Die cu_die;

    if (dwarf_siblingof(dbg, no_die, &cu_die, &err) != DW_DLV_OK)
      continue;

    /* Search all DIEs in this CU */
    std::stack<Dwarf_Die> die_stack;
    Dwarf_Die child_die;
    if (dwarf_child(cu_die, &child_die, &err) == DW_DLV_OK)
    {
      die_stack.push(child_die);
    }

    while (!die_stack.empty())
    {
      Dwarf_Die current_die = die_stack.top();
      die_stack.pop();

      char *die_name = nullptr;
      if (dwarf_diename(current_die, &die_name, &err) == DW_DLV_OK && die_name)
      {
        if (name == die_name)
        {
          /* Check if this is a forward declaration (incomplete type) */
          Dwarf_Bool is_declaration = 0;
          Dwarf_Attribute decl_attr;
          if (dwarf_attr(current_die, DW_AT_declaration, &decl_attr, &err) ==
              DW_DLV_OK)
          {
            dwarf_formflag(decl_attr, &is_declaration, &err);
          }

          /* Skip forward declarations - look for the actual definition */
          if (is_declaration)
          {
            /* Add sibling and child before continuing */
            Dwarf_Die sibling_die;
            if (dwarf_siblingof(dbg, current_die, &sibling_die, &err) ==
                DW_DLV_OK)
            {
              die_stack.push(sibling_die);
            }

            Dwarf_Die next_child;
            if (dwarf_child(current_die, &next_child, &err) == DW_DLV_OK)
            {
              die_stack.push(next_child);
            }
            continue;
          }

          type_info info;
          info.name = name;
          parse_type_info(dbg, current_die, info);
          type_cache[name] = info;
          return;
        }
      }

      /* Add sibling */
      Dwarf_Die sibling_die;
      if (dwarf_siblingof(dbg, current_die, &sibling_die, &err) == DW_DLV_OK)
      {
        die_stack.push(sibling_die);
      }

      /* Add child */
      Dwarf_Die next_child;
      if (dwarf_child(current_die, &next_child, &err) == DW_DLV_OK)
      {
        die_stack.push(next_child);
      }
    }
  }
}

/* ======================================================================
 * Section 4: Variable Address Resolution
 * ====================================================================== */

static bool get_variable_address(Dwarf_Debug dbg, Dwarf_Die die,
                                 uintptr_t &addr)
{
  Dwarf_Error err;
  Dwarf_Attribute loc_attr;

  if (dwarf_attr(die, DW_AT_location, &loc_attr, &err) != DW_DLV_OK)
    return false;

  Dwarf_Unsigned expr_len;
  Dwarf_Ptr expr_bytes;

  if (dwarf_formexprloc(loc_attr, &expr_len, &expr_bytes, &err) != DW_DLV_OK)
    return false;

  const unsigned char *data =
      reinterpret_cast<const unsigned char *>(expr_bytes);

  /* Simple address: DW_OP_addr <address> */
  if (expr_len > 0 && data[0] == DW_OP_addr)
  {
    if (expr_len >= 1 + sizeof(uintptr_t))
    {
      memcpy(&addr, data + 1, sizeof(uintptr_t));
      return true;
    }
  }

  /* TODO: Support more location expressions */

  return false;
}

/* ======================================================================
 * Section 5: Initialization
 * ====================================================================== */

void dwarf_init(const char *executable)
{
  if (dbg != nullptr)
  {
    dwarf_cleanup();
  }

  dwarf_fd = open(executable, O_RDONLY);
  if (dwarf_fd < 0)
  {
    LOG_ERROR("Failed to open %s for DWARF parsing", executable);
    return;
  }

  Dwarf_Error err;
  if (dwarf_init(dwarf_fd, DW_DLC_READ, nullptr, nullptr, &dbg, &err) !=
      DW_DLV_OK)
  {
    LOG_ERROR("Failed initializing libdwarf");
    close(dwarf_fd);
    dwarf_fd = -1;
    return;
  }

  /* Parse all compilation units to cache global variables */
  Dwarf_Unsigned cu_header_length, abbrev_offset, next_cu_header;
  Dwarf_Half version_stamp, address_size;

  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                              &abbrev_offset, &address_size, &next_cu_header,
                              &err) == DW_DLV_OK)
  {
    Dwarf_Die no_die = 0;
    Dwarf_Die cu_die;

    if (dwarf_siblingof(dbg, no_die, &cu_die, &err) != DW_DLV_OK)
      continue;

    Dwarf_Die child_die;
    if (dwarf_child(cu_die, &child_die, &err) != DW_DLV_OK)
      continue;

    Dwarf_Die current_die = child_die;
    do
    {
      Dwarf_Half tag;
      if (dwarf_tag(current_die, &tag, &err) != DW_DLV_OK)
        continue;

      if (tag == DW_TAG_variable)
      {
        Dwarf_Attribute attr;
        Dwarf_Bool is_external;

        if (dwarf_attr(current_die, DW_AT_external, &attr, &err) == DW_DLV_OK)
        {
          if (dwarf_formflag(attr, &is_external, &err) == DW_DLV_OK &&
              is_external)
          {
            char *var_name = nullptr;
            if (dwarf_diename(current_die, &var_name, &err) != DW_DLV_OK)
              continue;

            Dwarf_Attribute type_attr;
            if (dwarf_attr(current_die, DW_AT_type, &type_attr, &err) !=
                DW_DLV_OK)
              continue;

            Dwarf_Off type_offset;
            if (dwarf_global_formref(type_attr, &type_offset, &err) !=
                DW_DLV_OK)
              continue;

            Dwarf_Die type_die;
            if (dwarf_offdie(dbg, type_offset, &type_die, &err) != DW_DLV_OK)
              continue;

            var_info info;
            info.name = var_name;
            info.type_name = get_type_name_internal(dbg, type_die);

            cache_type_info(dbg, info.type_name, type_die);
            info.is_global = true;
            info.is_local = false;
            info.stack_offset = 0;

            if (get_variable_address(dbg, current_die, info.address))
            {
              global_vars[var_name] = info;
              LOG_DEBUG("DWARF: Found global variable %s at 0x%lx (type: %s)",
                        var_name, info.address, info.type_name.c_str());
            }
          }
        }
      }
    } while (dwarf_siblingof(dbg, current_die, &current_die, &err) ==
             DW_DLV_OK);
  }

  LOG_INFO("DWARF: Parsed %zu global variables", global_vars.size());

  {
    while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                                &abbrev_offset, &address_size, &next_cu_header,
                                &err) == DW_DLV_OK)
    {
    }

    while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                                &abbrev_offset, &address_size, &next_cu_header,
                                &err) == DW_DLV_OK)
    {
      Dwarf_Die no_die = 0;
      Dwarf_Die cu_die;

      if (dwarf_siblingof(dbg, no_die, &cu_die, &err) != DW_DLV_OK)
        continue;

      std::stack<Dwarf_Die> die_stack;
      Dwarf_Die child_die;
      if (dwarf_child(cu_die, &child_die, &err) == DW_DLV_OK)
      {
        die_stack.push(child_die);
      }

      while (!die_stack.empty())
      {
        Dwarf_Die current_die = die_stack.top();
        die_stack.pop();

        Dwarf_Half tag;
        if (dwarf_tag(current_die, &tag, &err) != DW_DLV_OK)
          goto next_die;

        if (tag == DW_TAG_structure_type || tag == DW_TAG_union_type ||
            tag == DW_TAG_typedef || tag == DW_TAG_enumeration_type)
        {
          char *type_name = nullptr;
          if (dwarf_diename(current_die, &type_name, &err) == DW_DLV_OK &&
              type_name)
          {
            if (type_cache.find(type_name) == type_cache.end())
            {
              Dwarf_Bool is_declaration = 0;
              Dwarf_Attribute decl_attr;
              if (dwarf_attr(current_die, DW_AT_declaration, &decl_attr,
                             &err) == DW_DLV_OK)
              {
                dwarf_formflag(decl_attr, &is_declaration, &err);
              }

              if (!is_declaration)
              {
                type_info info;
                info.name = type_name;
                parse_type_info(dbg, current_die, info);
                if (!info.name.empty())
                {
                  type_cache[info.name] = info;
                }
              }
            }
          }
        }

      next_die:
        Dwarf_Die sibling_die;
        if (dwarf_siblingof(dbg, current_die, &sibling_die, &err) == DW_DLV_OK)
        {
          die_stack.push(sibling_die);
        }

        Dwarf_Die next_child;
        if (dwarf_child(current_die, &next_child, &err) == DW_DLV_OK)
        {
          die_stack.push(next_child);
        }
      }
    }
  }

  LOG_INFO("DWARF: Cached %zu types", type_cache.size());

  /* Also load ELF symbols as fallback for symbols not in DWARF */
  load_elf_symbols(executable);
}

void dwarf_cleanup(void)
{
  if (dbg != nullptr)
  {
    Dwarf_Error err;
    dwarf_finish(dbg, &err);
    dbg = nullptr;
  }
  if (dwarf_fd >= 0)
  {
    close(dwarf_fd);
    dwarf_fd = -1;
  }
  global_vars.clear();
  type_cache.clear();
  type_size_cache.clear();
  member_offset_cache.clear();
  elf_library_cache.clear();
  current_elf_path.clear();
  elf_symbols_loaded_for_pid = -1;
}

/* Legacy compatibility wrapper */
#include "monitor.h"
void init_dwarf(void)
{
  if (ptmc_state.tracee[0].executable == nullptr)
  {
    LOG_ERROR("No executable specified for DWARF parsing");
    return;
  }
  LOG_INFO("DWARF: Parsing %s", ptmc_state.tracee[0].executable);
  dwarf_init(ptmc_state.tracee[0].executable);
}

void cleanup_dwarf(void)
{
  /* Close dwarf debug handle */
  if (dbg)
  {
    Dwarf_Error err;
    dwarf_finish(dbg, &err);
    dbg = nullptr;
  }

  /* Close file descriptor */
  if (dwarf_fd >= 0)
  {
    close(dwarf_fd);
    dwarf_fd = -1;
  }

  /* Clear all caches */
  global_vars.clear();
  type_cache.clear();
  type_size_cache.clear();
  member_offset_cache.clear();
  elf_library_cache.clear();
}

/* ==================================================================
 * Section 5.5: ELF Symbol Table Parsing (fallback for non-DWARF symbols)
 * ====================================================================== */

/* Parse /proc/<pid>/maps and return list of loaded shared libraries with base
 * addresses Format: <start_addr>-<end_addr> <perms> <offset> <dev> <inode>
 * <pathname> Returns map of pathname -> base_address */
static std::unordered_map<std::string, uintptr_t> parse_proc_maps(int pid)
{
  std::unordered_map<std::string, uintptr_t> libraries;
  char maps_path[256];
  snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);

  FILE *fp = fopen(maps_path, "r");
  if (!fp)
  {
    LOG_ERROR("Failed to open %s", maps_path);
    return libraries;
  }

  char line[512];
  while (fgets(line, sizeof(line), fp))
  {
    uintptr_t start_addr;
    char path[256] = {0};

    /* Parse: start_addr-end_addr perms offset dev inode [path] */
    if (sscanf(line, "%lx-%*x %*s %*s %*s %*s %255s", &start_addr, path) >= 1)
    {
      /* Skip if no path or path is special */
      if (path[0] == '\0' || path[0] == '[')
        continue;

      /* Skip if already seen (we want the first mapping which is the base) */
      if (libraries.find(path) != libraries.end())
        continue;

      /* For executables and shared libraries, the first mapping is the base */
      libraries[path] = start_addr;
    }
  }

  fclose(fp);
  return libraries;
}

/* Load symbols from a single ELF file (executable or library)
 * base_addr: runtime load address (0 for non-PIE executables) */
static void load_symbols_from_file(const char *path, uintptr_t base_addr)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return;

  Elf *e = elf_begin(fd, ELF_C_READ, nullptr);
  if (!e)
  {
    close(fd);
    return;
  }

  /* Check if this is a PIE or shared library (needs base address adjustment) */
  GElf_Ehdr ehdr;
  bool needs_relocation = false;
  if (gelf_getehdr(e, &ehdr) == &ehdr)
  {
    /* ET_DYN = shared library or PIE executable - needs base address */
    /* ET_EXEC = non-PIE executable - use addresses as-is */
    needs_relocation = (ehdr.e_type == ET_DYN);
  }

  /* If base_addr is 0 but needs relocation, we can't resolve symbols */
  if (needs_relocation && base_addr == 0)
  {
    elf_end(e);
    close(fd);
    return;
  }

  size_t shstrndx;
  if (elf_getshdrstrndx(e, &shstrndx) != 0)
  {
    elf_end(e);
    close(fd);
    return;
  }

  /* Create library entry */
  library_symbols lib_syms;
  lib_syms.path = path;
  lib_syms.base_addr = base_addr;

  /* Iterate through all sections to find symbol tables */
  Elf_Scn *scn = nullptr;
  while ((scn = elf_nextscn(e, scn)) != nullptr)
  {
    GElf_Shdr shdr;
    if (gelf_getshdr(scn, &shdr) != &shdr)
      continue;

    /* Look for .symtab or .dynsym sections */
    if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
      continue;

    /* Get the string table for this symbol table */
    Elf_Scn *str_scn = elf_getscn(e, shdr.sh_link);
    if (!str_scn)
      continue;

    GElf_Shdr str_shdr;
    if (gelf_getshdr(str_scn, &str_shdr) != &str_shdr)
      continue;

    Elf_Data *str_data = elf_getdata(str_scn, nullptr);
    if (!str_data || !str_data->d_buf)
      continue;

    /* Get symbol table data */
    Elf_Data *sym_data = elf_getdata(scn, nullptr);
    if (!sym_data)
      continue;

    size_t num_syms = shdr.sh_size / shdr.sh_entsize;

    for (size_t i = 0; i < num_syms; i++)
    {
      GElf_Sym sym;
      if (gelf_getsym(sym_data, i, &sym) != &sym)
        continue;

      /* Only interested in symbols with names */
      if (sym.st_name == 0)
        continue;

      /* Get binding info */
      int bind = GELF_ST_BIND(sym.st_info);

      /* Skip local symbols (reduces cache size) - keep global and weak */
      if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_LOCAL)
        continue;

      /* Get symbol name */
      const char *sym_name = (const char *)str_data->d_buf + sym.st_name;
      if (!sym_name || !*sym_name)
        continue;

      /* Only cache object and function symbols with valid addresses */
      int type = GELF_ST_TYPE(sym.st_info);
      if (type != STT_OBJECT && type != STT_FUNC)
        continue;

      /* Skip undefined symbols (imported from other libraries) */
      if (sym.st_shndx == SHN_UNDEF)
        continue;

      /* Skip local symbols that start with '_' (internal) unless whitelisted */
      if (bind == STB_LOCAL && sym_name[0] == '_')
        continue;

      elf_symbol esym;
      esym.name = sym_name;
      /* Calculate runtime address: base + offset */
      esym.address = base_addr + (uintptr_t)sym.st_value;
      esym.size = (size_t)sym.st_size;
      esym.type = type;
      esym.bind = bind;
      esym.lib_path = path;

      lib_syms.symbols[sym_name] = esym;
    }
  }

  elf_end(e);
  close(fd);

  /* Store in global cache */
  if (!lib_syms.symbols.empty())
  {
    elf_library_cache[path] = std::move(lib_syms);
    LOG_INFO("ELF: Loaded %zu symbols from %s (base=0x%lx)",
             elf_library_cache[path].symbols.size(), path, base_addr);
  }
}

/* Load symbols from main executable */
static void load_elf_symbols(const char *executable)
{
  if (elf_version(EV_CURRENT) == EV_NONE)
  {
    LOG_ERROR("ELF library initialization failed");
    return;
  }

  /* Clear existing cache */
  elf_library_cache.clear();
  current_elf_path = executable;

  /* Load main executable with base=0 (we'll fix this when we know runtime base)
   */
  load_symbols_from_file(executable, 0);
}

/* Load symbols from all shared libraries mapped in process */
static void load_shared_library_symbols(int pid)
{
  auto libraries = parse_proc_maps(pid);

  for (const auto &[path, base_addr] : libraries)
  {
    /* Skip main executable (already loaded) */
    if (path == current_elf_path)
    {
      /* Update base address for main executable if it's PIE */
      auto it = elf_library_cache.find(path);
      if (it != elf_library_cache.end())
      {
        /* Check if we need to update addresses */
        if (it->second.base_addr == 0 && base_addr != 0)
        {
          /* This is a PIE executable, update symbol addresses */
          for (auto &sym_pair : it->second.symbols)
          {
            sym_pair.second.address += base_addr;
          }
          it->second.base_addr = base_addr;
          LOG_INFO("ELF: Updated base address for %s to 0x%lx", path.c_str(),
                   base_addr);
        }
      }
      continue;
    }

    /* Skip if already loaded */
    if (elf_library_cache.find(path) != elf_library_cache.end())
      continue;

    /* Load symbols from this library */
    load_symbols_from_file(path.c_str(), base_addr);
  }
}

/* Lookup a symbol in the ELF symbol cache (all libraries)
 * Returns 0 if not found */
static uintptr_t lookup_elf_symbol(const char *symname)
{
  if (!symname || !*symname)
    return 0;

  for (const auto &[path, lib] : elf_library_cache)
  {
    auto it = lib.symbols.find(symname);
    if (it != lib.symbols.end())
    {
      LOG_DEBUG("ELF: Found symbol %s at 0x%lx in %s", symname,
                it->second.address, path.c_str());
      return it->second.address;
    }
  }

  return 0;
}

/* Get ELF symbol type info
 * Returns "func" for functions, "void*" for data objects, "" if not found */
static std::string get_elf_symbol_type(const char *symname)
{
  if (!symname || !*symname)
    return "";

  for (const auto &[path, lib] : elf_library_cache)
  {
    auto it = lib.symbols.find(symname);
    if (it != lib.symbols.end())
    {
      if (it->second.type == STT_FUNC)
        return "func";
      else if (it->second.type == STT_OBJECT)
        return "void*";
    }
  }

  return "";
}

/* ======================================================================
 * Section 6: Variable Lookup API
 * ====================================================================== */

uintptr_t dwarf_get_global_addr(const char *varname)
{
  /* First try DWARF debug info */
  auto it = global_vars.find(varname);
  if (it != global_vars.end())
  {
    return it->second.address;
  }

  /* Fallback to ELF symbol table */
  uintptr_t addr = lookup_elf_symbol(varname);
  if (addr != 0)
    return addr;

  /* Try loading shared library symbols if not found
   * This is a lazy loading approach - we load shared library symbols
   * only when needed and cache them */
  return 0;
}

/* Load shared library symbols for a specific PID
 * Should be called when process is stopped */
void dwarf_load_shared_library_symbols(int pid)
{
  if (elf_symbols_loaded_for_pid == pid)
    return; /* Already loaded for this PID */

  LOG_INFO("ELF: Loading shared library symbols for PID %d", pid);
  load_shared_library_symbols(pid);
  elf_symbols_loaded_for_pid = pid;
}

std::string dwarf_get_global_type(const char *varname)
{
  /* First try DWARF debug info */
  auto it = global_vars.find(varname);
  if (it != global_vars.end())
  {
    return it->second.type_name;
  }

  /* Fallback to ELF symbol type */
  return get_elf_symbol_type(varname);
}

/* Parse a type name to extract base type and modifiers (array, pointer) */
static void parse_type_name(const std::string &type_name,
                            std::string &base_type, bool &is_array,
                            bool &is_pointer, std::string &array_size)
{
  base_type = type_name;
  is_array = false;
  is_pointer = false;
  array_size = "";

  /* Check for array notation: type[N] or type[] */
  size_t bracket_pos = base_type.find('[');
  if (bracket_pos != std::string::npos)
  {
    is_array = true;
    size_t close_bracket = base_type.find(']', bracket_pos);
    if (close_bracket != std::string::npos && close_bracket > bracket_pos + 1)
    {
      array_size =
          base_type.substr(bracket_pos + 1, close_bracket - bracket_pos - 1);
    }
    base_type = base_type.substr(0, bracket_pos);
    /* Trim whitespace */
    while (!base_type.empty() && isspace(base_type.back()))
      base_type.pop_back();
    return;
  }

  /* Check for pointer: type * or type* */
  size_t star_pos = base_type.find('*');
  if (star_pos != std::string::npos)
  {
    is_pointer = true;
    /* Extract just the first base type before any * */
    std::string first_base = base_type.substr(0, star_pos);
    while (!first_base.empty() && isspace(first_base.back()))
      first_base.pop_back();
    base_type = first_base;
    return;
  }
}

/* Get element type after one level of array/pointer indexing
 * e.g., "rect * *" -> "rect *", "point[]" -> "point", "int*" -> "int"
 */
std::string dwarf_get_element_type(const char *type_name)
{
  if (!type_name || !*type_name)
    return "";

  std::string name_str(type_name);

  /* Handle array: type[] -> type */
  size_t bracket_pos = name_str.find('[');
  if (bracket_pos != std::string::npos)
  {
    std::string elem_type = name_str.substr(0, bracket_pos);
    while (!elem_type.empty() && isspace(elem_type.back()))
      elem_type.pop_back();
    return elem_type;
  }

  /* Handle pointer: remove one level of pointer
   * e.g., "rect * *" -> "rect *", "int *" -> "int"
   */
  size_t last_star = name_str.rfind('*');
  if (last_star != std::string::npos)
  {
    std::string elem_type = name_str.substr(0, last_star);
    /* Trim trailing whitespace */
    while (!elem_type.empty() && isspace(elem_type.back()))
      elem_type.pop_back();
    return elem_type;
  }

  return "";
}

/* Static empty type_info for "not found" cases */
static type_info empty_type_info;

const type_info &dwarf_get_type_info(const char *type_name)
{
  if (!type_name || !*type_name)
  {
    return empty_type_info;
  }

  std::string name_str(type_name);
  std::string base_type;
  bool is_array = false, is_pointer = false;
  std::string array_size;

  parse_type_name(name_str, base_type, is_array, is_pointer, array_size);

  /* Handle array type name like "point[]" - construct a synthetic type_info
   * Use thread_local to store result safely for returning by reference
   */
  if (is_array)
  {
    const type_info &elem_info = dwarf_get_type_info(base_type.c_str());
    thread_local type_info array_info;
    array_info = type_info();
    array_info.name = type_name;
    array_info.is_array = true;
    array_info.element_type = base_type;
    array_info.size =
        elem_info.size * (array_size.empty() ? 10 : std::stoi(array_size));
    array_info.array_elements = array_size.empty() ? 10 : std::stoi(array_size);
    return array_info;
  }

  /* Handle pointer type - construct a synthetic type_info and cache */
  if (is_pointer)
  {
    auto it = type_cache.find(type_name);
    if (it != type_cache.end())
    {
      return it->second;
    }
    type_info ptr_info;
    ptr_info.name = type_name;
    ptr_info.is_pointer = true;
    ptr_info.element_type = base_type;
    ptr_info.size = 8;
    ptr_info.is_struct = false;
    ptr_info.is_array = false;
    type_cache[type_name] = std::move(ptr_info);
    return type_cache.find(type_name)->second;
  }

  /* First try the original type name */
  auto it = type_cache.find(type_name);
  if (it != type_cache.end())
  {
    return it->second;
  }

  /* Try base type name */
  it = type_cache.find(base_type);
  if (it != type_cache.end())
  {
    return it->second;
  }

  /* Try to find and cache base type */
  if (dbg)
  {
    cache_type_by_name(dbg, base_type.c_str());
    it = type_cache.find(base_type);
    if (it != type_cache.end())
    {
      return it->second;
    }
  }

  /* Last resort: try the original name directly */
  if (dbg && strcmp(type_name, base_type.c_str()) != 0)
  {
    cache_type_by_name(dbg, type_name);
    it = type_cache.find(type_name);
    if (it != type_cache.end())
    {
      return it->second;
    }
  }

  return empty_type_info;
}

/* ======================================================================
 * Section 7: Type Operations
 * ====================================================================== */

size_t dwarf_type_size(const char *type_name)
{
  std::string key(type_name);
  auto it = type_size_cache.find(key);
  if (it != type_size_cache.end())
  {
    return it->second;
  }

  type_info info = dwarf_get_type_info(type_name);
  type_size_cache[key] = info.size;
  return info.size;
}

ptrdiff_t dwarf_member_offset(const char *struct_type, const char *member)
{
  std::string struct_key(struct_type);
  std::string member_key(member);

  auto struct_it = member_offset_cache.find(struct_key);
  if (struct_it != member_offset_cache.end())
  {
    auto member_it = struct_it->second.find(member_key);
    if (member_it != struct_it->second.end())
    {
      return member_it->second;
    }
  }

  type_info info = dwarf_get_type_info(struct_type);
  if (!info.is_struct && info.members.empty())
  {
    /* Try to find through typedef */
    auto var_it = global_vars.find(struct_type);
    if (var_it != global_vars.end())
    {
      info = dwarf_get_type_info(var_it->second.type_name.c_str());
    }
  }

  for (const auto &m : info.members)
  {
    if (m.name == member)
    {
      member_offset_cache[struct_key][member_key] = m.offset;
      return m.offset;
    }
  }
  return -1;
}

bool dwarf_member_info(const char *struct_type, const char *member,
                       ptrdiff_t *offset, size_t *size, std::string *type_name)
{
  type_info info = dwarf_get_type_info(struct_type);
  if (!info.is_struct && info.members.empty())
  {
    /* Try to find through typedef */
    auto var_it = global_vars.find(struct_type);
    if (var_it != global_vars.end())
    {
      info = dwarf_get_type_info(var_it->second.type_name.c_str());
    }
  }

  for (const auto &m : info.members)
  {
    if (m.name == member)
    {
      if (offset)
        *offset = m.offset;
      if (size)
        *size = m.size;
      if (type_name)
        *type_name = m.type_name;
      return true;
    }
  }
  return false;
}

void dwarf_dump_type(const char *type_name)
{
  type_info info = dwarf_get_type_info(type_name);

  if (info.name.empty())
  {
    detsim::ui::ui_printf("Type '%s' not found\n", type_name);
    return;
  }

  detsim::ui::ui_printf("Type: %s\n", info.name.c_str());
  detsim::ui::ui_printf("  Size: %zu bytes\n", info.size);
  detsim::ui::ui_printf("  Is pointer: %s\n", info.is_pointer ? "yes" : "no");
  detsim::ui::ui_printf("  Is struct: %s\n", info.is_struct ? "yes" : "no");
  detsim::ui::ui_printf("  Is array: %s\n", info.is_array ? "yes" : "no");

  if (info.is_array)
  {
    detsim::ui::ui_printf("  Array elements: %zu\n", info.array_elements);
    detsim::ui::ui_printf("  Element type: %s\n", info.element_type.c_str());
  }

  if (!info.members.empty())
  {
    detsim::ui::ui_printf("  Members:\n");
    for (const auto &m : info.members)
    {
      detsim::ui::ui_printf("    %s: %s (offset=%zu, size=%zu)\n",
                            m.name.c_str(), m.type_name.c_str(), m.offset,
                            m.size);
    }
  }
}

/* ======================================================================
 * Section 8: Expression Parser and Evaluator
 * ====================================================================== */

/* Token types for expression parsing */
enum expr_token_type
{
  TOK_EOF = 0,
  TOK_NUMBER,
  TOK_IDENTIFIER,
  TOK_DOT,       /* . */
  TOK_ARROW,     /* -> */
  TOK_LBRACKET,  /* [ */
  TOK_RBRACKET,  /* ] */
  TOK_STAR,      /* * */
  TOK_AMPERSAND, /* & */
  TOK_LPAREN,    /* ( */
  TOK_RPAREN,    /* ) */
};

struct expr_token
{
  expr_token_type type;
  std::string str;
  long num_val;
};

/* Simple lexer */
static std::vector<expr_token> tokenize_expr(const char *expr)
{
  std::vector<expr_token> tokens;
  const char *p = expr;

  while (*p)
  {
    /* Skip whitespace */
    while (*p && isspace(*p))
      p++;
    if (!*p)
      break;

    expr_token tok;

    /* Number */
    if (isdigit(*p))
    {
      tok.type = TOK_NUMBER;
      tok.num_val = strtol(p, (char **)&p, 0);
      tokens.push_back(tok);
      continue;
    }

    /* Identifier */
    if (isalpha(*p) || *p == '_')
    {
      tok.type = TOK_IDENTIFIER;
      const char *start = p;
      while (isalnum(*p) || *p == '_')
        p++;
      tok.str = std::string(start, p - start);
      tokens.push_back(tok);
      continue;
    }

    /* Operators */
    switch (*p)
    {
      case '.':
        tok.type = TOK_DOT;
        tokens.push_back(tok);
        p++;
        break;
      case '-':
        if (*(p + 1) == '>')
        {
          tok.type = TOK_ARROW;
          tokens.push_back(tok);
          p += 2;
        }
        else
        {
          /* Not handled in this simple parser */
          p++;
        }
        break;
      case '[':
        tok.type = TOK_LBRACKET;
        tokens.push_back(tok);
        p++;
        break;
      case ']':
        tok.type = TOK_RBRACKET;
        tokens.push_back(tok);
        p++;
        break;
      case '*':
        tok.type = TOK_STAR;
        tokens.push_back(tok);
        p++;
        break;
      case '&':
        tok.type = TOK_AMPERSAND;
        tokens.push_back(tok);
        p++;
        break;
      case '(':
        tok.type = TOK_LPAREN;
        tokens.push_back(tok);
        p++;
        break;
      case ')':
        tok.type = TOK_RPAREN;
        tokens.push_back(tok);
        p++;
        break;
      default:
        /* Skip unknown characters */
        p++;
        break;
    }
  }

  /* EOF token */
  expr_token eof;
  eof.type = TOK_EOF;
  tokens.push_back(eof);

  return tokens;
}

/* Forward declarations */
static uintptr_t eval_expr_recursive(const std::vector<expr_token> &tokens,
                                     size_t &pos, int pid, bool &success);

/* Evaluate a primary expression (identifier, number, parenthesized expr) */
static uintptr_t eval_primary(const std::vector<expr_token> &tokens,
                              size_t &pos, int pid, bool &success)
{
  if (pos >= tokens.size() || tokens[pos].type == TOK_EOF)
  {
    success = false;
    return 0;
  }

  const expr_token &tok = tokens[pos];

  switch (tok.type)
  {
    case TOK_NUMBER:
      pos++;
      return tok.num_val;

    case TOK_IDENTIFIER:
    {
      /* Variable lookup */
      auto it = global_vars.find(tok.str);
      if (it == global_vars.end())
      {
        LOG_ERROR("Unknown variable: %s", tok.str.c_str());
        success = false;
        return 0;
      }
      pos++;
      return it->second.address;
    }

    case TOK_STAR:
    {
      /* Dereference */
      pos++;
      uintptr_t addr = eval_primary(tokens, pos, pid, success);
      if (!success)
        return 0;

      /* Read pointer value from tracee memory */
      uintptr_t value = 0;
      memcpy_guest2host(&value, (void *)addr, sizeof(uintptr_t));
      return value;
    }

    case TOK_AMPERSAND:
    {
      /* Address-of */
      pos++;
      if (tokens[pos].type != TOK_IDENTIFIER)
      {
        success = false;
        return 0;
      }
      auto it = global_vars.find(tokens[pos].str);
      if (it == global_vars.end())
      {
        success = false;
        return 0;
      }
      pos++;
      return it->second.address; /* Address is already the value */
    }

    case TOK_LPAREN:
    {
      pos++;
      uintptr_t val = eval_expr_recursive(tokens, pos, pid, success);
      if (pos < tokens.size() && tokens[pos].type == TOK_RPAREN)
      {
        pos++;
      }
      return val;
    }

    default:
      success = false;
      return 0;
  }
}

/* Evaluate postfix expressions (member access, array indexing) */
static uintptr_t eval_postfix(const std::vector<expr_token> &tokens,
                              size_t &pos, int pid, bool &success,
                              type_info *out_type = nullptr)
{
  /* Get base address */
  uintptr_t base_addr = eval_primary(tokens, pos, pid, success);
  if (!success)
    return 0;

  /* Track type for member resolution */
  type_info current_type;
  if (pos > 0 && tokens[pos - 1].type == TOK_IDENTIFIER)
  {
    std::string var_name = tokens[pos - 1].str;
    auto it = global_vars.find(var_name);
    if (it != global_vars.end())
    {
      current_type = dwarf_get_type_info(it->second.type_name.c_str());
    }
  }

  /* Process postfix operators */
  while (pos < tokens.size())
  {
    const expr_token &tok = tokens[pos];

    if (tok.type == TOK_DOT)
    {
      /* Member access: struct.member */
      pos++;
      if (pos >= tokens.size() || tokens[pos].type != TOK_IDENTIFIER)
      {
        success = false;
        return 0;
      }
      std::string member_name = tokens[pos].str;
      pos++;

      /* Check if we have valid type info with members */
      if (current_type.members.empty())
      {
        LOG_ERROR("Member '%s' accessed on non-struct type",
                  member_name.c_str());
        success = false;
        return 0;
      }

      /* Find member offset */
      ptrdiff_t offset = -1;
      for (const auto &m : current_type.members)
      {
        if (m.name == member_name)
        {
          offset = m.offset;
          /* Update current type to member type */
          current_type = dwarf_get_type_info(m.type_name.c_str());
          break;
        }
      }

      if (offset < 0)
      {
        LOG_ERROR("Member '%s' not found in type '%s'", member_name.c_str(),
                  current_type.name.c_str());
        success = false;
        return 0;
      }

      base_addr += offset;
    }
    else if (tok.type == TOK_ARROW)
    {
      /* Pointer access: ptr->member */
      pos++;
      if (pos >= tokens.size() || tokens[pos].type != TOK_IDENTIFIER)
      {
        success = false;
        return 0;
      }
      std::string member_name = tokens[pos].str;
      pos++;

      /* Dereference pointer first */
      uintptr_t ptr_val = 0;
      memcpy_guest2host(&ptr_val, (void *)base_addr, sizeof(uintptr_t));
      base_addr = ptr_val;

      /* Get pointed-to type */
      if (current_type.is_pointer ||
          current_type.name.find("*") != std::string::npos)
      {
        /* Strip pointer to get struct type */
        std::string base_type = current_type.name;
        size_t star_pos = base_type.find("*");
        if (star_pos != std::string::npos)
        {
          base_type = base_type.substr(0, star_pos);
          /* Trim whitespace */
          while (!base_type.empty() && isspace(base_type.back()))
            base_type.pop_back();
          while (!base_type.empty() && isspace(base_type.front()))
            base_type.erase(0, 1);
        }
        current_type = dwarf_get_type_info(base_type.c_str());
      }

      /* Find member offset */
      ptrdiff_t offset = -1;
      for (const auto &m : current_type.members)
      {
        if (m.name == member_name)
        {
          offset = m.offset;
          current_type = dwarf_get_type_info(m.type_name.c_str());
          break;
        }
      }

      if (offset < 0)
      {
        LOG_ERROR("Member '%s' not found", member_name.c_str());
        success = false;
        return 0;
      }

      base_addr += offset;
    }
    else if (tok.type == TOK_LBRACKET)
    {
      /* Array indexing: arr[index] */
      pos++;
      uintptr_t index = eval_expr_recursive(tokens, pos, pid, success);
      if (!success)
        return 0;

      if (pos >= tokens.size() || tokens[pos].type != TOK_RBRACKET)
      {
        success = false;
        return 0;
      }
      pos++; /* consume ] */

      /* Calculate element size and new address */
      size_t elem_size = current_type.size;
      if (current_type.is_array && !current_type.element_type.empty())
      {
        type_info elem_type =
            dwarf_get_type_info(current_type.element_type.c_str());
        elem_size = elem_type.size;
        current_type = elem_type;
      }

      base_addr += index * elem_size;
    }
    else
    {
      break;
    }
  }

  if (out_type)
  {
    *out_type = current_type;
  }

  return base_addr;
}

/* Main recursive evaluator */
static uintptr_t eval_expr_recursive(const std::vector<expr_token> &tokens,
                                     size_t &pos, int pid, bool &success)
{
  return eval_postfix(tokens, pos, pid, success);
}

/* ======================================================================
 * Section 9: Public API Implementation
 * ====================================================================== */

uintptr_t dwarf_eval_expr(int pid, const char *expr)
{
  if (!expr || !*expr)
  {
    LOG_ERROR("Empty expression");
    return 0;
  }

  bool success = true;
  std::vector<expr_token> tokens = tokenize_expr(expr);
  size_t pos = 0;

  uintptr_t addr = eval_expr_recursive(tokens, pos, pid, success);

  if (!success)
  {
    LOG_ERROR("Failed to evaluate expression: %s", expr);
    return 0;
  }

  return addr;
}

bool dwarf_read_member(int pid, const char *base_var,
                       const std::vector<std::string> &members, void *out_buf,
                       size_t out_size)
{
  /* Build expression from components */
  std::string expr = base_var;
  for (const auto &m : members)
  {
    expr += "." + m;
  }

  uintptr_t addr = dwarf_eval_expr(pid, expr.c_str());
  if (addr == 0)
  {
    return false;
  }

  /* Read value from tracee memory */
  memcpy_guest2host(out_buf, (void *)addr, out_size);
  return true;
}

/* ======================================================================
 * Stack Trace Implementation
 * ====================================================================== */

/* Get register values for a frame using libunwind */
static bool get_frame_registers(unw_cursor_t *cursor, uintptr_t *pc,
                                uintptr_t *sp, uintptr_t *bp)
{
  unw_word_t wpc, wsp, wbp;

  if (unw_get_reg(cursor, UNW_REG_IP, &wpc) < 0)
    return false;
  if (unw_get_reg(cursor, UNW_REG_SP, &wsp) < 0)
    return false;

  /* Try to get BP (RBP on x86_64) */
  unw_word_t wbp_temp;
  if (unw_get_reg(cursor, UNW_X86_64_RBP, &wbp_temp) < 0)
  {
    wbp = 0;
  }
  else
  {
    wbp = wbp_temp;
  }

  *pc = (uintptr_t)wpc;
  *sp = (uintptr_t)wsp;
  *bp = (uintptr_t)wbp;
  return true;
}

/* Parse function parameters and local variables from DWARF DIE */
static void parse_function_params(Dwarf_Debug dbg, Dwarf_Die func_die,
                                  func_info &info)
{
  Dwarf_Error err;
  Dwarf_Die child_die;

  if (dwarf_child(func_die, &child_die, &err) != DW_DLV_OK)
    return;

  do
  {
    Dwarf_Half tag;
    if (dwarf_tag(child_die, &tag, &err) != DW_DLV_OK)
      continue;

    if (tag == DW_TAG_formal_parameter || tag == DW_TAG_variable)
    {
      param_info p;

      /* Get name */
      char *name = nullptr;
      if (dwarf_diename(child_die, &name, &err) == DW_DLV_OK && name)
      {
        p.name = name;
      }

      /* Get type */
      Dwarf_Attribute type_attr;
      if (dwarf_attr(child_die, DW_AT_type, &type_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Off type_offset;
        if (dwarf_global_formref(type_attr, &type_offset, &err) == DW_DLV_OK)
        {
          Dwarf_Die type_die;
          if (dwarf_offdie(dbg, type_offset, &type_die, &err) == DW_DLV_OK)
          {
            type_info tinfo;
            parse_type_info(dbg, type_die, tinfo);
            p.type_name = tinfo.name;
          }
        }
      }

      /* Get location (register or stack offset) */
      Dwarf_Attribute loc_attr;
      if (dwarf_attr(child_die, DW_AT_location, &loc_attr, &err) == DW_DLV_OK)
      {
        Dwarf_Unsigned expr_len;
        Dwarf_Ptr expr_bytes;

        if (dwarf_formexprloc(loc_attr, &expr_len, &expr_bytes, &err) ==
            DW_DLV_OK)
        {
          const unsigned char *data = (const unsigned char *)expr_bytes;

          if (expr_len > 0)
          {
            /* DW_OP_regX means parameter is in register X */
            /* DW_OP_fbreg means offset from frame base */
            if (data[0] >= DW_OP_reg0 && data[0] <= DW_OP_reg31)
            {
              p.reg_num = data[0] - DW_OP_reg0;
              p.in_reg = true;
            }
            else if (data[0] == DW_OP_fbreg && expr_len > 1)
            {
              /* SLEB128 encoded offset */
              p.stack_offset = (ptrdiff_t)data[1];
              p.in_reg = false;
            }
          }
        }
      }

      if (tag == DW_TAG_formal_parameter)
      {
        info.params.push_back(p);
      }
      else
      {
        /* Local variable - add to both params and local_vars for now */
        var_info v;
        v.name = p.name;
        v.type_name = p.type_name;
        v.is_global = false;
        v.is_local = true;
        v.stack_offset = p.stack_offset;
        info.local_vars.push_back(v);
      }
    }
  } while (dwarf_siblingof(dbg, child_die, &child_die, &err) == DW_DLV_OK);
}

/* Find function info by PC */
static bool find_function_by_pc(uintptr_t pc, func_info &out_info)
{
  if (!dbg)
    return false;

  Dwarf_Error err;
  Dwarf_Unsigned cu_header_length, abbrev_offset, next_cu_header;
  Dwarf_Half version_stamp, address_size;

  /* Reset CU iteration */
  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                              &abbrev_offset, &address_size, &next_cu_header,
                              &err) == DW_DLV_OK)
  {
  }

  /* Search all CUs */
  while (dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                              &abbrev_offset, &address_size, &next_cu_header,
                              &err) == DW_DLV_OK)
  {
    Dwarf_Die no_die = 0;
    Dwarf_Die cu_die;

    if (dwarf_siblingof(dbg, no_die, &cu_die, &err) != DW_DLV_OK)
      continue;

    /* Search for subprogram DIEs */
    std::stack<Dwarf_Die> die_stack;
    Dwarf_Die child_die;
    if (dwarf_child(cu_die, &child_die, &err) == DW_DLV_OK)
    {
      die_stack.push(child_die);
    }

    while (!die_stack.empty())
    {
      Dwarf_Die current_die = die_stack.top();
      die_stack.pop();

      Dwarf_Half tag;
      if (dwarf_tag(current_die, &tag, &err) != DW_DLV_OK)
        continue;

      if (tag == DW_TAG_subprogram)
      {
        /* Check if PC is in this function's range */
        Dwarf_Attribute low_attr, high_attr;
        uintptr_t low_pc_val = 0, high_pc_val = 0;

        if (dwarf_attr(current_die, DW_AT_low_pc, &low_attr, &err) == DW_DLV_OK)
        {
          Dwarf_Addr addr;
          if (dwarf_formaddr(low_attr, &addr, &err) == DW_DLV_OK)
          {
            low_pc_val = (uintptr_t)addr;
          }
        }

        if (dwarf_attr(current_die, DW_AT_high_pc, &high_attr, &err) ==
            DW_DLV_OK)
        {
          Dwarf_Unsigned offset;
          if (dwarf_formudata(high_attr, &offset, &err) == DW_DLV_OK)
          {
            high_pc_val = low_pc_val + offset;
          }
        }

        if (pc >= low_pc_val && pc < high_pc_val)
        {
          /* Found the function */
          char *name = nullptr;
          if (dwarf_diename(current_die, &name, &err) == DW_DLV_OK && name)
          {
            out_info.name = name;
          }
          out_info.low_pc = low_pc_val;
          out_info.high_pc = high_pc_val;
          parse_function_params(dbg, current_die, out_info);
          return true;
        }
      }

      /* Add sibling and child to stack */
      Dwarf_Die sibling_die;
      if (dwarf_siblingof(dbg, current_die, &sibling_die, &err) == DW_DLV_OK)
      {
        die_stack.push(sibling_die);
      }

      Dwarf_Die next_child;
      if (dwarf_child(current_die, &next_child, &err) == DW_DLV_OK)
      {
        die_stack.push(next_child);
      }
    }
  }

  return false;
}

/* Print parameter value based on type */
static void print_param_value(int pid, const param_info &param,
                              unw_cursor_t *cursor)
{
  long value = 0;

  if (param.in_reg)
  {
    /* Read from register */
    unw_word_t reg_val;
    /* Map DWARF reg num to libunwind reg num */
    int unw_reg = UNW_X86_64_RAX + param.reg_num; /* Approximate mapping */
    if (unw_get_reg(cursor, unw_reg, &reg_val) == 0)
    {
      value = (long)reg_val;
    }
  }

  /* Format based on type */
  if (param.type_name == "int" || param.type_name == "long")
  {
    detsim::ui::ui_printf("%ld", value);
  }
  else if (param.type_name.find('*') != std::string::npos)
  {
    detsim::ui::ui_printf("0x%lx", (unsigned long)value);
  }
  else if (param.type_name == "char")
  {
    detsim::ui::ui_printf("'%c'", (char)value);
  }
  else
  {
    detsim::ui::ui_printf("%ld", value); /* Default to integer */
  }
}

/* Implementation of stack trace with parameters */
std::vector<stack_frame> dwarf_get_stack_trace(int pid)
{
  std::vector<stack_frame> frames;

  unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
  void *ui = _UPT_create(pid);
  if (!ui)
  {
    LOG_ERROR("_UPT_create failed for pid %d", pid);
    return frames;
  }

  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, as, ui) < 0)
  {
    LOG_ERROR("unw_init_remote failed");
    _UPT_destroy(ui);
    return frames;
  }

  int frame_id = 0;
  int consecutive_errors = 0;
  do
  {
    stack_frame frame;
    frame.frame_id = frame_id;

    /* Get PC, SP, and BP */
    unw_word_t pc, sp, bp;
    if (unw_get_reg(&cursor, UNW_REG_IP, &pc) != 0)
      pc = 0;
    if (unw_get_reg(&cursor, UNW_REG_SP, &sp) != 0)
      sp = 0;
    if (unw_get_reg(&cursor, UNW_X86_64_RBP, &bp) != 0)
      bp = 0;

    frame.pc = (uintptr_t)pc;
    frame.sp = (uintptr_t)sp;
    frame.bp = (uintptr_t)bp;

    /* Get function name */
    char func_name[256];
    if (unw_get_proc_name(&cursor, func_name, sizeof(func_name), NULL) == 0)
    {
      frame.function_name = func_name;
    }
    else
    {
      frame.function_name = "??";
    }

    /* Try to get function info and parameters from DWARF */
    func_info finfo;
    if (find_function_by_pc(frame.pc, finfo))
    {
      if (frame.function_name == "??")
      {
        frame.function_name = finfo.name;
      }

      /* Add parameters */
      for (const auto &p : finfo.params)
      {
        var_info v;
        v.name = p.name;
        v.type_name = p.type_name;
        v.is_global = false;
        v.is_local = true;
        v.stack_offset = p.stack_offset;
        frame.local_vars.push_back(v);
      }

      /* Add local variables */
      for (const auto &lv : finfo.local_vars)
      {
        frame.local_vars.push_back(lv);
      }
    }

    frames.push_back(frame);
    frame_id++;

    /* Try to step to next frame */
    int ret = unw_step(&cursor);
    if (ret < 0)
    {
      consecutive_errors++;
      if (consecutive_errors > 3)
        break; /* Stop after consecutive errors */
    }
    else if (ret == 0)
    {
      /* No more frames, but try to continue if we have BP info */
      /* Sometimes libunwind misses frames at syscall boundaries */
      if (frame.bp != 0 && frame.bp > 0x1000)
      {
        /* Try manual frame chain walking as fallback */
        /* This is a heuristic and may not always work */
      }
      break;
    }
    else
    {
      consecutive_errors = 0;
    }
  } while (frame_id < 100); /* Limit to 100 frames */

  _UPT_destroy(ui);
  unw_destroy_addr_space(as);

  return frames;
}

/* Print stack trace with parameters (for monitor.cpp) */
void dwarf_print_stack_trace(int pid)
{
  current_stack_trace = dwarf_get_stack_trace(pid);

  if (current_stack_trace.empty())
  {
    detsim::ui::ui_printf("No stack trace available\n");
    return;
  }

  unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
  void *ui = _UPT_create(pid);
  if (!ui)
  {
    detsim::ui::ui_printf("Failed to create unwind context\n");
    return;
  }

  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, as, ui) < 0)
  {
    detsim::ui::ui_printf("Failed to initialize unwind cursor\n");
    _UPT_destroy(ui);
    return;
  }

  for (size_t i = 0; i < current_stack_trace.size(); i++)
  {
    const auto &frame = current_stack_trace[i];

    detsim::ui::ui_printf("#%-2zu 0x%016lx in ", i, frame.pc);

    if (!frame.function_name.empty() && frame.function_name != "??")
    {
      detsim::ui::ui_printf("%s", frame.function_name.c_str());

      /* Print parameters if available */
      func_info finfo;
      if (find_function_by_pc(frame.pc, finfo) && !finfo.params.empty())
      {
        detsim::ui::ui_printf("(");
        for (size_t j = 0; j < finfo.params.size(); j++)
        {
          if (j > 0)
            detsim::ui::ui_printf(", ");
          const auto &p = finfo.params[j];
          detsim::ui::ui_printf("%s=", p.name.c_str());
          print_param_value(pid, p, &cursor);
        }
        detsim::ui::ui_printf(")");
      }
      else
      {
        detsim::ui::ui_printf("()");
      }
    }
    else
    {
      detsim::ui::ui_printf("?? ()");
    }

    detsim::ui::ui_printf("\n");

    /* Step cursor for next frame */
    if (i < current_stack_trace.size() - 1)
    {
      unw_step(&cursor);
    }
  }

  _UPT_destroy(ui);
  unw_destroy_addr_space(as);

  /* Show current frame */
  if (current_frame_id < (int)current_stack_trace.size())
  {
    detsim::ui::ui_printf("-- Current frame: #%d --\n", current_frame_id);
  }
}

/* Switch to a specific frame */
bool dwarf_set_frame(int frame_id)
{
  if (frame_id < 0 || frame_id >= (int)current_stack_trace.size())
  {
    return false;
  }
  current_frame_id = frame_id;
  return true;
}

/* Get current frame ID */
int dwarf_get_current_frame(void) { return current_frame_id; }

/* Get local variables for current frame */
std::vector<var_info> dwarf_get_local_vars(int pid, int frame_id)
{
  if (frame_id < 0 || frame_id >= (int)current_stack_trace.size())
  {
    return {};
  }

  const auto &frame = current_stack_trace[frame_id];

  /* Return cached local vars from stack trace */
  return frame.local_vars;
}

/* Get frame base address for local variable access */
uintptr_t dwarf_get_frame_base(int frame_id)
{
  if (frame_id < 0 || frame_id >= (int)current_stack_trace.size())
  {
    return 0;
  }
  return current_stack_trace[frame_id].bp;
}

/* Forward declaration - expr_print for variable values */
extern void expr_print(const char *e);

/* Print local variables with values - delegates to p command logic */
void dwarf_print_local_vars(int pid)
{
  if (current_frame_id < 0 ||
      current_frame_id >= (int)current_stack_trace.size())
  {
    detsim::ui::ui_printf("No frame selected\n");
    return;
  }

  const auto &frame = current_stack_trace[current_frame_id];

  if (frame.local_vars.empty())
  {
    detsim::ui::ui_printf("No local variables/parameters in frame #%d (%s)\n",
                          current_frame_id, frame.function_name.c_str());
    return;
  }

  detsim::ui::ui_printf("Local variables/parameters in frame #%d (%s):\n",
                        current_frame_id, frame.function_name.c_str());

  for (const auto &v : frame.local_vars)
  {
    detsim::ui::ui_printf("  %s = ", v.name.c_str());
    /* Use p command logic to print with proper type handling */
    expr_print(v.name.c_str());
  }
}

/* Look up local variable in current frame - returns address and type */
bool dwarf_lookup_local(const char *varname, uintptr_t *out_addr,
                        std::string *out_type)
{
  if (current_frame_id < 0 ||
      current_frame_id >= (int)current_stack_trace.size())
  {
    return false;
  }

  DwarfLocalCache::VarInfo cached;
  if (DwarfLocalCache::instance().lookup_local(current_frame_id, varname,
                                               cached))
  {
    if (out_addr)
      *out_addr = cached.address;
    if (out_type)
      *out_type = cached.type_name;
    return true;
  }

  const auto &frame = current_stack_trace[current_frame_id];

  for (const auto &v : frame.local_vars)
  {
    if (v.name == varname)
    {
      uintptr_t addr = frame.bp + v.stack_offset;
      if (out_addr)
        *out_addr = addr;
      if (out_type)
        *out_type = v.type_name;

      DwarfLocalCache::VarInfo info;
      info.address = addr;
      info.type_name = v.type_name;
      DwarfLocalCache::instance().cache_local(current_frame_id, varname, info);

      return true;
    }
  }

  return false;
}

bool dwarf_resolve_pc(uintptr_t pc, std::string &func_name,
                      std::string &file_name, int &line_num)
{
  LOG_WARN("dwarf_resolve_pc: not implemented yet");
  return false;
}

void *dwarf_lookup_die(uintptr_t pc)
{
  LOG_WARN("dwarf_lookup_die: not implemented yet");
  return nullptr;
}

/* Template instantiations */
template bool dwarf_read_var<int>(int pid, const char *varname, int *out);
template bool dwarf_read_var<long>(int pid, const char *varname, long *out);
template bool dwarf_read_var<unsigned long>(int pid, const char *varname,
                                            unsigned long *out);

template <typename T>
bool dwarf_read_var(int pid, const char *varname, T *out)
{
  uintptr_t addr = dwarf_get_global_addr(varname);
  if (addr == 0)
  {
    return false;
  }

  /* Get type size */
  std::string type_name = dwarf_get_global_type(varname);
  size_t size = dwarf_type_size(type_name.c_str());
  if (size == 0)
  {
    size = sizeof(T);
  }

  memcpy_guest2host(out, (void *)addr, size);
  return true;
}

DwarfLocalCache &DwarfLocalCache::instance()
{
  static DwarfLocalCache instance;
  return instance;
}

void DwarfLocalCache::cache_local(int frame_id, const std::string &varname,
                                  const VarInfo &info)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_frame_id_ != frame_id)
  {
    cache_.clear();
    current_frame_id_ = frame_id;
  }
  cache_[varname] = info;
}

bool DwarfLocalCache::lookup_local(int frame_id, const std::string &varname,
                                   VarInfo &out)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_frame_id_ != frame_id)
  {
    return false;
  }
  auto it = cache_.find(varname);
  if (it == cache_.end())
  {
    return false;
  }
  out = it->second;
  return true;
}

void DwarfLocalCache::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
  current_frame_id_ = -1;
}

void DwarfLocalCache::clear_frame(int frame_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_frame_id_ == frame_id)
  {
    cache_.clear();
    current_frame_id_ = -1;
  }
}
