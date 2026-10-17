#include <unistd.h>
int main(void) { return write(1,"RUNTIME_OK\n",11)==11 ? 0 : 1; }
