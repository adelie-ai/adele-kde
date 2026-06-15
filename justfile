set shell := ["bash", "-euo", "pipefail", "-c"]

panel_widget := "plasmoid/org.desktopassistant.panelchat"
desktop_widget := "plasmoid/org.desktopassistant.desktopchat"
kcm_dir := "kcm/desktop-assistant-settings"
kcm_build_dir := "build/kde-kcm"
client_dir := "client"
client_build_dir := "build/kde-client"
panel_widget_id := "org.desktopassistant.panelchat"
desktop_widget_id := "org.desktopassistant.desktopchat"
shared_chat_module_src := "shared/chat-module"
shared_chat_module_dst := env_var_or_default("XDG_DATA_HOME", env_var("HOME") + "/.local/share") + "/desktop-assistant/chat-module"
shared_chatview_src := "shared/chat-module/ui/ChatView.qml"
desktop_chatview_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/ChatView.qml"
shared_tasks_view_src := "shared/chat-module/ui/TasksView.qml"
shared_tasks_window_src := "shared/chat-module/ui/TasksWindow.qml"
shared_tasks_badge_src := "shared/chat-module/ui/TasksBadge.qml"
shared_link_safety_src := "shared/chat-module/ui/LinkSafety.js"
desktop_tasks_view_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/TasksView.qml"
desktop_tasks_window_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/TasksWindow.qml"
desktop_tasks_badge_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/TasksBadge.qml"
desktop_link_safety_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/LinkSafety.js"

# List available commands
default: list

@list:
    just --list

# Sync shared chat module to XDG data path
chat-module-sync:
    [ -d "{{shared_chat_module_src}}" ] || (echo "Missing shared module directory: {{shared_chat_module_src}}" >&2; exit 1)
    mkdir -p "$(dirname '{{shared_chat_module_dst}}')"
    rm -rf "{{shared_chat_module_dst}}"
    cp -a "{{shared_chat_module_src}}" "{{shared_chat_module_dst}}"

# Sync shared ChatView + Tasks*.qml + LinkSafety.js + the Python helper into plasmoid fallback copies
chatview-sync:
    [ -f "{{shared_chatview_src}}" ] || (echo "Missing shared ChatView: {{shared_chatview_src}}" >&2; exit 1)
    mkdir -p "$(dirname '{{desktop_chatview_fallback}}')"
    cp -a "{{shared_chatview_src}}" "{{desktop_chatview_fallback}}"
    cp -a "{{shared_tasks_view_src}}" "{{desktop_tasks_view_fallback}}"
    cp -a "{{shared_tasks_window_src}}" "{{desktop_tasks_window_fallback}}"
    cp -a "{{shared_tasks_badge_src}}" "{{desktop_tasks_badge_fallback}}"
    cp -a "{{shared_link_safety_src}}" "{{desktop_link_safety_fallback}}"

# Verify desktop plasmoid fallback copies match shared sources
chatview-verify:
    [ -f "{{shared_chatview_src}}" ] || (echo "Missing shared ChatView: {{shared_chatview_src}}" >&2; exit 1)
    [ -f "{{desktop_chatview_fallback}}" ] || (echo "Missing fallback ChatView: {{desktop_chatview_fallback}}" >&2; exit 1)
    cmp -s "{{shared_chatview_src}}" "{{desktop_chatview_fallback}}" || (echo "ChatView drift detected: run 'just chatview-sync'" >&2; exit 1)
    cmp -s "{{shared_tasks_view_src}}" "{{desktop_tasks_view_fallback}}" || (echo "TasksView drift detected: run 'just chatview-sync'" >&2; exit 1)
    cmp -s "{{shared_tasks_window_src}}" "{{desktop_tasks_window_fallback}}" || (echo "TasksWindow drift detected: run 'just chatview-sync'" >&2; exit 1)
    cmp -s "{{shared_tasks_badge_src}}" "{{desktop_tasks_badge_fallback}}" || (echo "TasksBadge drift detected: run 'just chatview-sync'" >&2; exit 1)
    cmp -s "{{shared_link_safety_src}}" "{{desktop_link_safety_fallback}}" || (echo "LinkSafety.js drift detected: run 'just chatview-sync'" >&2; exit 1)

