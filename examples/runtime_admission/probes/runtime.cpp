#include <iostream>
#include <vector>
int main() { std::vector<int> values{1,2,3}; if (values.at(2)!=3) return 1; std::cout << "RUNTIME_OK\n"; }
