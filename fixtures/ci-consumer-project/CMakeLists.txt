cmake_minimum_required(VERSION 3.16)
project(ci_consumer_project C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(ci_consumer
    src/main.c
)
