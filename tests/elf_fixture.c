#if GO_SECTION == 1
__attribute__((section(".go.buildinfo"), used)) const char go_marker[] =
    "fixture";
#elif GO_SECTION == 2
__attribute__((section(".gopclntab"), used)) const char go_marker[] = "fixture";
#endif
int main(void) { return 0; }
