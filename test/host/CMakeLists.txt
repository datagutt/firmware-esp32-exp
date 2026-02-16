cmake_minimum_required(VERSION 3.16)
project(firmware_host_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(host_unit_tests
  test_unit.cpp
  ../../main/system/ota_url_utils.cpp
  ../../main/scheduler/scheduler_fsm.cpp
  ../../main/network/config_contract.cpp
)

target_include_directories(host_unit_tests PRIVATE
  ../../main/system
  ../../main/scheduler
  ../../main/network
)

add_executable(host_json_fuzz
  fuzz_json_handlers.cpp
  ../../main/network/api_validation.cpp
  ../../managed_components/espressif__cjson/cJSON/cJSON.c
)

target_include_directories(host_json_fuzz PRIVATE
  ../../main/network
  ../../managed_components/espressif__cjson/cJSON
)
