cmake_minimum_required(VERSION 3.20)

project(hbt VERSION 0.1.0 DESCRIPTION "C++20 port of HftBacktest" LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(HBT_BUILD_TESTS "Build core and regression tests" ${PROJECT_IS_TOP_LEVEL})
option(HBT_BUILD_EXAMPLES "Build deterministic examples" ${PROJECT_IS_TOP_LEVEL})
option(HBT_BUILD_BENCHMARKS "Build replay benchmarks" OFF)
option(HBT_NATIVE_ARCH "Optimize for the build machine's CPU" OFF)
option(HBT_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

add_library(hbt INTERFACE)
add_library(hbt::hbt ALIAS hbt)
target_compile_features(hbt INTERFACE cxx_std_20)
target_include_directories(hbt INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(ZLIB REQUIRED)
add_library(hbt_io STATIC src/npy.cpp)
add_library(hbt::io ALIAS hbt_io)
set_target_properties(hbt_io PROPERTIES OUTPUT_NAME hbt_io EXPORT_NAME io)
set_target_properties(hbt PROPERTIES EXPORT_NAME hbt)
target_link_libraries(hbt_io PUBLIC hbt ZLIB::ZLIB)

set(_hbt_warning_flags)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  list(APPEND _hbt_warning_flags -Wall -Wextra -Wpedantic)
  if(HBT_WARNINGS_AS_ERRORS)
    list(APPEND _hbt_warning_flags -Werror)
  endif()
  if(HBT_NATIVE_ARCH)
    target_compile_options(hbt_io PRIVATE -march=native)
  endif()
elseif(MSVC)
  list(APPEND _hbt_warning_flags /W4)
  if(HBT_WARNINGS_AS_ERRORS)
    list(APPEND _hbt_warning_flags /WX)
  endif()
endif()
target_compile_options(hbt_io PRIVATE ${_hbt_warning_flags})

if(HBT_BUILD_EXAMPLES)
  add_executable(hbt_market_making examples/market_making.cpp)
  target_link_libraries(hbt_market_making PRIVATE hbt::hbt hbt::io)
  target_compile_options(hbt_market_making PRIVATE ${_hbt_warning_flags})
endif()

if(HBT_BUILD_BENCHMARKS)
  add_executable(hbt_replay_benchmark benchmarks/replay.cpp)
  target_link_libraries(hbt_replay_benchmark PRIVATE hbt::hbt)
  target_compile_options(hbt_replay_benchmark PRIVATE ${_hbt_warning_flags})
  if(HBT_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hbt_replay_benchmark PRIVATE -march=native)
  endif()
endif()

if(HBT_BUILD_TESTS)
  include(CTest)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  add_executable(hbt_tests
    tests/test_types.cpp
    tests/test_depth.cpp
    tests/test_models.cpp
    tests/test_npy.cpp
    tests/test_reader.cpp
    tests/test_order_bus.cpp
    tests/test_processors.cpp
    tests/test_backtest.cpp)
  target_link_libraries(hbt_tests PRIVATE hbt::hbt hbt::io GTest::gtest_main)
  target_compile_options(hbt_tests PRIVATE ${_hbt_warning_flags})
  gtest_discover_tests(hbt_tests)

  if(HBT_BUILD_EXAMPLES)
    add_test(NAME example.market_making COMMAND hbt_market_making)
    set_tests_properties(example.market_making PROPERTIES
      PASS_REGULAR_EXPRESSION "fills=2 position=0.000 balance=0.200 fee=0.020")
  endif()
endif()

install(TARGETS hbt hbt_io EXPORT hbtTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT hbtTargets NAMESPACE hbt:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hbt)

configure_package_config_file(cmake/hbtConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/hbtConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hbt)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/hbtConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/hbtConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/hbtConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hbt)

set(CPACK_PACKAGE_NAME hftbacktest-cpp)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR TGZ ZIP)
set(CPACK_SOURCE_GENERATOR TGZ ZIP)
set(CPACK_SOURCE_IGNORE_FILES
  "/build[^/]*/"
  "/stage/"
  "/consumer-build/"
  "/.cache/"
  "/.git/"
  "~$")
include(CPack)
