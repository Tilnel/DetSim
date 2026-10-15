/* expr_ast.hpp - Abstract Syntax Tree for expression evaluation */
#ifndef EXPR_AST_HPP
#define EXPR_AST_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>

struct type_info;

namespace detsim {
// Explicit logical process context for immutable observations.
struct ExpressionObservation {
  virtual ~ExpressionObservation() = default;
  virtual size_t process_count() const = 0;
  virtual void require_symbols(size_t node) const = 0;
  virtual void require_symbol_address(size_t node, uint64_t address) const = 0;
  virtual void read(size_t node, uint64_t address, void* output, size_t length) const = 0;
};
int64_t evaluate_observation(const ExpressionObservation&, size_t node, const std::string&);
}

/* Evaluation result: can be a value or an address */
struct EvalResult
{
  std::variant<long, uint64_t> value;
  bool is_address;
  std::string type_name;

  EvalResult() : is_address(false) {}
  EvalResult(long v) : value(v), is_address(false) {}
  EvalResult(uint64_t v) : value(v), is_address(true) {}

  long as_value() const
  {
    if (std::holds_alternative<long>(value))
    {
      return std::get<long>(value);
    }
    return static_cast<long>(std::get<uint64_t>(value));
  }
  uint64_t as_address() const
  {
    if (std::holds_alternative<uint64_t>(value))
    {
      return std::get<uint64_t>(value);
    }
    return static_cast<uint64_t>(std::get<long>(value));
  }
};

/* AST Node base class */
class ExprNode
{
  public:
  virtual ~ExprNode();
  virtual EvalResult eval(int pid, bool &success) const = 0;
  virtual EvalResult eval_address(int pid, bool &success) const;
  virtual std::string to_string() const = 0;
  virtual bool is_read_only() const { return true; }

  /* Get target process ID for process-qualified expressions (traceeN())
   * Returns -1 if not a process-qualified expression
   */
  virtual int get_target_pid() const { return -1; }
};

using ExprPtr = std::unique_ptr<ExprNode>;

/* Number literal */
class NumberNode : public ExprNode
{
  long value_;

  public:
  explicit NumberNode(long v) : value_(v) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override { return std::to_string(value_); }
};

/* Type for sizeof/offsetof */
class TypeNode : public ExprNode
{
  std::string type_name_;

  public:
  explicit TypeNode(const std::string &t) : type_name_(t) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override { return type_name_; }
  std::string get_type() const { return type_name_; }
};

/* Variable */
class VariableNode : public ExprNode
{
  std::string name_;

  public:
  explicit VariableNode(const std::string &n) : name_(n) {}
  EvalResult eval(int pid, bool &success) const override;
  EvalResult eval_address(int pid, bool &success) const override;
  std::string to_string() const override { return name_; }
};

/* Binary operators */
enum class BinaryOp
{
  ADD,
  SUB,
  MUL,
  DIV,
  MOD,
  LT,
  GT,
  LE,
  GE,
  EQ,
  NE,
  AND,
  OR,
  BITAND,
  BITOR,
  XOR,
  SHL,
  SHR
};

class BinaryOpNode : public ExprNode
{
  BinaryOp op_;
  ExprPtr left_, right_;

  public:
  bool is_read_only() const override { return left_->is_read_only() && right_->is_read_only(); }
  BinaryOpNode(BinaryOp op, ExprNode *l, ExprNode *r)
      : op_(op), left_(l), right_(r)
  {
  }
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Unary operators */
enum class UnaryOp
{
  NEG,
  NOT,
  BITNOT
};

class UnaryOpNode : public ExprNode
{
  UnaryOp op_;
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return operand_->is_read_only(); }
  UnaryOpNode(UnaryOp op, ExprNode *opnd) : op_(op), operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Prefix ++ -- */
enum class PrefixOp
{
  PRE_INC,
  PRE_DEC
};

class PrefixOpNode : public ExprNode
{
  PrefixOp op_;
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return false; }
  PrefixOpNode(PrefixOp op, ExprNode *opnd) : op_(op), operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Postfix ++ -- */
enum class PostfixOp
{
  POST_INC,
  POST_DEC
};

class PostfixOpNode : public ExprNode
{
  PostfixOp op_;
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return false; }
  PostfixOpNode(PostfixOp op, ExprNode *opnd) : op_(op), operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Member access . -> */
enum class MemberOp
{
  DOT,
  ARROW
};

class MemberAccessNode : public ExprNode
{
  MemberOp op_;
  ExprPtr object_;
  std::string member_;
  mutable ptrdiff_t cached_offset_;
  mutable std::string cached_base_type_;
  mutable std::string cached_member_type_;
  mutable bool offset_computed_;

