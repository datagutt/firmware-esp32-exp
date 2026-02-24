cmake_minimum_required(VERSION 3.16.0)

# Generate secrets.cmake from secrets.json if it exists
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/generate_secrets_cmake.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)

# Generate LittleFS image from webui/ directory and flash it to the "webui" partition.
# The webui/ directory must exist (even if empty) for this to work.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webui")
    littlefs_create_partition_image(webui webui FLASH_IN_PROJECT)
endif()
