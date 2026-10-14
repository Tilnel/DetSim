#include "dwarf_fixture.h"
struct FixtureOther fixture_other = {2.5, 'b'};
long fixture_other_value(void) { return (long)fixture_other.value; }
