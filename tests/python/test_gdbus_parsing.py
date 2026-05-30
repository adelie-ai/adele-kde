"""Tests for ``_parse_gdbus_output`` in ``dbus_client.py``.

On the D-Bus transport the widget shells out to ``gdbus call`` and parses its
human-readable GVariant text output back into Python values. The parser
strips GVariant type annotations (``@a{sv}``), unwraps typed integers
(``uint32 5`` -> ``5``), maps ``true``/``false`` to Python bools, and then
runs ``ast.literal_eval``. Every D-Bus read flows through it, so a parsing
slip corrupts conversation lists, message bodies and status checks. Malformed
output must raise ``DbusError`` rather than return junk.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


class ParseGdbusOutputTests(unittest.TestCase):
    def test_booleans(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(true,)"), (True,))
        self.assertEqual(dbus_client._parse_gdbus_output("(false,)"), (False,))

    def test_typed_integers_are_unwrapped(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(uint32 5,)"), (5,))
        self.assertEqual(dbus_client._parse_gdbus_output("(uint16 9,)"), (9,))
        self.assertEqual(dbus_client._parse_gdbus_output("(int64 -3,)"), (-3,))
        self.assertEqual(dbus_client._parse_gdbus_output("(byte 7,)"), (7,))

    def test_plain_string_tuple(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("('hello',)"), ("hello",))

    def test_mixed_tuple(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output("(uint32 5, 'name', true)"),
            (5, "name", True),
        )

    def test_type_annotation_is_stripped(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("@a{sv} {'k': 'v'}"), {"k": "v"})

    def test_nested_list_of_tuples_like_list_conversations(self) -> None:
        # Shape of a real ListConversations reply: (a(ssusb),) with one row.
        out = "([('c1', 'My chat', uint32 3, '2026-01-01', false)],)"
        self.assertEqual(
            dbus_client._parse_gdbus_output(out),
            ([("c1", "My chat", 3, "2026-01-01", False)],),
        )

    def test_garbage_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("this is (((not a literal")

    def test_empty_output_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("")


if __name__ == "__main__":
    unittest.main()
