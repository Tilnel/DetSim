/* expr_ast.cpp - AST node implementations */
#include "expr_ast.hpp"
#include "dwarf_info.h"
#include "monitor.h"
#include "state.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/ptrace.h>

// namespace removed

/* Globals */
int g_num_processes = 0;
PtmcState g_ptmc_state = {nullptr};

/* Parser globals - defined here, declared in parser */
ExprNode *g_parser_result = nullptr;
const char *g_input_string = nullptr;

/* External parser interface */
extern int yyparse(void);
extern void yy_scan_string(const char *str);
extern void yylex_destroy(void);

/* Base class */
ExprNode::~ExprNode() = default;

EvalResult ExprNode::eval_address(int pid, bool &success) const
{
  success = false;
  return EvalResult(0L);
}

/* Parser interface */
ExprNode *parse_expression(const char *input)
{
  g_parser_result = nullptr;
  yy_scan_string(input);
  int result = yyparse();
  yylex_destroy();
  if (result != 0)
  {
    return nullptr;
  }
  return g_parser_result;
}

ExprNode *parse_expression(const std::string &input)
{
  return parse_expression(input.c_str());
}

void free_parser_result()
{
  delete g_parser_result;
  g_parser_result = nullptr;
}

/* Get ts_hash for current process to read from snapshot */
static hash_type get_current_ts_hash(int pid)
{
  int index = -1;
  // Try to find index from pid
  for (int i = 0; i < NP; i++)
  {
    if (ptmc_state.pids[i] == pid)
    {
      index = i;
      break;
    }
  }
  if (index >= 0 && index < NP)
  {
    return ptmc_state.running_state.ts_hash[index];
  }
  return 0;
}

/* Memory read - optimized to use snapshot instead of ptrace */
static long read_memory_long(int pid, uint64_t addr, bool &success)
{
  // Try to read from snapshot first (much faster than ptrace)
  hash_type ts_hash = get_current_ts_hash(pid);
  if (ts_hash != 0)
  {
    void *data = read_mem(pid, ts_hash, addr, sizeof(long));
    if (data)
    {
      long result = *(long *)data;
      free(data);
      success = true;
      return result;
    }
  }

  // Fallback to ptrace if snapshot read fails
  errno = 0;
  long data = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
  if (errno != 0)
  {
    success = false;
    return 0;
  }
  success = true;
  return data;
}

static int get_type_size(const type_info &info)
{
  if (info.is_pointer)
    return 8;
  if (info.size > 0)
    return info.size;
  return 4;
}

/* NumberNode */
EvalResult NumberNode::eval(int pid, bool &success) const
{
  success = true;
  return EvalResult(value_);
}

/* TypeNode */
EvalResult TypeNode::eval(int pid, bool &success) const
{
  success = false;
  return EvalResult(0L);
}

/* VariableNode */
EvalResult VariableNode::eval(int pid, bool &success) const
{
  auto addr_res = eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();
  std::string type_name = addr_res.type_name;

  /* Special handling for ELF symbols (func and void*) - return address directly
   * For ELF symbols without DWARF type info, we don't know the actual type,
   * so we return the address. User can dereference with *var if needed.
   */
  if (type_name == "func" || type_name == "void*")
  {
    EvalResult res(static_cast<long>(addr));
    res.type_name = type_name;
    return res;
  }

  if (!type_name.empty())
  {
    type_info info = dwarf_get_type_info(type_name.c_str());

    if (info.is_struct || info.is_array)
    {
      EvalResult res(addr);
      res.type_name = type_name;
      return res;
    }

    int size = get_type_size(info);
    long val = read_memory_long(pid, addr, success);

    if (size == 1)
      val = static_cast<int8_t>(val);
    else if (size == 2)
      val = static_cast<int16_t>(val);
    else if (size == 4)
      val = static_cast<int32_t>(val);

    EvalResult res(val);
    res.type_name = type_name;
    return res;
  }

  long val = read_memory_long(pid, addr, success);
  return EvalResult(val);
}

