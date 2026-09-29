cmake_minimum_required(VERSION 3.20)
project(faust LANGUAGES C CXX)

find_package(LLVM 22.1 REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")

add_library(faust_frontend STATIC
  src/diagnostic.cpp
  src/lexer.cpp
  src/parser.cpp
  src/semantic.cpp)
target_compile_features(faust_frontend PUBLIC cxx_std_17)
target_include_directories(faust_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(faust_codegen STATIC src/codegen.cpp src/ir.cpp)
target_link_libraries(faust_codegen PUBLIC faust_frontend PRIVATE LLVM)
target_include_directories(faust_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(faust_codegen PRIVATE ${LLVM_DEFINITIONS_LIST})

add_executable(faust src/main.cpp)
target_link_libraries(faust PRIVATE faust_frontend faust_codegen)

foreach(target faust_frontend faust_codegen faust)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

install(TARGETS faust RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(LLVM_LLI NAMES lli HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
  add_test(NAME compiler COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compiler.py
    $<TARGET_FILE:faust> ${LLVM_LLI} ${CMAKE_C_COMPILER}
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif()
