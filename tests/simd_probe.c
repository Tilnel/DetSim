volatile long observed = 0;

int main(void)
{
  // No C call intervenes: XMM15 is intentionally live across raw syscalls.
  __asm__ volatile(
      "mov $11, %%rax\n\tmovq %%rax, %%xmm15\n\t"
      "mov $39, %%rax\n\tsyscall\n\t"
      "movq %%xmm15, observed(%%rip)\n\t"
      "mov $22, %%rax\n\tmovq %%rax, %%xmm15\n\t"
      "mov $39, %%rax\n\tsyscall\n\t"
      "movq %%xmm15, observed(%%rip)\n\t"
      "mov $39, %%rax\n\tsyscall\n\t"
      : : : "rax", "rcx", "r11", "xmm15", "memory");
  return 0;
}
