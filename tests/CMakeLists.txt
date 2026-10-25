cmake_minimum_required(VERSION 3.12)
project(mcaat_cycle_search_tests LANGUAGES CXX)
enable_testing()
add_executable(bounded_cycle_search_test bounded_cycle_search_test.cpp)
target_compile_features(bounded_cycle_search_test PRIVATE cxx_std_17)
target_include_directories(bounded_cycle_search_test PRIVATE ../include)
add_test(NAME bounded_cycle_search COMMAND bounded_cycle_search_test)
# Catch accidental path enumeration on the exponential-branching regression case.
set_tests_properties(bounded_cycle_search PROPERTIES TIMEOUT 120)
