"""Tests for the background-task surface added by adele-kde#7.

These tests exercise the new functions in ``dbus_client.py``:

* ``list_background_tasks`` (the bridge method that handles a
  ``BackgroundTasks`` response from the daemon).
* ``cancel_background_task`` (translates to a ``CancelBackgroundTask`` WS
  command).
* ``get_background_task_logs`` (translates to ``GetBackgroundTaskLogs``).
* ``subscribe_background_tasks`` (translates to ``SubscribeBackgroundTasks``).
* ``apply_task_event`` (the daemon-event router) — given a task list and a
  ``Task*`` event payload, returns the next task list and the running count.

The bridge functions speak WebSocket only (the D-Bus task interface is in
flight as desktop-assistant#116). When called over a non-WS transport they
must return clearly-typed errors so the QML caller can degrade gracefully.

The CLI is also covered: the ``tasks-list``, ``tasks-cancel``,
``tasks-logs`` and ``tasks-apply-event`` sub-commands serialize their
outputs as JSON the QML side parses with ``JSON.parse``. The CLI is the
ABI the QML widget consumes via ``Plasma5Support.DataSource`` so its shape
matters.
"""

from __future__ import annotations

import io
import json
import os
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
SHARED_CODE = REPO_ROOT / "shared" / "chat-module" / "code"
sys.path.insert(0, str(SHARED_CODE))

import dbus_client  # type: ignore[import-not-found]  # noqa: E402


def _running_task(task_id: str, title: str = "Running task", kind: str = "standalone") -> dict[str, Any]:
    return {
        "id": task_id,
        "kind": {kind: {"name": title, "conversation_id": "c-1"}},
        "status": "running",
        "started_at": 1717000000000,
        "title": title,
    }


def _completed_task(task_id: str, status: str = "completed") -> dict[str, Any]:
    return {
        "id": task_id,
        "kind": {"standalone": {"name": "done", "conversation_id": "c-1"}},
        "status": status,
        "started_at": 1717000000000,
        "ended_at": 1717000010000,
        "title": "done",
    }


class ListBackgroundTasksTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        dbus_client.TRANSPORT = "ws"
        self._orig_ws_request = dbus_client._ws_request

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def test_returns_normalized_task_views(self) -> None:
        sample = [_running_task("t-1", "Researcher"), _completed_task("t-2")]
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return {"background_tasks": sample}

        dbus_client._ws_request = fake_request  # type: ignore[assignment]

        tasks = dbus_client.list_background_tasks(include_finished=True, limit=10)

        self.assertEqual(captured["payload"], {
            "list_background_tasks": {"include_finished": True, "limit": 10}
        })
        self.assertEqual(len(tasks), 2)
        self.assertEqual(tasks[0]["id"], "t-1")
        self.assertEqual(tasks[0]["status"], "running")
        self.assertEqual(tasks[0]["title"], "Researcher")
        self.assertEqual(tasks[1]["status"], "completed")
        # ended_at should be carried through verbatim when present.
        self.assertEqual(tasks[1]["ended_at"], 1717000010000)

    def test_unknown_status_is_passed_through_unchanged(self) -> None:
        # The QML side shows a faint badge for unknown statuses rather
        # than dropping the row.
        unusual = {
            "id": "t-x",
            "kind": {"standalone": {"name": "x", "conversation_id": "c"}},
            "status": "queued_for_replay",
            "started_at": 0,
            "title": "x",
        }
        dbus_client._ws_request = lambda _payload: {"background_tasks": [unusual]}  # type: ignore[assignment]

        tasks = dbus_client.list_background_tasks()
        self.assertEqual(len(tasks), 1)
        self.assertEqual(tasks[0]["status"], "queued_for_replay")

    def test_non_ws_transport_returns_empty_list_without_raising(self) -> None:
        # D-Bus surface for tasks is the subject of desktop-assistant#116.
        # Until it lands, the helper must not raise — the badge simply
        # stays hidden and the window shows an empty list.
        dbus_client.TRANSPORT = "dbus"

        def boom(_payload: dict[str, Any]) -> Any:
            self.fail("_ws_request should not be called on D-Bus transport")

        dbus_client._ws_request = boom  # type: ignore[assignment]

        self.assertEqual(dbus_client.list_background_tasks(), [])

    def test_malformed_payload_raises_wserror(self) -> None:
        dbus_client._ws_request = lambda _payload: {"background_tasks": "oops"}  # type: ignore[assignment]
        with self.assertRaises(dbus_client.WsError):
            dbus_client.list_background_tasks()


class CancelBackgroundTaskTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        dbus_client.TRANSPORT = "ws"
        self._orig_ws_request = dbus_client._ws_request

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def test_sends_cancel_command_with_id(self) -> None:
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return "ack"

        dbus_client._ws_request = fake_request  # type: ignore[assignment]
        dbus_client.cancel_background_task("task-1")
        self.assertEqual(captured["payload"], {"cancel_background_task": {"id": "task-1"}})

    def test_empty_id_raises(self) -> None:
        dbus_client._ws_request = lambda _payload: "ack"  # type: ignore[assignment]
        with self.assertRaises(dbus_client.WsError):
            dbus_client.cancel_background_task("")

    def test_non_ws_transport_raises(self) -> None:
        dbus_client.TRANSPORT = "dbus"
        with self.assertRaises(dbus_client.WsError):
            dbus_client.cancel_background_task("task-1")


class GetTaskLogsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        dbus_client.TRANSPORT = "ws"
        self._orig_ws_request = dbus_client._ws_request

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def test_returns_entries_and_next_seq(self) -> None:
        payload_response = {
            "background_task_logs": {
                "entries": [
                    {
                        "seq": 1,
                        "timestamp": 1717000000001,
                        "level": "info",
                        "category": "lifecycle",
                        "message": "started",
                    },
                    {
                        "seq": 2,
                        "timestamp": 1717000000200,
                        "level": "info",
                        "category": "model_turn",
                        "message": "thinking",
                    },
                ],
                "next_seq": 3,
            }
        }
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return payload_response

        dbus_client._ws_request = fake_request  # type: ignore[assignment]
        result = dbus_client.get_background_task_logs("t-1", after_seq=0, limit=50)
        self.assertEqual(captured["payload"], {
            "get_background_task_logs": {"id": "t-1", "after_seq": 0, "limit": 50}
        })
        self.assertEqual(len(result["entries"]), 2)
        self.assertEqual(result["entries"][0]["seq"], 1)
        self.assertEqual(result["next_seq"], 3)

    def test_optional_after_seq_omitted_when_none(self) -> None:
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return {"background_task_logs": {"entries": [], "next_seq": 0}}

        dbus_client._ws_request = fake_request  # type: ignore[assignment]
        dbus_client.get_background_task_logs("t-1")
        # `after_seq` must not be present so the daemon returns from the
        # oldest entry.
        self.assertEqual(captured["payload"]["get_background_task_logs"], {"id": "t-1"})


class SubscribeBackgroundTasksTests(unittest.TestCase):
    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        dbus_client.TRANSPORT = "ws"
        self._orig_ws_request = dbus_client._ws_request

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request

    def test_sends_subscribe_command(self) -> None:
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return "ack"

        dbus_client._ws_request = fake_request  # type: ignore[assignment]
        dbus_client.subscribe_background_tasks()
        # SubscribeBackgroundTasks is a unit variant — serialized as a
        # bare string by serde.
        self.assertEqual(captured["payload"], "subscribe_background_tasks")


