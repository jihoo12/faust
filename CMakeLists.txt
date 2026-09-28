cmake_minimum_required(VERSION 3.20)
project(llvm_demo LANGUAGES C CXX)

find_package(LLVM 22.1 REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

add_executable(llvm-demo src/main.cpp)
target_compile_features(llvm-demo PRIVATE cxx_std_17)
target_include_directories(llvm-demo SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(llvm-demo PRIVATE ${LLVM_DEFINITIONS_LIST})
target_link_libraries(llvm-demo PRIVATE LLVM)

install(TARGETS llvm-demo RUNTIME DESTINATION bin)

enable_testing()
find_program(LLVM_LLI NAMES lli HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
add_test(NAME demo COMMAND ${CMAKE_COMMAND}
  -DDEMO=$<TARGET_FILE:llvm-demo>
  -DLLI=${LLVM_LLI}
  -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/demo.ll
  -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/demo.cmake)
