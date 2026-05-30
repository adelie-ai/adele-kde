#!/usr/bin/env bash
# D-Bus connectivity smoke test.
#
# Run this AFTER a change, WITH the daemon running. It verifies two things the
# plasmoid depends on but that no unit test can cover (they need a live bus):
#   1. the daemon owns its well-known name on the session bus, and
#   2. the widget's OWN client (shared/chat-module/code/dbus_client.py — the
#      same script the plasmoid runs via its Plasma5Support DataSource) can
#      round-trip a read-only call over D-Bus.
#
# Pairs with `just test-qml` (which proves the widgets *load*); together they
# are wired as `just smoke`. Exits non-zero with a clear reason on any failure.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

SERVICE="${DESKTOP_ASSISTANT_DBUS_SERVICE:-org.desktopAssistant}"
PY="${PYTHON:-python3}"
CLIENT="$REPO_ROOT/shared/chat-module/code/dbus_client.py"

fail() {
    echo "SMOKE FAIL: $*" >&2
    exit 1
}

# 1) Is the daemon reachable on the session bus?
echo "[1/3] D-Bus reachable: introspecting ${SERVICE} ..."
if ! gdbus introspect --session --dest "$SERVICE" \
        --object-path /org/desktopAssistant/Conversations >/dev/null 2>&1; then
    fail "${SERVICE} is not on the session bus — is the daemon running? Start it, then re-run."
fi
echo "      ok — ${SERVICE} owns the Conversations interface"

# 2) Can the widget's own client round-trip a read-only call?
echo "[2/3] widget client round-trip: dbus_client.py list ..."
[ -f "$CLIENT" ] || fail "client not found at ${CLIENT}"
OUT="$("$PY" "$CLIENT" list 2>&1)" || fail "dbus_client.py exited non-zero: ${OUT}"
printf '%s' "$OUT" | "$PY" -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception as e:
    sys.exit("not valid JSON: %s" % e)
if not isinstance(d, dict) or "conversations" not in d or "error" in d:
    sys.exit("expected JSON with a conversations key and no error, got: %r" % d)
print("      ok — %d conversation(s) returned over D-Bus" % len(d["conversations"]))
' || fail "unexpected dbus_client.py output: ${OUT}"

# 3) Does the widget client's own status check agree the daemon is up?
#    This exercises the status path (NameHasOwner over D-Bus, or a WS ping)
#    that the unit tests can only stub.
echo "[3/3] widget client status round-trip: dbus_client.py status ..."
OUT="$("$PY" "$CLIENT" status 2>&1)" || fail "dbus_client.py status exited non-zero: ${OUT}"
printf '%s' "$OUT" | "$PY" -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception as e:
    sys.exit("not valid JSON: %s" % e)
if not isinstance(d, dict) or "error" in d:
    sys.exit("status reported an error: %r" % d)
if not d.get("production_running"):
    sys.exit("status says the daemon is not running: %r" % d)
print("      ok — status reports the daemon running over %s" % d.get("transport", "?"))
' || fail "unexpected dbus_client.py status output: ${OUT}"

echo "SMOKE PASS: ${SERVICE} reachable and the widget client round-trips (list + status) over D-Bus."
