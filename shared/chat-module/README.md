# Adele AI Shared Chat Module

This module contains the shared chat UI (QML) reused across the KDE plasmoids.
Transport and the conversation reducer live in the native client plugin
(`client/`, the `org.desktopassistant.client` QML module) — `ChatView.qml` is a
thin view over it, not a transport layer.

## Layout

- `ui/ChatView.qml` — reusable chat view over the native client plugin
- `ui/TasksView.qml`, `ui/TasksWindow.qml`, `ui/TasksBadge.qml` — background-tasks UI
- `ui/LinkSafety.js` — link sanitization helpers
- `images/` — shared avatar assets

## Install location

KDE wrappers load this module from:

- `$XDG_DATA_HOME/desktop-assistant/chat-module`
- fallback: `~/.local/share/desktop-assistant/chat-module`

Use `just chat-module-sync` to copy this module into the XDG data path during local development.
