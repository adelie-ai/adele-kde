"""Tests for the read-path message shaping in ``dbus_client.py``.

``get_messages`` and ``get_conversation`` apply tail truncation, after-count
pagination and (for ``get_messages``) role filtering before handing data to
the QML chat view. The contract is subtle: ``message_count`` is always the
*total unfiltered* count (callers use it as the next ``after_count``), role
filtering happens *after* slicing, and tail only applies in non-paginated
reads. ``_build_override`` builds the optional per-message routing payload.

All daemon calls are stubbed; no transport is opened.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402

FIVE_MESSAGES = [
    {"role": "user", "content": "u1"},
    {"role": "assistant", "content": "a1"},
    {"role": "system", "content": "s1"},
    {"role": "user", "content": "u2"},
    {"role": "assistant", "content": "a2"},
]


class GetMessagesWsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        self._orig_ws_request = dbus_client._ws_request
        dbus_client.TRANSPORT = "ws"

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def _stub(self, messages) -> None:
        dbus_client._ws_request = lambda _p: {"conversation": {"id": "c", "messages": messages}}

    def test_default_roles_filter_out_system(self) -> None:
        self._stub(FIVE_MESSAGES)
        out = dbus_client.get_messages("c")
        self.assertEqual(out["message_count"], 5)  # total, unfiltered
        self.assertEqual(
            [m["role"] for m in out["messages"]],
            ["user", "assistant", "user", "assistant"],
        )
        self.assertFalse(out["truncated"])

    def test_empty_roles_returns_all_roles(self) -> None:
        self._stub(FIVE_MESSAGES)
        out = dbus_client.get_messages("c", include_roles=[])
        self.assertEqual(len(out["messages"]), 5)

    def test_role_allowlist_assistant_only(self) -> None:
        self._stub(FIVE_MESSAGES)
        out = dbus_client.get_messages("c", include_roles=["assistant"])
        self.assertEqual([m["content"] for m in out["messages"]], ["a1", "a2"])
        self.assertEqual(out["message_count"], 5)

    def test_tail_truncates_after_filtering(self) -> None:
        self._stub(FIVE_MESSAGES)
        out = dbus_client.get_messages("c", tail=1)
        self.assertEqual([m["content"] for m in out["messages"]], ["a2"])
        self.assertTrue(out["truncated"])
        self.assertEqual(out["message_count"], 5)

    def test_after_count_slices_before_role_filter(self) -> None:
        self._stub(FIVE_MESSAGES)
        # raw[2:] = [system s1, user u2, assistant a2]; default roles drop system.
        out = dbus_client.get_messages("c", after_count=2)
        self.assertEqual([m["content"] for m in out["messages"]], ["u2", "a2"])
        self.assertFalse(out["truncated"])
        self.assertEqual(out["message_count"], 5)

    def test_empty_conversation(self) -> None:
        self._stub([])
        out = dbus_client.get_messages("c")
        self.assertEqual(out["message_count"], 0)
        self.assertEqual(out["messages"], [])


class GetMessagesDbusTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        self._orig_run_gdbus = dbus_client._run_gdbus
        dbus_client.TRANSPORT = "dbus"

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._run_gdbus = self._orig_run_gdbus

    def test_passes_through_server_result_and_formats_gvariant_args(self) -> None:
        captured: dict = {}

        def fake(method, *args):
            captured["method"] = method
            captured["args"] = args
            return (5, True, [("user", "u1"), ("assistant", "a1")])

        dbus_client._run_gdbus = fake
        out = dbus_client.get_messages("c", tail=2, include_roles=["user", "assistant"])

        self.assertEqual(captured["method"], "GetMessages")
        # Pins the GVariant arg formatting: tail as str, negative after-count
        # sentinel as a typed literal, roles as a single-quoted GVariant array.
        self.assertEqual(captured["args"], ("c", "2", "int32 -1", "['user', 'assistant']"))
        self.assertEqual(out["message_count"], 5)
        self.assertTrue(out["truncated"])
        self.assertEqual([m["role"] for m in out["messages"]], ["user", "assistant"])


class GetConversationTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        self._orig_ws_request = dbus_client._ws_request
        dbus_client.TRANSPORT = "ws"

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def _stub(self, conversation) -> None:
        dbus_client._ws_request = lambda _p: {"conversation": conversation}

    def test_default_returns_all_messages_untruncated(self) -> None:
        self._stub({
            "id": "c", "title": "T",
            "messages": [{"role": "user", "content": "u1"}, {"role": "assistant", "content": "a1"}],
        })
        out = dbus_client.get_conversation("c")
        self.assertEqual(out["id"], "c")
        self.assertEqual(out["title"], "T")
        self.assertEqual(len(out["messages"]), 2)
        self.assertEqual(out["message_count"], 2)
        self.assertFalse(out["truncated"])
        self.assertIsNone(out["after_count"])

    def test_tail_truncates_to_last_n(self) -> None:
        msgs = [{"role": "user", "content": str(i)} for i in range(5)]
        self._stub({"id": "c", "title": "T", "messages": msgs})
        out = dbus_client.get_conversation("c", tail=2)
        self.assertEqual([m["content"] for m in out["messages"]], ["3", "4"])
        self.assertTrue(out["truncated"])
        self.assertEqual(out["message_count"], 5)

    def test_after_count_slices_and_reports_index(self) -> None:
        msgs = [{"role": "user", "content": str(i)} for i in range(5)]
        self._stub({"id": "c", "title": "T", "messages": msgs})
        out = dbus_client.get_conversation("c", after_count=3)
        self.assertEqual([m["content"] for m in out["messages"]], ["3", "4"])
        self.assertFalse(out["truncated"])
        self.assertEqual(out["after_count"], 3)
        self.assertEqual(out["message_count"], 5)

    def test_warnings_are_forwarded(self) -> None:
        self._stub({
            "id": "c", "title": "T", "messages": [],
            "warnings": [{"kind": "DanglingModelSelection"}],
        })
        out = dbus_client.get_conversation("c")
        self.assertEqual(out["warnings"], [{"kind": "DanglingModelSelection"}])

    def test_last_model_selection_forwarded_and_effort_lowercased(self) -> None:
        self._stub({
            "id": "c", "title": "T", "messages": [],
            "last_model_selection": {"connection_id": "x", "model_id": "m", "effort": "HIGH"},
        })
        out = dbus_client.get_conversation("c")
        self.assertEqual(
            out["last_model_selection"],
            {"connection_id": "x", "model_id": "m", "effort": "high"},
        )


class BuildOverrideTests(unittest.TestCase):
    def test_empty_connection_returns_none(self) -> None:
        self.assertIsNone(dbus_client._build_override("", "m", "low"))

    def test_empty_model_returns_none(self) -> None:
        self.assertIsNone(dbus_client._build_override("c", "", "low"))

    def test_without_effort(self) -> None:
        self.assertEqual(
            dbus_client._build_override("c", "m", ""),
            {"connection_id": "c", "model_id": "m"},
        )

    def test_with_valid_effort(self) -> None:
        self.assertEqual(
            dbus_client._build_override("c", "m", "high"),
            {"connection_id": "c", "model_id": "m", "effort": "high"},
        )

    def test_invalid_effort_is_omitted(self) -> None:
        self.assertEqual(
            dbus_client._build_override("c", "m", "ludicrous"),
            {"connection_id": "c", "model_id": "m"},
        )

    def test_strips_and_lowercases(self) -> None:
        self.assertEqual(
            dbus_client._build_override("  c  ", "  m  ", "  Medium  "),
            {"connection_id": "c", "model_id": "m", "effort": "medium"},
        )


if __name__ == "__main__":
    unittest.main()
