set(UBALLOC_UNIT_TEST_TARGETS
    test_issue1 test_issue2 test_issue4 test_issue5 test_issue6 test_issue7
    test_growth
    test_affinity
    test_huge_cross_free
    test_multi_proc_per_node
    test_reclaim
    test_heartbeat
    test_checksum
    test_recover_state
    test_size_routing
    test_introspection
    test_defrag_hints
    test_ar01_va_layout
    test_ar02_bootstrap_pid
    test_ar03_lazy_init
    test_ar04_explicit_init
    test_ar05_size_classes
    test_ar06_realloc
    test_ar07_heap_growth
    test_ar08_remote_attach
    test_ar09_publish_lookup
    test_ar10_publishing_malloc
    test_ar11_stl_containers
    test_ar12_sigsegv_handler
    test_ar13_userfaultfd
    test_ar14_ubse_provider
    test_ar15_cleanup)

foreach(_target ${UBALLOC_UNIT_TEST_TARGETS})
    add_executable(${_target} ${_target}.cpp)
    target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${_target} PRIVATE uballoc rt pthread)
endforeach()

foreach(_target ${UBALLOC_UNIT_TEST_TARGETS})
    add_test(NAME ${_target} COMMAND ${_target})
endforeach()
