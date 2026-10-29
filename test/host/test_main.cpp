#include <cstring>

#include "TestFramework.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (auto& t : hctest::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    int before = hctest::failures();
    t.fn();
    ++run;
    std::printf("[%s] %s\n", hctest::failures() == before ? " OK " : "FAIL", t.name);
  }
  std::printf("\n%d tests, %d checks, %d failures\n", run, hctest::checks(), hctest::failures());
  return hctest::failures() == 0 ? 0 : 1;
}
