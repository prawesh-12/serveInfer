#include "testHarness.h"

#ifndef EDGE_TEST_SUITE_NAME
#define EDGE_TEST_SUITE_NAME "edge tests"
#endif

int main() {
  return edgetest::run(EDGE_TEST_SUITE_NAME);
}
