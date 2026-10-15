// Same finite reservation contract as native_inputs.c, using a C++ value object.
#include <array>
#include <cstdio>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

class Account {
    int balance_ = 1;
public:
    void debit(bool requested, unsigned sequence) {
        if (!requested) return;
#ifdef INJECT_BUG
        if (sequence == 1 || balance_ >= 1) --balance_;
#else
        (void)sequence;
        if (balance_ >= 1) --balance_;
#endif
    }
    int balance() const { return balance_; }
};

int main(int argc, char **argv) {
    std::array<int, 2> bits{};
    if (argc == 2) {
        if (!argv[1][0] || !argv[1][1] || argv[1][2] ||
            (argv[1][0] != '0' && argv[1][0] != '1') ||
            (argv[1][1] != '0' && argv[1][1] != '1')) return 2;
        bits[0] = argv[1][0] - '0'; bits[1] = argv[1][1] - '0';
    } else if (argc == 1) {
        timespec base{};
        if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &base)) return 2;
        long long previous = static_cast<long long>(base.tv_sec) * 1000000 + base.tv_nsec / 1000;
        for (auto &bit : bits) {
            timeval now{};
            if (syscall(SYS_gettimeofday, &now, 0)) return 2;
            long long current = static_cast<long long>(now.tv_sec) * 1000000 + now.tv_usec;
            if (current - previous < 0 || current - previous > 1) return 2;
            bit = static_cast<int>(current - previous); previous = current;
        }
    } else return 2;
    Account account;
    for (unsigned i = 0; i < bits.size(); ++i) account.debit(bits[i] != 0, i);
    char output[64];
    int size = std::snprintf(output, sizeof output, "CASE_%s %d%d %d\n",
                            account.balance() < 0 ? "BUG" : "OK", bits[0], bits[1], account.balance());
    return write(STDOUT_FILENO, output, static_cast<size_t>(size)) == size ? 0 : 2;
}
