cmake_minimum_required(VERSION 3.15)
project(HikBridge)

set(CMAKE_VERBOSE_MAKEFILE on)
set(CMAKE_CXX_STANDARD 17)

add_subdirectory(backward-cpp)

add_executable(HikBridge main.cpp ${BACKWARD_ENABLE} StateMachine.cpp StateMachine.h)
if (DEFINED REMOTE)
    message("** Building remotely")
    target_link_libraries(HikBridge PUBLIC bfd)
    target_link_libraries(HikBridge PUBLIC dl)
    set(BACKWARD_HAS_BFD 1)
endif()

add_backward(HikBridge)

target_link_directories(HikBridge PUBLIC hik-lib)
target_link_libraries(HikBridge PUBLIC hcnetsdk)

include_directories(plog/include)
include_directories(cxxopts/include)
include_directories(hik-sdk-include)
include_directories(backward-cpp)

configure_file("DiveMulaw.wav" "DiveMulaw.wav" COPYONLY)

add_definitions(-include plog/Log.h)
