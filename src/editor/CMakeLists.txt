# ---------------------------------------------------------------------------
# engine_editor — ImGui editor layer (context, SDL3 platform backend, panels).
# The SDL3 input backend is vendored (src/editor/vendor) rather than pulled via
# imgui[sdl3-binding]: that vcpkg feature re-enables SDL3's default features
# (dbus/ibus -> libsystemd -> libxcrypt), a dependency chain the engine does
# not want.
# ---------------------------------------------------------------------------
if(ENGINE_EDITOR)
    add_library(engine_editor STATIC
        editor.cpp
        scene_hierarchy.cpp
        inspector.cpp
        tuning_panels.cpp
        weather_panel.cpp
        time_of_day_panel.cpp
        asset_browser.cpp
        animation_preview.cpp
        physics_debug.cpp
        picking.cpp
        audio_inspector.cpp
        script_inspector.cpp
        animation_inspector.cpp
        camera_rig_inspector.cpp
        grass_inspector.cpp
        particle_inspector.cpp
        physics_inspector.cpp
        behavior_panel.cpp
        behavior_canvas.cpp
        inspector_world.cpp
        panels/console.cpp
        panels/inspector_parts.cpp
        panels/profiler.cpp
        shell/command_palette.cpp
        shell/commands.cpp
        shell/dialogs.cpp
        shell/editor_shell.cpp
        shell/export_dialog.cpp
        shell/fuzzy.cpp
        shell/layout.cpp
        shell/menu.cpp
        shell/panels.cpp
        shell/prefs.cpp
        shell/scene_actions.cpp
        shell/stats_panel.cpp
        shell/status_bar.cpp
        shell/toolbar.cpp
        shell/viewport.cpp
        ui/appearance.cpp
        ui/fonts.cpp
        ui/theme.cpp
        ui/toasts.cpp
        ui/widgets_asset.cpp
        ui/widgets_controls.cpp
        ui/widgets_property.cpp
        vendor/imgui_impl_sdl3.cpp)
    # engine_scene is PUBLIC: editor.hpp exposes entt entity types (selection)
    # and the hierarchy/inspector panels reflect scene components.
    # imgui-node-editor (Behaviour panel, G2) comes before imgui::imgui: its
    # vcpkg target does not declare the imgui dependency, so a single-pass
    # linker must see it first to resolve its ImGui references.
    target_link_libraries(engine_editor
        PUBLIC engine_flags engine_scene engine_script engine_project
        PRIVATE engine_editor_build
                unofficial::imgui-node-editor::imgui-node-editor
                imgui::imgui imguizmo::imguizmo SDL3::SDL3)
    target_include_directories(engine_editor PUBLIC ${ENGINE_PUBLIC_INCLUDE})
    # Inspector sections (ROADMAP S9): one file per component in inspectors/,
    # each defining register_<file stem>_inspector(InspectorRegistry&). The call
    # list is generated from the directory: a new section is a new file.
    quat_generate_registration(engine_editor
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/inspectors
        PATTERN   "*.cpp"
        TEMPLATE  ${CMAKE_CURRENT_SOURCE_DIR}/inspectors.cpp.in
        OUTPUT    ${CMAKE_CURRENT_BINARY_DIR}/generated/inspectors.cpp
        DECLARE   "void register_@NAME@_inspector(InspectorRegistry& registry)"
        CALL      "    register_@NAME@_inspector(registry)")
    # Vendored third-party backend: not warning-clean under our strict flags.
    set_source_files_properties(vendor/imgui_impl_sdl3.cpp
        PROPERTIES COMPILE_OPTIONS "${ENGINE_THIRD_PARTY_TU_FLAGS}")
endif()
