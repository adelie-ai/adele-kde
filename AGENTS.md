# Agent Instructions — adele-kde

Repo-specific conventions for the KDE Plasma plasmoids and System Settings KCM. Cross-project workflow rules (issue/PR/board sync, parallel worktrees, warnings-are-failures, security review posture, TDD posture) live in the user's memory and are not duplicated here.

## What this repo is

Three pieces that all talk to `desktop-assistant-daemon`:

- **Two Plasma 6 plasmoids** under `plasmoid/` — `org.desktopassistant.panelchat` (popup) and `org.desktopassistant.desktopchat` (always-visible). QML UI; transport via the shared chat module.
- **Shared chat module** under `shared/chat-module/` — Python D-Bus/WS client (`code/dbus_client.py`) plus shared QML, deployed to `$XDG_DATA_HOME/desktop-assistant/chat-module/`.
- **KCM (System Settings module)** under `kcm/desktop-assistant-settings/` — C++/CMake/Qt6/KF6 module with QML pages for connections, purposes, knowledge.

This is a mixed-language repo (QML / Python / C++) — the per-piece conventions below matter more than usual.

## Where things live

- `plasmoid/<name>/contents/` — per-plasmoid QML and metadata.
- `plasmoid/<name>/metadata.json` — plasmoid manifest. Update version here when changing behavior.
- `shared/chat-module/code/dbus_client.py` — the Python transport. Both plasmoids and any tooling that needs to talk to the daemon should go through this rather than re-implementing transport.
- `shared/chat-module/ui/` — QML shared across plasmoids.
- `kcm/desktop-assistant-settings/` — `CMakeLists.txt`, C++ source (`desktopassistantkcm.cpp/h`), JSON metadata, and `ui/*.qml`.

## Plasmoid (QML) conventions

- **Reuse the shared chat module.** Don't fork transport logic into a plasmoid. Both plasmoids consume `shared/chat-module/`; a change to chat behavior is one change in the shared module, not two parallel changes.
- **`Kirigami` over raw QtQuick.** Stick to Kirigami / `PlasmaComponents3` widgets so the plasmoids inherit Plasma theming. Hard-coded colors or sizes break under accent-color / scaling changes.
- **Settings via `Plasma.Configuration`.** Per-plasmoid settings go through the standard config schema (XML), not ad-hoc JSON. Widget transport settings that span both plasmoids live in `~/.config/desktop-assistant/widget_settings.json`.

## Shared chat module (Python) conventions

- **The D-Bus / WS client is the contract.** Plasmoid QML calls into Python via the established `dbus_client.py` interface. When that interface needs a new method, change it in one place and bump the deployed module — both plasmoids pick it up.
- **No secrets in QML or in the module.** Credentials live in the daemon and are surfaced through transport calls; the chat module should not be reading API keys.
- **Subprocess hygiene.** When shelling out to `python3` or `gdbus` from QML, quote arguments and avoid string concatenation with untrusted input. Assistant message content is untrusted from a shell-injection perspective.

## KCM (C++/Qt/KF6) conventions

- **CMake build only.** No `cargo`, no `just`. `cmake -B build -G Ninja` + `ninja -C build`. Install variants are in the repo `justfile` (`just kcm-install`, `just kcm-install-system`).
- **One install mode at a time.** System and user installs of the KCM can shadow each other and cause settings to silently revert. The `README` describes the cleanup recipes; if you change install layout, preserve that single-mode invariant.
- **QML pages stay declarative.** `kcm/.../ui/*.qml` should bind to KCM properties, not call into C++ business logic. Logic belongs in `desktopassistantkcm.cpp`.
- **Daemon talks happen via D-Bus from the KCM C++ side**, not from QML. QML should not be opening D-Bus connections.

## Install / upgrade recipes

The `justfile` is the source of truth for widget and KCM install/upgrade/remove flows:

- `just widget-install` / `just widget-upgrade` / `just widget-hard-refresh` / `just widget-remove`
- `just kcm-install` (user) / `just kcm-install-system` (system) / `just kcm-refresh` / `just kcm-cleanup`

When adding a new install behavior, extend these recipes rather than adding a new entry point.

## Cross-client coordination

When the daemon's D-Bus / WS protocol changes, the corresponding update to the shared chat module and KCM transport code needs to land in lockstep with the TUI and GTK clients. Mention the corresponding daemon PR in the commit message so the cross-repo coordination is reconstructable later.

## Dependency safety

The user-memory security-review rule covers the posture. Repo-specific notes:

- The KCM links against Qt6 / KF6 system libraries — CVE scans against the build environment matter as much as against in-repo deps.
- The Python chat module's transitive Python deps (if any get pulled in) need the same scan; the current `dbus_client.py` is intentionally narrow to keep that surface small.
