cmake_minimum_required(VERSION 3.22)
project(dicewatch VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(dicewatch_compile_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /guard:cf)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fstack-protector-strong)
  endif()
endfunction()

add_library(dicewatch_rounds STATIC src/rounds.cpp)
set_target_properties(dicewatch_rounds PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(dicewatch_rounds PUBLIC src)
dicewatch_compile_options(dicewatch_rounds)

add_library(dicewatch SHARED src/dicewatch.cpp)
set_target_properties(dicewatch PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(dicewatch SYSTEM PRIVATE vendor/hailstone)
target_link_libraries(dicewatch PRIVATE dicewatch_rounds)
dicewatch_compile_options(dicewatch)
if(MSVC)
  target_link_options(dicewatch PRIVATE /GUARD:CF)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(dicewatch PRIVATE -Wl,-z,relro,-z,now,-z,noexecstack)
endif()

install(TARGETS dicewatch LIBRARY DESTINATION dicewatch RUNTIME DESTINATION dicewatch)
install(DIRECTORY ui DESTINATION dicewatch)
install(FILES LICENSE DESTINATION dicewatch RENAME LICENSE.txt)
install(FILES vendor/hailstone/LICENSE DESTINATION dicewatch RENAME LICENSE-Hailstone-SDK.txt)

if(BUILD_TESTING)
  add_executable(dicewatch_rounds_test tests/rounds_test.cpp)
  target_link_libraries(dicewatch_rounds_test PRIVATE dicewatch_rounds)
  dicewatch_compile_options(dicewatch_rounds_test)
  add_test(NAME dicewatch.rounds COMMAND dicewatch_rounds_test)

  add_executable(dicewatch_host_test tests/host_test.cpp)
  target_include_directories(dicewatch_host_test SYSTEM PRIVATE vendor/hailstone)
  target_link_libraries(dicewatch_host_test PRIVATE dicewatch)
  dicewatch_compile_options(dicewatch_host_test)
  add_test(NAME dicewatch.host COMMAND dicewatch_host_test)

  find_program(DICEWATCH_LUAC NAMES luac5.4 luac54 luac)
  if(DICEWATCH_LUAC)
    foreach(interface modern gamepad)
      add_test(NAME dicewatch.lua.${interface} COMMAND "${DICEWATCH_LUAC}" -p
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/${interface}/dicewatch.lua")
    endforeach()
  endif()
endif()
