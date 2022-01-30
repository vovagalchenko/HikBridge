cmake_minimum_required(VERSION 3.15)
project(HikBridge)

find_package(PkgConfig)

set(CMAKE_VERBOSE_MAKEFILE on)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -v -pthread")

add_subdirectory(backward-cpp)

add_executable(HikBridge main.cpp ${BACKWARD_ENABLE})
if (DEFINED REMOTE)
    message("** Building remotely")
    target_link_libraries(HikBridge PUBLIC bfd)
    target_link_libraries(HikBridge PUBLIC dl)
    set(BACKWARD_HAS_BFD 1)

    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    target_link_libraries(HikBridge PUBLIC PkgConfig::ALSA)
endif()

add_backward(HikBridge)

target_link_directories(HikBridge PUBLIC hik-lib)
target_link_libraries(HikBridge PUBLIC hcnetsdk)

include_directories(plog/include)
include_directories(cxxopts/include)
include_directories(hik-sdk-include)
include_directories(cpp-httplib)
include_directories(backward-cpp)
include_directories(alsa-lib-1.2.6.1/include)

configure_file("BattleMulaw2.wav" "BattleMulaw2.wav" COPYONLY)
