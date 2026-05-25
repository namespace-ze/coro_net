# =============================================================================
# coro_net/test/CMakeLists.txt
# =============================================================================
# 注册所有 test_*.cc 为独立可执行文件，并加入 CTest。
# =============================================================================

# 自动发现所有 test_*.cc，每个文件编一个独立 binary
file(GLOB TEST_SOURCES test_*.cc)

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE coro_net)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
