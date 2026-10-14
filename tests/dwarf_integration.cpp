#include "config.h"
#include "dwarf_fixture.h"
#include "dwarf_info.h"
#include "ncurses_ui.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The executable entry point owns the UI accessor; all runtime state and DWARF
// behavior below are linked from the production objects.
extern "C" detsim::ui::NCursesUI *get_ncurses_ui() { return nullptr; }

static void require(bool condition, const char *message)
{
  if (!condition)
  {
    std::fprintf(stderr, "DWARF regression: %s\n", message);
    std::exit(1);
  }
}

int main(int argc, char **argv)
{
  loglevel = 4; // Keep parser errors visible without debug traversal logs.
  require(argc == 3, "expected DWARF 4 and DWARF 5 fixtures");
  if (std::strcmp(argv[1], "--class") == 0)
  {
    struct ExpectedLayout
    {
      long value;
      int count;
    };
    dwarf_init(argv[2]);
    require(dwarf_type_size("ForwardClass") == sizeof(ExpectedLayout),
            "class behind a pointer must retain its DWARF byte size");
    dwarf_cleanup();
    std::puts("DWARF class size regression passed");
    return 0;
  }
  if (std::strcmp(argv[1], "--forward") == 0)
  {
    for (int pass = 0; pass != 2; ++pass)
    {
      dwarf_init(argv[2]);
      require(dwarf_type_size("ForwardAlias") == sizeof(long),
              "forward typedef must resolve across compilation units");
      require(dwarf_member_offset("ForwardAlias", "value") == 0,
              "resolved typedef preserves members");
      require(dwarf_type_size("MissingAlias") == 0,
              "opaque typedef remains incomplete");
      require(dwarf_type_size("ForwardResolved") == sizeof(long),
              "same-name typedef resolves to definition rather than itself");
      require(dwarf_type_size("ForwardMissing") == 0,
              "same-name opaque typedef does not recurse into itself");
      require(dwarf_type_size("ForwardTail") == sizeof(int),
              "nested lookup must not skip later types");
      dwarf_cleanup();
    }
    std::puts("DWARF forward-declaration regression passed");
    return 0;
  }
  if (std::strcmp(argv[1], "--redis") == 0)
  {
    dwarf_init(argv[2]);
    require(dwarf_type_size("sds") == sizeof(char *), "Redis sds debug type");
    dwarf_cleanup();
    std::puts("Redis DWARF initialization passed");
    return 0;
  }
  for (int pass = 0; pass != 4; ++pass)
  {
    // Alternate formats, including direct reinitialization of a live parser.
    dwarf_init(argv[1 + pass % 2]);
    require(dwarf_get_global_addr("fixture_record") != 0, "first CU global");
    require(dwarf_get_global_addr("fixture_other") != 0, "second CU global");
    require(dwarf_get_global_type("fixture_record") == "FixtureRecord",
            "global type");
    require(dwarf_get_global_type("fixture_other") == "FixtureOther",
            "second CU global type");
    require(dwarf_get_global_type("fixture_numbers") == "int[3]",
            "DWARF array bounds");
    require(dwarf_type_size("FixtureRecord") == sizeof(FixtureRecord),
            "struct size");
    require(dwarf_type_size("FixtureOther") == sizeof(FixtureOther),
            "second CU type size");
    require(dwarf_member_offset("FixtureRecord", "count") ==
                offsetof(FixtureRecord, count),
            "member alignment");
    require(dwarf_member_offset("FixtureRecord", "total") ==
                offsetof(FixtureRecord, total),
            "member offset");
    require(dwarf_member_offset("FixtureOther", "flag") ==
                offsetof(FixtureOther, flag),
            "second CU offset");
    require(dwarf_type_size("int[3]") == sizeof(int[3]), "array size");
    require(dwarf_get_global_addr("fixture_other_value") != 0,
            "ELF function fallback");
    if (pass % 2)
    {
      dwarf_cleanup();
      require(dwarf_get_global_addr("fixture_record") == 0,
              "cleanup clears symbols");
      dwarf_cleanup();
    }
  }
  std::puts("DWARF 4/5 multi-CU production parser regression passed");
}
