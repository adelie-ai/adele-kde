# Agent Instructions — adele-kde

Shared standards live in [AGENTS.base.md](AGENTS.base.md), which is generated. This file holds the rules specific to this repo.

Repo-specific conventions for the KDE Plasma plasmoids and System Settings KCM. The overrides and additions to the base are listed at the end of this file.

## What this repo is

Three pieces that all talk to `desktop-assistant-daemon`:

- **Two Plasma 6 plasmoids** under `plasmoid/` — `org.desktopassistant.panelchat` (popup) and `org.desktopassistant.desktopchat` (always-visible). QML UI loaded from the shared chat module; chat runs on the native `client/` plugin.
- **Shared chat module** under `shared/chat-module/` — shared QML (`ui/`, incl. `ChatView.qml`) consumed by both plasmoids, deployed to `$XDG_DATA_HOME/desktop-assistant/chat-module/`. Chat runs on the native Rust core via the `client/` plugin — there is no transport helper.
- **KCM (System Settings module)** under `kcm/desktop-assistant-settings/` — C++/CMake/Qt6/KF6 module with QML pages for connections, purposes, knowledge.

This is a mixed-language repo (QML / C++ / Rust) — the per-piece conventions below matter more than usual.

## Transport: D-Bus to the bridge; shared Rust core for chat

KDE clients reach the daemon over **D-Bus** — the `org.desktopAssistant` bridge —
and only D-Bus, unless a deviation is justified and **documented in this section**.
D-Bus is the canonical desktop IPC and keeps the clients consistent; never bypass
the bridge (e.g. raw UDS) for a KDE client.

There are two shapes, by surface:

- **KCM (settings):** talks the bridge directly with **QtDBus** from its C++
  (`Connections` / `Settings` / `Knowledge`). It is settings-only — no chat state,
  no shared model needed — so keep it on direct QtDBus.

- **Plasmoid chat (model + controller):** the conversation model/controller is the
  shared, view-agnostic Rust reducer in the **`client-ui-common`** crate — the same
  `WindowState` streaming state machine gtk/tui run. **Reuse it; never reimplement
  it in C++/QML** (that is both a rewrite and a segfault farm). A thin **Rust core**
  (an FFI cdylib) owns that reducer plus a `client-common` `Connector` in **D-Bus
  mode** (so the wire transport is still the bridge), runs the reducer's RPC effects
  itself, and pushes its view effects out to the widget via a callback. The C++/QML
  side is **glue only**: user input → intent calls; pushed view-effects → QML. Keep
  C++ minimal — the model+controller and the transport stay in safe Rust.

So, for a KDE client: keep the daemon transport on D-Bus (the bridge) and don't
bypass it, and for chat **don't reimplement the reducer** — consume
`client-ui-common` through the thin FFI. If the bridge is missing something the
reducer needs, **extend the bridge** first. Deviate only if that is genuinely
impractical — and then record the what and the why right here.

**Bridge surface:** `Conversations` (CRUD + `SendPrompt` + `SubscribeConversations`
+ streamed `ResponseChunk` / `ResponseComplete` / `ResponseError` /
`UserMessageAdded` / `ConversationListChanged` / `ClientToolCall` + the richer
status/context/title/warning/scratchpad signals), `Commands` (generic
`SendCommand`), `Connections` (incl. `ListAvailableModels`), `Knowledge`,
`Settings`, `BackgroundTasks` (+ `Task*`), `Reload`. Voice is a separate service:
`org.desktopAssistant.Voice`.

## Where things live

