cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/App
    ${CMAKE_CURRENT_LIST_DIR}/Device
    ${CMAKE_CURRENT_LIST_DIR}/Middleware
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(image_sd_video)