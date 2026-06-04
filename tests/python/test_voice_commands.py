"""Tests for the voice-service CLI surface of ``dbus_client.py`` (adele-kde#29).

The chat plasmoid drives the voice daemon (repo adelie-ai/voice,
``org.desktopAssistant.Voice``) through these subcommands exactly the way it
drives the conversation commands: it runs ``dbus_client.py voice-*`` via a
``Plasma5Support`` DataSource and ``JSON.parse``-s stdout. These tests pin:

  * the JSON shape of each command's stdout,
  * gdbus output parsing (``a(sssu)`` voice list, ``(si)`` current voice),
  * the graceful-degradation contract: ``voice-status`` ALWAYS exits 0 and
    reports ``available: false`` when the service has no bus owner, while the
    mutating/reading commands surface the ``ServiceUnknown`` error + exit 1.

The actual ``gdbus`` invocation is stubbed (``_run_gdbus_voice`` /
``_name_has_owner``) so nothing touches a live bus.
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


class VoiceParsingTests(unittest.TestCase):
    """Unit tests for the voice value parsers over stubbed gdbus output."""

    def setUp(self) -> None:
        self._saved = {name: getattr(dbus_client, name) for name in ("_run_gdbus_voice",)}

    def tearDown(self) -> None:
        for name, value in self._saved.items():
            setattr(dbus_client, name, value)

    def test_list_voices_parses_sssu_array(self) -> None:
        # gdbus renders `a(sssu)` as a 1-tuple wrapping a list of tuples.
        dbus_client._run_gdbus_voice = lambda *_a: (
            [
                ("en_US-amy-medium", "Amy (US)", "en_US", 1),
                ("en_GB-vctk", "VCTK", "en_GB", 109),
            ],
        )
        voices = dbus_client.voice_list_voices()
        self.assertEqual(len(voices), 2)
        self.assertEqual(voices[0]["voice_id"], "en_US-amy-medium")
        self.assertEqual(voices[0]["num_speakers"], 1)
        self.assertEqual(voices[1]["num_speakers"], 109)

    def test_list_voices_handles_empty(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: ([],)
        self.assertEqual(dbus_client.voice_list_voices(), [])

    def test_list_voices_skips_malformed_rows(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: (
            [("good", "Good", "en", 1), ("too", "short")],
        )
        voices = dbus_client.voice_list_voices()
        self.assertEqual(len(voices), 1)
        self.assertEqual(voices[0]["voice_id"], "good")

    def test_get_voice_struct(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: ("en_US-amy-medium", 2)
        voice = dbus_client.voice_get_voice()
        self.assertEqual(voice["voice_id"], "en_US-amy-medium")
        self.assertEqual(voice["speaker_id"], 2)

    def test_get_voice_unset_speaker(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: ("single", -1)
        self.assertEqual(dbus_client.voice_get_voice()["speaker_id"], -1)

    def test_get_state_unwraps_tuple(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: ("Listening",)
        self.assertEqual(dbus_client.voice_get_state(), "Listening")

    def test_get_enabled_unwraps_bool(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: (True,)
        self.assertTrue(dbus_client.voice_get_enabled())

    def test_set_voice_passes_negative_speaker_with_type(self) -> None:
        captured: list[tuple] = []
        dbus_client._run_gdbus_voice = lambda *a: captured.append(a) or None
        dbus_client.voice_set_voice("v", -1)
        # gdbus would treat a bare "-1" as an option flag, so it must be typed.
        self.assertEqual(captured[0], ("SetVoice", "v", "int32 -1"))

    def test_set_voice_passes_plain_positive_speaker(self) -> None:
        captured: list[tuple] = []
        dbus_client._run_gdbus_voice = lambda *a: captured.append(a) or None
        dbus_client.voice_set_voice("v", 4)
        self.assertEqual(captured[0], ("SetVoice", "v", "4"))

    def test_set_enabled_maps_bool_to_gvariant_literal(self) -> None:
        captured: list[tuple] = []
        dbus_client._run_gdbus_voice = lambda *a: captured.append(a) or None
        dbus_client.voice_set_enabled(True)
        dbus_client.voice_set_enabled(False)
        self.assertEqual(captured[0], ("SetEnabled", "true"))
        self.assertEqual(captured[1], ("SetEnabled", "false"))


class VoiceAvailabilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self._saved = {name: getattr(dbus_client, name) for name in ("_name_has_owner",)}

    def tearDown(self) -> None:
        for name, value in self._saved.items():
            setattr(dbus_client, name, value)

    def test_available_true(self) -> None:
        dbus_client._name_has_owner = lambda name: name == dbus_client.VOICE_SERVICE
        self.assertTrue(dbus_client.voice_available())

    def test_available_false(self) -> None:
        dbus_client._name_has_owner = lambda _name: False
        self.assertFalse(dbus_client.voice_available())

    def test_available_swallows_dbus_error(self) -> None:
        def boom(_name):
            raise dbus_client.DbusError("bus down")

        dbus_client._name_has_owner = boom
        # "can't tell" must be treated as "not running", never raise.
        self.assertFalse(dbus_client.voice_available())


class VoiceCliShapeTests(unittest.TestCase):
    _GLOBALS = (
        "_run_gdbus_voice",
        "_name_has_owner",
        "_load_widget_settings_payload",
    )

    def setUp(self) -> None:
        self._saved = {name: getattr(dbus_client, name) for name in self._GLOBALS}
        self._orig_argv = sys.argv
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

    def test_voice_status_available_folds_in_state(self) -> None:
        dbus_client._name_has_owner = lambda name: name == dbus_client.VOICE_SERVICE

        def fake_voice(method, *_a):
            return {
                "GetState": ("Idle",),
                "GetEnabled": (True,),
                "GetVoice": ("en_US-amy-medium", -1),
            }[method]

        dbus_client._run_gdbus_voice = fake_voice
        rc, payload = self._run(["voice-status"])
        self.assertEqual(rc, 0)
        self.assertTrue(payload["available"])
        self.assertEqual(payload["state"], "Idle")
        self.assertTrue(payload["enabled"])
        self.assertEqual(payload["voice"]["voice_id"], "en_US-amy-medium")

    def test_voice_status_unavailable_exits_zero(self) -> None:
        dbus_client._name_has_owner = lambda _name: False
        rc, payload = self._run(["voice-status"])
        # Service down is a normal state, not an error: exit 0, available false.
        self.assertEqual(rc, 0)
        self.assertFalse(payload["available"])
        self.assertNotIn("state", payload)

    def test_voice_status_owner_but_call_fails_is_unavailable(self) -> None:
        dbus_client._name_has_owner = lambda name: name == dbus_client.VOICE_SERVICE

        def boom(_method, *_a):
            raise dbus_client.DbusError("daemon mid-shutdown")

        dbus_client._run_gdbus_voice = boom
        rc, payload = self._run(["voice-status"])
        self.assertEqual(rc, 0)
        self.assertFalse(payload["available"])
        self.assertIn("error", payload)

    def test_voice_list_voices_shape(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: (
            [("v1", "Voice One", "en", 1)],
        )
        rc, payload = self._run(["voice-list-voices"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["voices"][0]["display_name"], "Voice One")

    def test_voice_get_voice_shape(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: ("v1", 3)
        rc, payload = self._run(["voice-get-voice"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["voice"]["speaker_id"], 3)

    def test_voice_set_voice_echoes_args(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: None
        rc, payload = self._run(["voice-set-voice", "v1", "--speaker", "2"])
        self.assertEqual(rc, 0)
        self.assertEqual(payload["voice_id"], "v1")
        self.assertEqual(payload["speaker"], 2)

    def test_voice_set_enabled_echoes_bool(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: None
        rc, payload = self._run(["voice-set-enabled", "true"])
        self.assertEqual(rc, 0)
        self.assertTrue(payload["enabled"])

    def test_voice_push_to_talk_ok(self) -> None:
        dbus_client._run_gdbus_voice = lambda *_a: None
        rc, payload = self._run(["voice-push-to-talk"])
        self.assertEqual(rc, 0)
        self.assertTrue(payload["ok"])

    def test_voice_command_error_maps_to_exit_1(self) -> None:
        def boom(*_a):
            raise dbus_client.DbusError(
                "Error: GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: gone"
            )

        dbus_client._run_gdbus_voice = boom
        rc, payload = self._run(["voice-list-voices"])
        self.assertEqual(rc, 1)
        self.assertIn("ServiceUnknown", payload["error"])


if __name__ == "__main__":
    unittest.main()
