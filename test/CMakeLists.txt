cmake_minimum_required(VERSION 3.25...3.29)

project(squiz_tests LANGUAGES CXX)

# ---- Options ----

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage("gh:doctest/doctest@2.4.9")
CPMAddPackage("gh:TheLartians/Format.cmake@1.7.3")

if(TEST_INSTALLED_VERSION)
  find_package(Squiz REQUIRED)
else()
  CPMAddPackage(NAME Squiz SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

# ---- Create binary ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)
file(GLOB linux_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/linux/*.cpp)
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_executable(${PROJECT_NAME} ${sources} ${linux_sources})
else()
  add_executable(${PROJECT_NAME} ${sources})
endif()
target_link_libraries(${PROJECT_NAME} doctest::doctest Squiz::Squiz)
target_link_libraries(${PROJECT_NAME} Squiz::Squiz)
# set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 26)

# enable compiler warnings
if(NOT TEST_INSTALLED_VERSION)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(Squiz PUBLIC -Wall -Wpedantic -Wextra -Werror)
  elseif(MSVC)
    target_compile_options(Squiz PUBLIC /W4 /WX)
    target_compile_definitions(${PROJECT_NAME} PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
  endif()
endif()

# ---- Add Squiz tests ----

enable_testing()

# Note: doctest and similar testing frameworks can automatically configure CMake tests. For other
# testing frameworks add the tests target instead: add_test(NAME ${PROJECT_NAME} COMMAND
# ${PROJECT_NAME})

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(${PROJECT_NAME})

# ---- code coverage ----

if(ENABLE_TEST_COVERAGE)
  target_compile_options(Squiz PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(Squiz PUBLIC -fprofile-arcs -ftest-coverage)
endif()
