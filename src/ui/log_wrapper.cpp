#include "log_wrapper.h"
#include "ncurses_ui.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/time.h>

namespace detsim
{
namespace ui
{

NCursesUI *g_ncurses_ui = nullptr;
bool g_ui_should_exit = false;

// 引用 config 中的 loglevel
int &g_ui_loglevel = loglevel;

void request_ui_exit() { g_ui_should_exit = true; }

void init_log_wrapper(NCursesUI *ui) { g_ncurses_ui = ui; }

void set_ui_loglevel(int level) { g_ui_loglevel = level; }

int get_ui_loglevel() { return g_ui_loglevel; }

// 获取级别名称和颜色
static const char *get_level_str(int level)
{
  switch (level)
  {
    case LOG_LEVEL_TRACE:
      return "TRACE";
    case LOG_LEVEL_DEBUG:
      return "DEBUG";
    case LOG_LEVEL_INFO:
      return "INFO";
    case LOG_LEVEL_WARN:
      return "WARN";
    case LOG_LEVEL_ERROR:
      return "ERROR";
    case LOG_LEVEL_CRIT:
      return "CRIT";
    default:
      return "UNKNOWN";
  }
}

static const char *get_level_color(int level)
{
  switch (level)
  {
    case LOG_LEVEL_TRACE:
      return "\033[90m"; // 灰色
    case LOG_LEVEL_DEBUG:
      return "\033[36m"; // 青色
    case LOG_LEVEL_INFO:
      return "\033[32m"; // 绿色
    case LOG_LEVEL_WARN:
      return "\033[33m"; // 黄色
    case LOG_LEVEL_ERROR:
      return "\033[31m"; // 红色
    case LOG_LEVEL_CRIT:
      return "\033[35m"; //  magenta
    default:
      return "\033[0m";
  }
}

void ui_log_at_level(int level, const char *file, int line, const char *fmt,
                     ...)
{
  va_list args;
  va_start(args, fmt);

  // 获取当前时间
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm *tm_now = localtime(&tv.tv_sec);
  char time_buf[64];
  strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_now);

  // 格式化消息
  char msg_buf[4096];
  vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  // 提取文件名（去掉路径）
  const char *filename = file;
  const char *last_slash = strrchr(file, '/');
  if (last_slash)
  {
    filename = last_slash + 1;
  }

  // 完整格式化：时间 [级别][文件:行号] 消息
  char full_buf[4600];
  const char *level_str = get_level_str(level);

  snprintf(full_buf, sizeof(full_buf), "[%s][%s][%s:%d] %s", time_buf,
           level_str, filename, line, msg_buf);

  if (g_ncurses_ui)
  {
    // 输出到 UI
    g_ncurses_ui->add_line(full_buf);
  }
  else
  {
    // 回退到标准输出（带颜色）
    const char *color = get_level_color(level);
    printf("%s%s\033[0m\n", color, full_buf);
    fflush(stdout);
  }

  // 如果指定了日志文件（-1 参数），额外写入日志文件
  if (log_fp != NULL && log_fp != stdout)
  {
    fprintf(log_fp, "[%s][%s][%s:%d] %s\n", time_buf, level_str, filename, line,
            msg_buf);
    fflush(log_fp);
  }
}

void ui_printf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), fmt, args);

  va_end(args);

  if (g_ncurses_ui)
  {
    g_ncurses_ui->print(buffer);
  }
  else
  {
    printf("%s", buffer);
    fflush(stdout);
  }

  // 如果指定了日志文件，额外写入
  if (log_fp != NULL && log_fp != stdout)
  {
    fprintf(log_fp, "%s", buffer);
    fflush(log_fp);
  }
}

void ui_println(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  char buffer[4096];
  vsnprintf(buffer, sizeof(buffer), fmt, args);

  va_end(args);

  if (g_ncurses_ui)
  {
    g_ncurses_ui->add_line(buffer);
  }
  else
  {
    printf("%s\n", buffer);
    fflush(stdout);
  }

  // 如果指定了日志文件，额外写入
  if (log_fp != NULL && log_fp != stdout)
  {
    fprintf(log_fp, "%s\n", buffer);
    fflush(log_fp);
  }
}

void ui_print_raw(const std::string &text)
{
  if (g_ncurses_ui)
  {
    g_ncurses_ui->print(text);
  }
  else
  {
    printf("%s", text.c_str());
    fflush(stdout);
  }

  // 如果指定了日志文件，额外写入
  if (log_fp != NULL && log_fp != stdout)
  {
    fprintf(log_fp, "%s", text.c_str());
    fflush(log_fp);
  }
}

void ui_print_raw(const char *text)
{
  if (g_ncurses_ui)
  {
    g_ncurses_ui->print(text);
  }
  else
  {
    printf("%s", text);
    fflush(stdout);
  }

  // 如果指定了日志文件，额外写入
  if (log_fp != NULL && log_fp != stdout)
  {
    fprintf(log_fp, "%s", text);
    fflush(log_fp);
  }
}

} // namespace ui
} // namespace detsim

// C 接口实现
extern "C"
{

  static void c_log_at_level(int level, const char *file, int line,
                             const char *fmt, va_list args)
  {
    char msg_buf[4096];
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);

    // 调用 C++ 函数
    detsim::ui::ui_log_at_level(level, file, line, "%s", msg_buf);
  }

  void ui_log_trace_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_TRACE))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_TRACE, file, line, fmt, args);
    va_end(args);
  }

  void ui_log_debug_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_DEBUG))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_DEBUG, file, line, fmt, args);
    va_end(args);
  }

  void ui_log_info_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_INFO))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_INFO, file, line, fmt, args);
    va_end(args);
  }

  void ui_log_warn_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_WARN))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_WARN, file, line, fmt, args);
    va_end(args);
  }

  void ui_log_error_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_ERROR))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_ERROR, file, line, fmt, args);
    va_end(args);
  }

  void ui_log_crit_c(const char *file, int line, const char *fmt, ...)
  {
    if (!detsim::ui::should_log_ui(LOG_LEVEL_CRIT))
      return;
    va_list args;
    va_start(args, fmt);
    c_log_at_level(LOG_LEVEL_CRIT, file, line, fmt, args);
    va_end(args);
  }

  void ui_printf_c(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    va_end(args);

    if (detsim::ui::g_ncurses_ui)
    {
      detsim::ui::g_ncurses_ui->print(buffer);
    }
    else
    {
      printf("%s", buffer);
      fflush(stdout);
    }
  }

  void ui_println_c(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);

    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    va_end(args);

    if (detsim::ui::g_ncurses_ui)
    {
      detsim::ui::g_ncurses_ui->add_line(buffer);
    }
    else
    {
      printf("%s\n", buffer);
      fflush(stdout);
    }
  }

} // extern "C"
