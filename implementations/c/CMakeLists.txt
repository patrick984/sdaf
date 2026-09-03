cmake_minimum_required(VERSION 3.20)
project(sdaf VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(SDAF_BUILD_TESTS "Build the C99 Unity test suite" ON)
option(SDAF_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
endif()

add_library(sdaf_c
  src/sdaf_crc32c.c
  src/sdaf_schema.c
  src/sdaf_codec.c
  src/sdaf_reader.c
  src/sdaf_writer.c)
add_library(sdaf::c ALIAS sdaf_c)
target_compile_features(sdaf_c PUBLIC c_std_99)
set_target_properties(sdaf_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(sdaf_c PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(sdaf_c PRIVATE src)

if(TARGET PkgConfig::ZSTD)
  target_compile_definitions(sdaf_c PRIVATE SDAF_HAVE_ZSTD=1)
  target_link_libraries(sdaf_c PRIVATE PkgConfig::ZSTD)
endif()

if(MSVC)
  target_compile_options(sdaf_c PRIVATE /W4 /WX)
else()
  target_compile_options(sdaf_c PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

if(SDAF_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(sdaf_c PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(sdaf_c PUBLIC -fsanitize=address,undefined)
endif()

add_executable(sdaf-c cli/main.c cli/exports.c)
target_compile_features(sdaf-c PRIVATE c_std_99)
set_target_properties(sdaf-c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(sdaf-c PRIVATE cli)
target_link_libraries(sdaf-c PRIVATE sdaf_c)
if(MSVC)
  target_compile_options(sdaf-c PRIVATE /W4 /WX)
else()
  target_compile_options(sdaf-c PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

install(TARGETS sdaf_c sdaf-c
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(SDAF_BUILD_TESTS)
  include(CTest)
  include(FetchContent)
  FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.7.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(unity)
  target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE=1)
  add_executable(sdaf_c_tests
    tests/test_main.c
    tests/test_crc.c
    tests/test_conformance.c
    tests/test_roundtrip.c
    tests/test_layout.c
    tests/test_cli.c
    cli/exports.c)
  target_compile_features(sdaf_c_tests PRIVATE c_std_99)
  set_target_properties(sdaf_c_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(sdaf_c_tests PRIVATE cli)
  target_compile_definitions(sdaf_c_tests PRIVATE SDAF_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sdaf_c_tests PRIVATE sdaf_c unity)
  if(MSVC)
    target_compile_options(sdaf_c_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(sdaf_c_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
  add_test(NAME sdaf_c_tests COMMAND sdaf_c_tests)
endif()
