# Include secrets
include(${CMAKE_SOURCE_DIR}/secrets.cmake OPTIONAL)

# Define defaults if not set in secrets.cmake
if(NOT DEFINED VAL_WIFI_SSID)
    set(VAL_WIFI_SSID "XplaceholderWIFISSID____________")
endif()
if(NOT DEFINED VAL_WIFI_PASSWORD)
    set(VAL_WIFI_PASSWORD "XplaceholderWIFIPASSWORD________________________________________")
endif()
if(NOT DEFINED VAL_REMOTE_URL)
    set(VAL_REMOTE_URL "XplaceholderREMOTEURL___________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________________")
endif()

file(GLOB_RECURSE MAIN_SRCS "*.cpp" "*.c")

# Conditionally include touch_control.c only for Tidbyt Gen2
if(NOT CONFIG_BOARD_TIDBYT_GEN2)
    list(REMOVE_ITEM MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/touch_control.c")
endif()

idf_component_register(SRCS ${MAIN_SRCS}
                       INCLUDE_DIRS "." "display" "webp_player" "network" "config" "system" "scheduler" "startup")

idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=esp_getaddrinfo" APPEND)

target_compile_definitions(${COMPONENT_LIB} PRIVATE 
    WIFI_SSID="${VAL_WIFI_SSID}"
    WIFI_PASSWORD="${VAL_WIFI_PASSWORD}"
    REMOTE_URL="${VAL_REMOTE_URL}"
)

if(CONFIG_NO_FAST_FUNCTIONS)
    idf_build_set_property(COMPILE_DEFINITIONS "-DNO_FAST_FUNCTIONS" APPEND)
endif()
