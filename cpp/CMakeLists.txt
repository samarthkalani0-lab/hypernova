cmake_minimum_required(VERSION 3.16)
project(Supernova LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Engines are useless unoptimized; default to Release for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(supernova_core STATIC
  src/bitboard.cpp
  src/nnue.cpp
  src/position.cpp
  src/eval.cpp
  src/search.cpp
  src/datagen.cpp
)
target_include_directories(supernova_core PUBLIC src)
# AVX2/BMI2-class CPUs (2015+); lets the compiler vectorize the NNUE loops.
set(SN_ARCH "x86-64-v3" CACHE STRING "-march value")
target_compile_options(supernova_core PUBLIC -march=${SN_ARCH})
# Embed a network file into the executable: cmake -DSN_NET=path/to/net.nnue
set(SN_NET "" CACHE FILEPATH "network to embed")
if(SN_NET)
  file(TO_CMAKE_PATH "${SN_NET}" SN_NET_PATH)
  target_compile_definitions(supernova_core PRIVATE SN_NET="${SN_NET_PATH}")
  set_source_files_properties(src/nnue.cpp PROPERTIES OBJECT_DEPENDS "${SN_NET_PATH}")
endif()
if(MSVC)
  target_compile_options(supernova_core PUBLIC /W4)
else()
  target_compile_options(supernova_core PUBLIC -Wall -Wextra)
endif()

add_executable(supernova src/uci.cpp)
target_link_libraries(supernova PRIVATE supernova_core Threads::Threads)

add_executable(supernova_tests tests/tests.cpp)
target_link_libraries(supernova_tests PRIVATE supernova_core)

# Link MinGW runtimes statically so the .exe runs from any GUI without extra DLLs on PATH.
if(MINGW)
  target_link_options(supernova PRIVATE -static)
  target_link_options(supernova_tests PRIVATE -static)
endif()

enable_testing()
add_test(NAME supernova_tests COMMAND supernova_tests --quick)
