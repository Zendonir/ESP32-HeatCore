# Host build of the platform-independent HeatCore core and its test suite.
# The ESP32-S3 firmware itself is built with PlatformIO (see platformio.ini).
cmake_minimum_required(VERSION 3.16)
project(heatcore_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(HC_SANITIZE "Build tests with address/undefined sanitizers" ON)

# ArduinoJson is header-only. Use a local checkout if provided, else fetch it.
set(ARDUINOJSON_DIR "" CACHE PATH "Path to an ArduinoJson checkout (contains src/ArduinoJson.h)")
if(ARDUINOJSON_DIR)
  set(ARDUINOJSON_INCLUDE "${ARDUINOJSON_DIR}/src")
else()
  include(FetchContent)
  FetchContent_Declare(ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(ArduinoJson)
  if(NOT arduinojson_POPULATED)
    FetchContent_Populate(ArduinoJson)
  endif()
  set(ARDUINOJSON_INCLUDE "${arduinojson_SOURCE_DIR}/src")
endif()

file(GLOB_RECURSE HC_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/heatcore/src/*.cpp)
add_library(heatcore_core STATIC ${HC_CORE_SOURCES})
target_include_directories(heatcore_core PUBLIC ${CMAKE_SOURCE_DIR}/lib/heatcore/src ${ARDUINOJSON_INCLUDE})
target_compile_options(heatcore_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(heatcore_core PUBLIC Threads::Threads)

file(GLOB HC_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/host/*.cpp)
add_executable(heatcore_tests ${HC_TEST_SOURCES})
target_link_libraries(heatcore_tests PRIVATE heatcore_core)
target_compile_options(heatcore_tests PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)


enable_testing()
add_test(NAME heatcore_tests COMMAND heatcore_tests)

# Host simulator serving the real web UI against the simulated core
add_executable(heatcore_sim tools/sim/sim_server.cpp)
target_link_libraries(heatcore_sim PRIVATE heatcore_core)

if(HC_SANITIZE)
  foreach(t heatcore_core heatcore_tests heatcore_sim)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()
