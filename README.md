# Adele KDE

KDE Plasma widgets and System Settings module for the Adelie Desktop Assistant.

Provides two Plasma 6 plasmoids and a KCM (KDE Control Module) that communicate with the `desktop-assistant-daemon` over D-Bus or WebSocket.

## Components

- **Panel chat widget** (`org.desktopassistant.panelchat`) — popup chat from the panel/taskbar
- **Desktop chat widget** (`org.desktopassistant.desktopchat`) — always-visible chat card on the desktop
- **Shared chat module** (`shared/chat-module/`) — Python D-Bus/WS client and shared QML, deployed to `$XDG_DATA_HOME/desktop-assistant/chat-module/`
- **KDE System Settings KCM** (`kcm/desktop-assistant-settings/`) — configure LLM connector, model, API keys, and MCP servers from System Settings

## Requirements

- KDE Plasma 6.0+
- Python 3, `gdbus` (glib2 tools)
- A running `desktop-assistant-daemon` instance
- For KCM: CMake, Ninja, KDE Frameworks 6 development packages

## Install Widgets

```sh
just widget-install
```

This syncs the shared chat module and installs both plasmoids for the current user.

## Upgrade Widgets

After making changes:

```sh
just widget-upgrade
```

To force a full reinstall and restart plasmashell:

```sh
just widget-hard-refresh
```

## Remove Widgets

```sh
just widget-remove
```

## Install KCM

User-local (for development):

```sh
just kcm-install
just kcm-open
```

System-wide (recommended for daily use, requires sudo):

```sh
just kcm-install-system
just kcm-open-system
```

Refresh cache and verify:

```sh
just kcm-refresh
```

**Important:** Choose one install mode (system or local) and stick to it. Having both installed simultaneously can cause settings to appear to randomly revert. Use `just kcm-cleanup` or `just kcm-cleanup-system` to remove the unwanted copy.

## Widget Usage

- Both widgets include a service selector (Production/Development) visible when both daemon instances are running.
- Widgets shell out to `python3` and `gdbus` for D-Bus calls, or use a raw WebSocket client for WS transport.
- Widget transport and connection settings are configured in `~/.config/desktop-assistant/widget_settings.json`.

## All Just Recipes

```sh
just --list
```

## License

Licensed under **GNU Affero General Public License v3.0 or later** (`AGPL-3.0-or-later`).