EvalResult VariableNode::eval_address(int pid, bool &success) const
{
  /* Try DWARF/ELF main executable first */
  uintptr_t addr = dwarf_get_global_addr(name_.c_str());
  if (addr != 0)
  {
    success = true;
    EvalResult res(static_cast<uint64_t>(addr));
    res.type_name = dwarf_get_global_type(name_.c_str());
    return res;
  }

  /* Try loading shared library symbols and search again */
  dwarf_load_shared_library_symbols(pid);
  addr = dwarf_get_global_addr(name_.c_str());
  if (addr != 0)
  {
    success = true;
    EvalResult res(static_cast<uint64_t>(addr));
    res.type_name = dwarf_get_global_type(name_.c_str());
    return res;
  }

  std::string local_type;
  if (dwarf_lookup_local(name_.c_str(), &addr, &local_type))
  {
    success = true;
    EvalResult res(static_cast<uint64_t>(addr));
    res.type_name = local_type;
    return res;
  }

  success = false;
  return EvalResult(0L);
}

/* BinaryOpNode */
EvalResult BinaryOpNode::eval(int pid, bool &success) const
{
  EvalResult left = left_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  if (op_ == BinaryOp::AND)
  {
    if (!left.as_value())
      return EvalResult(0L);
    EvalResult right = right_->eval(pid, success);
    if (!success)
      return EvalResult(0L);
    return EvalResult(right.as_value() ? 1L : 0L);
  }

  if (op_ == BinaryOp::OR)
  {
    if (left.as_value())
      return EvalResult(1L);
    EvalResult right = right_->eval(pid, success);
    if (!success)
      return EvalResult(0L);
    return EvalResult(right.as_value() ? 1L : 0L);
  }

  EvalResult right = right_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  long l = left.as_value();
  long r = right.as_value();
  long result = 0;

  switch (op_)
  {
    case BinaryOp::ADD:
      result = l + r;
      break;
    case BinaryOp::SUB:
      result = l - r;
      break;
    case BinaryOp::MUL:
      result = l * r;
      break;
    case BinaryOp::DIV:
      result = r != 0 ? l / r : 0;
      break;
    case BinaryOp::MOD:
      result = r != 0 ? l % r : 0;
      break;
    case BinaryOp::LT:
      result = l < r ? 1 : 0;
      break;
    case BinaryOp::GT:
      result = l > r ? 1 : 0;
      break;
    case BinaryOp::LE:
      result = l <= r ? 1 : 0;
      break;
    case BinaryOp::GE:
      result = l >= r ? 1 : 0;
      break;
    case BinaryOp::EQ:
      result = l == r ? 1 : 0;
      break;
    case BinaryOp::NE:
      result = l != r ? 1 : 0;
      break;
    case BinaryOp::BITAND:
      result = l & r;
      break;
    case BinaryOp::BITOR:
      result = l | r;
      break;
    case BinaryOp::XOR:
      result = l ^ r;
      break;
    case BinaryOp::SHL:
      result = l << r;
      break;
    case BinaryOp::SHR:
      result = l >> r;
      break;
  }

  success = true;
  return EvalResult(result);
}

std::string BinaryOpNode::to_string() const
{
  const char *op_str = "";
  switch (op_)
  {
    case BinaryOp::ADD:
      op_str = "+";
      break;
    case BinaryOp::SUB:
      op_str = "-";
      break;
    case BinaryOp::MUL:
      op_str = "*";
      break;
    case BinaryOp::DIV:
      op_str = "/";
      break;
    case BinaryOp::MOD:
      op_str = "%";
      break;
    case BinaryOp::LT:
      op_str = "<";
      break;
    case BinaryOp::GT:
      op_str = ">";
      break;
    case BinaryOp::LE:
      op_str = "<=";
      break;
    case BinaryOp::GE:
      op_str = ">=";
      break;
    case BinaryOp::EQ:
      op_str = "==";
      break;
    case BinaryOp::NE:
      op_str = "!=";
      break;
    case BinaryOp::AND:
      op_str = "&&";
      break;
    case BinaryOp::OR:
      op_str = "||";
      break;
    case BinaryOp::BITAND:
      op_str = "&";
      break;
    case BinaryOp::BITOR:
      op_str = "|";
      break;
    case BinaryOp::XOR:
      op_str = "^";
      break;
    case BinaryOp::SHL:
      op_str = "<<";
      break;
    case BinaryOp::SHR:
      op_str = ">>";
      break;
  }
  return "(" + left_->to_string() + " " + op_str + " " + right_->to_string() +
         ")";
}

