add_executable(revenant_overlay_demo WIN32
    overlay.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_demo.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_impl_win32.cpp
    ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_impl_dx11.cpp
)
target_include_directories(revenant_overlay_demo PRIVATE ${PROJECT_SOURCE_DIR}/third_party/imgui)
target_link_libraries(revenant_overlay_demo PRIVATE revenant::revenant d3d11 dxgi)
target_compile_definitions(revenant_overlay_demo PRIVATE UNICODE _UNICODE)

if (REVENANT_SHARED)
    add_custom_command(TARGET revenant_overlay_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:revenant> $<TARGET_FILE_DIR:revenant_overlay_demo>)
endif()
