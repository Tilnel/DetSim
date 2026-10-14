// Replace only output, using the real interface. No runtime state is mocked.
#include "ui/log_wrapper.h"

extern "C" void ui_log_debug_c(const char *, int, const char *, ...) {}
extern "C" void ui_log_info_c(const char *, int, const char *, ...) {}
extern "C" void ui_log_warn_c(const char *, int, const char *, ...) {}
