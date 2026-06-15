# Adele KDE

KDE Plasma 6 widgets and System Settings module for the
[Adelie AI Platform](https://github.com/adelie-ai/desktop-assistant).

Provides two Plasma 6 plasmoids plus a KCM (KDE Control Module) that talk to
the `desktop-assistant-daemon` over D-Bus.

## Components

- **Panel chat widget** (`org.desktopassistant.panelchat`) — popup chat from
  the panel / taskbar, with a tasks badge in the chat header.
- **Desktop chat widget** (`org.desktopassistant.desktopchat`) — always-visible
  chat card on the desktop.
- **Tasks window** (`TasksWindow.qml`) — separate window listing background
  tasks, opened from the badge to escape the popup's height constraint.
- **Native client plugin** (`client/`) — the `org.desktopassistant.client` QML
  plugin: `AdeleCore` loads the Rust core cdylib (`libadele_client_core`, built
  from `client-ui-common/ffi`) that runs the shared reducer over a D-Bus
  `Connector`; `VoiceController` is native QtDBus glue for the voice daemon.
- **Shared chat module** (`shared/chat-module/`) — shared QML (`ui/`, incl.
  `ChatView.qml`, a thin view over the native client plugin), deployed to
  `$XDG_DATA_HOME/desktop-assistant/chat-module/`.
- **System Settings KCM** (`kcm/desktop-assistant-settings/`) — multi-connection
  LLM configuration, API keys, MCP servers, and a knowledge base
  browser/editor tab, with immediate-save UX.

> **Transport note.** Chat runs on the native Rust core over D-Bus (the
> `org.desktopAssistant` bridge); background tasks arrive over that same D-Bus
> connection. Voice is reached separately via `org.desktopAssistant.Voice`.

## Requirements

- KDE Plasma 6.0+
- Rust toolchain (`cargo`) — to build the native client plugin (Rust core)
- A running `desktop-assistant-daemon` instance
- For the native client plugin + KCM: CMake, Ninja, KDE Frameworks 6 development packages

## Widgets

```sh
just widget-install       # install both plasmoids + shared module
just widget-upgrade       # apply changes
just widget-hard-refresh  # force reinstall + restart plasmashell
just widget-remove
```

Both widgets include a Production/Development service selector visible when
both daemon instances are running. Widget transport and connection settings
live in `~/.config/desktop-assistant/widget_settings.json`. Chat runs on the
native Rust core (`client/` plugin) over D-Bus — no `python3`/`gdbus` shell-outs.

## KCM

Install system-wide and open (needs sudo). System is the only supported
install: a user-local (`~/.local`) copy is invisible to a normally launched
System Settings (that prefix isn't on the default Qt plugin search path) yet
still shadows the system one, which makes settings appear to randomly revert.
There is no user-local install recipe, and `just kcm-install` purges any stray
local copy before it installs, so the system copy stays authoritative.

```sh
just kcm-install        # build + install to system paths (sudo)
just kcm-open
```

Refresh cache and verify:

```sh
just kcm-refresh
```

Inspect which plugin copies exist with `just kcm-doctor`. To remove the KCM,
use `just kcm-uninstall` (system install, sudo); `just kcm-cleanup` clears a
stray local copy on its own.

## All recipes

```sh
just --list
```

## License

GNU Affero General Public License v3.0 or later (`AGPL-3.0-or-later`).
