"""Tests for how ``dbus_client.py`` maps transport failures to errors.

The widget surfaces failures to QML as ``{"error": "..."}`` JSON, so the
message that comes out of a failed ``gdbus`` call or WS response decides what
the user sees. These tests pin:

* ``_run_command`` — the gdbus subprocess wrapper: missing binary, timeout,
  and the stderr/stdout/hint precedence of a non-zero exit;
* ``_ws_expect_variant`` — the WS envelope guard;
* ``cmd_status`` — the JSON shape and running-flags for both transports,
  including the degraded (daemon-down) branch.

``subprocess.run`` and the transport calls are mocked; nothing touches a real
bus, socket or daemon.
"""

from __future__ import annotations

import io
import json
import subprocess
import sys
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


class RunCommandTests(unittest.TestCase):
    def test_success_parses_stdout(self) -> None:
        with mock.patch.object(
            dbus_client.subprocess, "run",
            return_value=types.SimpleNamespace(stdout="(true,)"),
        ):
            self.assertEqual(dbus_client._run_command(["gdbus"], "hint"), (True,))

    def test_missing_gdbus_binary_raises_with_install_hint(self) -> None:
        with mock.patch.object(dbus_client.subprocess, "run", side_effect=FileNotFoundError()):
            with self.assertRaises(dbus_client.DbusError) as ctx:
                dbus_client._run_command(["gdbus"], "hint")
        self.assertIn("not found", str(ctx.exception))

    def test_timeout_raises_with_hint_and_duration(self) -> None:
        exc = subprocess.TimeoutExpired(cmd=["gdbus"], timeout=12.0)
        with mock.patch.object(dbus_client.subprocess, "run", side_effect=exc):
            with self.assertRaises(dbus_client.DbusError) as ctx:
                dbus_client._run_command(["gdbus"], "my hint", timeout_sec=12.0)
        message = str(ctx.exception)
        self.assertIn("my hint", message)
        self.assertIn("timed out", message)

    def test_called_process_error_prefers_stderr(self) -> None:
        exc = subprocess.CalledProcessError(1, ["gdbus"], output="out msg", stderr="err msg")
        with mock.patch.object(dbus_client.subprocess, "run", side_effect=exc):
            with self.assertRaises(dbus_client.DbusError) as ctx:
                dbus_client._run_command(["gdbus"], "hint")
        self.assertEqual(str(ctx.exception), "err msg")

    def test_called_process_error_falls_back_to_stdout(self) -> None:
        exc = subprocess.CalledProcessError(1, ["gdbus"], output="out only", stderr="")
        with mock.patch.object(dbus_client.subprocess, "run", side_effect=exc):
            with self.assertRaises(dbus_client.DbusError) as ctx:
                dbus_client._run_command(["gdbus"], "hint")
        self.assertEqual(str(ctx.exception), "out only")

    def test_called_process_error_falls_back_to_hint(self) -> None:
        exc = subprocess.CalledProcessError(1, ["gdbus"], output="", stderr="")
        with mock.patch.object(dbus_client.subprocess, "run", side_effect=exc):
            with self.assertRaises(dbus_client.DbusError) as ctx:
                dbus_client._run_command(["gdbus"], "the hint")
        self.assertEqual(str(ctx.exception), "the hint")


class ExpectVariantTests(unittest.TestCase):
    def test_returns_value_for_present_key(self) -> None:
        self.assertEqual(dbus_client._ws_expect_variant({"ack": {"x": 1}}, "ack"), {"x": 1})

    def test_missing_key_raises(self) -> None:
        with self.assertRaises(dbus_client.WsError):
            dbus_client._ws_expect_variant({"other": 1}, "ack")

    def test_non_dict_raises(self) -> None:
        with self.assertRaises(dbus_client.WsError):
            dbus_client._ws_expect_variant("nope", "ack")

    def test_empty_dict_raises(self) -> None:
        with self.assertRaises(dbus_client.WsError):
            dbus_client._ws_expect_variant({}, "ack")


class CmdStatusTests(unittest.TestCase):
    _GLOBALS = ("TRANSPORT", "WS_URL", "_ws_request", "_name_has_owner",
                "CONNECTION_NAME", "SERVICE")

    def setUp(self) -> None:
        self._saved = {name: getattr(dbus_client, name) for name in self._GLOBALS}

    def tearDown(self) -> None:
        for name, value in self._saved.items():
            setattr(dbus_client, name, value)

    def _run(self) -> tuple[int, dict]:
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = dbus_client.cmd_status()
        return rc, json.loads(buf.getvalue())

    def test_ws_running_reports_production_up(self) -> None:
        dbus_client.TRANSPORT = "ws"
        dbus_client._ws_request = lambda _p: {"pong": {"value": "pong"}}
        rc, payload = self._run()
        self.assertEqual(rc, 0)
        self.assertEqual(payload["transport"], "ws")
        self.assertTrue(payload["production_running"])
        self.assertFalse(payload["dev_running"])
        self.assertNotIn("error", payload)
        for key in ("selected_connection", "default_connection", "ws_url",
                    "selected_service", "default_service", "dev_service"):
            self.assertIn(key, payload)

    def test_ws_failure_sets_error_and_not_running(self) -> None:
        dbus_client.TRANSPORT = "ws"

        def boom(_p):
            raise dbus_client.WsError("connect refused")

        dbus_client._ws_request = boom
        rc, payload = self._run()
        self.assertEqual(rc, 0)
        self.assertFalse(payload["production_running"])
        self.assertEqual(payload["error"], "connect refused")

    def test_dbus_running_checks_name_owner(self) -> None:
        dbus_client.TRANSPORT = "dbus"
        dbus_client._name_has_owner = lambda name: name == dbus_client.DEFAULT_SERVICE
        rc, payload = self._run()
        self.assertEqual(rc, 0)
        self.assertEqual(payload["transport"], "dbus")
        self.assertTrue(payload["production_running"])
        self.assertFalse(payload["dev_running"])
        self.assertEqual(payload["ws_url"], "")  # ws_url is blanked on D-Bus

    def test_dbus_failure_sets_error(self) -> None:
        dbus_client.TRANSPORT = "dbus"

        def boom(_name):
            raise dbus_client.DbusError("no bus")

        dbus_client._name_has_owner = boom
        rc, payload = self._run()
        self.assertEqual(rc, 0)
        self.assertFalse(payload["production_running"])
        self.assertEqual(payload["error"], "no bus")


if __name__ == "__main__":
    unittest.main()
