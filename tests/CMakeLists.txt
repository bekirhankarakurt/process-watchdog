set(PW_UNIT_SUITES util ini config restart heartbeat rotating_log process control)

foreach(suite IN LISTS PW_UNIT_SUITES)
    add_executable(test_${suite} unit/test_${suite}.cpp)
    target_include_directories(test_${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_${suite} PRIVATE pwcore)
    add_test(NAME unit.${suite} COMMAND test_${suite})
    set_tests_properties(unit.${suite} PROPERTIES TIMEOUT 30 LABELS unit)
endforeach()

# Deliberately misbehaving child program used by the integration tests.
add_executable(pw-crasher helpers/crasher.cpp)
target_link_libraries(pw-crasher PRIVATE pwcore)

add_executable(integration_tests integration/integration_tests.cpp)
target_include_directories(integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(integration_tests PRIVATE pwcore)

set(PW_INTEGRATION_SCENARIOS
    crash_loop_gives_up
    clean_exit_not_restarted
    always_policy_restarts_clean_exit
    segfault_is_reported
    backoff_grows_between_restarts
    heartbeat_keeps_service_alive
    heartbeat_timeout_restarts
    heartbeat_wrong_token_ignored
    heartbeat_over_udp
    stop_escalates_to_sigkill
    control_start_stop_restart
    control_rejects_bad_requests
    log_capture_and_rotation
    resource_limits_applied
    missing_binary_fails
    shutdown_stops_children
    pwctl_cli)

foreach(scenario IN LISTS PW_INTEGRATION_SCENARIOS)
    add_test(NAME integration.${scenario}
             COMMAND integration_tests ${scenario} $<TARGET_FILE:pwatchd> $<TARGET_FILE:pw-crasher>
                     $<TARGET_FILE:pwctl>)
    set_tests_properties(integration.${scenario} PROPERTIES TIMEOUT 60 LABELS integration)
endforeach()
