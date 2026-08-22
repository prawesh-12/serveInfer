#pragma once

// A tiny dependency-free harness. Each case registers itself at startup.
//
//   EDGE_TEST(some_id, "plain language description of the behaviour") {
//     CHECK(condition);
//     CHECK_EQ(actual, expected);
//   }

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace edgetest {

struct Failure {};

struct Case {
  std::string description;
  std::function<void()> body;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* description, void (*body)()) {
    cases().push_back({description, body});
  }
};

inline void report(const char* file, int line, const std::string& detail) {
  std::cout << "    " << file << ":" << line << " " << detail << '\n';
}

inline void check(bool ok, const char* expression, const char* file, int line) {
  if (ok) {
    return;
  }
  report(file, line, std::string("failed: ") + expression);
  throw Failure{};
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* expression, const char* file,
             int line) {
  if (actual == expected) {
    return;
  }
  std::ostringstream out;
  out << "failed: " << expression << "\n      actual:   " << actual
      << "\n      expected: " << expected;
  report(file, line, out.str());
  throw Failure{};
}

inline int run(const char* suiteName) {
  int failed = 0;
  std::cout << "# " << suiteName << " (" << cases().size() << " cases)\n";
  for (const Case& test : cases()) {
    try {
      test.body();
      std::cout << "ok - " << test.description << '\n';
    } catch (const Failure&) {
      failed += 1;
      std::cout << "not ok - " << test.description << '\n';
    } catch (const std::exception& err) {
      failed += 1;
      std::cout << "not ok - " << test.description << " (threw: " << err.what() << ")\n";
    }
  }
  std::cout << "# " << (cases().size() - failed) << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace edgetest

#define EDGE_TEST(id, description)                                     \
  static void id();                                                    \
  static const edgetest::Registrar edge_registrar_##id(description, id); \
  static void id()

#define CHECK(condition) edgetest::check((condition), #condition, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  edgetest::checkEq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
