cmake_minimum_required(VERSION 3.16)

project(NetStarWorkstationQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Sql WebSockets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Sql WebSockets)

if(APPLE)
    configure_file(
        packaging/macos/Info.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        @ONLY
    )
endif()

set(NETSTAR_WORKSTATION_SOURCES
    resources.qrc
    src/main.cpp
    src/app/RuntimeConfig.cpp
    src/app/RuntimeConfig.h
    src/app/UploadManager.cpp
    src/app/UploadManager.h
    src/api/ApiClient.cpp
    src/api/ApiClient.h
    src/api/WebSocketClient.cpp
    src/api/WebSocketClient.h
    src/storage/ConfigStore.cpp
    src/storage/ConfigStore.h
    src/storage/LocalDatabase.cpp
    src/storage/LocalDatabase.h
    src/system/FileHasher.cpp
    src/system/FileHasher.h
    src/system/DiagnosticExporter.cpp
    src/system/DiagnosticExporter.h
    src/system/LogManager.cpp
    src/system/LogManager.h
    src/system/SystemDiagnostics.cpp
    src/system/SystemDiagnostics.h
    src/system/SystemIdentity.cpp
    src/system/SystemIdentity.h
    src/system/WatchManager.cpp
    src/system/WatchManager.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
)

set(NETSTAR_WORKSTATION_APP_OPTIONS)
if(WIN32)
    list(APPEND NETSTAR_WORKSTATION_APP_OPTIONS WIN32)
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(netstar-workstation ${NETSTAR_WORKSTATION_APP_OPTIONS} ${NETSTAR_WORKSTATION_SOURCES})
else()
    add_executable(netstar-workstation ${NETSTAR_WORKSTATION_APP_OPTIONS} ${NETSTAR_WORKSTATION_SOURCES})
endif()

target_link_libraries(netstar-workstation
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Sql
        Qt${QT_VERSION_MAJOR}::WebSockets
)

target_include_directories(netstar-workstation PRIVATE src)

target_compile_definitions(netstar-workstation
    PRIVATE
        APP_VERSION="${PROJECT_VERSION}"
)

if(APPLE)
    set_target_properties(netstar-workstation PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "NetStar Parse Hub"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.netstar.parsehub.workstation"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
endif()

install(TARGETS netstar-workstation
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
install(FILES packaging/linux/netstar-workstation.desktop DESTINATION share/applications)

if(UNIX AND NOT APPLE)
    install(FILES assets/netstar-workstation.png
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME netstar-workstation.png
    )
endif()

set(CPACK_PACKAGE_NAME "netstar-workstation")
set(CPACK_PACKAGE_VENDOR "NetStar")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "NetStar")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    if(QT_VERSION_MAJOR EQUAL 6)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6widgets6, libqt6network6, libqt6sql6, libqt6sql6-sqlite, libqt6websockets6")
    else()
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5widgets5, libqt5network5, libqt5sql5, libqt5sql5-sqlite, libqt5websockets5")
    endif()
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
endif()
include(CPack)
