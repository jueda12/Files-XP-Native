add_executable(FilesXPNativeAccessibilityProbe
    native_accessibility_probe.cpp)

target_compile_features(FilesXPNativeAccessibilityProbe PRIVATE cxx_std_20)
target_compile_definitions(FilesXPNativeAccessibilityProbe PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

if(MSVC)
    target_compile_options(FilesXPNativeAccessibilityProbe PRIVATE
        /W4
        /WX
        /permissive-
        /EHsc
        /Zc:__cplusplus
        /utf-8
        $<$<CONFIG:Release>:/O2>)
endif()

target_link_libraries(FilesXPNativeAccessibilityProbe PRIVATE ole32 oleacc oleaut32)
