#ifndef LOG_WRAPPER_H
#define LOG_WRAPPER_H

#include <string>
#include <cstdarg>
#include "core/config.h"  // 包含 config.h 以使用 loglevel

// 日志级别定义（与 debug.h 保持一致）
#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_WARN  3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_CRIT  5

namespace detsim {
namespace ui {

class NCursesUI;

// 全局 UI 指针（由 main.cpp 设置）
extern NCursesUI* g_ncurses_ui;

// 全局退出标志（用于通知 UI 退出）
extern bool g_ui_should_exit;

// 请求 UI 退出
void request_ui_exit();

// 当前日志级别（引用 config 的 loglevel）
extern int& g_ui_loglevel;

// 初始化日志包装器
void init_log_wrapper(NCursesUI* ui);

// 设置/获取日志级别
void set_ui_loglevel(int level);
int get_ui_loglevel();

// 检查是否应该记录某级别的日志
static inline bool should_log_ui(int level) {
    return level >= g_ui_loglevel;
}

// 带级别的日志输出（内部使用）
void ui_log_at_level(int level, const char* file, int line, const char* fmt, ...);

// 基本输出（不检查日志级别，直接使用）
void ui_printf(const char* fmt, ...);
void ui_println(const char* fmt, ...);
void ui_print_raw(const std::string& text);
void ui_print_raw(const char* text);

// 各级别日志宏
#define UI_LOG_TRACE(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_TRACE)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_TRACE, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

#define UI_LOG_DEBUG(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_DEBUG)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

#define UI_LOG_INFO(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_INFO)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

#define UI_LOG_WARN(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_WARN)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

#define UI_LOG_ERROR(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_ERROR)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

#define UI_LOG_CRIT(...) do { \
    if (detsim::ui::should_log_ui(LOG_LEVEL_CRIT)) \
        detsim::ui::ui_log_at_level(LOG_LEVEL_CRIT, __FILE__, __LINE__, __VA_ARGS__); \
} while(0)

} // namespace ui
} // namespace detsim

// C 兼容接口
#ifdef __cplusplus
extern "C" {
#endif

// 各级别的 C 接口
void ui_log_trace_c(const char* file, int line, const char* fmt, ...);
void ui_log_debug_c(const char* file, int line, const char* fmt, ...);
void ui_log_info_c(const char* file, int line, const char* fmt, ...);
void ui_log_warn_c(const char* file, int line, const char* fmt, ...);
void ui_log_error_c(const char* file, int line, const char* fmt, ...);
void ui_log_crit_c(const char* file, int line, const char* fmt, ...);

// 便捷宏（C 代码使用）
#define LOG_TRACE(...) ui_log_trace_c(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) ui_log_debug_c(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ui_log_info_c(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ui_log_warn_c(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ui_log_error_c(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRIT(...)  ui_log_crit_c(__FILE__, __LINE__, __VA_ARGS__)

// 普通输出（总是输出，不受日志级别控制）
void ui_printf_c(const char* fmt, ...);
void ui_println_c(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // LOG_WRAPPER_H
