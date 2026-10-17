#include <linux/futex.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
_Alignas(16) static unsigned char worker_stack[65536];
volatile int scenario, main_marker, worker_marker, word;
volatile long main_result;
struct timespec main_delay = {0, 1000}, worker_delay = {0, 2000};
struct timeval worker_time, main_time;
extern long spawn(void *);
asm(".text\n.globl spawn\n.type spawn,@function\nspawn:\n"
    "mov %rdi,%rsi\nmov $0x50f00,%edi\nxor %edx,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\n"
    "mov $56,%eax\nsyscall\ntest %rax,%rax\njz 1f\nret\n"
    "1: cmpl $99,scenario(%rip)\nje 2f\n"
    "cmpl $114,scenario(%rip)\nje 3f\n"
    "cmpl $119,scenario(%rip)\nje 4f\ncmpl $100,scenario(%rip)\nje 4f\n"
    "cmpl $109,scenario(%rip)\nje 4f\ncmpl $112,scenario(%rip)\nje 7f\njmp 5f\n"
    "2: mov $96,%eax\nlea worker_time(%rip),%rdi\nxor %esi,%esi\nsyscall\njmp 5f\n"
    "7: lea word(%rip),%rdi\nmov $128,%esi\nxor %edx,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\nxor %r9d,%r9d\nmov $202,%eax\nsyscall\njmp 5f\n"
    "3: lea word(%rip),%rdi\nmov $129,%esi\nmov $1,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\nxor %r9d,%r9d\nmov $202,%eax\nsyscall\njmp 5f\n"
    "4: mov $35,%eax\nlea worker_delay(%rip),%rdi\nxor %esi,%esi\nsyscall\n"
    "5: movl $1,worker_marker(%rip)\nmov $1,%eax\nmov $1,%edi\nlea letter(%rip),%rsi\nmov $1,%edx\nsyscall\n"
    "cmpl $119,scenario(%rip)\njne 6f\nmovl $1,word(%rip)\nlea word(%rip),%rdi\nmov $129,%esi\nmov $1,%edx\n"
    "xor %r10d,%r10d\nxor %r8d,%r8d\nxor %r9d,%r9d\nmov $202,%eax\nsyscall\n"
    "6: mov $60,%eax\nxor %edi,%edi\nsyscall\nud2\n.size spawn,.-spawn\n"
    ".section .rodata\nletter: .ascii \"B\"\n.text\n");
int main(int argc, char **argv) {
    scenario = argc > 1 ? argv[1][0] : 'a';
    if (spawn(worker_stack + sizeof worker_stack) < 0) return 71;
    if (scenario == 'w' || scenario == 'r')
        main_result = syscall(SYS_futex, &word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                              0, scenario == 'r' ? &main_delay : 0, 0, 0);
    else if (scenario == 'd') main_result = syscall(SYS_nanosleep, &main_delay, 0);
    if (scenario == 'p') {
        word = 1; /* A native early wake makes the later WAIT return EAGAIN. */
        main_result = syscall(SYS_futex, &word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0, 0, 0);
    }
    if (scenario == 'm' || scenario == 'p') main_result = syscall(SYS_gettimeofday, &main_time, 0);
    main_marker = 1;
    syscall(SYS_write, 1, "A", 1);
    syscall(SYS_exit, 0);
    __builtin_unreachable();
}