# Install all KDE Plasma widgets for the current user
widget-install:
    just client-install
    just chatview-sync
    just chat-module-sync
    kpackagetool6 --type Plasma/Applet --install {{panel_widget}}
    kpackagetool6 --type Plasma/Applet --install {{desktop_widget}}

# Upgrade all KDE Plasma widgets after local changes
widget-upgrade:
    just client-install
    just chatview-sync
    just chat-module-sync
    kpackagetool6 --type Plasma/Applet --upgrade {{panel_widget}}
    kpackagetool6 --type Plasma/Applet --upgrade {{desktop_widget}}

# Reinstall all KDE Plasma widgets (remove + install)
widget-reinstall:
    just client-install
    just chatview-sync
    just chat-module-sync
    kpackagetool6 --type Plasma/Applet --remove {{panel_widget_id}} || true
    kpackagetool6 --type Plasma/Applet --remove {{desktop_widget_id}} || true
    kpackagetool6 --type Plasma/Applet --install {{panel_widget}}
    kpackagetool6 --type Plasma/Applet --install {{desktop_widget}}

# Hard refresh KDE widgets (reinstall + restart plasmashell)
widget-hard-refresh:
    just widget-reinstall
    kquitapp6 plasmashell >/dev/null 2>&1 || pkill -TERM -x plasmashell || true
    sleep 0.5
    pgrep -x plasmashell >/dev/null && pkill -KILL -x plasmashell || true
    sleep 0.2
    nohup plasmashell --replace >/tmp/plasmashell-desktop-assistant.log 2>&1 &

# Restore Plasma shell config files from a backup directory created by plasma-shell-reset
plasma-shell-restore backup_dir:
    [ -d "{{backup_dir}}" ] || (echo "Missing backup directory: {{backup_dir}}" >&2; exit 1)
    [ -f "{{backup_dir}}/plasma-org.kde.plasma.desktop-appletsrc" ] || (echo "Missing file: {{backup_dir}}/plasma-org.kde.plasma.desktop-appletsrc" >&2; exit 1)
    cp -a "{{backup_dir}}/plasma-org.kde.plasma.desktop-appletsrc" "$HOME/.config/plasma-org.kde.plasma.desktop-appletsrc"
    if [ -f "{{backup_dir}}/plasmashellrc" ]; then cp -a "{{backup_dir}}/plasmashellrc" "$HOME/.config/plasmashellrc"; fi
    systemctl --user restart plasma-plasmashell.service >/dev/null 2>&1 || systemctl --user restart plasmashell.service >/dev/null 2>&1 || true
    sleep 1
    systemctl --user --no-pager --full status plasma-plasmashell.service 2>/dev/null | sed -n '1,80p' || systemctl --user --no-pager --full status plasmashell.service 2>/dev/null | sed -n '1,80p' || true

# Remove all KDE Plasma widgets
widget-remove:
    kpackagetool6 --type Plasma/Applet --remove {{panel_widget_id}} || true
    kpackagetool6 --type Plasma/Applet --remove {{desktop_widget_id}} || true