/* UnaryOpNode */
EvalResult UnaryOpNode::eval(int pid, bool &success) const
{
  EvalResult op = operand_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  long v = op.as_value();
  long result = 0;

  switch (op_)
  {
    case UnaryOp::NEG:
      result = -v;
      break;
    case UnaryOp::NOT:
      result = !v ? 1 : 0;
      break;
    case UnaryOp::BITNOT:
      result = ~v;
      break;
  }

  success = true;
  return EvalResult(result);
}

std::string UnaryOpNode::to_string() const
{
  const char *op_str = "";
  switch (op_)
  {
    case UnaryOp::NEG:
      op_str = "-";
      break;
    case UnaryOp::NOT:
      op_str = "!";
      break;
    case UnaryOp::BITNOT:
      op_str = "~";
      break;
  }
  return std::string(op_str) + operand_->to_string();
}

/* PrefixOpNode */
EvalResult PrefixOpNode::eval(int pid, bool &success) const
{
  EvalResult addr_res = operand_->eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();
  long old_val = read_memory_long(pid, addr, success);
  if (!success)
    return EvalResult(0L);

  long new_val = (op_ == PrefixOp::PRE_INC) ? old_val + 1 : old_val - 1;

  if (ptrace(PTRACE_POKEDATA, pid, addr, new_val) < 0)
  {
    success = false;
    return EvalResult(0L);
  }

  success = true;
  return EvalResult(new_val);
}

std::string PrefixOpNode::to_string() const
{
  return std::string(op_ == PrefixOp::PRE_INC ? "++" : "--") +
         operand_->to_string();
}

/* PostfixOpNode */
EvalResult PostfixOpNode::eval(int pid, bool &success) const
{
  EvalResult addr_res = operand_->eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();
  long old_val = read_memory_long(pid, addr, success);
  if (!success)
    return EvalResult(0L);

  long new_val = (op_ == PostfixOp::POST_INC) ? old_val + 1 : old_val - 1;

  if (ptrace(PTRACE_POKEDATA, pid, addr, new_val) < 0)
  {
    success = false;
    return EvalResult(0L);
  }

  success = true;
  return EvalResult(old_val);
}

std::string PostfixOpNode::to_string() const
{
  return operand_->to_string() + (op_ == PostfixOp::POST_INC ? "++" : "--");
}

/* MemberAccessNode */
EvalResult MemberAccessNode::eval(int pid, bool &success) const
{
  EvalResult addr_res = eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();
  std::string member_type = addr_res.type_name;

  if (!member_type.empty())
  {
    const type_info &member_info = dwarf_get_type_info(member_type.c_str());

    if (member_info.is_struct || member_info.is_array)
    {
      EvalResult res(addr);
      res.type_name = member_type;
      return res;
    }

    long val = read_memory_long(pid, addr, success);
    if (!success)
      return EvalResult(0L);

    size_t size = member_info.size;
    if (size == 0)
      size = 4;

    if (size == 1)
      val = static_cast<int8_t>(val);
    else if (size == 2)
      val = static_cast<int16_t>(val);
    else if (size == 4)
      val = static_cast<int32_t>(val);

    EvalResult res(val);
    res.type_name = member_type;
    return res;
  }

  long val = read_memory_long(pid, addr, success);
  if (!success)
    return EvalResult(0L);

  EvalResult res(val);
  res.type_name = member_type;
  return res;
}

