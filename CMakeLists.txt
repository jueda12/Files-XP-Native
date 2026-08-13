cmake_minimum_required(VERSION 3.28)

project(FilesXPNative VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Files XP Native targets Windows 10 and later. Run the portable tests directly on other platforms.")
endif()

add_subdirectory(src/app)

