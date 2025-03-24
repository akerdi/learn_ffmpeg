cmake_minimum_required(VERSION 3.0)

project(01)

set(CMAKE_C_FLAGS "-std=c11")

find_package(SDL2 REQUIRED)

# # Add log messages
message ([STATUS] "SDL2_INCLUDE_DIR" ${SDL2_INCLUDE_DIRS})
message ([STATUS] "SDL2_LIBRARYS" ${SDL2_LIBRARIES})
message ([STATUS] "SDL2_VERSION" ${SDL2_VERSION})

message([STATUS] "CMAKE_SYSTEM_NAME" ${CMAKE_SYSTEM_NAME})

add_definitions("-Wno-invalid-source-encoding")
add_definitions("-O2")

add_subdirectory(tutorial01)
