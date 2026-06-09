"""Tests for the in-package ``dbus_client.py`` launcher shims (KDE-3).

Each plasmoid ships a tiny shim at ``contents/code/dbus_client.py`` that
prefers the XDG shared chat-module helper (which `just chat-module-sync`
keeps fresh) and falls back to an in-package ``dbus_client_impl.py`` copy
that `just chatview-sync` syncs and `just chatview-verify` drift-gates.

The KDE-3 failure mode being locked down: desktopchat's fallback impl was a
frozen pre-voice/tasks/connections snapshot that was NOT in the drift gate,
so a missing XDG copy silently amputated features; panelchat's shim reached
across packages into desktopchat (a hidden install-order dependency). Both
shims must be identical, fall back only within their own package, and fail
LOUDLY (actionable JSON error) when no helper can be found.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DESKTOP_SHIM = (
    REPO_ROOT
    / "plasmoid"
    / "org.desktopassistant.desktopchat"
    / "contents"
    / "code"
    / "dbus_client.py"
)
PANEL_SHIM = (
    REPO_ROOT
    / "plasmoid"
    / "org.desktopassistant.panelchat"
    / "contents"
    / "code"
    / "dbus_client.py"
)
SHARED_HELPER = REPO_ROOT / "shared" / "chat-module" / "code" / "dbus_client.py"

FAKE_IMPL = 'import sys\nprint("FAKE_IMPL " + " ".join(sys.argv[1:]))\n'


def _run_shim(shim: Path, xdg_data_home: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["XDG_DATA_HOME"] = str(xdg_data_home)
    return subprocess.run(
        [sys.executable, str(shim), *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
    )


class HelperShimTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _xdg_with_shared_helper(self) -> Path:
        xdg = self.tmp / "xdg"
        code_dir = xdg / "desktop-assistant" / "chat-module" / "code"
        code_dir.mkdir(parents=True)
        (code_dir / "dbus_client.py").write_text(
            'import sys\nprint("SHARED_HELPER " + " ".join(sys.argv[1:]))\n'
        )
        return xdg

    def _empty_xdg(self) -> Path:
        xdg = self.tmp / "xdg-empty"
        xdg.mkdir()
        return xdg

    def _shim_copy(self, shim: Path, with_impl: bool) -> Path:
        """Copy a shim into an isolated dir, optionally with a fake impl."""
        pkg_dir = self.tmp / f"pkg-{shim.parent.parent.parent.name}-{with_impl}"
        pkg_dir.mkdir()
        target = pkg_dir / "dbus_client.py"
        shutil.copy(shim, target)
        if with_impl:
            (pkg_dir / "dbus_client_impl.py").write_text(FAKE_IMPL)
        return target

    # --- shared-copy preference (already true before KDE-3, must stay) -------

    def test_shims_prefer_xdg_shared_copy(self) -> None:
        xdg = self._xdg_with_shared_helper()
        for shim in (DESKTOP_SHIM, PANEL_SHIM):
            with self.subTest(shim=shim):
                result = _run_shim(shim, xdg, "status")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "SHARED_HELPER status")

    # --- KDE-3: in-package fallback only, never cross-package ----------------

    def test_shims_fall_back_to_in_package_impl(self) -> None:
        for shim in (DESKTOP_SHIM, PANEL_SHIM):
            with self.subTest(shim=shim):
                isolated = self._shim_copy(shim, with_impl=True)
                result = _run_shim(isolated, self._empty_xdg(), "voice-status")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), "FAKE_IMPL voice-status")

    def test_shims_are_identical(self) -> None:
        # Two packages, one shim: any divergence reintroduces the silent
        # feature-amputation bug class.
        self.assertEqual(DESKTOP_SHIM.read_text(), PANEL_SHIM.read_text())

    # --- KDE-3: loud, actionable failure when no helper exists ---------------

    def test_missing_helper_fails_loudly_with_remediation(self) -> None:
        for shim in (DESKTOP_SHIM, PANEL_SHIM):
            with self.subTest(shim=shim):
                isolated = self._shim_copy(shim, with_impl=False)
                result = _run_shim(isolated, self._empty_xdg(), "status")
                self.assertEqual(result.returncode, 1)
                payload = json.loads(result.stdout.strip())
                self.assertIn("error", payload)
                # The error must tell the user how to fix it, not just shrug.
                self.assertIn("chat-module-sync", payload["error"])

    # --- drift-gate prerequisite: the synced fallback copies exist -----------

    def test_in_package_impls_match_shared_helper(self) -> None:
        shared = SHARED_HELPER.read_text()
        for shim in (DESKTOP_SHIM, PANEL_SHIM):
            impl = shim.with_name("dbus_client_impl.py")
            with self.subTest(impl=impl):
                self.assertTrue(impl.exists(), f"missing synced fallback: {impl}")
                self.assertEqual(impl.read_text(), shared, "run 'just chatview-sync'")


if __name__ == "__main__":
    unittest.main()