EvalResult MemberAccessNode::eval_address(int pid, bool &success) const
{
  if (!object_)
  {
    success = false;
    return EvalResult(0L);
  }

  EvalResult base_res(0L);
  std::string base_type;
  uint64_t base_addr = 0;

  if (op_ == MemberOp::ARROW)
  {
    /* For -> operator: object is a pointer
     * First try eval() - for value expressions like g_container.rects[0],
     * this returns the pointer value directly.
     * For variables like g_tree_root.container, this also works if the
     * VariableNode::eval correctly reads the pointer value.
     */
    base_res = object_->eval(pid, success);
    if (!success)
      return EvalResult(0L);

    base_addr = base_res.as_address();
    base_type = base_res.type_name;

    /* The type should be a pointer, extract the element type */
    if (!base_type.empty())
    {
      type_info info = dwarf_get_type_info(base_type.c_str());
      if (info.is_pointer && !info.element_type.empty())
      {
        base_type = info.element_type;
      }
    }
  }
  else
  {
    /* For . operator: get the address of the object */
    base_res = object_->eval_address(pid, success);
    if (!success)
      return EvalResult(0L);
    base_addr = base_res.as_address();
    base_type = base_res.type_name;
  }

  if (base_type.empty())
  {
    success = false;
    return EvalResult(0L);
  }

  ptrdiff_t offset = -1;
  std::string member_type;

  if (offset_computed_ && cached_base_type_ == base_type)
  {
    offset = cached_offset_;
    member_type = cached_member_type_;
  }
  else
  {
    const type_info &base_info = dwarf_get_type_info(base_type.c_str());

    for (const auto &m : base_info.members)
    {
      if (m.name == member_)
      {
        offset = m.offset;
        member_type = m.type_name;
        break;
      }
    }

    if (offset >= 0)
    {
      cached_offset_ = offset;
      cached_member_type_ = member_type;
      cached_base_type_ = base_type;
      offset_computed_ = true;
    }
  }

  if (offset < 0)
  {
    success = false;
    return EvalResult(0L);
  }

  success = true;
  EvalResult res(base_addr + offset);
  res.type_name = member_type;
  return res;
}

std::string MemberAccessNode::to_string() const
{
  return object_->to_string() + (op_ == MemberOp::ARROW ? "->" : ".") + member_;
}

/* ArrayIndexNode */
EvalResult ArrayIndexNode::eval(int pid, bool &success) const
{
  EvalResult addr_res = eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();
  std::string elem_type = addr_res.type_name;

  if (!elem_type.empty())
  {
    type_info info = dwarf_get_type_info(elem_type.c_str());

    /* For struct or array types, return address without dereferencing */
    if (info.is_struct || info.is_array)
    {
      EvalResult res(addr);
      res.type_name = elem_type;
      return res;
    }

    /* For pointer types, read the pointer value from memory */
    if (info.is_pointer)
    {
      long val = read_memory_long(pid, addr, success);
      if (!success)
        return EvalResult(0L);
      EvalResult res(val);
      res.type_name = elem_type;
      return res;
    }

    /* For basic types, read the value from memory */
    long val = read_memory_long(pid, addr, success);
    int size = get_type_size(info);
    if (size == 1)
      val = static_cast<int8_t>(val);
    else if (size == 2)
      val = static_cast<int16_t>(val);
    else if (size == 4)
      val = static_cast<int32_t>(val);

    EvalResult res(val);
    res.type_name = elem_type;
    return res;
  }

  long val = read_memory_long(pid, addr, success);
  return EvalResult(val);
}

EvalResult ArrayIndexNode::eval_address(int pid, bool &success) const
{
  /* Evaluate the array expression to get base address */
  EvalResult base_res = array_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t base_addr = base_res.as_address();
  std::string array_type = base_res.type_name;

  /* Evaluate index */
  EvalResult idx_res = index_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  long index = idx_res.as_value();

  /* Get element type and size */
  std::string elem_type = dwarf_get_element_type(array_type.c_str());
  int elem_size = 8;
  if (!elem_type.empty())
  {
    type_info elem_info = dwarf_get_type_info(elem_type.c_str());
    elem_size = get_type_size(elem_info);
  }

  /* Calculate element address: base + index * elem_size */
  uint64_t elem_addr = base_addr + index * elem_size;

  success = true;
  EvalResult res(elem_addr);
  res.type_name = elem_type;
  return res;
}

std::string ArrayIndexNode::to_string() const
{
  return array_->to_string() + "[" + index_->to_string() + "]";
}

/* AddressOfNode */
EvalResult AddressOfNode::eval(int pid, bool &success) const
{
  EvalResult res = operand_->eval_address(pid, success);
  if (success && !res.type_name.empty())
  {
    /* Add pointer to type name */
    res.type_name = res.type_name + " *";
  }
  return res;
}

std::string AddressOfNode::to_string() const
{
  return "&" + operand_->to_string();
}

