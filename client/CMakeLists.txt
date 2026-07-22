cmake_minimum_required(VERSION 3.20)
project(desktop_assistant_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_DEFAULT_MAJOR_VERSION 6)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 REQUIRED COMPONENTS Core Qml DBus)
# KF6::ConfigCore backs the client-context opt-out (#549): AdeleCore reads the
# "share device info" preference the KCM persists to a KConfig file.
find_package(KF6 REQUIRED COMPONENTS Config)
find_package(Threads REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

# --- Locate + build the Rust core (client-ui-common/ffi = client-ui-ffi) ------
# The native core lives in the client-ui-common repo: it wraps that crate's
# shared reducer + a client-common Connector in D-Bus mode. Default to the
# standard sibling-checkout layout — from <adele-kde>/client that is
# <adele-kde>/../../client-ui-common — and allow an override for worktrees
# (point it at the checkout that has the `ffi` crate, e.g. a feature worktree
# before the crate lands on main) or for packaging.
set(CLIENT_UI_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../client-ui-common"
    CACHE PATH "Path to the client-ui-common checkout (must contain ffi/Cargo.toml)")
get_filename_component(CLIENT_UI_COMMON_DIR "${CLIENT_UI_COMMON_DIR}" ABSOLUTE)

set(FFI_CRATE_DIR "${CLIENT_UI_COMMON_DIR}/ffi")
if(NOT EXISTS "${FFI_CRATE_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "client-ui-ffi crate not found at ${FFI_CRATE_DIR}.\n"
        "Set -DCLIENT_UI_COMMON_DIR=/path/to/client-ui-common "
        "(it must contain ffi/Cargo.toml).")
endif()

find_program(CARGO_EXECUTABLE cargo REQUIRED)

# CMAKE_BUILD_TYPE -> cargo profile. Anything non-Debug builds the release cdylib.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CARGO_PROFILE_DIR "debug")
    set(CARGO_BUILD_FLAGS "")
else()
    set(CARGO_PROFILE_DIR "release")
    set(CARGO_BUILD_FLAGS "--release")
endif()

set(CORE_CDYLIB "${CLIENT_UI_COMMON_DIR}/target/${CARGO_PROFILE_DIR}/libadele_client_core.so")
set(CORE_HEADER_DIR "${FFI_CRATE_DIR}/include")

# Build the self-contained cdylib (all Rust deps link statically into it; build.rs
# also (re)generates the C header). Always invoked — cargo is incremental, so this
# is a no-op when nothing changed and picks up reducer/transport changes when they
# do.
add_custom_target(adele_core_build
    BYPRODUCTS "${CORE_CDYLIB}"
    COMMAND ${CARGO_EXECUTABLE} build ${CARGO_BUILD_FLAGS} -p client-ui-ffi
    WORKING_DIRECTORY "${CLIENT_UI_COMMON_DIR}"
    COMMENT "Building the Adelie client core (libadele_client_core, ${CARGO_PROFILE_DIR})"
    VERBATIM
    USES_TERMINAL
)

# The cdylib is a complete shared object, so linking it needs no Rust-runtime
# native-lib list — it resolves its own deps. Consumers find it at runtime via an
# $ORIGIN RPATH (it installs next to the plugin).
add_library(adele_core SHARED IMPORTED GLOBAL)
set_target_properties(adele_core PROPERTIES IMPORTED_LOCATION "${CORE_CDYLIB}")
target_include_directories(adele_core INTERFACE "${CORE_HEADER_DIR}")

# --- The QML module: org.desktopassistant.client -----------------------------
# PLUGIN_TARGET == target merges backing library + plugin into one .so (the
# module is pure C++ types, no .qml files), so deployment is a single plugin.
qt_add_qml_module(adelecore
    URI org.desktopassistant.client
    VERSION 1.0
    PLUGIN_TARGET adelecore
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml/org/desktopassistant/client"
    SOURCES
        adelecore.cpp
        adelecore.h
        voicecontroller.cpp
        voicecontroller.h
)
add_dependencies(adelecore adele_core_build)
target_link_libraries(adelecore PRIVATE Qt6::Core Qt6::Qml Qt6::DBus KF6::ConfigCore adele_core)
target_compile_options(adelecore PRIVATE -Werror)
# Qt resolves the qmldir's `plugin adelecore` to a file named `libadelecore.so`,
# but qt_add_qml_module leaves this merged plugin target prefix-less
# (`adelecore.so`), so plasmashell can't find the plugin → it never loads →
# AdeleCore/VoiceController never register → blank popup. Force the `lib` prefix
# to match Qt's resolver (and the KDE plugin convention, e.g. libkcmutilsqmlplugin.so).
# INSTALL_RPATH "$ORIGIN" finds the co-installed core cdylib at runtime.
# LINK_DEPENDS relinks the plugin whenever the cdylib changes — otherwise CMake
# doesn't treat the imported-from-custom-target .so as a link input, so a cdylib
# rebuild (e.g. gaining a SONAME) leaves the plugin's DT_NEEDED stale (it would
# keep an absolute build-tree path instead of the bare soname $ORIGIN resolves).
set_target_properties(adelecore PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    PREFIX "lib"
    LINK_DEPENDS "${CORE_CDYLIB}")

# --- Install ------------------------------------------------------------------
set(CLIENT_QML_INSTALL_DIR "${KDE_INSTALL_QMLDIR}/org/desktopassistant/client")
install(TARGETS adelecore
    LIBRARY DESTINATION "${CLIENT_QML_INSTALL_DIR}"
    RUNTIME DESTINATION "${CLIENT_QML_INSTALL_DIR}")
install(FILES "${CMAKE_BINARY_DIR}/qml/org/desktopassistant/client/qmldir"
    DESTINATION "${CLIENT_QML_INSTALL_DIR}")
install(FILES "${CORE_CDYLIB}" DESTINATION "${CLIENT_QML_INSTALL_DIR}")

# --- C++ unit tests -----------------------------------------------------------
# Bus-free spec: lifecycle (create/destroy) + the JSON-view-event -> Qt-signal
# marshalling + the `connected` property. The live D-Bus path needs a running
# daemon (manual QA + the daemon's own tests); here we exercise everything that
# needs no daemon. Built by default so the local `just check` gate runs it.
option(BUILD_CLIENT_TESTS "Build the client QML-plugin C++ unit tests" ON)
if(BUILD_CLIENT_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    add_executable(tst_adelecore
        tst_adelecore.cpp
        adelecore.cpp
        adelecore.h
    )
    add_dependencies(tst_adelecore adele_core_build)
    target_link_libraries(tst_adelecore PRIVATE Qt6::Core Qt6::Qml Qt6::Test KF6::ConfigCore adele_core)
    target_compile_options(tst_adelecore PRIVATE -Werror)
    # The test links the cdylib directly (not via the installed module), so point
    # its runtime loader at the cargo target dir.
    set_target_properties(tst_adelecore PROPERTIES
        BUILD_RPATH "${CLIENT_UI_COMMON_DIR}/target/${CARGO_PROFILE_DIR}")
    add_test(NAME tst_adelecore COMMAND tst_adelecore)

    # VoiceController is pure QtDBus glue (no Rust core), so its test links
    # neither the cdylib nor needs the cargo BUILD_RPATH.
    add_executable(tst_voicecontroller
        tst_voicecontroller.cpp
        voicecontroller.cpp
        voicecontroller.h
    )
    target_link_libraries(tst_voicecontroller PRIVATE Qt6::Core Qt6::Qml Qt6::DBus Qt6::Test)
    target_compile_options(tst_voicecontroller PRIVATE -Werror)
    add_test(NAME tst_voicecontroller COMMAND tst_voicecontroller)
endif()
