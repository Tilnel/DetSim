#ifndef __EMU_H
#define __EMU_H
#include <sys/time.h>
extern int choose_many[450];
int emu_gettimeofday(struct timeval *tv, struct timezone *tz);
#endif