class ApplyTaskEventTests(unittest.TestCase):
    """The daemon-event router for `Task*` events.

    `apply_task_event` is a pure function: given the previous task list
    and an `Event::Task*` JSON payload, it returns ``(next_tasks,
    running_count)``. This is the substitute for a C++ ``TasksModel``
    that mutates rows in response to events: instead of a stateful
    object, we hand the QML side a deterministic transformer it can
    call whenever it polls a fresh page.
    """

    def test_task_started_inserts_running_row(self) -> None:
        before: list[dict[str, Any]] = []
        event = {"task_started": {"task": _running_task("t-1", "Researcher")}}

        after, running = dbus_client.apply_task_event(before, event)
        self.assertEqual(len(after), 1)
        self.assertEqual(after[0]["id"], "t-1")
        self.assertEqual(after[0]["status"], "running")
        self.assertEqual(running, 1)

    def test_task_completed_updates_existing_row(self) -> None:
        before = [_running_task("t-1")]
        event = {"task_completed": {"id": "t-1", "status": "completed"}}

        after, running = dbus_client.apply_task_event(before, event)
        self.assertEqual(len(after), 1)
        self.assertEqual(after[0]["status"], "completed")
        self.assertEqual(running, 0)

    def test_task_failed_carries_last_error(self) -> None:
        before = [_running_task("t-1")]
        event = {
            "task_completed": {
                "id": "t-1",
                "status": "failed",
                "last_error": "boom",
            }
        }

        after, running = dbus_client.apply_task_event(before, event)
        self.assertEqual(after[0]["status"], "failed")
        self.assertEqual(after[0]["last_error"], "boom")
        self.assertEqual(running, 0)

    def test_task_progress_updates_progress_hint(self) -> None:
        before = [_running_task("t-1")]
        event = {"task_progress": {"id": "t-1", "progress_hint": "step 2/4"}}

        after, running = dbus_client.apply_task_event(before, event)
        self.assertEqual(after[0]["progress_hint"], "step 2/4")
        self.assertEqual(running, 1)

    def test_task_completed_for_unknown_id_is_ignored(self) -> None:
        # The QML caller may receive a TaskCompleted for a row it never
        # observed (e.g. polling lag). Dropping it on the floor is
        # safer than fabricating a row with no kind/title.
        before = [_running_task("t-1")]
        event = {"task_completed": {"id": "ghost", "status": "completed"}}

        after, running = dbus_client.apply_task_event(before, event)
        self.assertEqual(len(after), 1)
        self.assertEqual(after[0]["id"], "t-1")
        self.assertEqual(after[0]["status"], "running")
        self.assertEqual(running, 1)

    def test_malformed_event_returns_unchanged(self) -> None:
        # Defensive: a malformed event should not corrupt the model.
        before = [_running_task("t-1")]
        for malformed in (None, "task_started", {}, {"task_started": {}}):
            after, running = dbus_client.apply_task_event(before, malformed)  # type: ignore[arg-type]
            self.assertEqual(after, before)
            self.assertEqual(running, 1)

    def test_rapid_burst_preserves_order(self) -> None:
        # TaskStarted(a), TaskStarted(b), TaskCompleted(a), TaskStarted(c)
        # — the surviving list ordering should be a, b, c (insertion
        # order; completed tasks stay in place rather than reorder).
        before: list[dict[str, Any]] = []
        events = [
            {"task_started": {"task": _running_task("a", "a")}},
            {"task_started": {"task": _running_task("b", "b")}},
            {"task_completed": {"id": "a", "status": "completed"}},
            {"task_started": {"task": _running_task("c", "c")}},
        ]
        tasks: list[dict[str, Any]] = before
        running = 0
        for ev in events:
            tasks, running = dbus_client.apply_task_event(tasks, ev)

        self.assertEqual([t["id"] for t in tasks], ["a", "b", "c"])
        self.assertEqual(running, 2)
        # ``a`` was completed; its status must reflect that even though it
        # kept its place in the list.
        self.assertEqual(tasks[0]["status"], "completed")

    def test_running_count_for_pending_and_running(self) -> None:
        before: list[dict[str, Any]] = []
        pending = _running_task("p")
        pending["status"] = "pending"
        running = _running_task("r")
        for task in (pending, running):
            event = {"task_started": {"task": task}}
            before, _running_count = dbus_client.apply_task_event(before, event)
        _, count = dbus_client.apply_task_event(before, {"task_progress": {"id": "r"}})
        # Both Pending and Running count as "running" for the badge —
        # the user thinks of either as "work in flight".
        self.assertEqual(count, 2)