/* DereferenceNode */
EvalResult DereferenceNode::eval(int pid, bool &success) const
{
  EvalResult ptr_res = operand_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  /*
   * VariableNode::eval() for pointer types reads memory to get the pointer's
   * value. So ptr_res contains the address stored in the pointer variable. For
   * int*: ptr_res = target address (e.g., 0x404040) For int**: ptr_res =
   * address of int* variable (e.g., 0x404088)
   */
  uint64_t ptr_value = ptr_res.is_address
                           ? ptr_res.as_address()
                           : static_cast<uint64_t>(ptr_res.as_value());

  /* Get inner type by removing one level of pointer */
  std::string inner_type;
  if (!ptr_res.type_name.empty())
  {
    inner_type = dwarf_get_element_type(ptr_res.type_name.c_str());
  }

  if (!inner_type.empty())
  {
    type_info info = dwarf_get_type_info(inner_type.c_str());

    /*
     * For struct, array, or pointer types (e.g., int*), we need to read
     * memory at ptr_value to get the actual target address.
     */
    if (info.is_struct || info.is_array || info.is_pointer)
    {
      uint64_t target_addr =
          static_cast<uint64_t>(read_memory_long(pid, ptr_value, success));
      if (!success)
        return EvalResult(0L);

      EvalResult res(target_addr);
      res.type_name = inner_type;
      return res;
    }

    /*
     * For basic types (e.g., int), ptr_value IS the target address.
     * VariableNode::eval() for int* already gave us the target address.
     */
    long val = read_memory_long(pid, ptr_value, success);
    int size = get_type_size(info);
    if (size == 1)
      val = static_cast<int8_t>(val);
    else if (size == 2)
      val = static_cast<int16_t>(val);
    else if (size == 4)
      val = static_cast<int32_t>(val);

    EvalResult res(val);
    res.type_name = inner_type;
    return res;
  }

  /* No type info, assume ptr_value is the target address */
  long val = read_memory_long(pid, ptr_value, success);
  return EvalResult(val);
}

EvalResult DereferenceNode::eval_address(int pid, bool &success) const
{
  /* Evaluate operand to get the pointer value */
  EvalResult ptr_res = operand_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  /* The pointer value IS the target address (no additional read needed) */
  uint64_t target_addr = static_cast<uint64_t>(ptr_res.as_value());

  /* Get inner type by removing one level of pointer */
  std::string inner_type;
  if (!ptr_res.type_name.empty())
  {
    inner_type = dwarf_get_element_type(ptr_res.type_name.c_str());
  }

  success = true;
  EvalResult res(target_addr);
  res.type_name = inner_type;
  return res;
}

std::string DereferenceNode::to_string() const
{
  return "*" + operand_->to_string();
}

/* ConditionalNode */
EvalResult ConditionalNode::eval(int pid, bool &success) const
{
  EvalResult c = cond_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  if (c.as_value())
  {
    return then_->eval(pid, success);
  }
  else
  {
    return else_->eval(pid, success);
  }
}

std::string ConditionalNode::to_string() const
{
  return cond_->to_string() + " ? " + then_->to_string() + " : " +
         else_->to_string();
}

/* AssignNode */
EvalResult AssignNode::eval(int pid, bool &success) const
{
  EvalResult addr_res = left_->eval_address(pid, success);
  if (!success)
    return EvalResult(0L);

  uint64_t addr = addr_res.as_address();

  EvalResult val_res = right_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  long val = val_res.as_value();

  if (ptrace(PTRACE_POKEDATA, pid, addr, val) < 0)
  {
    success = false;
    return EvalResult(0L);
  }

  success = true;
  return EvalResult(val);
}

std::string AssignNode::to_string() const
{
  return left_->to_string() + " = " + right_->to_string();
}

/* CastNode */
EvalResult CastNode::eval(int pid, bool &success) const
{
  /* Get the target type name from the type node */
  std::string target_type = type_->to_string();

  /* Evaluate the operand */
  EvalResult result = operand_->eval(pid, success);
  if (!success)
    return EvalResult(0L);

  /* For pointer/struct casts, we just change the type annotation.
   * The underlying value (address) remains the same.
   * This allows subsequent member access (->) to work correctly.
   */
  result.type_name = target_type;
  return result;
}

std::string CastNode::to_string() const
{
  return "(" + type_->to_string() + ")" + operand_->to_string();
}

