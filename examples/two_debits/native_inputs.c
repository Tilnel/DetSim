/* Two optional debit requests against one unit of credit. The model supplies
 * each request through a clock delta; argv supplies explicit bits for the native
 * oracle. INJECT_BUG omits validation of the second request. */
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int bits[2] = {0, 0};
    if (argc == 2) {
        if (!argv[1][0] || !argv[1][1] || argv[1][2] ||
            (argv[1][0] != '0' && argv[1][0] != '1') ||
            (argv[1][1] != '0' && argv[1][1] != '1')) return 2;
        bits[0] = argv[1][0] - '0'; bits[1] = argv[1][1] - '0';
    } else if (argc == 1) {
        struct timespec base;
        if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &base)) return 2;
        long long previous = (long long)base.tv_sec * 1000000 + base.tv_nsec / 1000;
        for (int i = 0; i < 2; ++i) {
            struct timeval now;
            if (syscall(SYS_gettimeofday, &now, 0)) return 2;
            long long current = (long long)now.tv_sec * 1000000 + now.tv_usec;
            if (current - previous < 0 || current - previous > 1) return 2;
            bits[i] = (int)(current - previous); previous = current;
        }
    } else return 2;
    int balance = 1;
    for (int i = 0; i < 2; ++i) {
        if (!bits[i]) continue;
#ifdef INJECT_BUG
        if (i == 1 || balance >= 1) --balance;
#else
        if (balance >= 1) --balance;
#endif
    }
    char output[64];
    int size = snprintf(output, sizeof output, "CASE_%s %d%d %d\n",
                        balance < 0 ? "BUG" : "OK", bits[0], bits[1], balance);
    return write(STDOUT_FILENO, output, (size_t)size) == size ? 0 : 2;
}