  public:
  bool is_read_only() const override { return object_->is_read_only(); }
  MemberAccessNode(MemberOp op, ExprNode *obj, const std::string &mem)
      : op_(op), object_(obj), member_(mem), cached_offset_(-1),
        cached_base_type_(), cached_member_type_(), offset_computed_(false)
  {
  }
  EvalResult eval(int pid, bool &success) const override;
  EvalResult eval_address(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Array index [] */
class ArrayIndexNode : public ExprNode
{
  ExprPtr array_;
  ExprPtr index_;

  public:
  bool is_read_only() const override { return array_->is_read_only() && index_->is_read_only(); }
  ArrayIndexNode(ExprNode *arr, ExprNode *idx) : array_(arr), index_(idx) {}
  EvalResult eval(int pid, bool &success) const override;
  EvalResult eval_address(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Address-of & */
class AddressOfNode : public ExprNode
{
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return operand_->is_read_only(); }
  explicit AddressOfNode(ExprNode *opnd) : operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Dereference * */
class DereferenceNode : public ExprNode
{
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return operand_->is_read_only(); }
  explicit DereferenceNode(ExprNode *opnd) : operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  EvalResult eval_address(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Conditional a ? b : c */
class ConditionalNode : public ExprNode
{
  ExprPtr cond_, then_, else_;

  public:
  bool is_read_only() const override { return cond_->is_read_only() && then_->is_read_only() && else_->is_read_only(); }
  ConditionalNode(ExprNode *c, ExprNode *t, ExprNode *e)
      : cond_(c), then_(t), else_(e)
  {
  }
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Assignment = */
class AssignNode : public ExprNode
{
  ExprPtr left_, right_;

  public:
  bool is_read_only() const override { return false; }
  AssignNode(ExprNode *l, ExprNode *r) : left_(l), right_(r) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Type cast */
class CastNode : public ExprNode
{
  ExprPtr type_, operand_;

  public:
  bool is_read_only() const override { return type_->is_read_only() && operand_->is_read_only(); }
  CastNode(ExprNode *t, ExprNode *op) : type_(t), operand_(op) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* sizeof expression */
class SizeofExprNode : public ExprNode
{
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return operand_->is_read_only(); }
  explicit SizeofExprNode(ExprNode *opnd) : operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* sizeof(type) */
class SizeofTypeNode : public ExprNode
{
  std::string type_name_;

  public:
  explicit SizeofTypeNode(const std::string &t) : type_name_(t) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* offsetof */
class OffsetofNode : public ExprNode
{
  std::string type_name_;
  std::string member_;

  public:
  OffsetofNode(const std::string &t, const std::string &m)
      : type_name_(t), member_(m)
  {
  }
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* typeof */
class TypeofNode : public ExprNode
{
  ExprPtr operand_;

  public:
  bool is_read_only() const override { return operand_->is_read_only(); }
  explicit TypeofNode(ExprNode *opnd) : operand_(opnd) {}
  EvalResult eval(int pid, bool &success) const override;
  std::string to_string() const override;
  std::string get_type_name() const;
};

/* Process-qualified: traceeN(expr) */
class ProcessQualifiedNode : public ExprNode
{
  int proc_id_;
  ExprPtr expr_;

  public:
  bool is_read_only() const override { return expr_->is_read_only(); }
  ProcessQualifiedNode(int pid, ExprNode *e) : proc_id_(pid), expr_(e) {}
  int get_proc_id() const { return proc_id_; }
  int get_target_pid() const override;
  EvalResult eval(int pid, bool &success) const override;
  EvalResult eval_address(int pid, bool &success) const override;
  std::string to_string() const override;
};

/* Parser interface */
ExprNode *parse_expression(const char *input);
ExprNode *parse_expression(const std::string &input);
void free_parser_result();

/* Globals */
extern int g_num_processes;
extern struct PtmcState
{
  int *pids;
} g_ptmc_state;

/* Parser globals - defined in expr_ast.cpp */
extern ExprNode *g_parser_result;
extern const char *g_input_string;

class ExprCache
{
  public:
  static ExprCache &instance();
  std::shared_ptr<ExprNode> get(const std::string &expr);
  void clear();
  size_t size() const;

  private:
  ExprCache() = default;
  std::unordered_map<std::string, std::shared_ptr<ExprNode>> cache_;
  mutable std::mutex mutex_;
  static constexpr size_t MAX_CACHE_SIZE = 1024;
};

#endif /* EXPR_AST_HPP */
