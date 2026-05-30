"""Tests for the ``dbus_client.py`` CLI JSON contracts (non-task commands).

The CLI is the ABI the QML widget binds to via ``Plasma5Support.DataSource``:
QML runs ``dbus_client.py <cmd>`` and ``JSON.parse``-s stdout. The
background-task subcommands are covered in ``test_background_tasks.py``; this
file covers the conversation/connection/status surface — ``list``, ``get``,
``connections``, ``status``, ``create``, ``delete``, ``clear``, ``send`` — and
the uniform ``{"error": ...}`` + exit-code-1 failure contract.

Settings loading and the transport calls are stubbed so the tests don't read
the developer's real ``widget_settings.json`` or touch a daemon.
"""

from __future__ import annotations

import io
import json
import os
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


class CliShapeTests(unittest.TestCase):
    _GLOBALS = ("TRANSPORT", "_ws_request", "_run_gdbus", "_name_has_owner",
                "_load_widget_settings_payload")

    def setUp(self) -> None:
        self._saved = {name: getattr(dbus_client, name) for name in self._GLOBALS}
        self._orig_argv = sys.argv
        # Hermetic: never read the developer's real config file, and strip any
        # ambient DESKTOP_ASSISTANT_WIDGET_* env that would override transport.
        dbus_client._load_widget_settings_payload = lambda: {}
        env_patcher = mock.patch.dict(os.environ, {}, clear=False)
        env_patcher.start()
        self.addCleanup(env_patcher.stop)
        for key in [k for k in list(os.environ) if k.startswith("DESKTOP_ASSISTANT_WIDGET_")]:
            os.environ.pop(key, None)

    def tearDown(self) -> None:
        for name, value in self._saved.items():
            setattr(dbus_client, name, value)
        sys.argv = self._orig_argv

    def _run(self, argv: list[str]) -> tuple[int, dict]:
        sys.argv = ["dbus_client.py", *argv]
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = dbus_client.main()
        return rc, json.loads(buf.getvalue().strip() or "{}")

    def test_list_prints_conversations_array(self) -> None:
        dbus_client._ws_request = lambda _p: {"conversations": [
            {"id": "c1", "title": "One", "message_count": 3, "updated_at": "2026", "archived": False},
        ]}
        rc, payload = self._run(["--transport", "ws", "list"])
        self.assertEqual(rc, 0)
        self.assertEqual(len(payload["conversations"]), 1)
        self.assertEqual(payload["conversations"][0]["id"], "c1")

    def test_get_prints_messages_envelope(self) -> None:
        dbus_client._ws_request = lambda _p: {"conversation": {
            "id": "c", "title": "T",
            "messages": [{"role": "user", "content": "hi"}, {"role": "assistant", "content": "yo"}],
        }}
        rc, payload = self._run(["--transport", "ws", "get", "c"])
        self.assertEqual(rc, 0)
        self.assertIn("messages", payload)
        self.assertEqual(payload["message_count"], 2)

    def test_connections_serializes_configured_connections(self) -> None:
        dbus_client._load_widget_settings_payload = lambda: {
            "connections": [
                {"name": "local"},
                {"name": "cluster", "transport": "ws", "ws_url": "ws://k/ws", "ws_subject": "sub"},
            ],
            "default_connection": "local",
        }
        rc, payload = self._run(["connections"])
        self.assertEqual(rc, 0)
        self.assertEqual({c["name"] for c in payload["connections"]}, {"local", "cluster"})
        self.assertEqual(payload["default_connection"], "local")
        self.assertIn("selected_connection", payload)

    def test_status_dbus_reports_running(self) -> None:
        dbus_client._name_has_owner = lambda name: name == dbus_client.DEFAULT_SERVICE
        rc, payload = self._run(["--transport", "dbus", "status"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["transport"], "dbus")
        self.assertTrue(payload["production_running"])

    def test_create_prints_conversation_id(self) -> None:
        dbus_client._ws_request = lambda _p: {"conversation_id": {"id": "new-id"}}
        rc, payload = self._run(["--transport", "ws", "create", "--title", "Hi"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["conversation_id"], "new-id")

    def test_delete_prints_confirmation(self) -> None:
        dbus_client._ws_request = lambda _p: {"ack": {}}
        rc, payload = self._run(["--transport", "ws", "delete", "c1"])
        self.assertEqual(rc, 0)
        self.assertTrue(payload["deleted"])
        self.assertEqual(payload["conversation_id"], "c1")

    def test_clear_prints_deleted_count(self) -> None:
        dbus_client._ws_request = lambda _p: {"cleared": {"deleted_count": 4}}
        rc, payload = self._run(["--transport", "ws", "clear"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["deleted_count"], 4)

    def test_send_prints_request_id_key(self) -> None:
        dbus_client._ws_request = lambda _p: {"ack": {}}
        rc, payload = self._run(["--transport", "ws", "send", "c1", "hello"])
        self.assertEqual(rc, 0)
        self.assertIn("request_id", payload)

    def test_error_path_sets_error_and_exit_code_1(self) -> None:
        def boom(_p):
            raise dbus_client.WsError("daemon down")

        dbus_client._ws_request = boom
        rc, payload = self._run(["--transport", "ws", "list"])
        self.assertEqual(rc, 1)
        self.assertEqual(payload["error"], "daemon down")

    def test_invalid_transport_override_errors(self) -> None:
        rc, payload = self._run(["--transport", "carrier-pigeon", "list"])
        self.assertEqual(rc, 1)
        self.assertIn("invalid transport", payload["error"])


if __name__ == "__main__":
    unittest.main()
