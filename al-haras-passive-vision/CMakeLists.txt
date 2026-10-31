# Alternative build for SDK cross-compilation (the Makefile is the verified host build).
# STATUS: not run — CMake is not installed on the development host and no Luckfox
# SDK toolchain file is available. Example (toolchain file from the installed SDK):
#   cmake -S . -B build/rv1106 -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.16)
project(al_haras_passive_vision VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

file(STRINGS VERSION AHPV_VERSION_TEXT LIMIT_COUNT 1)
file(GLOB AHPV_LIB_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER AHPV_LIB_SOURCES EXCLUDE REGEX "src/app/")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(FILTER AHPV_LIB_SOURCES EXCLUDE REGEX "v4l2_source\\.cpp$")
endif()
file(GLOB AHPV_HASH_INPUTS src/*/*.cpp src/*/*.hpp)
set(AHPV_HASH_TEXT "")
foreach(f ${AHPV_HASH_INPUTS})
  file(READ ${f} content)
  string(APPEND AHPV_HASH_TEXT "${content}")
endforeach()
string(SHA256 AHPV_SOURCE_HASH "${AHPV_HASH_TEXT}")
string(SUBSTRING ${AHPV_SOURCE_HASH} 0 16 AHPV_BUILD_ID)

add_library(ahpv_core STATIC ${AHPV_LIB_SOURCES})
target_include_directories(ahpv_core PUBLIC src)
target_compile_definitions(ahpv_core PUBLIC AHPV_VERSION="${AHPV_VERSION_TEXT}" AHPV_BUILD_ID="${AHPV_BUILD_ID}")
target_compile_options(ahpv_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
target_link_libraries(ahpv_core PUBLIC Threads::Threads)

add_executable(ahpv src/app/main.cpp)
target_link_libraries(ahpv PRIVATE ahpv_core)
file(GLOB AHPV_TEST_SOURCES CONFIGURE_DEPENDS tests/unit/*.cpp)
add_executable(ahpv-unit-tests ${AHPV_TEST_SOURCES})
target_link_libraries(ahpv-unit-tests PRIVATE ahpv_core)
install(TARGETS ahpv RUNTIME DESTINATION bin)
