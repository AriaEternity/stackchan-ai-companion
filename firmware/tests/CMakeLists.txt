cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(companion_safety_test
    companion_safety_test.cpp
    ../main/companion/safety_limits.cpp
)

target_include_directories(companion_safety_test PRIVATE
    ../main
)

target_compile_features(companion_safety_test PRIVATE cxx_std_17)

add_test(NAME companion_safety_test COMMAND $<TARGET_FILE:companion_safety_test>)

if(NOT DEFINED PROTOCOL_EXAMPLES_DIR)
    message(FATAL_ERROR "PROTOCOL_EXAMPLES_DIR is required for shared protocol contract tests")
endif()
if(NOT DEFINED PROTOCOL_V2_EXAMPLES_DIR)
    message(FATAL_ERROR "PROTOCOL_V2_EXAMPLES_DIR is required for shared protocol v2 contract tests")
endif()

add_executable(protocol_v1_test
    protocol_v1_test.cpp
    ../main/companion/protocol_v1.cpp
)

target_include_directories(protocol_v1_test PRIVATE
    ../main
)

target_compile_features(protocol_v1_test PRIVATE cxx_std_17)
target_compile_definitions(protocol_v1_test PRIVATE
    PROTOCOL_EXAMPLES_DIR="${PROTOCOL_EXAMPLES_DIR}"
)

add_test(NAME protocol_v1_test COMMAND $<TARGET_FILE:protocol_v1_test>)

add_executable(protocol_v2_test
    protocol_v2_test.cpp
    ../main/companion/protocol_v2.cpp
)

target_include_directories(protocol_v2_test PRIVATE
    ../main
)

target_compile_features(protocol_v2_test PRIVATE cxx_std_17)
target_compile_definitions(protocol_v2_test PRIVATE
    PROTOCOL_V2_EXAMPLES_DIR="${PROTOCOL_V2_EXAMPLES_DIR}"
)

add_test(NAME protocol_v2_test COMMAND $<TARGET_FILE:protocol_v2_test>)

add_executable(companion_config_test
    companion_config_test.cpp
    ../main/companion/companion_config.cpp
)

target_include_directories(companion_config_test PRIVATE
    ../main
)

target_compile_features(companion_config_test PRIVATE cxx_std_17)
target_compile_definitions(companion_config_test PRIVATE
    COMPANION_HOST_TEST=1
)

add_test(NAME companion_config_test COMMAND $<TARGET_FILE:companion_config_test>)

add_executable(connection_state_test
    connection_state_test.cpp
    ../main/companion/connection_state.cpp
    ../main/companion/protocol_v2.cpp
)

target_include_directories(connection_state_test PRIVATE
    ../main
)

target_compile_features(connection_state_test PRIVATE cxx_std_17)

add_test(NAME connection_state_test COMMAND $<TARGET_FILE:connection_state_test>)

add_executable(websocket_policy_test
    websocket_policy_test.cpp
    ../main/companion/websocket_policy.cpp
)

target_include_directories(websocket_policy_test PRIVATE
    ../main
)

target_compile_features(websocket_policy_test PRIVATE cxx_std_17)

add_test(NAME websocket_policy_test COMMAND $<TARGET_FILE:websocket_policy_test>)

add_executable(command_scheduler_test
    command_scheduler_test.cpp
    ../main/companion/command_scheduler.cpp
    ../main/companion/protocol_v2.cpp
)

target_include_directories(command_scheduler_test PRIVATE
    ../main
)

target_compile_features(command_scheduler_test PRIVATE cxx_std_17)

add_test(NAME command_scheduler_test COMMAND $<TARGET_FILE:command_scheduler_test>)

add_executable(motion_safety_store_test
    motion_safety_store_test.cpp
    ../main/companion/motion_safety_store.cpp
    ../main/companion/command_scheduler.cpp
    ../main/companion/protocol_v2.cpp
)

target_include_directories(motion_safety_store_test PRIVATE
    ../main
)

target_compile_features(motion_safety_store_test PRIVATE cxx_std_17)
target_compile_definitions(motion_safety_store_test PRIVATE
    COMPANION_HOST_TEST=1
)

add_test(NAME motion_safety_store_test COMMAND $<TARGET_FILE:motion_safety_store_test>)

add_executable(motion_safety_plan_test
    motion_safety_plan_test.cpp
)

target_include_directories(motion_safety_plan_test PRIVATE
    ../main
)

target_compile_features(motion_safety_plan_test PRIVATE cxx_std_17)

add_test(NAME motion_safety_plan_test COMMAND $<TARGET_FILE:motion_safety_plan_test>)

add_executable(touch_session_test
    touch_session_test.cpp
    ../main/companion/touch_session.cpp
)

target_include_directories(touch_session_test PRIVATE
    ../main
)

target_compile_features(touch_session_test PRIVATE cxx_std_17)

add_test(NAME touch_session_test COMMAND $<TARGET_FILE:touch_session_test>)

add_executable(provisioning_frame_test
    provisioning_frame_test.cpp
    ../main/companion/provisioning_frame.cpp
    ../main/companion/companion_config.cpp
)

target_include_directories(provisioning_frame_test PRIVATE
    ../main
)

target_compile_features(provisioning_frame_test PRIVATE cxx_std_17)
target_compile_definitions(provisioning_frame_test PRIVATE
    COMPANION_HOST_TEST=1
)

add_test(NAME provisioning_frame_test COMMAND $<TARGET_FILE:provisioning_frame_test>)

add_executable(screen_layout_policy_test
    screen_layout_policy_test.cpp
)

target_include_directories(screen_layout_policy_test PRIVATE
    ../main
)

target_compile_features(screen_layout_policy_test PRIVATE cxx_std_17)

add_test(NAME screen_layout_policy_test COMMAND $<TARGET_FILE:screen_layout_policy_test>)
