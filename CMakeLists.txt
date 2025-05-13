cmake_minimum_required(VERSION 3.5)
set(PROJECT_VER "1.17")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")

set(CMAKE_CXX_STANDARD 20)
add_compile_options(-fdiagnostics-color=always)
idf_build_set_property(MINIMAL_BUILD ON)
project(kleutertour_v1_pcb)