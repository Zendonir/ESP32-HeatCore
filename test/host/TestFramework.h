#pragma once
// Tiny dependency-free test framework for the host test suite.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace hctest {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline int& failures() {
  static int f = 0;
  return f;
}
inline int& checks() {
  static int c = 0;
  return c;
}

inline void report(bool ok, const char* expr, const char* file, int line, const std::string& detail = "") {
  ++checks();
  if (!ok) {
    ++failures();
    std::printf("    FAIL %s:%d: %s %s\n", file, line, expr, detail.c_str());
  }
}

}  // namespace hctest

#define HC_CAT2(a, b) a##b
#define HC_CAT(a, b) HC_CAT2(a, b)
#define TEST(name)                                                                     \
  static void HC_CAT(test_fn_, name)();                                                \
  static ::hctest::Registrar HC_CAT(test_reg_, name)(#name, &HC_CAT(test_fn_, name)); \
  static void HC_CAT(test_fn_, name)()

#define CHECK(cond) ::hctest::report(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                                                   \
  do {                                                                                                   \
    auto _va = (a);                                                                                      \
    auto _vb = (b);                                                                                      \
    ::hctest::report(_va == _vb, #a " == " #b, __FILE__, __LINE__,                                       \
                     "(" + std::to_string(static_cast<long long>(_va)) + " vs " +                        \
                         std::to_string(static_cast<long long>(_vb)) + ")");                             \
  } while (0)
#define CHECK_STR(a, b)                                                                                  \
  do {                                                                                                   \
    std::string _sa = (a);                                                                               \
    std::string _sb = (b);                                                                               \
    ::hctest::report(_sa == _sb, #a " == " #b, __FILE__, __LINE__, "(\"" + _sa + "\" vs \"" + _sb + "\")"); \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                            \
  do {                                                                                                   \
    double _va = (a);                                                                                    \
    double _vb = (b);                                                                                    \
    ::hctest::report(std::fabs(_va - _vb) <= (tol), #a " ~= " #b, __FILE__, __LINE__,                    \
                     "(" + std::to_string(_va) + " vs " + std::to_string(_vb) + ")");                    \
  } while (0)