- `plasmoid/<name>/contents/` — per-plasmoid QML and metadata.
- `plasmoid/<name>/metadata.json` — plasmoid manifest. Update version here when changing behavior.
- `shared/chat-module/ui/` — QML shared across plasmoids (incl. `ChatView.qml`, the thin view over the `client/` plugin).
- `kcm/desktop-assistant-settings/` — `CMakeLists.txt`, C++ source (`desktopassistantkcm.cpp/h`), JSON metadata, and `ui/*.qml`.
- `client/` — the native C++/QML plugin (`org.desktopassistant.client`). Two QML elements: `AdeleCore` loads the Rust core cdylib (`libadele_client_core`, built from `client-ui-common/ffi`) and turns its pushed view-events into a Qt `viewEvent(type, data)` signal (intents go out via `Q_INVOKABLE`s); `VoiceController` is native QtDBus glue for the separate voice daemon (`org.desktopAssistant.Voice`). This is the FFI **glue** the Transport section describes — model/controller/transport stay in Rust. Built + unit-tested via `just client-build`; installed into the system Qt QML import path via `just client-install` (sudo) so the plasmoids' `import org.desktopassistant.client` resolves. `ChatView.qml` consumes both — the Python helper is gone.

## Plasmoid (QML) conventions

- **Reuse the shared chat module.** Don't fork transport logic into a plasmoid. Both plasmoids consume `shared/chat-module/`; a change to chat behavior is one change in the shared module, not two parallel changes.
- **`Kirigami` over raw QtQuick.** Stick to Kirigami / `PlasmaComponents3` widgets so the plasmoids inherit Plasma theming. Hard-coded colors or sizes break under accent-color / scaling changes.
- **Settings via `Plasma.Configuration`.** Per-plasmoid settings go through the standard config schema (XML), not ad-hoc JSON. Widget transport settings that span both plasmoids live in `~/.config/desktop-assistant/widget_settings.json`.

## Shared chat module (QML) conventions

- **Chat logic lives in the Rust core, not QML.** `ChatView.qml` is a thin view over the `org.desktopassistant.client` plugin: render the core's `viewEvent(type, data)` deltas and forward user actions through the intent `Q_INVOKABLE`s. Don't reintroduce transport, polling, or a daemon parser in QML — extend the reducer (`client-ui-common`) or the FFI instead.
- **No secrets in QML.** Credentials live in the daemon and are reached through the core's transport; QML never reads API keys.
- **Voice is a separate service.** The voice pipeline (mic, wake word, TTS) is reached via `VoiceController` (native QtDBus to `org.desktopAssistant.Voice`), independent of the chat connection. `ChatView` routes the core's `speak` event there and reflects its `StateChanged` signal.

## KCM (C++/Qt/KF6) conventions

