#!/usr/bin/env python3
"""Launcher shim for the chat-module D-Bus helper.

Prefers the XDG shared copy (kept fresh by `just chat-module-sync`) so an
installed widget picks up helper updates without a package reinstall, and
falls back to the in-package ``dbus_client_impl.py`` that `just
chatview-sync` syncs from ``shared/chat-module/code/dbus_client.py`` and
`just chatview-verify` drift-gates. Never reaches outside its own package.

This file is intentionally identical across the panelchat and desktopchat
packages (asserted by tests/python/test_helper_shim.py).
"""
import os
import sys
from pathlib import Path


def main() -> int:
    data_home = os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))
    shared = Path(data_home) / "desktop-assistant" / "chat-module" / "code" / "dbus_client.py"
    local_impl = Path(__file__).resolve().with_name("dbus_client_impl.py")

    target = shared if shared.exists() else local_impl
    if not target.exists():
        # Loud + actionable: a silent or vague failure here amputates voice,
        # tasks, and model-selection features with no visible cause (KDE-3).
        print(
            '{"error":"desktop-assistant chat helper not found: '
            "run 'just chat-module-sync' (or reinstall the widgets with "
            "'just widget-install') from the adele-kde repo\"}"
        )
        return 1

    os.execv(sys.executable, [sys.executable, str(target), *sys.argv[1:]])
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
