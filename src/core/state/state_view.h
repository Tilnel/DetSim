#ifndef DETSIM_STATE_VIEW_H
#define DETSIM_STATE_VIEW_H
#include "state.h"
#include "expr_ast.hpp"
#include "api/plugin.h"
#include <array>
#include <string>
namespace detsim {
// Immutable observation of a saved machine identity. No UI cursor or live-memory
// fallback is involved. The global symbol service currently supports only the
// configured matching non-PIE main executable; other binary/relocation contexts
// fail explicitly. Raw reads and primitive-address expressions are unrestricted.
class StateView final : private ExpressionObservation {
 public:
  explicit StateView(const sys_state& state);
  // Only use during a runtime callback while every tracee is stopped. This is
  // an explicit live view for environment choices before successor capture.
  static StateView stopped_live();
  size_t process_count() const override { return NP; }
  void read(size_t node, uint64_t address, void* destination, size_t length) const override;
  int64_t evaluate(size_t node, const std::string& expression) const;
  timeval time(size_t node) const;
  std::string inspect_json(size_t node) const;
  size_t read_buffer(size_t node, detsim_buffer_kind kind, int fd,
                     uint64_t item, uint64_t message, uint64_t offset,
                     void* destination, size_t length) const;
 private:
  StateView() = default;
  std::array<hash_type, NP> snapshots_{};
  std::array<int, NP> statuses_{};
  std::array<timeval, NP> clocks_{};
  mutable std::array<std::shared_ptr<const tracee_state>, NP> metadata_{};
  const tracee_state& metadata(size_t node) const;
  std::array<int, NP> live_pids_{};
  bool live_ = false;
  uint64_t store_generation_ = 0;
  void check_snapshot_lifetime() const;
  std::array<std::string, NP> executables_{};
  void require_symbols(size_t node) const override;
  void require_symbol_address(size_t node, uint64_t address) const override;
};
}
#endif
