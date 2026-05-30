"""Tests for transport / connection resolution in ``dbus_client.py``.

The widget decides *how* to reach the daemon (D-Bus vs WebSocket, which
service name, which URL) by parsing ``~/.config/desktop-assistant/
widget_settings.json``. That resolution has several non-obvious rules —
per-connection transport defaults, a legacy flat-config fallback, name
de-duplication, and an env-var override — none of which were unit-tested.
A regression here points the widget at the wrong endpoint or crashes it on
a malformed config file.

Covers ``_normalize_transport``, ``_load_widget_settings_payload``,
``_load_widget_connections`` and ``_load_widget_connection_name``.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


class NormalizeTransportTests(unittest.TestCase):
    def test_ws_variants_normalize_to_ws(self) -> None:
        for value in ("ws", "WS", " ws ", "Ws"):
            self.assertEqual(dbus_client._normalize_transport(value), "ws", value)

    def test_everything_else_falls_back_to_dbus(self) -> None:
        # Only the exact token "ws" selects WebSocket; "websocket"/"wss"/""
        # and anything unknown must degrade to D-Bus rather than error.
        for value in ("dbus", "DBUS", "", "  ", "uds", "websocket", "wss", "garbage"):
            self.assertEqual(dbus_client._normalize_transport(value), "dbus", value)


class LoadSettingsPayloadTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_settings_path = dbus_client.SETTINGS_PATH
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def tearDown(self) -> None:
        dbus_client.SETTINGS_PATH = self._orig_settings_path

    def _point_at(self, content: str | None) -> None:
        path = Path(self._tmp.name) / "widget_settings.json"
        if content is not None:
            path.write_text(content)
        dbus_client.SETTINGS_PATH = path

    def test_valid_dict_is_returned(self) -> None:
        self._point_at(json.dumps({"transport": "ws", "ws_url": "ws://x/ws"}))
        self.assertEqual(
            dbus_client._load_widget_settings_payload(),
            {"transport": "ws", "ws_url": "ws://x/ws"},
        )

    def test_json_list_returns_empty_dict(self) -> None:
        self._point_at(json.dumps([1, 2, 3]))
        self.assertEqual(dbus_client._load_widget_settings_payload(), {})

    def test_json_scalar_returns_empty_dict(self) -> None:
        self._point_at("42")
        self.assertEqual(dbus_client._load_widget_settings_payload(), {})

    def test_corrupt_json_returns_empty_dict(self) -> None:
        self._point_at("{ this is not valid json")
        self.assertEqual(dbus_client._load_widget_settings_payload(), {})

    def test_missing_file_returns_empty_dict(self) -> None:
        self._point_at(None)  # never create the file
        self.assertEqual(dbus_client._load_widget_settings_payload(), {})


class LoadConnectionsTests(unittest.TestCase):
    def test_empty_payload_yields_default_local_dbus(self) -> None:
        conns, default = dbus_client._load_widget_connections({})
        self.assertEqual(default, "local")
        self.assertIn("local", conns)
        self.assertEqual(conns["local"]["transport"], "dbus")
        self.assertEqual(conns["local"]["dbus_service"], dbus_client.DEFAULT_SERVICE)

    def test_explicit_connections_normalize_per_entry(self) -> None:
        payload = {
            "connections": [
                {"name": "a", "transport": "ws", "ws_url": "ws://a/ws"},
                {"name": "local"},  # name 'local' with no transport -> dbus
                {"name": "b"},      # any other name with no transport -> ws
            ],
            "default_connection": "a",
        }
        conns, default = dbus_client._load_widget_connections(payload)
        self.assertEqual(default, "a")
        self.assertEqual(conns["a"]["transport"], "ws")
        self.assertEqual(conns["a"]["ws_url"], "ws://a/ws")
        self.assertEqual(conns["local"]["transport"], "dbus")
        self.assertEqual(conns["b"]["transport"], "ws")

    def test_duplicate_names_first_wins(self) -> None:
        payload = {
            "connections": [
                {"name": "x", "transport": "ws"},
                {"name": "x", "transport": "dbus"},
            ]
        }
        conns, _ = dbus_client._load_widget_connections(payload)
        self.assertEqual(len(conns), 1)
        self.assertEqual(conns["x"]["transport"], "ws")  # first definition wins

    def test_empty_name_entries_are_skipped(self) -> None:
        payload = {"connections": [{"name": "   "}, {"name": "y", "transport": "ws"}]}
        conns, _ = dbus_client._load_widget_connections(payload)
        self.assertNotIn("", conns)
        self.assertEqual(list(conns.keys()), ["y"])

    def test_non_dict_connection_items_are_skipped(self) -> None:
        payload = {"connections": ["nope", 5, {"name": "ok", "transport": "ws"}]}
        conns, _ = dbus_client._load_widget_connections(payload)
        self.assertEqual(list(conns.keys()), ["ok"])

    def test_legacy_flat_ws_config_creates_legacy_ws(self) -> None:
        payload = {"transport": "ws", "ws_url": "ws://legacy/ws", "ws_subject": "sub"}
        conns, default = dbus_client._load_widget_connections(payload)
        self.assertEqual(default, "legacy-ws")
        self.assertEqual(conns["legacy-ws"]["transport"], "ws")
        self.assertEqual(conns["legacy-ws"]["ws_url"], "ws://legacy/ws")
        self.assertEqual(conns["legacy-ws"]["ws_subject"], "sub")

    def test_legacy_ws_url_alone_triggers_legacy(self) -> None:
        # A bare ws_url (no transport key) still implies the legacy WS path.
        conns, default = dbus_client._load_widget_connections({"ws_url": "ws://only/ws"})
        self.assertIn("legacy-ws", conns)
        self.assertEqual(default, "legacy-ws")

    def test_invalid_default_connection_falls_back_to_first(self) -> None:
        payload = {
            "connections": [{"name": "a", "transport": "ws"}],
            "default_connection": "ghost",
        }
        _conns, default = dbus_client._load_widget_connections(payload)
        # 'local' is not present, so the first defined connection wins.
        self.assertEqual(default, "a")

    def test_dbus_service_propagates_as_per_connection_default(self) -> None:
        payload = {
            "dbus_service": "org.example.Svc",
            "connections": [{"name": "a", "transport": "dbus"}],
        }
        conns, _ = dbus_client._load_widget_connections(payload)
        self.assertEqual(conns["a"]["dbus_service"], "org.example.Svc")


class LoadConnectionNameTests(unittest.TestCase):
    ENV = "DESKTOP_ASSISTANT_WIDGET_CONNECTION"

    def test_env_var_wins_over_payload(self) -> None:
        with mock.patch.dict(os.environ, {self.ENV: "from-env"}):
            self.assertEqual(
                dbus_client._load_widget_connection_name({"connection_name": "from-payload"}),
                "from-env",
            )

    def test_payload_connection_name_when_env_absent(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(self.ENV, None)
            self.assertEqual(
                dbus_client._load_widget_connection_name({"connection_name": "cn"}), "cn"
            )

    def test_falls_back_to_legacy_connection_key(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(self.ENV, None)
            self.assertEqual(
                dbus_client._load_widget_connection_name({"connection": "legacy"}), "legacy"
            )

    def test_connection_name_beats_connection(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(self.ENV, None)
            self.assertEqual(
                dbus_client._load_widget_connection_name(
                    {"connection_name": "primary", "connection": "secondary"}
                ),
                "primary",
            )

    def test_nothing_set_returns_empty_string(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(self.ENV, None)
            self.assertEqual(dbus_client._load_widget_connection_name({}), "")


if __name__ == "__main__":
    unittest.main()
