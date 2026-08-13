add_executable(FilesXPNative WIN32
    main.cpp
    archive_worker.cpp
    archive_worker.h
    bulk_rename_worker.cpp
    bulk_rename_worker.h
    app_window.cpp
    app_window.h
    explorer_browser_host.cpp
    explorer_browser_host.h
    flatten_worker.cpp
    flatten_worker.h
    folder_selection_worker.cpp
    folder_selection_worker.h
    ftp_worker.cpp
    ftp_worker.h
    file_operation_progress_sink.h
    git_worker.cpp
    git_worker.h
    localization.cpp
    localization.h
    preview_worker.cpp
    preview_worker.h
    search_worker.cpp
    search_worker.h
    session_store.cpp
    session_store.h
    settings_store.cpp
    settings_store.h
    shell_operation_worker.cpp
    shell_operation_worker.h
    shell_artifact_worker.cpp
    shell_artifact_worker.h
    tag_worker.cpp
    tag_worker.h
    xp_theme.cpp
    xp_theme.h
    app.rc
    app.manifest
    resource.h
    assets/app.ico
    assets/app-icon.svg
    assets/Square16x16Logo.png
    assets/Square44x44Logo.png
    assets/Square150x150Logo.png
    assets/Square256x256Logo.png
    assets/StoreLogo.png
    ../core/bounded_queue.h
    ../core/batch_cursor.h
    ../core/bulk_rename_request.h
    ../core/archive_request.h
    ../core/command_match.h
    ../core/copydata_path.h
    ../core/generation_gate.h
    ../core/git_policy.h
    ../core/git_status.h
    ../core/filename_policy.h
    ../core/flatten_policy.h
    ../core/folder_selection_request.h
    ../core/ftp_request.h
    ../core/localization_policy.h
    ../core/locale_pack.h
    ../core/natural_order.h
    ../core/preview_policy.h
    ../core/preview_provider.h
    ../core/preview_queue.h
    ../core/selection_inversion.h
    ../core/search_request.h
    ../core/session_codec.h
    ../core/settings_codec.h
    ../core/shell_artifact_request.h
    ../core/shell_operation_request.h
    ../core/shortcut_map.h
    ../core/tag_codec.h
    ../core/tag_color.h
    ../core/tag_identity.h
    ../core/tag_request.h
    ../core/tag_result_codec.h
    ../core/windows_command_line.h)

target_compile_features(FilesXPNative PRIVATE cxx_std_20)
target_compile_definitions(FilesXPNative PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

if(MSVC)
    target_compile_options(FilesXPNative PRIVATE
        /W4
        /permissive-
        /EHsc
        /guard:cf
        /sdl
        /Zc:__cplusplus
        /utf-8
        $<$<CONFIG:Release>:/O2>)
    target_link_options(FilesXPNative PRIVATE
        /DYNAMICBASE
        /HIGHENTROPYVA
        /NXCOMPAT
        /guard:cf
        /CETCOMPAT
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>)
endif()

target_link_libraries(FilesXPNative PRIVATE
    comctl32
    ole32
    shell32
    shlwapi
    msimg32
    uuid)
target_link_libraries(FilesXPNative PRIVATE propsys mpr oleacc)
target_link_libraries(FilesXPNative PRIVATE advapi32)

set_target_properties(FilesXPNative PROPERTIES
    OUTPUT_NAME "FilesXPNative"
    VS_GLOBAL_EnableMicrosoftCodeAnalysis true)

add_custom_command(TARGET FilesXPNative POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Locales" "$<TARGET_FILE_DIR:FilesXPNative>/Locales")

