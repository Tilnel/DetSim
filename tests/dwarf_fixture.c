#include "dwarf_fixture.h"
struct FixtureRecord fixture_record = {'a', 7, 99};
int fixture_numbers[3] = {1, 2, 3};
extern long fixture_other_value(void);
int main(void) { return (int)fixture_other_value(); }
