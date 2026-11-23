cmake_minimum_required(VERSION 3.22)

# Point QJS_ESP_DIR at an esp-quickjs-ng checkout (with its quickjs submodule)
if(NOT DEFINED ENV{QJS_ESP_DIR})
    message(FATAL_ERROR "set QJS_ESP_DIR to your esp-quickjs-ng checkout")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.." "$ENV{QJS_ESP_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tvg_quickjs)
