# CMake generated Testfile for 
# Source directory: C:/Users/user/stream-server/server
# Build directory: C:/Users/user/stream-server/server/build-core
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[core_parity]=] "C:/Users/user/stream-server/server/build-core/core/ucv-core-test.exe")
set_tests_properties([=[core_parity]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/user/stream-server/server/CMakeLists.txt;25;add_test;C:/Users/user/stream-server/server/CMakeLists.txt;0;")
subdirs("core")