- **CMake build only.** No `cargo`, no `just`. `cmake -B build -G Ninja` + `ninja -C build`. Install is via the repo `justfile` (`just kcm-install`).
- **System install only.** A user-local (`~/.local`) KCM copy is invisible to a normally launched System Settings (that prefix isn't on the default Qt plugin search path) and only shadows/drifts against the system copy, causing settings to silently revert. There is no user-local install recipe: `just kcm-install` installs to system paths (sudo) and purges any user-local stray first so the system copy stays authoritative. Use `just kcm-cleanup` to remove strays and `just kcm-doctor` to inspect. Preserve this single-mode invariant if you change install layout.
- **QML pages stay declarative.** `kcm/.../ui/*.qml` should bind to KCM properties, not call into C++ business logic. Logic belongs in `desktopassistantkcm.cpp`.
- **Daemon talks happen via D-Bus from the KCM C++ side**, not from QML. QML should not be opening D-Bus connections.

## Install / upgrade recipes

The `justfile` is the source of truth for widget and KCM install/upgrade/remove flows:

- `just widget-install` / `just widget-upgrade` / `just widget-hard-refresh` / `just widget-remove`
- `just kcm-install` (sudo) / `just kcm-open` / `just kcm-refresh` / `just kcm-cleanup` / `just kcm-uninstall` (sudo) / `just kcm-doctor`

When adding a new install behavior, extend these recipes rather than adding a new entry point.

## Cross-client coordination

When the daemon's D-Bus / WS protocol changes, the chat side updates through the shared reducer (`client-ui-common`) + its FFI (shared with the TUI and GTK clients), and the KCM's QtDBus transport code updates directly — all in lockstep. Mention the corresponding daemon PR in the commit message so the cross-repo coordination is reconstructable later.

## Dependency safety

Base rule 6.1 and the 6.1 override at the end of this file cover the posture. Repo-specific notes:

- The KCM links against Qt6 / KF6 system libraries — CVE scans against the build environment matter as much as against in-repo deps.
- The chat plugin links the Rust core cdylib (built from `client-ui-common/ffi`); its Cargo dependency tree gets the same `cargo audit` / `cve-mcp` scan as any Rust change — and scan before the first build, since build scripts run then.

## Overrides and additions to the shared base

Everything in [AGENTS.base.md](AGENTS.base.md) applies to this repo. This section
records only the points where this repo deliberately differs from the base, or adds a
rule the base does not have.

### 3.1 The gate for this repo (addition)

The `adelie-ai` repos have no CI. The gate is local and the author runs it: `just check`,
which runs the shared-QML drift check, the KCM CMake build, the native client build, the
`qmllint` pass over the QML, and the tests. Run `just install-hooks` once per clone to put
the same gate on pre-push.

This repo mixes three languages, so warnings-as-errors is enforced three ways. The KCM and
the native client set `-Werror` and `CMAKE_COMPILE_WARNING_AS_ERROR`. The Rust core builds
under `warnings = "deny"`. QML has no compiler hard-fail, so `qmllint` and the test suite
are its gate - keep both green.

### 4.3 Branch and pull request - merge when green (override, weaker than the base)

The base opens a pull request and waits for the user. In these repos the merge is delegated:
merge your own pull request as soon as it is green and independently shippable. Green here
means more than a clean build. The gate above passed, the tests cover the new behavior and
not only the absence of a panic, the security pass is done, and the change stands on its own.
Assign `dspadea` with `gh pr edit --add-assignee` and verify it; a review request from the
same account no-ops without an error, so never report a pull request as review-requested.
When in doubt, hold.

### 4.4 Worktrees - the group convention (addition)

Put the worktree at `.worktrees/<repo>/issue-N-slug/` under the group directory, on a branch
that mirrors the slug. Before you run tasks in parallel worktrees, look for shared files,
shared `Cargo.toml` dependency edits, and shared migration ordinals. Serialize the work where
they overlap, and tell each parallel agent the scope it owns.

### 6.1 Dependencies - a high or critical advisory is a hard blocker (override, stricter than the base)

Scan after you add a dependency and before the first build:

1. Add the dependency (`cargo add <crate>`). This writes the lockfile but does not build.
2. Scan the updated lockfile with the `cve-mcp` server's `scan_packages` tool, or with
   `cargo audit`. Pass every (name, version, ecosystem) tuple.
3. A high or critical finding blocks the change. Patch it in the same change, or prove the
   path unreachable and write down why, or file an issue and reference it from the change.
4. Build only after the scan is clean, or after you have accepted the findings in writing.

Never pin around an advisory without a comment or a tracked issue.

### 9.1 Tracker for this project

GitHub Issues on `github.com/adelie-ai/adele-kde`, together with the shared `adelie-ai` project
board. Manage entries with the `gh` CLI (`gh issue create`, `gh issue list`, `gh issue edit`,
`gh pr create`). The board states in use are In Progress, In Review, and Done.

### Capability-based degradation (addition)

Every reliance on an optional operating-system or desktop service - logind, the screen lock,
KDE and Plasma, PipeWire specifics, any session-bus or system-bus D-Bus interface - must be
capability-detected, and must degrade cleanly when the service is absent. Never make one a
hard dependency that errors or hangs. The product can run headless, in a container, on
another desktop environment, or as a system service.

Distinguish "is the capability present?" from "did my call succeed?". There are three states.
Absent: disable that feature, log once, and fall back to the prior behavior. Present and
known: use it. Present but anomalous: stay conservative, or hold the last known state, and
warn. Scope any privacy or safety fail-safe to the last two states only. A fail-safe that is
correct on the desktop can be pathological headless. "Treat an unknown session as inactive"
means the microphone never opens.

Detect each optional dependency on its own. The absence of one never disables the others and
never aborts startup. Surface the detected capability, so an operator can see why a feature
is on or off.