class CliTests(unittest.TestCase):
    """The CLI is the ABI the QML widget binds to. Output shape matters."""

    def setUp(self) -> None:
        self._orig_transport = dbus_client.TRANSPORT
        self._orig_ws_request = dbus_client._ws_request
        # Restore globals after each test so they don't leak.
        self._orig_argv = sys.argv

    def tearDown(self) -> None:
        dbus_client.TRANSPORT = self._orig_transport
        dbus_client._ws_request = self._orig_ws_request
        sys.argv = self._orig_argv

    def _run_main(self, argv: list[str]) -> tuple[int, dict[str, Any]]:
        sys.argv = ["dbus_client.py", *argv]
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = dbus_client.main()
        return rc, json.loads(buf.getvalue().strip() or "{}")

    def test_tasks_list_subcommand_prints_json_array(self) -> None:
        dbus_client.TRANSPORT = "ws"
        dbus_client._ws_request = lambda _p: {"background_tasks": [_running_task("t-1")]}  # type: ignore[assignment]
        rc, payload = self._run_main(["--transport", "ws", "tasks-list"])
        self.assertEqual(rc, 0)
        self.assertIn("tasks", payload)
        self.assertEqual(len(payload["tasks"]), 1)
        self.assertEqual(payload["tasks"][0]["id"], "t-1")

    def test_tasks_cancel_subcommand(self) -> None:
        dbus_client.TRANSPORT = "ws"
        captured: dict[str, Any] = {}

        def fake_request(payload: dict[str, Any]) -> Any:
            captured["payload"] = payload
            return "ack"

        dbus_client._ws_request = fake_request  # type: ignore[assignment]
        rc, payload = self._run_main(["--transport", "ws", "tasks-cancel", "task-1"])
        self.assertEqual(rc, 0)
        self.assertEqual(captured["payload"], {"cancel_background_task": {"id": "task-1"}})
        self.assertTrue(payload.get("cancelled"))

    def test_tasks_logs_subcommand(self) -> None:
        dbus_client.TRANSPORT = "ws"
        dbus_client._ws_request = lambda _p: {  # type: ignore[assignment]
            "background_task_logs": {
                "entries": [
                    {
                        "seq": 1,
                        "timestamp": 1717000000001,
                        "level": "info",
                        "category": "lifecycle",
                        "message": "hello",
                    }
                ],
                "next_seq": 2,
            }
        }
        rc, payload = self._run_main(
            ["--transport", "ws", "tasks-logs", "t-1", "--after-seq", "0"]
        )
        self.assertEqual(rc, 0)
        self.assertEqual(payload["entries"][0]["message"], "hello")
        self.assertEqual(payload["next_seq"], 2)

    def test_tasks_apply_event_subcommand(self) -> None:
        # The CLI must accept JSON via stdin for apply-event so the QML
        # caller can stream `Task*` events as they arrive.
        dbus_client.TRANSPORT = "ws"
        old_stdin = sys.stdin
        try:
            event = {"task_started": {"task": _running_task("t-1", "Researcher")}}
            sys.stdin = io.StringIO(
                json.dumps({"tasks": [], "event": event})
            )
            rc, payload = self._run_main(["tasks-apply-event"])
        finally:
            sys.stdin = old_stdin
        self.assertEqual(rc, 0)
        self.assertEqual(len(payload["tasks"]), 1)
        self.assertEqual(payload["tasks"][0]["id"], "t-1")
        self.assertEqual(payload["running_count"], 1)


if __name__ == "__main__":
    unittest.main()