/* SizeofExprNode */
EvalResult SizeofExprNode::eval(int pid, bool &success) const
{
  /* Try to get type from eval_address first (for variables) */
  bool dummy;
  EvalResult op_res = operand_->eval_address(pid, dummy);

  int size = 0;
  if (!op_res.type_name.empty())
  {
    size = dwarf_type_size(op_res.type_name.c_str());
  }

  /* Fallback: try eval */
  if (size == 0)
  {
    op_res = operand_->eval(pid, dummy);
    if (!op_res.type_name.empty())
    {
      size = dwarf_type_size(op_res.type_name.c_str());
    }
  }

  /* Default to pointer size */
  if (size == 0)
    size = 8;

  success = true;
  return EvalResult(static_cast<long>(size));
}

std::string SizeofExprNode::to_string() const
{
  return "sizeof(" + operand_->to_string() + ")";
}

/* SizeofTypeNode */
EvalResult SizeofTypeNode::eval(int pid, bool &success) const
{
  int size = dwarf_type_size(type_name_.c_str());

  /* If type not found, try evaluating as expression */
  if (size == 0)
  {
    auto expr = ExprCache::instance().get(type_name_);
    if (expr)
    {
      bool dummy;
      EvalResult op_res = expr->eval_address(pid, dummy);
      if (!op_res.type_name.empty())
      {
        size = dwarf_type_size(op_res.type_name.c_str());
      }
    }
  }

  success = true;
  return EvalResult(static_cast<long>(size));
}

std::string SizeofTypeNode::to_string() const
{
  return "sizeof(" + type_name_ + ")";
}

/* OffsetofNode */
EvalResult OffsetofNode::eval(int pid, bool &success) const
{
  ptrdiff_t offset = dwarf_member_offset(type_name_.c_str(), member_.c_str());
  if (offset < 0)
  {
    success = false;
    return EvalResult(0L);
  }
  success = true;
  return EvalResult(static_cast<long>(offset));
}

std::string OffsetofNode::to_string() const
{
  return "offsetof(" + type_name_ + ", " + member_ + ")";
}

/* TypeofNode */
EvalResult TypeofNode::eval(int pid, bool &success) const
{
  bool dummy;
  EvalResult op_res = operand_->eval(pid, dummy);

  /* Return the type name as a value - we encode it as a pointer to a static
   * string */
  /* For now, just return success and let the caller extract the type name */
  success = true;
  EvalResult res(0L);
  res.type_name = op_res.type_name;
  return res;
}

std::string TypeofNode::to_string() const
{
  return "typeof(" + operand_->to_string() + ")";
}

std::string TypeofNode::get_type_name() const { return operand_->to_string(); }

/* ProcessQualifiedNode */
int ProcessQualifiedNode::get_target_pid() const
{
  if (proc_id_ >= 0 && proc_id_ < g_num_processes && g_ptmc_state.pids)
  {
    return g_ptmc_state.pids[proc_id_];
  }
  return 0;
}

EvalResult ProcessQualifiedNode::eval(int pid, bool &success) const
{
  int target_pid = get_target_pid();
  if (target_pid == 0)
  {
    success = false;
    return EvalResult(0L);
  }
  return expr_->eval(target_pid, success);
}

EvalResult ProcessQualifiedNode::eval_address(int pid, bool &success) const
{
  int target_pid = get_target_pid();
  if (target_pid == 0)
  {
    success = false;
    return EvalResult(0L);
  }
  return expr_->eval_address(target_pid, success);
}

std::string ProcessQualifiedNode::to_string() const
{
  return "tracee" + std::to_string(proc_id_) + "(" + expr_->to_string() + ")";
}

// namespace end

ExprCache &ExprCache::instance()
{
  static ExprCache instance;
  return instance;
}

std::shared_ptr<ExprNode> ExprCache::get(const std::string &expr)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = cache_.find(expr);
  if (it != cache_.end())
  {
    return it->second;
  }

  ExprNode *ast = parse_expression(expr);
  if (!ast)
  {
    return nullptr;
  }

  std::shared_ptr<ExprNode> result(ast);
  if (cache_.size() >= MAX_CACHE_SIZE)
  {
    auto mid = cache_.begin();
    std::advance(mid, cache_.size() / 2);
    cache_.erase(cache_.begin(), mid);
  }
  cache_[expr] = result;
  return result;
}

void ExprCache::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
}

size_t ExprCache::size() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_.size();
}
