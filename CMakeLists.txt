cmake_minimum_required(VERSION 3.16)
project(gbemu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)

add_executable(gbemu src/main.cpp)

target_link_libraries(gbemu PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
)

# Tests are a second, independent program: gbemu is not linked against them and their
# failure cannot break the emulator build. Assert-based, so keep them out of Release.
enable_testing()

add_executable(cpu_tests tests/cpu_tests.cpp)
add_test(NAME cpu_tests COMMAND cpu_tests)