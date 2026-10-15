/*
 * expr_eval.hpp - Template-based expression evaluation interface
 *
 * Provides type-safe eval<T>(expr, proc_idx) for use in check functions
 */

#ifndef EXPR_EVAL_HPP
#define EXPR_EVAL_HPP

#include <string>
#include <cstdint>

/*
 * eval<T> - Evaluate expression and return typed result
 *
 * Usage:
 *   int state = eval<int>("counter", 0);        // process 0
 *   uint64_t term = eval<uint64_t>("sequence", 1);
 *   bool is_leader = eval<bool>("counter == 2", 0);
 *
 * Supports types: int, uint32_t, uint64_t, long, bool, void*
 *
 * @param expr: C expression string (supports variables, members, arrays)
 * @param proc_idx: process index (0 to NP-1)
 * @param success: optional bool to indicate success/failure
 * @return: evaluated value cast to type T
 */
template<typename T>
T eval(const std::string& expr, int proc_idx = 0, bool* success = nullptr);

/* Convenience overload for const char* */
template<typename T>
T eval(const char* expr, int proc_idx = 0, bool* success = nullptr) {
    return eval<T>(std::string(expr), proc_idx, success);
}

#endif /* EXPR_EVAL_HPP */
