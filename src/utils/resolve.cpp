#include "log_wrapper.h"
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#include <stdio.h>
#include <stdlib.h>

/* find last instuction addr */
int resolve_rip_func(const char *exefile, uintptr_t rip)
{
  Dwarf_Addr addr = rip;

  static char *debuginfo_path[] = {NULL};
  Dwfl_Callbacks callbacks = {.find_elf = dwfl_build_id_find_elf,
                              .find_debuginfo = dwfl_build_id_find_debuginfo,
                              .debuginfo_path = debuginfo_path};

  Dwfl *dwfl = dwfl_begin(&callbacks);
  if (!dwfl)
  {
    fprintf(stderr, "dwfl_begin failed: %s\n", dwfl_errmsg(-1));
    return 1;
  }

  if (!dwfl_report_offline(dwfl, "", exefile, -1))
  {
    fprintf(stderr, "dwfl_report_offline failed: %s\n", dwfl_errmsg(-1));
    dwfl_end(dwfl);
    return 1;
  }

  dwfl_report_end(dwfl, NULL, NULL);

  Dwfl_Module *mod = dwfl_addrmodule(dwfl, addr);
  if (!mod)
  {
    detsim::ui::ui_printf("\n");
    dwfl_end(dwfl);
    return 1;
  }

  /*
  const char *funcname = NULL;
  funcname = dwfl_module_addrname(mod, addr);
  if (funcname)
    detsim::ui::ui_printf("%s", funcname);
    */

  Dwfl_Line *line = dwfl_module_getsrc(mod, addr);
  if (line)
  {
    const char *file;
    int lineno, col;
    file = dwfl_lineinfo(line, &addr, &lineno, &col, NULL, NULL);
    if (file)
      detsim::ui::ui_printf(" :%s:%d\n", file, lineno);
  }
  else
  {
    detsim::ui::ui_printf("\n");
  }

  dwfl_end(dwfl);
  return 0;
}
