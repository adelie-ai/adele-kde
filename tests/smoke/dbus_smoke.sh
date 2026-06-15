#!/usr/bin/env bash
# D-Bus connectivity smoke test.
#
# Run AFTER a change, WITH the daemon running. It verifies the D-Bus surface the
# native chat plugin depends on — the one no unit test can cover (it needs a live
# bus). The plugin (org.desktopassistant.client → a client-common Connector in
# D-Bus mode) talks to the daemon's Conversations interface, so we check:
#   1. the daemon owns its well-known name + Conversations interface, and
#   2. a read-only ListConversations call round-trips over the session bus
#      (the same call the Connector issues on connect).
#
# The plugin itself (AdeleCore / VoiceController) is covered by the C++ tests
# (`just client-build`) and live plasmashell QA; this is the bus-level check.
# Pairs with `just test-qml` (widgets load); together wired as `just smoke`.
set -uo pipefail

SERVICE="${DESKTOP_ASSISTANT_DBUS_SERVICE:-org.desktopAssistant}"
CONV_PATH="/org/desktopAssistant/Conversations"
CONV_IFACE="org.desktopAssistant.Conversations"

fail() {
    echo "SMOKE FAIL: $*" >&2
    exit 1
}

# 1) Is the daemon reachable on the session bus?
echo "[1/2] D-Bus reachable: introspecting ${SERVICE} ..."
if ! gdbus introspect --session --dest "$SERVICE" \
        --object-path "$CONV_PATH" >/dev/null 2>&1; then
    fail "${SERVICE} is not on the session bus — is the daemon running? Start it, then re-run."
fi
echo "      ok — ${SERVICE} owns the Conversations interface"

# 2) Read-only round-trip — the same ListConversations(max_age_days, include_archived)
#    call the plugin's Connector makes on connect.
echo "[2/2] read-only round-trip: ${CONV_IFACE}.ListConversations ..."
OUT="$(gdbus call --session --dest "$SERVICE" --object-path "$CONV_PATH" \
        --method "${CONV_IFACE}.ListConversations" 0 false 2>&1)" \
    || fail "ListConversations failed over D-Bus: ${OUT}"
echo "      ok — ListConversations round-tripped over D-Bus"

echo "SMOKE PASS: ${SERVICE} reachable and ListConversations round-trips over D-Bus."
