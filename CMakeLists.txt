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
# Skip on boards whose partition table has no "webui" partition (e.g. 4 MB devices).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webui")
    partition_table_get_partition_info(webui_size "--partition-name webui" "size")
    if(webui_size)
        littlefs_create_partition_image(webui webui FLASH_IN_PROJECT)
    else()
        message(STATUS "No 'webui' partition in partition table — skipping LittleFS image")
    endif()
endif()
