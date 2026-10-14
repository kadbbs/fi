cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(fi LANGUAGES CXX)

add_executable(fi fi.cpp)
target_compile_features(fi PRIVATE cxx_std_17)
target_compile_options(fi PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
