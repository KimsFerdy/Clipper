add_executable(clipper_tests core_tests.cpp)
target_link_libraries(clipper_tests PRIVATE clipper_core)
clipper_warnings(clipper_tests)
add_test(NAME core_tests COMMAND clipper_tests)
