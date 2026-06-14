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

find_package(Qt6 REQUIRED COMPONENTS Core Qml)
find_package(Threads REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

# --- Locate + build the Rust FFI (desktop-assistant/crates/client-ffi) --------
# The native client core lives in the desktop-assistant repo (shared with
# gtk/tui). Default to the standard sibling-checkout layout — from <repo>/client
# that is <repo>/../desktop-assistant — and allow an override for worktrees
# (point it at the primary checkout, mirroring the adele-gtk path-dep) or for
# packaging.
set(DESKTOP_ASSISTANT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../desktop-assistant"
    CACHE PATH "Path to the desktop-assistant checkout (must contain crates/client-ffi)")
get_filename_component(DESKTOP_ASSISTANT_DIR "${DESKTOP_ASSISTANT_DIR}" ABSOLUTE)

set(FFI_CRATE_DIR "${DESKTOP_ASSISTANT_DIR}/crates/client-ffi")
if(NOT EXISTS "${FFI_CRATE_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "client-ffi crate not found at ${FFI_CRATE_DIR}.\n"
        "Set -DDESKTOP_ASSISTANT_DIR=/path/to/desktop-assistant "
        "(it must contain crates/client-ffi).")
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

set(FFI_CDYLIB "${DESKTOP_ASSISTANT_DIR}/target/${CARGO_PROFILE_DIR}/libdesktop_assistant_client_ffi.so")
set(FFI_HEADER_DIR "${FFI_CRATE_DIR}/include")

# Build the self-contained cdylib (all Rust deps link statically into it; build.rs
# also (re)generates the C header). Always invoked — cargo is incremental, so this
# is a no-op when nothing changed and picks up daemon-side changes when they do.
add_custom_target(client_ffi_build
    BYPRODUCTS "${FFI_CDYLIB}"
    COMMAND ${CARGO_EXECUTABLE} build ${CARGO_BUILD_FLAGS} -p desktop-assistant-client-ffi
    WORKING_DIRECTORY "${DESKTOP_ASSISTANT_DIR}"
    COMMENT "Building desktop-assistant client FFI (${CARGO_PROFILE_DIR})"
    VERBATIM
    USES_TERMINAL
)

# The cdylib is a complete shared object, so linking it needs no Rust-runtime
# native-lib list — it resolves its own deps. Consumers find it at runtime via an
# $ORIGIN RPATH (it installs next to the plugin).
add_library(client_ffi SHARED IMPORTED GLOBAL)
set_target_properties(client_ffi PROPERTIES IMPORTED_LOCATION "${FFI_CDYLIB}")
target_include_directories(client_ffi INTERFACE "${FFI_HEADER_DIR}")

# --- The QML module: org.desktopassistant.client -----------------------------
# PLUGIN_TARGET == target merges backing library + plugin into one .so (the
# module is pure C++ types, no .qml files), so deployment is a single plugin.
qt_add_qml_module(adeleclient
    URI org.desktopassistant.client
    VERSION 1.0
    PLUGIN_TARGET adeleclient
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml/org/desktopassistant/client"
    SOURCES
        adeleclient.cpp
        adeleclient.h
)
add_dependencies(adeleclient client_ffi_build)
target_link_libraries(adeleclient PRIVATE Qt6::Core Qt6::Qml client_ffi)
target_compile_options(adeleclient PRIVATE -Werror)
# Find the co-installed FFI cdylib at runtime.
set_target_properties(adeleclient PROPERTIES INSTALL_RPATH "$ORIGIN")

# --- Install ------------------------------------------------------------------
set(CLIENT_QML_INSTALL_DIR "${KDE_INSTALL_QMLDIR}/org/desktopassistant/client")
install(TARGETS adeleclient
    LIBRARY DESTINATION "${CLIENT_QML_INSTALL_DIR}"
    RUNTIME DESTINATION "${CLIENT_QML_INSTALL_DIR}")
install(FILES "${CMAKE_BINARY_DIR}/qml/org/desktopassistant/client/qmldir"
    DESTINATION "${CLIENT_QML_INSTALL_DIR}")
install(FILES "${FFI_CDYLIB}" DESTINATION "${CLIENT_QML_INSTALL_DIR}")

# --- C++ unit tests -----------------------------------------------------------
# Bus-free spec: graceful connect-failure + the tagged-JSON -> Qt-signal
# marshalling. The live UDS path needs a running daemon (manual QA + daemon
# tests). Built by default so the local `just check` gate compiles + runs it.
option(BUILD_CLIENT_TESTS "Build the client QML-plugin C++ unit tests" ON)
if(BUILD_CLIENT_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    add_executable(tst_adeleclient
        tst_adeleclient.cpp
        adeleclient.cpp
        adeleclient.h
    )
    add_dependencies(tst_adeleclient client_ffi_build)
    target_link_libraries(tst_adeleclient PRIVATE Qt6::Core Qt6::Qml Qt6::Test client_ffi)
    target_compile_options(tst_adeleclient PRIVATE -Werror)
    # The test links the cdylib directly (not via the installed module), so point
    # its runtime loader at the cargo target dir.
    set_target_properties(tst_adeleclient PROPERTIES
        BUILD_RPATH "${DESKTOP_ASSISTANT_DIR}/target/${CARGO_PROFILE_DIR}")
    add_test(NAME tst_adeleclient COMMAND tst_adeleclient)
endif()