# Configure and build KDE System Settings KCM (also runs the C++ unit tests)
kcm-build:
    cmake -S {{kcm_dir}} -B {{kcm_build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build {{kcm_build_dir}}
    ctest --test-dir {{kcm_build_dir}} --output-on-failure

# Build the native client QML plugin (cargo-builds the Rust core) + run its C++ tests
client-build:
    #!/usr/bin/env bash
    set -euo pipefail
    # Degrade to a skip when cargo or the client-ui-common checkout (with the
    # `ffi` crate) is absent, so the gate still runs without the Rust toolchain
    # or the sibling repo set up.
    if ! command -v cargo >/dev/null 2>&1; then
        echo "client-build: cargo not found — skipping the native client plugin" >&2
        exit 0
    fi
    if [ ! -f "{{client_dir}}/../../client-ui-common/ffi/Cargo.toml" ]; then
        echo "client-build: ../client-ui-common/ffi not found — skipping (set up the sibling checkout)" >&2
        exit 0
    fi
    cmake -S {{client_dir}} -B {{client_build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build {{client_build_dir}}
    ctest --test-dir {{client_build_dir}} --output-on-failure

# Build + install the native client QML plugin into the system Qt QML import path
# (sudo). Plasmashell's QML engine always includes Qt's QML dir, so the plasmoids'
# `import org.desktopassistant.client` resolves there. Installs the plugin
# (adelecore.so), its qmldir, and the co-located Rust core cdylib (release build).
client-install:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v cargo >/dev/null 2>&1; then
        echo "client-install: cargo not found — cannot build the native client core" >&2
        exit 1
    fi
    qml_dir="$(qtpaths6 --query QT_INSTALL_QML 2>/dev/null || qtpaths --query QT_INSTALL_QML 2>/dev/null || echo /usr/lib/qt6/qml)"
    echo "client-install: installing into ${qml_dir}/org/desktopassistant/client"
    cmake -S {{client_dir}} -B build/kde-client-system -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DKDE_INSTALL_QMLDIR="$qml_dir" -DBUILD_CLIENT_TESTS=OFF
    cmake --build build/kde-client-system
    sudo cmake --install build/kde-client-system
    # Remove a stray pre-lib-prefix plugin from an earlier install (Qt loads
    # libadelecore.so; a leftover adelecore.so is dead but confusing).
    sudo rm -f "$qml_dir/org/desktopassistant/client/adelecore.so"

# System is the only supported install — there is no user-local mode: a ~/.local
# copy is invisible to a normally launched System Settings yet still shadows the
# system one, which is what makes settings appear to silently revert. kcm-cleanup
# runs first to purge any such stray and keep the system copy authoritative.
# Build + install the KCM into system paths (sudo); purges user-local strays first.
kcm-install:
    just kcm-cleanup
    plugin_dir="/usr/lib64/qt6/plugins"; \
    if [ -f /etc/os-release ]; then \
        . /etc/os-release; \
        os_id="${ID:-}"; \
        os_like="${ID_LIKE:-}"; \
        if [ "$os_id" = "cachyos" ] || [ "$os_id" = "arch" ] || [[ "$os_like" == *"arch"* ]]; then \
            plugin_dir="/usr/lib/qt6/plugins"; \
        fi; \
    fi; \
    cmake -S {{kcm_dir}} -B build/kde-kcm-system -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DKDE_INSTALL_PLUGINDIR="$plugin_dir"
    cmake --build build/kde-kcm-system
    sudo cmake --install build/kde-kcm-system
    sudo rm -f /usr/share/kservices5/kcm_desktopassistant_service.desktop

# Refresh KDE cache and list Desktop Assistant KCM in current shell
kcm-refresh:
    kbuildsycoca6 || true
    kcmshell6 --list | grep -i kcm_desktopassistant || true

# Open Desktop Assistant KCM in System Settings
kcm-open:
    unset QT_PLUGIN_PATH
    unset DESKTOP_STARTUP_ID
    unset GTK_USE_PORTAL
    unset GIO_USE_PORTALS
    kquitapp6 systemsettings || true
    pkill -f '^systemsettings' || true
    sleep 0.3
    QT_LOGGING_RULES="qt.qpa.services.warning=false" systemsettings kcm_desktopassistant

# Diagnose which KCM plugin copy is active and whether Bedrock strings are present
kcm-doctor:
    @echo "Qt plugin dir:"
    @qtpaths6 --plugin-dir || true
    @echo
    @echo "KCM plugin copies:"
    @for p in "$HOME/.local/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so" \
        "/usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so"; do \
        if [ -f "$p" ]; then \
            ls -l "$p"; \
        else \
            echo "missing: $p"; \
        fi; \
    done
    @echo
    @echo "Embedded connector strings:"
    @for p in "$HOME/.local/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so" \
        "/usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so"; do \
        if [ -f "$p" ]; then \
            echo "=== $p ==="; \
            strings -a "$p" | grep -E "aws-bedrock|bedrock|anthropic|ollama|openai" || true; \
        fi; \
    done
    @echo
    @echo "KCM service registration:"
    @kcmshell6 --list | grep -i kcm_desktopassistant || true

# Remove stale/local KCM plugin copies (keeps system install intact)
kcm-cleanup:
    rm -f "$HOME/.local/lib64/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so"
    rm -f "$HOME/.local/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so"
    rm -f "$HOME/.local/share/applications/kcm_desktopassistant.desktop"
    rm -f "$HOME/.local/share/systemsettings/categories/settings-applications-desktopassistant.desktop"

# Uninstall the system KCM (requires sudo)
kcm-uninstall:
    sudo rm -f /usr/lib64/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so
    sudo rm -f /usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so
    sudo rm -f /usr/share/applications/kcm_desktopassistant.desktop
    sudo rm -f /usr/share/systemsettings/categories/settings-applications-desktopassistant.desktop
    kbuildsycoca6 || true

# Uninstall everything (widgets + KCM: system install and any local stray)
uninstall:
    just widget-remove
    just kcm-cleanup
    just kcm-uninstall

# Run QML autotests via qmltestrunner
test-qml:
    ./tests/run_qml_tests.sh

# Run all tests
test:
    just test-qml

# Smoke test to run after a change: widgets load (QML) + the widget client
# connects to the daemon over D-Bus. The D-Bus leg needs the daemon running.
smoke:
    just test-qml
    ./tests/smoke/dbus_smoke.sh

# Clean build artifacts
clean:
    rm -rf {{kcm_build_dir}} {{client_build_dir}} build/kde-kcm-system build/kde-client-system

# --- Local verification ("local CI") -----------------------------------------
# Run locally instead of GitHub Actions. `install-hooks` wires `check` into a
# git pre-push hook so it runs automatically before every push. (Not Rust, so
# there's no cargo gate — this runs the QML/C++/Python checks that apply here.)

# Full local gate: shared-QML drift, qmllint, KCM C++ build, native client
# plugin (Rust core + C++ tests), Python + QML tests
check: chatview-verify kcm-build client-build lint test

# Lint every QML file (production + tests) with qmllint; excludes build artifacts.
# ChatView imports the native client plugin (org.desktopassistant.client); when the
# plugin is built we add its module dir to the import path so qmllint resolves the
# AdeleCore/VoiceController types and statically validates ChatView's load (the
# #18-class "non-existent attached object" check). Without it built (client-build
# skipped), ChatView is skipped — it's linted once the plugin is present.
lint:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -d "{{client_build_dir}}/qml" ]; then
        find . -name '*.qml' -not -path './build/*' -print0 \
            | xargs -0 -r qmllint -I "{{client_build_dir}}/qml"
    else
        echo "lint: native client plugin not built — skipping ChatView (run 'just client-build' to lint it)" >&2
        find . -name '*.qml' -not -path './build/*' -not -name 'ChatView.qml' -print0 \
            | xargs -0 -r qmllint
    fi

# Smoke / integration test — needs the daemon running for the D-Bus leg
test-integration:
    just smoke

# Rebase onto latest origin/main then run the gate (catches clean-rebase-but-broken-build)
premerge:
    git fetch origin
    git rebase origin/main
    just check

# Install git hooks (pre-push runs `just check`). Local config; run once per clone.
install-hooks:
    git config core.hooksPath .githooks
    @echo "pre-push hook active — bypass once with: git push --no-verify"
