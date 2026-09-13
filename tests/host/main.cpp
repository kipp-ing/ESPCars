// Entry point for the can_gateway host unit tests. Cases live in test_*.cpp and
// self-register; this file only runs them.
//
//   ./build/run_tests            run every case
//   ./build/run_tests rules_     run cases whose name contains "rules_"

#include "harness.h"

int main(int argc, char **argv) { return hosttest::run_all(argc, argv); }
