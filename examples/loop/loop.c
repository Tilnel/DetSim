#include <unistd.h>

char msg[15] = "Hello World !\n";
int cnt = 0;

int main(int argc, char *argv[]) {
  msg[11] = argv[1][0];
  while (1)
  {
    cnt++;
    write(1, msg, 14);
  }
  return 0;
}

