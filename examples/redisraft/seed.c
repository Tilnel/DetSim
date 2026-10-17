/* Benchmark-only: stabilize the tracer's search order without changing production code. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
void srand(unsigned int seed) {
    void (*real_srand)(unsigned int) = (void (*)(unsigned int))dlsym(RTLD_NEXT, "srand");
    if (!real_srand) _Exit(127);
    const char *fixed = getenv("DETSIM_BENCH_SEED");
    real_srand(fixed ? (unsigned int)strtoul(fixed, 0, 10) : seed);
}
