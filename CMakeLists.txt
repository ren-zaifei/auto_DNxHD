cmake_minimum_required(VERSION 3.25)
project(auto_DNxHD VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

add_executable(auto_DNxHD
        main.cpp
        transcoder.cpp
)

target_link_libraries(auto_DNxHD
        PRIVATE
        nlohmann_json::nlohmann_json
        ${FFMPEG_LIBRARIES}
)

target_include_directories(auto_DNxHD
        PRIVATE ${FFMPEG_INCLUDE_DIRS}
)

include(GNUInstallDirs)

install(TARGETS auto_DNxHD
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
        README.md
        config.example.json
        LICENSE
        DESTINATION .
)

set(CPACK_PACKAGE_NAME "auto-DNxHD")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "TGZ")

include(CPack)
