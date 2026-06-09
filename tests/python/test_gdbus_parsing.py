"""Tests for ``_parse_gdbus_output`` in ``dbus_client.py``.

On the D-Bus transport the widget shells out to ``gdbus call`` and parses its
human-readable GVariant text output back into Python values. Every D-Bus read
flows through it, so a parsing slip corrupts conversation lists, message
bodies and status checks. The parser must treat string literals as opaque:
``true``/``false``, ``int32 5`` and ``@type``-shaped tokens INSIDE a quoted
string are message content, not GVariant syntax (KDE-1 — the old
regex+ast.literal_eval round-trip mangled exactly those). Malformed output
must raise ``DbusError`` rather than return junk.
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

    # --- KDE-1: GVariant syntax tokens inside string literals are content ----
    # Reproduced live: the old regex pass rewrote the whole output, including
    # quoted message bodies, before ast.literal_eval. These exact cases came
    # out of a real GetConversation reply.

    def test_string_containing_true_false_is_preserved(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output("('user', 'is this true or false?')"),
            ("user", "is this true or false?"),
        )

    def test_string_containing_typed_int_and_annotation_is_preserved(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output("('assistant', 'value @s ok int32 5')"),
            ("assistant", "value @s ok int32 5"),
        )

    def test_code_snippet_message_body_is_preserved(self) -> None:
        body = "if (ok) { return true; } // uint64 9, @a{sv} {}"
        self.assertEqual(
            dbus_client._parse_gdbus_output("('assistant', \"%s\")" % body),
            ("assistant", body),
        )

    def test_double_quoted_string_with_single_quote(self) -> None:
        # g_variant_print switches to double quotes when the string contains
        # an apostrophe.
        self.assertEqual(
            dbus_client._parse_gdbus_output('("don\'t",)'),
            ("don't",),
        )

    def test_single_quoted_string_with_escaped_quote(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output(r"('say \'hi\'',)"),
            ("say 'hi'",),
        )

    def test_string_escapes(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output(r"('line1\nline2\ttab \\ é \U0001f600',)"),
            ("line1\nline2\ttab \\ é \U0001f600",),
        )

    # --- Full GVariant text-format coverage the regex pass never had ---------

    def test_variant_is_unwrapped(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(<'hello'>,)"), ("hello",))

    def test_dict_of_variants_like_a_sv(self) -> None:
        out = "({'state': <'Listening'>, 'count': <uint32 3>, 'on': <true>},)"
        self.assertEqual(
            dbus_client._parse_gdbus_output(out),
            ({"state": "Listening", "count": 3, "on": True},),
        )

    def test_objectpath_and_signature_keywords(self) -> None:
        self.assertEqual(
            dbus_client._parse_gdbus_output("(objectpath '/org/foo', signature 'a{sv}')"),
            ("/org/foo", "a{sv}"),
        )

    def test_doubles(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(0.45, -1.5e3)"), (0.45, -1500.0))

    def test_typed_double(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(double 0.35,)"), (0.35,))

    def test_hex_byte(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(byte 0x07,)"), (7,))

    def test_empty_annotated_array(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(@as [],)"), ([],))

    def test_empty_tuple_unit_reply(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("()"), ())

    def test_maybe_nothing_and_just(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(@ms nothing,)"), (None,))
        self.assertEqual(dbus_client._parse_gdbus_output("(just 'x',)"), ("x",))

    def test_bytestring(self) -> None:
        self.assertEqual(dbus_client._parse_gdbus_output("(b'ab',)"), (b"ab",))

    def test_trailing_garbage_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("('a',) extra")

    def test_unterminated_string_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("('abc")

    def test_garbage_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("this is (((not a literal")

    def test_empty_output_raises_dbuserror(self) -> None:
        with self.assertRaises(dbus_client.DbusError):
            dbus_client._parse_gdbus_output("")


if __name__ == "__main__":
    unittest.main()
