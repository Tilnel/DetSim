#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

int cnt = 0;
int main() {
  setbuf(stdout, NULL);
  while (1) {
    void *ptr = malloc(rand() & 0xfffff);
    write(1, "Hello\n", 6);
    free(ptr);
    write(1, "Hello\n", 6);
    cnt++;
  }
}
