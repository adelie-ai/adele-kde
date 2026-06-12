cmake_minimum_required(VERSION 3.20)
project(desktop_assistant_kcm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_DEFAULT_MAJOR_VERSION 6)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries" FORCE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" CACHE PATH "Library output directory" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Network Qml)
find_package(KF6 REQUIRED COMPONENTS CoreAddons KCMUtils)

add_definitions(-DTRANSLATION_DOMAIN="kcm_desktopassistant")

kcmutils_add_qml_kcm(kcm_desktopassistant
    SOURCES
        desktopassistantkcm.cpp
        daemonreply.cpp
        voiceconfig.cpp
    INSTALL_NAMESPACE
        plasma/kcms/systemsettings
)

install(FILES settings-applications-desktopassistant.desktop DESTINATION ${KDE_INSTALL_DATADIR}/systemsettings/categories)

target_link_libraries(kcm_desktopassistant
    PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Network
        Qt6::Qml
        KF6::CoreAddons
        KF6::KCMUtils
)

# --- C++ unit tests (KDE-2 / #57) --------------------------------------------
# adele-kde's first C++ QtTest target. Covers the bus-free daemon-reply parsing
# helpers (daemonreply.cpp) that the async D-Bus call path is built on; the live
# D-Bus path itself needs a session bus + System Settings host and is verified
# by manual QA. Built by default (the local `just check` gate compiles it); make
# it optional only if a downstream packaging build wants to skip tests.
option(BUILD_KCM_TESTS "Build the KCM C++ unit tests" ON)
if(BUILD_KCM_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    add_executable(tst_daemonreply
        tst_daemonreply.cpp
        daemonreply.cpp
    )
    target_link_libraries(tst_daemonreply
        PRIVATE
            Qt6::Core
            Qt6::Test
    )
    add_test(NAME tst_daemonreply COMMAND tst_daemonreply)

    # Voice-config TOML helpers (KDE-6 #61 / KDE-7 #62): inline-comment stripping
    # on read + surgical merge that preserves inline comments / unknown keys on
    # write. Pure, file-free (voiceconfig.cpp), so covered without a filesystem.
    add_executable(tst_voiceconfig
        tst_voiceconfig.cpp
        voiceconfig.cpp
    )
    target_link_libraries(tst_voiceconfig
        PRIVATE
            Qt6::Core
            Qt6::Test
    )
    add_test(NAME tst_voiceconfig COMMAND tst_voiceconfig)
endif()
