set shell := ["bash", "-euo", "pipefail", "-c"]

panel_widget := "plasmoid/org.desktopassistant.panelchat"
desktop_widget := "plasmoid/org.desktopassistant.desktopchat"
kcm_dir := "kcm/desktop-assistant-settings"
kcm_build_dir := "build/kde-kcm"
panel_widget_id := "org.desktopassistant.panelchat"
desktop_widget_id := "org.desktopassistant.desktopchat"
shared_chat_module_src := "shared/chat-module"
shared_chat_module_dst := env_var_or_default("XDG_DATA_HOME", env_var("HOME") + "/.local/share") + "/desktop-assistant/chat-module"
shared_chatview_src := "shared/chat-module/ui/ChatView.qml"
desktop_chatview_fallback := "plasmoid/org.desktopassistant.desktopchat/contents/ui/ChatView.qml"

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

# Sync shared ChatView into desktop plasmoid fallback copy
chatview-sync:
    [ -f "{{shared_chatview_src}}" ] || (echo "Missing shared ChatView: {{shared_chatview_src}}" >&2; exit 1)
    mkdir -p "$(dirname '{{desktop_chatview_fallback}}')"
    cp -a "{{shared_chatview_src}}" "{{desktop_chatview_fallback}}"

# Verify desktop plasmoid fallback ChatView matches shared ChatView
chatview-verify:
    [ -f "{{shared_chatview_src}}" ] || (echo "Missing shared ChatView: {{shared_chatview_src}}" >&2; exit 1)
    [ -f "{{desktop_chatview_fallback}}" ] || (echo "Missing fallback ChatView: {{desktop_chatview_fallback}}" >&2; exit 1)
    cmp -s "{{shared_chatview_src}}" "{{desktop_chatview_fallback}}" || (echo "ChatView drift detected: run 'just chatview-sync'" >&2; exit 1)

# Install all KDE Plasma widgets for the current user
widget-install:
    just chatview-sync
    just chat-module-sync
    kpackagetool6 --type Plasma/Applet --install {{panel_widget}}
    kpackagetool6 --type Plasma/Applet --install {{desktop_widget}}

# Upgrade all KDE Plasma widgets after local changes
widget-upgrade:
    just chatview-sync
    just chat-module-sync
    kpackagetool6 --type Plasma/Applet --upgrade {{panel_widget}}
    kpackagetool6 --type Plasma/Applet --upgrade {{desktop_widget}}

# Reinstall all KDE Plasma widgets (remove + install)
widget-reinstall:
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

# Configure and build KDE System Settings KCM
kcm-build:
    cmake -S {{kcm_dir}} -B {{kcm_build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build {{kcm_build_dir}}

# Install KDE System Settings KCM (user-local prefix)
kcm-install:
    cmake -S {{kcm_dir}} -B {{kcm_build_dir}} -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DKDE_INSTALL_PLUGINDIR="$HOME/.local/lib64/qt6/plugins"
    cmake --build {{kcm_build_dir}}
    cmake --install {{kcm_build_dir}}
    rm -f "$HOME/.local/share/kservices5/kcm_desktopassistant_service.desktop"

# Install KDE System Settings KCM into system paths (requires sudo)
kcm-install-system:
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
    kcmshell6 --list | grep -i kcm_desktopassistant || (if [ -f {{kcm_build_dir}}/prefix.sh ]; then set +u; source {{kcm_build_dir}}/prefix.sh; set -u; export QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins:${QT_PLUGIN_PATH:-}"; kcmshell6 --list | grep -i kcm_desktopassistant || true; fi)

# Open Desktop Assistant KCM with local plugin environment
kcm-open:
    if [ -f {{kcm_build_dir}}/prefix.sh ]; then set +u; source {{kcm_build_dir}}/prefix.sh; set -u; fi
    export QT_PLUGIN_PATH="$HOME/.local/lib64/qt6/plugins:${QT_PLUGIN_PATH:-}"
    unset DESKTOP_STARTUP_ID
    unset GTK_USE_PORTAL
    unset GIO_USE_PORTALS
    kquitapp6 systemsettings || true
    pkill -f '^systemsettings' || true
    sleep 0.3
    QT_LOGGING_RULES="qt.qpa.services.warning=false" systemsettings kcm_desktopassistant

# Open Desktop Assistant KCM from system install paths
kcm-open-system:
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

# Remove system KCM install copies (requires sudo)
kcm-cleanup-system:
    sudo rm -f /usr/lib64/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so
    sudo rm -f /usr/lib64/qt6/plugins/plasma/kcms/systemsettings/kcm_desktopassistant.so
    sudo rm -f /usr/share/applications/kcm_desktopassistant.desktop
    sudo rm -f /usr/share/systemsettings/categories/settings-applications-desktopassistant.desktop
    kbuildsycoca6 || true

# Uninstall everything (widgets + KCM)
uninstall:
    just widget-remove
    just kcm-cleanup

# Run Python unit tests
test-python:
    ./tests/run_python_tests.sh

# Run QML autotests via qmltestrunner
test-qml:
    ./tests/run_qml_tests.sh

# Run all tests
test:
    just test-python
    just test-qml

# Clean build artifacts
clean:
    rm -rf {{kcm_build_dir}} build/kde-kcm-system
