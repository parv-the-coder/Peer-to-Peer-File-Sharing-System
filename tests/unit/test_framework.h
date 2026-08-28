#pragma once

// A deliberately tiny test harness.
//
// Catch2 and GoogleTest were both considered. Vendoring an amalgamated
// header means committing ~900KB of third-party code into a project this
// small, and pulling one in with FetchContent makes every configure --
// including CI -- depend on the network. Neither is worth it for the
// handful of assertions here, so this is ~50 lines doing the same job:
// register tests, run them, report failures with file and line, and exit
// non-zero so ctest notices.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int &failures() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const std::string &name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

inline void report(const char *file, int line, const std::string &expr,
                   const std::string &detail) {
  std::printf("    FAIL %s:%d\n      %s\n", file, line, expr.c_str());
  if (!detail.empty()) {
    std::printf("      %s\n", detail.c_str());
  }
  ++failures();
}

inline int run_all() {
  int failed_tests = 0;
  for (auto &t : registry()) {
    int before = failures();
    std::printf("  %s\n", t.name.c_str());
    t.fn();
    if (failures() != before) {
      ++failed_tests;
    }
  }
  std::printf("\n%zu tests, %d failed assertion(s), %d failed test(s)\n",
              registry().size(), failures(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}

} // namespace testing

#define TEST(name)                                                             \
  static void name();                                                          \
  static ::testing::Registrar reg_##name(#name, name);                         \
  static void name()

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) ::testing::report(__FILE__, __LINE__, #cond, "");             \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    auto _a = (a);                                                             \
    auto _b = (b);                                                             \
    if (!(_a == _b)) {                                                         \
      std::string d = "got: ";                                                 \
      { std::ostringstream _o; _o << _a; d += _o.str(); }                      \
      d += "\n      want: ";                                                   \
      { std::ostringstream _o; _o << _b; d += _o.str(); }                      \
      ::testing::report(__FILE__, __LINE__, #a " == " #b, d);                  \
    }                                                                          \
  } while (0)
