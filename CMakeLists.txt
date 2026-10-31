cmake_minimum_required(VERSION 3.16)
project(rv1106_tracker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)
option(RV1106_BUILD_OPENCV_BASELINE "Build the earlier OpenCV template tracker and CLI" ON)

add_library(nanotrack_core STATIC src/nanotrack_decoder.cpp src/nanotrack_session.cpp)
target_include_directories(nanotrack_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_options(nanotrack_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nanotrack_core PUBLIC Threads::Threads)

if(BUILD_TESTING)
    add_executable(nanotrack-core-test tests/native/nanotrack_core_test.cpp)
    target_link_libraries(nanotrack-core-test PRIVATE nanotrack_core)
    add_test(NAME nanotrack-core COMMAND nanotrack-core-test)
endif()

install(TARGETS nanotrack_core ARCHIVE DESTINATION lib)
install(FILES include/rv1106_tracker/nanotrack_decoder.hpp
              include/rv1106_tracker/nanotrack_session.hpp DESTINATION include/rv1106_tracker)

if(RV1106_BUILD_OPENCV_BASELINE)
find_package(OpenCV REQUIRED COMPONENTS core imgproc video videoio highgui)

add_library(tracker_core src/tracker.cpp src/config.cpp src/telemetry.cpp)
target_include_directories(tracker_core PUBLIC include)
target_link_libraries(tracker_core PUBLIC ${OpenCV_LIBS})
target_compile_options(tracker_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(rv1106-track src/main.cpp)
target_link_libraries(rv1106-track PRIVATE tracker_core ${OpenCV_LIBS})

add_executable(rv1106-evaluate src/evaluate.cpp)
target_link_libraries(rv1106-evaluate PRIVATE tracker_core ${OpenCV_LIBS})

install(TARGETS tracker_core rv1106-track rv1106-evaluate)
endif()
