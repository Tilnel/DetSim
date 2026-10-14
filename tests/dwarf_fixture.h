#ifndef DETSIM_DWARF_FIXTURE_H
#define DETSIM_DWARF_FIXTURE_H
struct FixtureRecord
{
  char tag;
  int count;
  long total;
};
struct FixtureOther
{
  double value;
  char flag;
};
#endif
