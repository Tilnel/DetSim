#ifndef __DEBUG_H
#define __DEBUG_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define RESET "\033[0m"

#define FG_BLACK "\033[30m"
#define FG_RED "\033[31m"
#define FG_GREEN "\033[32m"
#define FG_YELLOW "\033[33m"
#define FG_BLUE "\033[34m"
#define FG_MAGENTA "\033[35m"
#define FG_CYAN "\033[36m"
#define FG_WHITE "\033[37m"

#define Assert(cond, ...)                                                      \
  do                                                                           \
  {                                                                            \
    if (!(cond))                                                               \
    {                                                                          \
      fprintf(stderr, "\33[1;31m");                                            \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\33[0m\n");                                              \
      assert(cond);                                                            \
    }                                                                          \
  } while (0)

#define panic(...) Assert(0, __VA_ARGS__)

#define TODO() panic("please implement me")

// 日志功能已移至 ui/log_wrapper.h
// 为了保持向后兼容，包含 log_wrapper.h 并定义宏
#include "ui/log_wrapper.h"

// 如果尚未定义，定义日志宏
#ifndef LOG_TRACE
#define LOG_TRACE(...) UI_LOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) UI_LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  UI_LOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  UI_LOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) UI_LOG_ERROR(__VA_ARGS__)
#define LOG_CRIT(...)  UI_LOG_CRIT(__VA_ARGS__)
#endif

#endif /* __DEBUG_H */
