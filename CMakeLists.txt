cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(DataLogger)

# Suppress common warnings in ESP-IDF 5.5.0
add_compile_options(-Wno-missing-field-initializers)
