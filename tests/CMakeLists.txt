add_executable(revenant_tests tests.cpp)
target_link_libraries(revenant_tests PRIVATE revenant::revenant)
if (MSVC)
    # + function padding give the test functions an MSVC hot-patchable layout
    target_compile_options(revenant_tests PRIVATE /W4 /permissive- /utf-8)
    target_link_options(revenant_tests PRIVATE /FUNCTIONPADMIN:6 /INCREMENTAL:NO /OPT:NOICF)
endif()
add_test(NAME revenant_tests COMMAND revenant_tests)

add_executable(revenant_lde_tests lde_tests.cpp ${PROJECT_SOURCE_DIR}/src/lde/rvlde.cpp)
target_include_directories(revenant_lde_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(revenant_lde_tests PRIVATE cxx_std_20)
if (MSVC)
    target_compile_options(revenant_lde_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME revenant_lde_tests COMMAND revenant_lde_tests)

if (REVENANT_SHARED)
    add_custom_command(TARGET revenant_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:revenant> $<TARGET_FILE_DIR:revenant_tests>)
endif()
