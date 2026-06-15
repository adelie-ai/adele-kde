# Agent Instructions — adele-kde

Repo-specific conventions for the KDE Plasma plasmoids and System Settings KCM. Cross-project engineering standards are embedded below under **Cross-project engineering standards**.

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

The user-memory security-review rule covers the posture. Repo-specific notes:

- The KCM links against Qt6 / KF6 system libraries — CVE scans against the build environment matter as much as against in-repo deps.
- The chat plugin links the Rust core cdylib (built from `client-ui-common/ffi`); its Cargo dependency tree gets the same `cargo audit` / `cve-mcp` scan as any Rust change — and scan before the first build, since build scripts run then.

## Cross-project engineering standards

These apply to every repo under `github.com/adelie-ai`. They're embedded in each repo's `AGENTS.md` (not centralized) so a contributor working in a single repo has them in hand. Operator-specific preferences and machine-specific deploy recipes are intentionally not here.

### Don't break `main`
- `main` is the release: at any commit it must build, test, and run.
- Merge a green change as soon as it's independently shippable — additive, behavior-preserving, or behind a default that preserves the old path. Don't hold green work hostage to a coordinated release.
- Co-dependent changes land together; name the interlock ("blocked-by #X" / "must merge with #Y") so it's visible without reading the diff.
- "Green" is more than CI: review passed, tests cover the new behavior (not just "no panic"), warnings clean, security pass done, change stands on its own. With no active CI in these repos, "green" rests on the repo's local gates — the KCM CMake build, the native client build (`cargo` core + C++ tests), `qmllint`, and the QML tests — run by the author (via `just check`).
- When in doubt, hold. A half-coupled "fix-forward" merge breaks `main` for everyone.

### Tests are spec-driven (TDD)
- Every change carries a Testing section: acceptance criteria as testable assertions, each criterion a named test whose name is legible from test output.
- Write failing tests first, in their own commit before the implementation commit — that commit is the spec.
- Cover all new code: every branch, error path, edge case. Gaps are a review finding.
- Assert the desired outcome, not just that a call returned `Ok`.
- Enumerate unhappy paths deliberately: empty/missing input, boundary/max, concurrent/racy, authorization/tenant boundaries, partial reads/writes/dropped streams, malformed input. A test list with none of these is testing wishes.

### Warnings are failures
- Compiler warnings, clippy lints, formatter diffs, and advisories all count — fix the root cause. If a lint truly doesn't apply, suppress at the narrowest scope with a one-line justification; never crate-wide.
- This repo enforces warnings-as-errors **mechanically** where the compiler allows it: the KCM and native client (`client/`) builds set `-Werror` / `CMAKE_COMPILE_WARNING_AS_ERROR`, and the Rust core builds under `warnings = "deny"`. QML has no compiler hard-fail, so it's gated by `qmllint` and the test suite (run via `just check`) — keep green.
- Never `--no-verify` past hooks. If a hook is genuinely broken, fix it in its own commit and explain why.
- Don't `#[ignore]` a test you broke; fix it, or open a tracking issue and reference it from the attribute.
- Pre-existing warnings in a file you touch are yours to address (in-change or a small follow-up) — don't pile new code on an ignored signal.

### Security review before requesting review
- Read your own diff adversarially: untrusted input crossing trust boundaries (network, IPC, D-Bus, MCP tool args), secrets in logs, missing auth checks, panic-on-input, unparameterized SQL/shell.
- Scan dependencies whenever the lockfile changed (`cargo audit` or the `cve-mcp` server) — and scan BEFORE the first build, because build scripts execute attacker-controlled code at build time.
- High/critical CVEs are hard blockers: patch in the same change, prove the path unreachable and document why, or file a tracked follow-up referenced in the change. Never ship past one silently; never pin around an advisory without a comment or tracking issue.

### Maintainability / cognitive load
- Keep each change small enough to land independently with a clear deliverable.
- Don't introduce a new abstraction until ~3 call sites prove the pattern; when one new type unifies several needs, justify the unification explicitly.
- Reuse existing traits and patterns rather than inventing parallel ones; extend an existing crate over adding one unless the seam is obvious.


### Capability-based degradation
- Every reliance on an optional OS/desktop service (logind, screen-lock, KDE/Plasma, PipeWire specifics, any session- or system-bus D-Bus interface) must be capability-detected and degrade gracefully — never a hard dependency that errors or hangs when absent. The product may run headless, in containers, on other DEs, or as a system service.
- Distinguish "is the capability present?" from "did my call succeed?" Three states: absent → disable that feature, log once, fall back to prior behavior; present-and-known → use it; present-but-anomalous → stay conservative / last-known-state and warn. Scope any privacy/safety fail-safe to the last two — a fail-safe correct on the desktop can be pathological headless (e.g. "treat unknown session as inactive" ⇒ mic never opens).
- Detect each optional dependency independently; absence of one never disables the others or aborts startup. Surface the detected capability so an operator sees *why* a feature is on or off.

### GitHub issue / PR / board hygiene
- Self-assign an issue when you start it (or comment to claim it) so parallel work doesn't collide; move the board card to In Progress.
- Link the PR to the issue: `Closes #N` to auto-close, `Refs #N` when it only partially addresses it.
- Keep the board in sync with reality (In Review on open, Done on merge); if you can't move the card, comment the intended status.
- On multi-session work, leave a short status comment before stopping — what landed, what's next, what's blocked — so state is reconstructable without git log.

### Worktrees
- Do code work in a git worktree on its own branch off `origin/main`, never the primary checkout, so concurrent sessions don't collide. Convention: `~/Projects/adelie-ai/.worktrees/<repo>/issue-N-slug/`, branch mirroring the slug.
- Run independent tasks in parallel worktrees, but check first for shared files / shared `Cargo.toml` dep edits / shared migration ordinals — if they overlap, serialize. Brief each parallel agent on its scope ("own crate X, don't touch Y").
