# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

add_compile_options(-fdiagnostics-color=always)

# ESP-WHO (release/v1.1.0, with its esp-dl submodule) supplies the camera pin
# configuration and the face-detection models. It is a git submodule in
# ./esp-who; ESP_WHO_PATH can point at a different checkout instead.
# (PROJECT_DIR is not defined until project() runs, so it cannot be used here.)
if(DEFINED ENV{ESP_WHO_PATH})
    set(ESP_WHO_PATH "$ENV{ESP_WHO_PATH}")
else()
    set(ESP_WHO_PATH "${CMAKE_CURRENT_LIST_DIR}/esp-who")
endif()
if(NOT EXISTS "${ESP_WHO_PATH}/components/modules/CMakeLists.txt" OR
   NOT EXISTS "${ESP_WHO_PATH}/components/esp-dl/CMakeLists.txt")
    message(FATAL_ERROR
        "ESP-WHO release/v1.1.0 not found at ${ESP_WHO_PATH}. From the project folder run:\n"
        "  git submodule update --init --recursive esp-who\n"
        "(the newer ESP-WHO master branch does not work with this project).")
endif()
set(EXTRA_COMPONENT_DIRS "${ESP_WHO_PATH}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(backseat-minder)
