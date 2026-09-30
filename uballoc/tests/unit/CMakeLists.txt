set(UBALLOC_UNIT_TEST_TARGETS
    test_issue1 test_issue2 test_issue4 test_issue5 test_issue6 test_issue7
    test_growth
    test_affinity
    test_huge_cross_free
    test_multi_proc_per_node
    test_reclaim
    test_size_routing
    test_introspection
    test_defrag_hints)

foreach(_target ${UBALLOC_UNIT_TEST_TARGETS})
    add_executable(${_target} ${_target}.cpp)
    target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${_target} PRIVATE uballoc rt pthread)
endforeach()

foreach(_target ${UBALLOC_UNIT_TEST_TARGETS})
    add_test(NAME ${_target} COMMAND ${_target})
endforeach()
