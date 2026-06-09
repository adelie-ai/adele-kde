#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

DEFAULT_SERVICE = "org.desktopAssistant"
DEV_SERVICE = "org.desktopAssistant.Dev"
SETTINGS_PATH = Path.home() / ".config" / "desktop-assistant" / "widget_settings.json"
DEFAULT_CONNECTION_NAME = "local"
DEFAULT_TRANSPORT = "dbus"
DEFAULT_WS_URL = "ws://127.0.0.1:11339/ws"
DEFAULT_WS_SUBJECT = "desktop-widget"
SERVICE = DEFAULT_SERVICE
PATH = "/org/desktopAssistant/Conversations"
IFACE = "org.desktopAssistant.Conversations"
SETTINGS_PATH_DBUS = "/org/desktopAssistant/Settings"
SETTINGS_IFACE = "org.desktopAssistant.Settings"
# The voice daemon (repo adelie-ai/voice) owns a DISTINCT well-known name from
# the orchestrator. The plasmoid is just another client of it: it calls these
# methods over the session bus and degrades gracefully (controls disabled)
# when the name has no owner (service not installed / not running).
VOICE_SERVICE = "org.desktopAssistant.Voice"
VOICE_PATH = "/org/desktopAssistant/Voice"
VOICE_IFACE = "org.desktopAssistant.Voice"
DBUS_DAEMON_DEST = "org.freedesktop.DBus"
DBUS_DAEMON_PATH = "/org/freedesktop/DBus"
DBUS_DAEMON_IFACE = "org.freedesktop.DBus"
DEFAULT_GDBUS_TIMEOUT_SEC = 12.0
DEFAULT_WS_TIMEOUT_SEC = 12.0
TRANSPORT = DEFAULT_TRANSPORT
WS_URL = DEFAULT_WS_URL
WS_SUBJECT = DEFAULT_WS_SUBJECT
WS_JWT = ""
CONNECTION_NAME = DEFAULT_CONNECTION_NAME
DEFAULT_CONFIG_CONNECTION = DEFAULT_CONNECTION_NAME


class DbusError(RuntimeError):
    pass


class WsError(RuntimeError):
    pass


def _load_widget_settings_payload() -> dict[str, Any]:
    try:
        payload = json.loads(SETTINGS_PATH.read_text())
    except Exception:
        return {}

    if not isinstance(payload, dict):
        return {}
    return payload


def _normalize_transport(value: str) -> str:
    normalized = value.strip().lower()
    return "ws" if normalized == "ws" else "dbus"


def _load_widget_connections(payload: dict[str, Any]) -> tuple[dict[str, dict[str, str]], str]:
    raw_connections = payload.get("connections")
    parsed: dict[str, dict[str, str]] = {}
    default_dbus_service = str(payload.get("dbus_service", "")).strip() or DEFAULT_SERVICE

    if isinstance(raw_connections, list):
        for item in raw_connections:
            if not isinstance(item, dict):
                continue

            name = str(item.get("name", "")).strip()
            if not name or name in parsed:
                continue

            raw_transport = str(item.get("transport", "")).strip()
            if raw_transport:
                transport = _normalize_transport(raw_transport)
            elif name == DEFAULT_CONNECTION_NAME:
                transport = "dbus"
            else:
                transport = "ws"

            dbus_service = str(item.get("dbus_service", "")).strip() or default_dbus_service
            ws_url = str(item.get("ws_url", "")).strip() or DEFAULT_WS_URL
            ws_subject = str(item.get("ws_subject", "")).strip() or DEFAULT_WS_SUBJECT

            parsed[name] = {
                "name": name,
                "transport": transport,
                "dbus_service": dbus_service,
                "ws_url": ws_url,
                "ws_subject": ws_subject,
            }

    default_connection = str(payload.get("default_connection", "")).strip()

    if not isinstance(raw_connections, list) or not raw_connections:
        legacy_transport = str(payload.get("transport", "")).strip().lower()
        legacy_ws_url = str(payload.get("ws_url", "")).strip()
        legacy_ws_subject = str(payload.get("ws_subject", "")).strip()
        use_legacy_ws = legacy_transport == "ws" or bool(legacy_ws_url)
        if use_legacy_ws:
            legacy_name = "legacy-ws"
            parsed[legacy_name] = {
                "name": legacy_name,
                "transport": "ws",
                "dbus_service": default_dbus_service,
                "ws_url": legacy_ws_url or DEFAULT_WS_URL,
                "ws_subject": legacy_ws_subject or DEFAULT_WS_SUBJECT,
            }
            if not default_connection:
                default_connection = legacy_name

    if not parsed:
        parsed[DEFAULT_CONNECTION_NAME] = {
            "name": DEFAULT_CONNECTION_NAME,
            "transport": "dbus",
            "dbus_service": default_dbus_service,
            "ws_url": DEFAULT_WS_URL,
            "ws_subject": DEFAULT_WS_SUBJECT,
        }

    if default_connection not in parsed:
        default_connection = (
            DEFAULT_CONNECTION_NAME
            if DEFAULT_CONNECTION_NAME in parsed
            else next(iter(parsed.keys()))
        )

    return parsed, default_connection


def _load_widget_connection_name(payload: dict[str, Any]) -> str:
    env_name = os.environ.get("DESKTOP_ASSISTANT_WIDGET_CONNECTION", "").strip()
    if env_name:
        return env_name
    value = str(payload.get("connection_name", "")).strip()
    if value:
        return value
    return str(payload.get("connection", "")).strip()


# --- GVariant text-format parser (KDE-1) -------------------------------------
# `gdbus call` prints replies in GVariant text format. The old implementation
# "normalized" that text with regexes (strip `@type`, unwrap `int32 N`, map
# true/false) and fed it to ast.literal_eval — but the regexes ran over the
# WHOLE output including quoted string literals, so message content like
# "is this true or false?" or "value @s ok int32 5" was rewritten (reproduced
# live). The only correct approach is a real parser that tokenizes string
# literals first; everything outside strings is then unambiguous syntax.
# Deliberately stdlib-only (no PyGObject dependency) to match the helper's
# zero-extra-packages constraint.

_GVARIANT_ESCAPES = {
    "a": "\a",
    "b": "\b",
    "f": "\f",
    "n": "\n",
    "r": "\r",
    "t": "\t",
    "v": "\v",
    "'": "'",
    '"': '"',
    "\\": "\\",
}

# Integer-ish type keywords that prefix a number literal, e.g. `uint32 5`.
_GVARIANT_NUMERIC_TYPES = {
    "byte",
    "int16",
    "uint16",
    "int32",
    "uint32",
    "int64",
    "uint64",
    "handle",
    "double",
}

# Characters legal in a GVariant type string after `@`, e.g. `@a{sv}`, `@(ss)`.
_GVARIANT_SIGNATURE_CHARS = frozenset("ybnqiuxthdsogvam(){}*?r")

_GVARIANT_NUMBER_RE = re.compile(
    r"""
    [+-]?
    (?:
        0x[0-9A-Fa-f]+            # hex (bytes are printed 0xNN)
      | (?:\d+\.\d*|\.\d+|\d+)    # decimal integer or float body
        (?:[eE][+-]?\d+)?         # optional exponent
    )
    """,
    re.VERBOSE,
)


class _GVariantParser:
    """Recursive-descent parser for the GVariant text format gdbus emits.

    Containers map to Python as: tuple -> tuple, array -> list, dict -> dict,
    variant `<v>` -> unwrapped value, maybe -> value or None. Type
    annotations (`@a{sv}`) and numeric type keywords (`uint32`) are consumed
    as syntax, never applied to text inside string literals.
    """

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0

    def fail(self, why: str) -> Exception:
        return ValueError(f"{why} at offset {self.pos}")

    def skip_ws(self) -> None:
        while self.pos < len(self.text) and self.text[self.pos] in " \t\r\n":
            self.pos += 1

    def peek(self) -> str:
        return self.text[self.pos] if self.pos < len(self.text) else ""

    def expect(self, char: str) -> None:
        if self.peek() != char:
            raise self.fail(f"expected {char!r}")
        self.pos += 1

    def parse_document(self) -> Any:
        self.skip_ws()
        value = self.parse_value()
        self.skip_ws()
        if self.pos != len(self.text):
            raise self.fail("trailing data after value")
        return value

    def parse_value(self) -> Any:
        self.skip_ws()
        ch = self.peek()
        if ch == "":
            raise self.fail("unexpected end of input")
        if ch == "@":
            self._consume_type_annotation()
            return self.parse_value()
        if ch in "'\"":
            return self._parse_string()
        if ch == "(":
            return self._parse_tuple()
        if ch == "[":
            return self._parse_array()
        if ch == "{":
            return self._parse_dict()
        if ch == "<":
            self.pos += 1
            inner = self.parse_value()
            self.skip_ws()
            self.expect(">")
            return inner
        if ch.isalpha():
            return self._parse_keyword()
        return self._parse_number()

    def _consume_type_annotation(self) -> None:
        self.expect("@")
        start = self.pos
        while self.pos < len(self.text) and self.text[self.pos] in _GVARIANT_SIGNATURE_CHARS:
            self.pos += 1
        if self.pos == start:
            raise self.fail("empty type annotation")

    def _parse_keyword(self) -> Any:
        start = self.pos
        while self.pos < len(self.text) and (self.text[self.pos].isalnum() or self.text[self.pos] == "_"):
            self.pos += 1
        word = self.text[start : self.pos]
        if word == "true":
            return True
        if word == "false":
            return False
        if word == "nothing":
            return None
        if word == "just":
            return self.parse_value()
        if word in ("objectpath", "signature"):
            self.skip_ws()
            if self.peek() not in "'\"":
                raise self.fail(f"expected string after {word}")
            return self._parse_string()
        if word == "b" and self.peek() in "'\"":
            return self._parse_string().encode("latin-1")
        if word in _GVARIANT_NUMERIC_TYPES:
            self.skip_ws()
            if self.peek() == "":
                raise self.fail(f"expected number after {word}")
            return self._parse_number()
        if word in ("inf", "nan"):
            return float(word)
        raise self.fail(f"unexpected token {word!r}")

    def _parse_number(self) -> Any:
        match = _GVARIANT_NUMBER_RE.match(self.text, self.pos)
        if match is None:
            # Negative special floats: `-inf`.
            if self.text.startswith("-inf", self.pos):
                self.pos += 4
                return float("-inf")
            raise self.fail("expected number")
        self.pos = match.end()
        token = match.group()
        if token.lower().lstrip("+-").startswith("0x"):
            return int(token, 16)
        if any(c in token for c in ".eE"):
            return float(token)
        return int(token)

    def _parse_string(self) -> str:
        quote = self.peek()
        self.pos += 1
        out: list[str] = []
        while True:
            if self.pos >= len(self.text):
                raise self.fail("unterminated string")
            ch = self.text[self.pos]
            if ch == quote:
                self.pos += 1
                return "".join(out)
            if ch == "\\":
                self.pos += 1
                if self.pos >= len(self.text):
                    raise self.fail("unterminated escape")
                esc = self.text[self.pos]
                if esc == "u":
                    out.append(self._parse_hex_escape(4))
                elif esc == "U":
                    out.append(self._parse_hex_escape(8))
                elif esc == "x":
                    out.append(self._parse_hex_escape(2))
                elif esc in _GVARIANT_ESCAPES:
                    out.append(_GVARIANT_ESCAPES[esc])
                    self.pos += 1
                else:
                    raise self.fail(f"unknown escape \\{esc}")
            else:
                out.append(ch)
                self.pos += 1

    def _parse_hex_escape(self, digits: int) -> str:
        # self.pos is on the escape letter (u/U/x); digits follow it.
        start = self.pos + 1
        token = self.text[start : start + digits]
        if len(token) != digits or any(c not in "0123456789abcdefABCDEF" for c in token):
            raise self.fail("invalid hex escape")
        self.pos = start + digits
        return chr(int(token, 16))

    def _parse_tuple(self) -> tuple[Any, ...]:
        self.expect("(")
        items: list[Any] = []
        self.skip_ws()
        if self.peek() == ")":
            self.pos += 1
            return ()
        while True:
            items.append(self.parse_value())
            self.skip_ws()
            if self.peek() == ",":
                self.pos += 1
                self.skip_ws()
                # Single-element tuples print as `(x,)`.
                if self.peek() == ")":
                    self.pos += 1
                    return tuple(items)
                continue
            self.expect(")")
            return tuple(items)

    def _parse_array(self) -> list[Any]:
        self.expect("[")
        items: list[Any] = []
        self.skip_ws()
        if self.peek() == "]":
            self.pos += 1
            return items
        while True:
            items.append(self.parse_value())
            self.skip_ws()
            if self.peek() == ",":
                self.pos += 1
                continue
            self.expect("]")
            return items

    def _parse_dict(self) -> dict[Any, Any]:
        self.expect("{")
        result: dict[Any, Any] = {}
        self.skip_ws()
        if self.peek() == "}":
            self.pos += 1
            return result
        while True:
            key = self.parse_value()
            self.skip_ws()
            self.expect(":")
            value = self.parse_value()
            result[key] = value
            self.skip_ws()
            if self.peek() == ",":
                self.pos += 1
                self.skip_ws()
                continue
            self.expect("}")
            return result


def _parse_gdbus_output(output: str) -> Any:
    try:
        return _GVariantParser(output).parse_document()
    except Exception as exc:
        raise DbusError(f"unexpected gdbus output: {output}") from exc


def _run_command(command: list[str], error_hint: str, timeout_sec: float = DEFAULT_GDBUS_TIMEOUT_SEC) -> Any:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=timeout_sec)
    except FileNotFoundError as exc:
        raise DbusError("gdbus command not found; install glib2 tools") from exc
    except subprocess.TimeoutExpired as exc:
        raise DbusError(f"{error_hint} (timed out after {timeout_sec:.1f}s)") from exc
    except subprocess.CalledProcessError as exc:
        raise DbusError(exc.stderr.strip() or exc.stdout.strip() or error_hint) from exc

    return _parse_gdbus_output(result.stdout.strip())


def _run_gdbus(method: str, *args: str) -> Any:
    command = [
        "gdbus",
        "call",
        "--session",
        "--dest",
        SERVICE,
        "--object-path",
        PATH,
        "--method",
        f"{IFACE}.{method}",
        *args,
    ]
    return _run_command(command, f"gdbus call failed: {method}")


def _run_gdbus_settings(method: str, *args: str) -> Any:
    command = [
        "gdbus",
        "call",
        "--session",
        "--dest",
        SERVICE,
        "--object-path",
        SETTINGS_PATH_DBUS,
        "--method",
        f"{SETTINGS_IFACE}.{method}",
        *args,
    ]
    return _run_command(command, f"gdbus settings call failed: {method}")


def _run_gdbus_voice(method: str, *args: str) -> Any:
    # The voice daemon is always reached over D-Bus (it's a local session-bus
    # service), regardless of which transport the chat connection uses. We do
    # NOT route voice through the WebSocket orchestrator — it's a separate
    # service with its own well-known name.
    command = [
        "gdbus",
        "call",
        "--session",
        "--dest",
        VOICE_SERVICE,
        "--object-path",
        VOICE_PATH,
        "--method",
        f"{VOICE_IFACE}.{method}",
        *args,
    ]
    return _run_command(command, f"gdbus voice call failed: {method}")


def _ws_connect(ws_url: str, token: str, timeout_sec: float = DEFAULT_WS_TIMEOUT_SEC) -> socket.socket:
    parsed = urlparse(ws_url)
    scheme = parsed.scheme.lower()
    if scheme not in {"ws", "wss"}:
        raise WsError(f"unsupported websocket URL scheme: {parsed.scheme}")

    host = parsed.hostname or ""
    if not host:
        raise WsError(f"websocket URL missing host: {ws_url}")
    port = parsed.port or (443 if scheme == "wss" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += f"?{parsed.query}"

    try:
        sock = socket.create_connection((host, port), timeout=timeout_sec)
    except OSError as exc:
        raise WsError(f"failed to connect websocket {ws_url}: {exc}") from exc

    if scheme == "wss":
        context = ssl.create_default_context()
        try:
            sock = context.wrap_socket(sock, server_hostname=host)
        except ssl.SSLError as exc:
            sock.close()
            raise WsError(f"failed TLS handshake for {ws_url}: {exc}") from exc

    key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
    host_header = f"{host}:{port}" if parsed.port else host
    request_lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host_header}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
        f"Authorization: Bearer {token}",
        "",
        "",
    ]
    request_data = "\r\n".join(request_lines).encode("utf-8")

    try:
        sock.sendall(request_data)
    except OSError as exc:
        sock.close()
        raise WsError(f"failed websocket handshake write: {exc}") from exc

    response = bytearray()
    deadline = time.monotonic() + timeout_sec
    try:
        while b"\r\n\r\n" not in response:
            if time.monotonic() > deadline:
                raise WsError("websocket handshake timed out")
            chunk = sock.recv(4096)
            if not chunk:
                raise WsError("websocket handshake closed unexpectedly")
            response.extend(chunk)
            if len(response) > 65536:
                raise WsError("websocket handshake response too large")
    except Exception:
        sock.close()
        raise

    headers_blob = response.split(b"\r\n\r\n", 1)[0].decode("utf-8", errors="replace")
    lines = headers_blob.split("\r\n")
    if not lines or " 101 " not in lines[0]:
        sock.close()
        raise WsError(f"websocket upgrade failed: {lines[0] if lines else headers_blob}")

    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        key_name, value = line.split(":", 1)
        headers[key_name.strip().lower()] = value.strip()

    expected_accept = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
    ).decode("ascii")
    if headers.get("sec-websocket-accept", "") != expected_accept:
        sock.close()
        raise WsError("websocket handshake validation failed (accept header mismatch)")

    sock.settimeout(timeout_sec)
    return sock


def _ws_send_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    header = bytearray([0x81])  # FIN + text frame
    header[0] = 0x80 | (opcode & 0x0F)
    length = len(payload)
    mask_key = secrets.token_bytes(4)

    if length < 126:
        header.append(0x80 | length)
    elif length < (1 << 16):
        header.append(0x80 | 126)
        header.extend(length.to_bytes(2, "big"))
    else:
        header.append(0x80 | 127)
        header.extend(length.to_bytes(8, "big"))

    header.extend(mask_key)
    masked = bytearray(payload)
    for idx in range(len(masked)):
        masked[idx] ^= mask_key[idx % 4]

    sock.sendall(bytes(header) + bytes(masked))


def _ws_send_text(sock: socket.socket, text: str) -> None:
    _ws_send_frame(sock, 0x1, text.encode("utf-8"))


def _ws_recv_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise WsError("websocket connection closed")
        data.extend(chunk)
    return bytes(data)


def _ws_recv_frame(sock: socket.socket) -> tuple[int, bytes]:
    first_two = _ws_recv_exact(sock, 2)
    opcode = first_two[0] & 0x0F
    masked = (first_two[1] & 0x80) != 0
    length = first_two[1] & 0x7F

    if length == 126:
        length = int.from_bytes(_ws_recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(_ws_recv_exact(sock, 8), "big")

    mask_key = _ws_recv_exact(sock, 4) if masked else b""
    payload = bytearray(_ws_recv_exact(sock, length))
    if masked:
        for idx in range(len(payload)):
            payload[idx] ^= mask_key[idx % 4]

    return opcode, bytes(payload)


def _ws_resolve_jwt() -> str:
    token = WS_JWT.strip()
    if token:
        return token

    try:
        response = _run_gdbus_settings("GenerateWsJwt", WS_SUBJECT)
    except DbusError as exc:
        raise WsError(
            "no websocket JWT configured and failed to bootstrap via D-Bus GenerateWsJwt"
        ) from exc

    if isinstance(response, tuple) and len(response) > 0:
        token = str(response[0]).strip()
    else:
        token = str(response).strip()

    if not token:
        raise WsError("GenerateWsJwt returned empty token")
    return token


def _ws_request(command: dict[str, Any]) -> Any:
    request_id = f"widget-{secrets.token_hex(8)}"
    request = {
        "id": request_id,
        "command": command,
    }
    token = _ws_resolve_jwt()
    sock = _ws_connect(WS_URL, token)

    try:
        _ws_send_text(sock, json.dumps(request))
        while True:
            opcode, payload = _ws_recv_frame(sock)
            if opcode == 0x1:  # text
                text = payload.decode("utf-8", errors="replace")
                try:
                    frame = json.loads(text)
                except json.JSONDecodeError:
                    continue
                if "result" in frame:
                    envelope = frame["result"]
                    if isinstance(envelope, dict) and envelope.get("id") == request_id:
                        return envelope.get("result")
                    continue
                if "error" in frame:
                    envelope = frame["error"]
                    if isinstance(envelope, dict) and envelope.get("id") == request_id:
                        raise WsError(str(envelope.get("error", "websocket request failed")))
                continue
            if opcode == 0x9:  # ping
                continue
            if opcode == 0x8:  # close
                raise WsError("websocket closed before response")
            # Ignore binary/continuation/control frames we don't use.
    except socket.timeout as exc:
        raise WsError("websocket request timed out") from exc
    finally:
        sock.close()


def _ws_expect_variant(result: Any, variant: str) -> Any:
    if isinstance(result, dict) and variant in result:
        return result[variant]
    raise WsError(f"unexpected websocket result variant, expected '{variant}': {result}")


def _name_has_owner(name: str) -> bool:
    command = [
        "gdbus",
        "call",
        "--session",
        "--dest",
        DBUS_DAEMON_DEST,
        "--object-path",
        DBUS_DAEMON_PATH,
        "--method",
        f"{DBUS_DAEMON_IFACE}.NameHasOwner",
        name,
    ]
    parsed = _run_command(command, "NameHasOwner call failed", timeout_sec=6.0)
    if isinstance(parsed, tuple) and len(parsed) > 0:
        return bool(parsed[0])
    if isinstance(parsed, bool):
        return parsed
    raise DbusError(f"unexpected NameHasOwner response for {name}: {parsed}")


def create_conversation(title: str) -> str:
    if TRANSPORT == "ws":
        response = _ws_request({"create_conversation": {"title": title}})
        payload = _ws_expect_variant(response, "conversation_id")
        if isinstance(payload, dict):
            return str(payload.get("id", ""))
        raise WsError(f"unexpected websocket create_conversation payload: {payload}")

    response = _run_gdbus("CreateConversation", title)
    return str(response[0])


def _build_override(
    connection_id: str,
    model_id: str,
    effort: str | None,
) -> dict[str, Any] | None:
    """Build a daemon-compatible SendPromptOverride payload.

    Returns None when either `connection_id` or `model_id` is empty (the daemon
    requires both). `effort` is optional and must be one of {"low","medium","high"}.
    """
    conn = (connection_id or "").strip()
    model = (model_id or "").strip()
    if not conn or not model:
        return None

    override: dict[str, Any] = {
        "connection_id": conn,
        "model_id": model,
    }
    eff = (effort or "").strip().lower()
    if eff in {"low", "medium", "high"}:
        override["effort"] = eff
    return override


def send_prompt(
    conversation_id: str,
    prompt: str,
    override: dict[str, Any] | None = None,
) -> str:
    if TRANSPORT == "ws":
        payload: dict[str, Any] = {
            "conversation_id": conversation_id,
            "content": prompt,
        }
        if override is not None:
            payload["override"] = override
        response = _ws_request({"send_message": payload})
        _ws_expect_variant(response, "ack")
        return ""

    # D-Bus transport does not currently surface SendPromptOverride; the
    # daemon's DBus adapter predates the multi-connection API. For parity we
    # fall back to the unoverridden SendPrompt call. A future daemon release
    # will add per-call overrides over D-Bus; see desktop-assistant#18.
    response = _run_gdbus("SendPrompt", conversation_id, prompt)
    return str(response[0])


def get_conversation(
    conversation_id: str,
    tail: int | None = None,
    after_count: int | None = None,
) -> dict[str, Any]:
    warnings: list[dict[str, Any]] = []
    last_model_selection: dict[str, Any] | None = None
    if TRANSPORT == "ws":
        response = _ws_request({"get_conversation": {"id": conversation_id}})
        response = _ws_expect_variant(response, "conversation")
        if not isinstance(response, dict):
            raise WsError(f"unexpected websocket get_conversation payload: {response}")
        conv_id = str(response.get("id", conversation_id))
        title = str(response.get("title", ""))
        messages = []
        for item in response.get("messages", []) or []:
            if not isinstance(item, dict):
                continue
            messages.append((str(item.get("role", "")), str(item.get("content", ""))))
        # Warnings (e.g. DanglingModelSelection) are one-shot advisories the
        # daemon emits after GetConversation. Forward them verbatim so the UI
        # can show a passive notification; desktop-assistant#17 guarantees the
        # server clears state so the warning does not recur.
        raw_warnings = response.get("warnings")
        if isinstance(raw_warnings, list):
            for warn in raw_warnings:
                if isinstance(warn, dict):
                    warnings.append(warn)
        # Future daemon revisions (desktop-assistant#18) will surface the
        # conversation's persisted selection so the UI can hydrate its picker
        # on load without sending a probe prompt. Forward the field when the
        # daemon includes it; callers should treat absence as "no selection
        # yet — inherit purpose default".
        raw_selection = response.get("last_model_selection")
        if isinstance(raw_selection, dict) and raw_selection.get("connection_id") and raw_selection.get("model_id"):
            last_model_selection = {
                "connection_id": str(raw_selection.get("connection_id", "")),
                "model_id": str(raw_selection.get("model_id", "")),
            }
            if raw_selection.get("effort"):
                last_model_selection["effort"] = str(raw_selection.get("effort", "")).lower()
    else:
        response = _run_gdbus("GetConversation", conversation_id)
        conv_id, title, messages = response
    total_messages = len(messages)
    normalized_after = max(0, int(after_count or 0))
    use_after = after_count is not None

    if use_after:
        visible_messages = messages[normalized_after:] if normalized_after < total_messages else []
        truncated = False
    else:
        # Performance guardrail for widget callers: keep historical back-loads bounded.
        # Large message batches can cause expensive QML layout/render work and freeze
        # the desktop shell, so callers should prefer a small `--tail` value.
        normalized_tail = max(0, int(tail or 0))
        truncated = normalized_tail > 0 and total_messages > normalized_tail
        visible_messages = messages[-normalized_tail:] if truncated else messages

    items = []
    for role, content in visible_messages:
        items.append({"role": role, "content": content})
    result: dict[str, Any] = {
        "id": conv_id,
        "title": title,
        "messages": items,
        "message_count": total_messages,
        "truncated": truncated,
        "after_count": normalized_after if use_after else None,
    }
    if warnings:
        result["warnings"] = warnings
    if last_model_selection is not None:
        result["last_model_selection"] = last_model_selection
    return result


def list_available_models(
    connection_id: str | None = None,
    refresh: bool = False,
) -> list[dict[str, Any]]:
    """Fetch `Connection · Model` listings across healthy connections.

    Returns an empty list when the daemon is unreachable over WS; this lets
    the chat widget degrade to the "no selector" state without crashing.
    """
    if TRANSPORT != "ws":
        # Multi-connection model listing is WS-only today. Return an empty
        # list so the widget selector simply shows no entries rather than
        # erroring out on D-Bus transports.
        return []

    payload: dict[str, Any] = {}
    if connection_id:
        payload["connection_id"] = connection_id
    if refresh:
        payload["refresh"] = True
    response = _ws_request({"list_available_models": payload})
    listings = _ws_expect_variant(response, "models")
    if not isinstance(listings, list):
        raise WsError(f"unexpected list_available_models payload: {listings}")

    results: list[dict[str, Any]] = []
    for item in listings:
        if not isinstance(item, dict):
            continue
        model = item.get("model", {}) or {}
        if not isinstance(model, dict):
            continue
        results.append(
            {
                "connection_id": str(item.get("connection_id", "")),
                "connection_label": str(item.get("connection_label", "")),
                "model_id": str(model.get("id", "")),
                "model_display_name": str(model.get("display_name", model.get("id", ""))),
            }
        )
    return results


def list_connections() -> list[dict[str, Any]]:
    if TRANSPORT != "ws":
        return []
    response = _ws_request({"list_connections": {}})
    views = _ws_expect_variant(response, "connections")
    if not isinstance(views, list):
        raise WsError(f"unexpected list_connections payload: {views}")

    rows: list[dict[str, Any]] = []
    for item in views:
        if not isinstance(item, dict):
            continue
        availability = item.get("availability") or {}
        rows.append(
            {
                "id": str(item.get("id", "")),
                "connector_type": str(item.get("connector_type", "")),
                "display_label": str(item.get("display_label", item.get("id", ""))),
                "status": str(availability.get("status", "ok")) if isinstance(availability, dict) else "ok",
                "reason": str(availability.get("reason", "")) if isinstance(availability, dict) else "",
                "has_credentials": bool(item.get("has_credentials", False)),
            }
        )
    return rows


def create_connection(connection_id: str, config: dict[str, Any]) -> None:
    if TRANSPORT != "ws":
        raise WsError("create_connection requires WebSocket transport")
    response = _ws_request(
        {"create_connection": {"id": connection_id, "config": config}}
    )
    _ws_expect_variant(response, "ack")


def update_connection(connection_id: str, config: dict[str, Any]) -> None:
    if TRANSPORT != "ws":
        raise WsError("update_connection requires WebSocket transport")
    response = _ws_request(
        {"update_connection": {"id": connection_id, "config": config}}
    )
    _ws_expect_variant(response, "ack")


def delete_connection(connection_id: str, force: bool = False) -> None:
    if TRANSPORT != "ws":
        raise WsError("delete_connection requires WebSocket transport")
    payload: dict[str, Any] = {"id": connection_id}
    if force:
        payload["force"] = True
    response = _ws_request({"delete_connection": payload})
    _ws_expect_variant(response, "ack")


def get_purposes() -> dict[str, Any]:
    if TRANSPORT != "ws":
        return {}
    response = _ws_request({"get_purposes": {}})
    view = _ws_expect_variant(response, "purposes")
    if not isinstance(view, dict):
        raise WsError(f"unexpected get_purposes payload: {view}")
    return view


def set_purpose(purpose: str, config: dict[str, Any]) -> None:
    if TRANSPORT != "ws":
        raise WsError("set_purpose requires WebSocket transport")
    response = _ws_request(
        {"set_purpose": {"purpose": purpose, "config": config}}
    )
    _ws_expect_variant(response, "ack")


def get_messages(
    conversation_id: str,
    tail: int | None = None,
    after_count: int | None = None,
    include_roles: list[str] | None = None,
) -> dict[str, Any]:
    """Fetch messages via GetMessages, with server-side filtering and pagination.

    - ``tail``: max visible messages to return (applied after filtering); 0 = unlimited.
    - ``after_count``: raw message index to start from; None means use tail mode.
    - ``include_roles``: allowlist of roles to return (e.g. ``["user", "assistant"]``).
      Defaults to ``["user", "assistant"]``.  Pass ``[]`` to receive all roles.

    The returned ``message_count`` is the *total* unfiltered count so callers
    can use it as the next ``after_count`` for incremental fetches.
    """
    roles = include_roles if include_roles is not None else ["user", "assistant"]

    if TRANSPORT == "ws":
        response = _ws_request({"get_conversation": {"id": conversation_id}})
        response = _ws_expect_variant(response, "conversation")
        if not isinstance(response, dict):
            raise WsError(f"unexpected websocket get_conversation payload: {response}")
        raw_messages = response.get("messages", []) or []
        normalized_raw = [
            {"role": str(item.get("role", "")), "content": str(item.get("content", ""))}
            for item in raw_messages
            if isinstance(item, dict)
        ]
        total_count = len(normalized_raw)
        if after_count is not None:
            start = max(0, int(after_count))
            visible = normalized_raw[start:] if start < total_count else []
            truncated = False
        else:
            visible = normalized_raw
            truncated = False

        if roles:
            role_set = {role.strip() for role in roles if role.strip()}
            if role_set:
                visible = [item for item in visible if item.get("role") in role_set]

        normalized_tail = max(0, int(tail or 0))
        if after_count is None and normalized_tail > 0 and len(visible) > normalized_tail:
            visible = visible[-normalized_tail:]
            truncated = True

        out: dict[str, Any] = {
            "messages": visible,
            "message_count": int(total_count),
            "truncated": bool(truncated),
        }
        # Conversation-level advisories / stored selection (desktop-assistant#17,
        # #18). Harmless when the daemon omits them.
        raw_warnings = response.get("warnings")
        if isinstance(raw_warnings, list) and raw_warnings:
            out["warnings"] = [w for w in raw_warnings if isinstance(w, dict)]
        raw_selection = response.get("last_model_selection")
        if (
            isinstance(raw_selection, dict)
            and raw_selection.get("connection_id")
            and raw_selection.get("model_id")
        ):
            entry: dict[str, Any] = {
                "connection_id": str(raw_selection.get("connection_id", "")),
                "model_id": str(raw_selection.get("model_id", "")),
            }
            if raw_selection.get("effort"):
                entry["effort"] = str(raw_selection.get("effort", "")).lower()
            out["last_model_selection"] = entry
        return out

    tail_arg = str(max(0, int(tail or 0)))
    # gdbus parses arguments starting with '-' as option flags, so negative
    # sentinel values must be prefixed with the explicit GVariant type.
    raw_after = int(after_count if after_count is not None else -1)
    after_arg = f"int32 {raw_after}" if raw_after < 0 else str(raw_after)
    # Build a GVariant array-of-strings literal for gdbus: ['role1', 'role2']
    # NOTE: GVariant text format uses single-quoted strings, not double-quoted.
    roles_arg = "[" + ", ".join(f"'{r}'" for r in roles) + "]"
    response = _run_gdbus("GetMessages", conversation_id, tail_arg, after_arg, roles_arg)
    total_count, truncated, messages = response
    items = [{"role": role, "content": content} for role, content in messages]
    return {
        "messages": items,
        "message_count": int(total_count),
        "truncated": bool(truncated),
    }


def delete_conversation(conversation_id: str) -> None:
    if TRANSPORT == "ws":
        response = _ws_request({"delete_conversation": {"id": conversation_id}})
        _ws_expect_variant(response, "ack")
        return
    _run_gdbus("DeleteConversation", conversation_id)


def clear_all_history() -> int:
    if TRANSPORT == "ws":
        response = _ws_request({"clear_all_history": {}})
        payload = _ws_expect_variant(response, "cleared")
        if isinstance(payload, dict):
            return int(payload.get("deleted_count", 0))
        raise WsError(f"unexpected websocket clear_all_history payload: {payload}")
    response = _run_gdbus("ClearAllHistory")
    if isinstance(response, tuple) and len(response) > 0:
        return int(response[0])
    return int(response)


def wait_for_assistant_reply(conversation_id: str, initial_count: int, timeout_sec: float, interval_sec: float) -> str:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        conversation = get_conversation(conversation_id)
        messages = conversation["messages"]
        if len(messages) > initial_count:
            new_messages = messages[initial_count:]
            latest_new_user_index = -1
            for i, message in enumerate(new_messages):
                if message.get("role") == "user":
                    latest_new_user_index = i

            if latest_new_user_index >= 0:
                for message in new_messages[latest_new_user_index + 1 :]:
                    if message.get("role") == "assistant":
                        return message.get("content", "")
            else:
                # Only accept an assistant reply without a new user inside this
                # window when the boundary is immediately after a user message.
                boundary_is_after_user = initial_count > 0 and initial_count <= len(messages) and (
                    messages[initial_count - 1].get("role") == "user"
                )
                if boundary_is_after_user:
                    for message in new_messages:
                        if message.get("role") == "assistant":
                            return message.get("content", "")
        time.sleep(interval_sec)

    return ""


def list_conversations(max_age_days: int | None = None, include_archived: bool = False) -> list[dict[str, Any]]:
    age = max(0, int(max_age_days or 0))
    if TRANSPORT == "ws":
        payload: dict[str, Any] = {"max_age_days": age if age > 0 else None, "include_archived": include_archived}
        response = _ws_request({"list_conversations": payload})
        response = _ws_expect_variant(response, "conversations")
        if not isinstance(response, list):
            raise WsError(f"unexpected websocket list_conversations payload: {response}")
        conversations = []
        for item in response:
            if not isinstance(item, dict):
                continue
            conversations.append(
                {
                    "id": str(item.get("id", "")),
                    "title": str(item.get("title", "")),
                    "message_count": int(item.get("message_count", 0)),
                    "updated_at": str(item.get("updated_at", "")),
                    "archived": bool(item.get("archived", False)),
                }
            )
        return conversations

    response = _run_gdbus("ListConversations", str(age), "true" if include_archived else "false")
    rows = response[0] if isinstance(response, tuple) and len(response) == 1 else response
    return [
        {
            "id": item[0],
            "title": item[1],
            "message_count": item[2],
            "updated_at": item[3] if len(item) > 3 else "",
            "archived": item[4] if len(item) > 4 else False,
        }
        for item in rows
    ]


def ensure_conversation(title: str) -> str:
    conversations = list_conversations(max_age_days=0)
    for conversation in conversations:
        if conversation["title"] == title:
            return str(conversation["id"])
    return create_conversation(title)


def list_background_tasks(
    include_finished: bool = False,
    limit: int | None = None,
) -> list[dict[str, Any]]:
    """Return the daemon's background-task list.

    Mirrors `Command::ListBackgroundTasks` from `desktop-assistant-api-model`.
    Returns an empty list on D-Bus transport so the badge / window degrade
    gracefully until the daemon-side D-Bus task interface (#116) ships.
    """
    if TRANSPORT != "ws":
        return []

    payload: dict[str, Any] = {"include_finished": bool(include_finished)}
    if limit is not None:
        payload["limit"] = int(limit)
    response = _ws_request({"list_background_tasks": payload})
    tasks = _ws_expect_variant(response, "background_tasks")
    if not isinstance(tasks, list):
        raise WsError(f"unexpected list_background_tasks payload: {tasks}")
    # Pass TaskView objects through verbatim — the QML side reads the
    # same field names (id, title, status, started_at, ended_at,
    # progress_hint, last_error, kind, parent, children). Keeping the
    # shape stable avoids a translation layer that would need updating
    # every time the daemon adds a field.
    return [t for t in tasks if isinstance(t, dict)]


def cancel_background_task(task_id: str) -> None:
    """Request cancellation of a background task.

    Translates to `Command::CancelBackgroundTask`. The daemon Acks
    synchronously; the actual lifecycle transition arrives later as
    `Event::TaskCompleted` with `status == cancelled`.
    """
    if TRANSPORT != "ws":
        raise WsError("cancel_background_task requires WebSocket transport (#116)")
    tid = (task_id or "").strip()
    if not tid:
        raise WsError("cancel_background_task: empty task id")
    _ws_request({"cancel_background_task": {"id": tid}})


def get_background_task_logs(
    task_id: str,
    after_seq: int | None = None,
    limit: int | None = None,
) -> dict[str, Any]:
    """Fetch a page of log entries for a background task.

    Returns ``{"entries": [...], "next_seq": int}``. Pass the returned
    ``next_seq`` back as ``after_seq`` to resume paging.
    """
    if TRANSPORT != "ws":
        return {"entries": [], "next_seq": 0}
    tid = (task_id or "").strip()
    if not tid:
        raise WsError("get_background_task_logs: empty task id")
    payload: dict[str, Any] = {"id": tid}
    if after_seq is not None:
        payload["after_seq"] = int(after_seq)
    if limit is not None:
        payload["limit"] = int(limit)
    response = _ws_request({"get_background_task_logs": payload})
    body = _ws_expect_variant(response, "background_task_logs")
    if not isinstance(body, dict):
        raise WsError(f"unexpected background_task_logs payload: {body}")
    entries = body.get("entries", [])
    next_seq = body.get("next_seq", 0)
    return {
        "entries": [e for e in entries if isinstance(e, dict)],
        "next_seq": int(next_seq) if isinstance(next_seq, (int, float)) else 0,
    }


def subscribe_background_tasks() -> None:
    """Subscribe this connection to ``Task*`` events for the calling user.

    The daemon serializes this unit variant as the bare string
    ``"subscribe_background_tasks"``.
    """
    if TRANSPORT != "ws":
        # Subscriptions only flow over WS today; on D-Bus the QML side
        # polls list_background_tasks via Timer instead. Quiet no-op
        # keeps the call sites uniform.
        return
    _ws_request("subscribe_background_tasks")


# Statuses that mean "in flight" for the purpose of the badge counter.
# `Pending` is included because, from the user's perspective, a queued
# task is also "work I asked for that hasn't finished".
_RUNNING_STATUSES = frozenset({"pending", "running"})


def _count_running(tasks: list[dict[str, Any]]) -> int:
    return sum(
        1 for t in tasks if isinstance(t, dict) and str(t.get("status", "")) in _RUNNING_STATUSES
    )


def apply_task_event(
    tasks: list[dict[str, Any]],
    event: Any,
) -> tuple[list[dict[str, Any]], int]:
    """Apply a single ``Event::Task*`` payload to an in-memory task list.

    Returns ``(next_tasks, running_count)``. The function is pure: the
    input list is not mutated. This is the substitute for a stateful
    C++ ``TasksModel`` — the QML side calls it whenever an event
    arrives (or on a polling tick) and replaces its model array.

    Unknown events, malformed events, and events referencing unknown
    task ids are dropped on the floor: returning the input unchanged is
    safer than corrupting the model with half-built rows.
    """
    if not isinstance(tasks, list):
        tasks = []
    if not isinstance(event, dict) or len(event) != 1:
        return list(tasks), _count_running(tasks)

    [(kind, body)] = event.items()
    if not isinstance(body, dict):
        return list(tasks), _count_running(tasks)

    next_tasks = list(tasks)

    if kind == "task_started":
        task = body.get("task")
        if not isinstance(task, dict) or not task.get("id"):
            return next_tasks, _count_running(next_tasks)
        new_id = task["id"]
        # Replace in place if we already know the id (idempotent under
        # reconnect-replay of the initial ListBackgroundTasks +
        # subsequent TaskStarted events).
        for idx, existing in enumerate(next_tasks):
            if isinstance(existing, dict) and existing.get("id") == new_id:
                next_tasks[idx] = task
                return next_tasks, _count_running(next_tasks)
        next_tasks.append(task)
        return next_tasks, _count_running(next_tasks)

    if kind == "task_completed":
        task_id = body.get("id")
        if not task_id:
            return next_tasks, _count_running(next_tasks)
        for idx, existing in enumerate(next_tasks):
            if isinstance(existing, dict) and existing.get("id") == task_id:
                updated = dict(existing)
                updated["status"] = body.get("status", existing.get("status", "completed"))
                if "last_error" in body and body["last_error"] is not None:
                    updated["last_error"] = body["last_error"]
                next_tasks[idx] = updated
                return next_tasks, _count_running(next_tasks)
        # Unknown id — drop the event rather than fabricate a row.
        return next_tasks, _count_running(next_tasks)

    if kind == "task_progress":
        task_id = body.get("id")
        if not task_id:
            return next_tasks, _count_running(next_tasks)
        for idx, existing in enumerate(next_tasks):
            if isinstance(existing, dict) and existing.get("id") == task_id:
                updated = dict(existing)
                if "progress_hint" in body:
                    updated["progress_hint"] = body["progress_hint"]
                next_tasks[idx] = updated
                return next_tasks, _count_running(next_tasks)
        return next_tasks, _count_running(next_tasks)

    if kind == "task_log_appended":
        # Log entries are not materialized in the task list itself; the
        # QML side fetches them lazily via get_background_task_logs.
        return next_tasks, _count_running(next_tasks)

    return next_tasks, _count_running(next_tasks)


# --- Voice service (repo adelie-ai/voice, org.desktopAssistant.Voice) --------
# The chat plasmoid is just another client of the voice daemon. Each function
# below shells out to `gdbus call` against the Voice interface and returns a
# plain Python value the QML side serializes to JSON. Callers should first
# check `voice_available()` (NameHasOwner) so the UI can disable cleanly when
# the service isn't installed/running rather than surfacing a D-Bus error.


def voice_available() -> bool:
    """True when org.desktopAssistant.Voice currently has an owner on the bus.

    Returns False (rather than raising) on any probe failure so the widget can
    treat "can't tell" the same as "not running" and disable its voice UI.
    """
    try:
        return _name_has_owner(VOICE_SERVICE)
    except DbusError:
        return False


def voice_get_state() -> str:
    response = _run_gdbus_voice("GetState")
    if isinstance(response, tuple) and len(response) > 0:
        return str(response[0])
    return str(response)


def voice_get_enabled() -> bool:
    response = _run_gdbus_voice("GetEnabled")
    if isinstance(response, tuple) and len(response) > 0:
        return bool(response[0])
    return bool(response)


def voice_set_enabled(enabled: bool) -> None:
    _run_gdbus_voice("SetEnabled", "true" if enabled else "false")


def voice_push_to_talk(conversation_id: str = "") -> None:
    # Dictate into a specific conversation (the chat the user is viewing) when
    # given an orchestrator conversation id, so the spoken prompt and reply land
    # in that chat. An empty id falls back to the daemon's own "Voice
    # Conversation" session (plain PushToTalk), matching the wake word.
    if conversation_id:
        _run_gdbus_voice("PushToTalkInConversation", conversation_id)
    else:
        _run_gdbus_voice("PushToTalk")


def voice_stop_speaking() -> None:
    _run_gdbus_voice("StopSpeaking")


def voice_stop_listening() -> None:
    # Abort an in-flight dictation/processing/speaking turn. The daemon's
    # StopListening() takes no args and returns the pipeline to Idle, so the
    # mic button can act as a toggle (start when Idle, stop otherwise).
    _run_gdbus_voice("StopListening")


def voice_say_text(text: str) -> None:
    _run_gdbus_voice("SayText", text)


def voice_list_voices() -> list[dict[str, Any]]:
    """Return installed TTS voices as a list of dicts.

    Daemon shape is `a(sssu)` — (voice_id, display_name, language,
    num_speakers). gdbus renders it as a 1-tuple wrapping the array.
    """
    response = _run_gdbus_voice("ListVoices")
    rows = response[0] if isinstance(response, tuple) and len(response) == 1 else response
    if not isinstance(rows, list):
        return []
    voices: list[dict[str, Any]] = []
    for item in rows:
        if not isinstance(item, (list, tuple)) or len(item) < 4:
            continue
        voices.append(
            {
                "voice_id": str(item[0]),
                "display_name": str(item[1]),
                "language": str(item[2]),
                "num_speakers": int(item[3]),
            }
        )
    return voices


def voice_get_voice() -> dict[str, Any]:
    """Return the active voice as {voice_id, speaker_id}; speaker_id -1 = unset."""
    response = _run_gdbus_voice("GetVoice")
    if isinstance(response, tuple) and len(response) >= 2:
        return {"voice_id": str(response[0]), "speaker_id": int(response[1])}
    # gdbus wraps the struct return in an outer tuple: ((id, speaker),)
    if isinstance(response, tuple) and len(response) == 1 and isinstance(response[0], (list, tuple)):
        inner = response[0]
        if len(inner) >= 2:
            return {"voice_id": str(inner[0]), "speaker_id": int(inner[1])}
    return {"voice_id": "", "speaker_id": -1}


def voice_set_voice(voice_id: str, speaker: int) -> None:
    # gdbus parses a bare leading '-' as an option flag, so the negative
    # sentinel speaker id must carry an explicit GVariant type annotation.
    speaker_arg = f"int32 {speaker}" if speaker < 0 else str(speaker)
    _run_gdbus_voice("SetVoice", voice_id, speaker_arg)


def cmd_voice_status() -> int:
    """Emit a single JSON object describing the voice service for the widget.

    Always exits 0: "voice service down" is a normal, expected state the UI
    renders as disabled controls, not an error. When the service is present we
    also fold in the current state/enabled/voice so the widget can paint its
    initial UI from one round-trip.
    """
    payload: dict[str, Any] = {"available": voice_available()}
    if payload["available"]:
        try:
            payload["state"] = voice_get_state()
            payload["enabled"] = voice_get_enabled()
            payload["voice"] = voice_get_voice()
        except DbusError as exc:
            # The name had an owner a moment ago but a call failed (e.g. the
            # daemon is mid-shutdown). Treat as unavailable for UI purposes.
            payload["available"] = False
            payload["error"] = str(exc)
    print(json.dumps(payload))
    return 0


def cmd_status() -> int:
    payload: dict[str, Any] = {
        "selected_connection": CONNECTION_NAME,
        "default_connection": DEFAULT_CONFIG_CONNECTION,
        "transport": TRANSPORT,
        "ws_url": WS_URL if TRANSPORT == "ws" else "",
        "selected_service": SERVICE,
        "default_service": DEFAULT_SERVICE,
        "dev_service": DEV_SERVICE,
    }

    if TRANSPORT == "ws":
        try:
            response = _ws_request({"ping": {}})
            pong = _ws_expect_variant(response, "pong")
            payload["production_running"] = bool(
                isinstance(pong, dict) and str(pong.get("value", "")) == "pong"
            )
            payload["dev_running"] = False
        except WsError as exc:
            payload["production_running"] = False
            payload["dev_running"] = False
            payload["error"] = str(exc)
    else:
        try:
            payload["production_running"] = _name_has_owner(DEFAULT_SERVICE)
            payload["dev_running"] = _name_has_owner(DEV_SERVICE)
        except DbusError as exc:
            payload["production_running"] = False
            payload["dev_running"] = False
            payload["error"] = str(exc)

    print(json.dumps(payload))
    return 0


def main() -> int:
    global CONNECTION_NAME, DEFAULT_CONFIG_CONNECTION, SERVICE, TRANSPORT, WS_JWT, WS_SUBJECT, WS_URL

    parser = argparse.ArgumentParser()
    parser.add_argument("--connection-name", default="")
    parser.add_argument("--service", default="")
    parser.add_argument("--transport", default="")
    parser.add_argument("--ws-url", default="")
    parser.add_argument("--ws-jwt", default="")
    parser.add_argument("--ws-subject", default="")
    subparsers = parser.add_subparsers(dest="command", required=True)

    ensure_cmd = subparsers.add_parser("ensure")
    ensure_cmd.add_argument("--title", default="Desktop Chat")

    create_cmd = subparsers.add_parser("create")
    create_cmd.add_argument("--title", default="Desktop Chat")

    list_cmd = subparsers.add_parser("list")
    list_cmd.add_argument("--max-age-days", type=int, default=7)

    send_cmd = subparsers.add_parser("send")
    send_cmd.add_argument("conversation_id")
    send_cmd.add_argument("prompt")
    send_cmd.add_argument(
        "--override-connection",
        default="",
        help="Connection id to route this single message through (WS transport only).",
    )
    send_cmd.add_argument(
        "--override-model",
        default="",
        help="Model id to use for this single message (WS transport only).",
    )
    send_cmd.add_argument(
        "--override-effort",
        default="",
        choices=["", "low", "medium", "high"],
        help="Optional effort hint for this single message.",
    )

    get_cmd = subparsers.add_parser("get")
    get_cmd.add_argument("conversation_id")
    get_cmd.add_argument("--tail", type=int, default=0)
    get_cmd.add_argument("--after-count", type=int)
    get_cmd.add_argument(
        "--roles",
        default="user,assistant",
        help="Comma-separated role allowlist (default: user,assistant). "
             "Pass an empty string to return all roles.",
    )

    delete_cmd = subparsers.add_parser("delete")
    delete_cmd.add_argument("conversation_id")

    subparsers.add_parser("clear")

    await_cmd = subparsers.add_parser("await")
    await_cmd.add_argument("conversation_id")
    await_cmd.add_argument("--initial-count", type=int, required=True)
    await_cmd.add_argument("--timeout", type=float, default=60.0)
    await_cmd.add_argument("--interval", type=float, default=0.8)

    subparsers.add_parser("connections")
    subparsers.add_parser("status")

    # Multi-connection daemon API (WebSocket transport only, desktop-assistant#17).
    subparsers.add_parser("list-llm-connections")

    models_cmd = subparsers.add_parser("list-models")
    models_cmd.add_argument("--connection-id", default="")
    models_cmd.add_argument("--refresh", action="store_true")

    create_conn_cmd = subparsers.add_parser("create-llm-connection")
    create_conn_cmd.add_argument("--id", required=True)
    create_conn_cmd.add_argument(
        "--config",
        required=True,
        help="JSON-encoded ConnectionConfigView, e.g. '{\"type\":\"openai\"}'",
    )

    update_conn_cmd = subparsers.add_parser("update-llm-connection")
    update_conn_cmd.add_argument("--id", required=True)
    update_conn_cmd.add_argument("--config", required=True)

    delete_conn_cmd = subparsers.add_parser("delete-llm-connection")
    delete_conn_cmd.add_argument("--id", required=True)
    delete_conn_cmd.add_argument("--force", action="store_true")

    subparsers.add_parser("get-purposes")

    purpose_cmd = subparsers.add_parser("set-purpose")
    purpose_cmd.add_argument(
        "--purpose",
        required=True,
        choices=["interactive", "dreaming", "embedding", "titling"],
    )
    purpose_cmd.add_argument("--connection", required=True)
    purpose_cmd.add_argument("--model", required=True)
    purpose_cmd.add_argument(
        "--effort",
        default="",
        choices=["", "low", "medium", "high"],
    )

    # Background tasks (#7 / desktop-assistant#114). WS-only today.
    tasks_list_cmd = subparsers.add_parser("tasks-list")
    tasks_list_cmd.add_argument("--include-finished", action="store_true")
    tasks_list_cmd.add_argument("--limit", type=int, default=0)

    tasks_cancel_cmd = subparsers.add_parser("tasks-cancel")
    tasks_cancel_cmd.add_argument("task_id")

    tasks_logs_cmd = subparsers.add_parser("tasks-logs")
    tasks_logs_cmd.add_argument("task_id")
    tasks_logs_cmd.add_argument("--after-seq", type=int, default=-1)
    tasks_logs_cmd.add_argument("--limit", type=int, default=0)

    subparsers.add_parser("tasks-subscribe")

    # `tasks-apply-event` reads JSON `{tasks: [...], event: {...}}` from
    # stdin and prints `{tasks: [...], running_count: N}` so the QML
    # side can route incoming events through the pure transformer
    # without re-implementing it in JS.
    subparsers.add_parser("tasks-apply-event")

    # Voice service (adele-kde#29 / repo adelie-ai/voice). Always over D-Bus.
    subparsers.add_parser("voice-status")
    subparsers.add_parser("voice-state")
    subparsers.add_parser("voice-get-enabled")

    voice_enable_cmd = subparsers.add_parser("voice-set-enabled")
    voice_enable_cmd.add_argument("enabled", choices=["true", "false"])

    voice_ptt_cmd = subparsers.add_parser("voice-push-to-talk")
    voice_ptt_cmd.add_argument("--conversation-id", default="")
    subparsers.add_parser("voice-stop-speaking")
    subparsers.add_parser("voice-stop-listening")

    voice_say_cmd = subparsers.add_parser("voice-say")
    voice_say_cmd.add_argument("text")

    subparsers.add_parser("voice-list-voices")
    subparsers.add_parser("voice-get-voice")

    voice_set_voice_cmd = subparsers.add_parser("voice-set-voice")
    voice_set_voice_cmd.add_argument("voice_id")
    voice_set_voice_cmd.add_argument("--speaker", type=int, default=-1)

    args = parser.parse_args()
    payload = _load_widget_settings_payload()
    connections, default_connection = _load_widget_connections(payload)
    DEFAULT_CONFIG_CONNECTION = default_connection

    requested_connection = args.connection_name.strip() or _load_widget_connection_name(payload) or default_connection
    if requested_connection not in connections:
        requested_connection = default_connection

    resolved = connections.get(requested_connection)
    if resolved is None:
        resolved = next(iter(connections.values()))
    CONNECTION_NAME = requested_connection
    TRANSPORT = _normalize_transport(str(resolved.get("transport", DEFAULT_TRANSPORT)))
    SERVICE = str(resolved.get("dbus_service", "")).strip() or DEFAULT_SERVICE
    WS_URL = str(resolved.get("ws_url", "")).strip() or DEFAULT_WS_URL
    WS_SUBJECT = str(resolved.get("ws_subject", "")).strip() or DEFAULT_WS_SUBJECT
    WS_JWT = str(payload.get("ws_jwt", "")).strip()

    service_override = args.service.strip() or os.environ.get("DESKTOP_ASSISTANT_WIDGET_DBUS_SERVICE", "").strip()
    if service_override:
        SERVICE = service_override

    transport_override = (args.transport.strip() or os.environ.get("DESKTOP_ASSISTANT_WIDGET_TRANSPORT", "").strip()).lower()
    if transport_override:
        if transport_override not in {"ws", "dbus"}:
            print(json.dumps({"error": f"invalid transport '{transport_override}'"}))
            return 1
        TRANSPORT = transport_override

    ws_url_override = args.ws_url.strip() or os.environ.get("DESKTOP_ASSISTANT_WIDGET_WS_URL", "").strip()
    if ws_url_override:
        WS_URL = ws_url_override

    ws_subject_override = args.ws_subject.strip() or os.environ.get("DESKTOP_ASSISTANT_WIDGET_WS_SUBJECT", "").strip()
    if ws_subject_override:
        WS_SUBJECT = ws_subject_override

    ws_jwt_override = args.ws_jwt.strip() or os.environ.get("DESKTOP_ASSISTANT_WIDGET_WS_JWT", "").strip()
    if ws_jwt_override:
        WS_JWT = ws_jwt_override

    try:
        if args.command == "ensure":
            print(json.dumps({"conversation_id": ensure_conversation(args.title)}))
            return 0
        if args.command == "create":
            print(json.dumps({"conversation_id": create_conversation(args.title)}))
            return 0
        if args.command == "list":
            print(json.dumps({"conversations": list_conversations(args.max_age_days)}))
            return 0
        if args.command == "send":
            override = _build_override(
                getattr(args, "override_connection", ""),
                getattr(args, "override_model", ""),
                getattr(args, "override_effort", ""),
            )
            request_id = send_prompt(args.conversation_id, args.prompt, override=override)
            print(json.dumps({"request_id": request_id}))
            return 0
        if args.command == "get":
            include = [r.strip() for r in args.roles.split(",") if r.strip()]
            print(json.dumps(get_messages(args.conversation_id, args.tail, args.after_count, include)))
            return 0
        if args.command == "delete":
            delete_conversation(args.conversation_id)
            print(json.dumps({"deleted": True, "conversation_id": args.conversation_id}))
            return 0
        if args.command == "clear":
            deleted_count = clear_all_history()
            print(json.dumps({"deleted_count": deleted_count}))
            return 0
        if args.command == "await":
            content = wait_for_assistant_reply(
                args.conversation_id,
                args.initial_count,
                args.timeout,
                args.interval,
            )
            print(json.dumps({"assistant_reply": content}))
            return 0
        if args.command == "connections":
            serialized_connections = []
            for connection in connections.values():
                serialized_connections.append(
                    {
                        "name": str(connection.get("name", "")),
                        "transport": _normalize_transport(str(connection.get("transport", DEFAULT_TRANSPORT))),
                        "dbus_service": str(connection.get("dbus_service", "")).strip(),
                        "ws_url": str(connection.get("ws_url", "")).strip(),
                        "ws_subject": str(connection.get("ws_subject", "")).strip(),
                    }
                )
            print(
                json.dumps(
                    {
                        "selected_connection": CONNECTION_NAME,
                        "default_connection": default_connection,
                        "connections": serialized_connections,
                    }
                )
            )
            return 0
        if args.command == "status":
            return cmd_status()
        if args.command == "list-llm-connections":
            print(json.dumps({"connections": list_connections()}))
            return 0
        if args.command == "list-models":
            conn_id = args.connection_id.strip() or None
            print(
                json.dumps(
                    {"models": list_available_models(conn_id, refresh=args.refresh)}
                )
            )
            return 0
        if args.command == "create-llm-connection":
            try:
                config = json.loads(args.config)
            except json.JSONDecodeError as exc:
                raise WsError(f"invalid --config JSON: {exc}") from exc
            create_connection(args.id, config)
            print(json.dumps({"created": True, "id": args.id}))
            return 0
        if args.command == "update-llm-connection":
            try:
                config = json.loads(args.config)
            except json.JSONDecodeError as exc:
                raise WsError(f"invalid --config JSON: {exc}") from exc
            update_connection(args.id, config)
            print(json.dumps({"updated": True, "id": args.id}))
            return 0
        if args.command == "delete-llm-connection":
            delete_connection(args.id, force=args.force)
            print(json.dumps({"deleted": True, "id": args.id, "force": args.force}))
            return 0
        if args.command == "get-purposes":
            print(json.dumps({"purposes": get_purposes()}))
            return 0
        if args.command == "tasks-list":
            limit = args.limit if args.limit > 0 else None
            tasks = list_background_tasks(
                include_finished=bool(args.include_finished),
                limit=limit,
            )
            print(json.dumps({"tasks": tasks, "running_count": _count_running(tasks)}))
            return 0
        if args.command == "tasks-cancel":
            cancel_background_task(args.task_id)
            print(json.dumps({"cancelled": True, "id": args.task_id}))
            return 0
        if args.command == "tasks-logs":
            after = args.after_seq if args.after_seq >= 0 else None
            limit = args.limit if args.limit > 0 else None
            body = get_background_task_logs(args.task_id, after_seq=after, limit=limit)
            print(json.dumps(body))
            return 0
        if args.command == "tasks-subscribe":
            subscribe_background_tasks()
            print(json.dumps({"subscribed": True}))
            return 0
        if args.command == "tasks-apply-event":
            try:
                payload = json.loads(sys.stdin.read() or "{}")
            except json.JSONDecodeError as exc:
                print(json.dumps({"error": f"invalid stdin JSON: {exc}"}))
                return 1
            tasks_in = payload.get("tasks", []) if isinstance(payload, dict) else []
            event = payload.get("event") if isinstance(payload, dict) else None
            next_tasks, running = apply_task_event(
                tasks_in if isinstance(tasks_in, list) else [],
                event,
            )
            print(json.dumps({"tasks": next_tasks, "running_count": running}))
            return 0
        if args.command == "set-purpose":
            config: dict[str, Any] = {
                "connection": args.connection,
                "model": args.model,
            }
            effort = args.effort.strip().lower()
            if effort in {"low", "medium", "high"}:
                config["effort"] = effort
            set_purpose(args.purpose, config)
            print(json.dumps({"ok": True, "purpose": args.purpose}))
            return 0
        if args.command == "voice-status":
            return cmd_voice_status()
        if args.command == "voice-state":
            print(json.dumps({"state": voice_get_state()}))
            return 0
        if args.command == "voice-get-enabled":
            print(json.dumps({"enabled": voice_get_enabled()}))
            return 0
        if args.command == "voice-set-enabled":
            voice_set_enabled(args.enabled == "true")
            print(json.dumps({"enabled": args.enabled == "true"}))
            return 0
        if args.command == "voice-push-to-talk":
            voice_push_to_talk(args.conversation_id)
            print(json.dumps({"ok": True}))
            return 0
        if args.command == "voice-stop-speaking":
            voice_stop_speaking()
            print(json.dumps({"ok": True}))
            return 0
        if args.command == "voice-stop-listening":
            voice_stop_listening()
            print(json.dumps({"ok": True}))
            return 0
        if args.command == "voice-say":
            voice_say_text(args.text)
            print(json.dumps({"ok": True}))
            return 0
        if args.command == "voice-list-voices":
            print(json.dumps({"voices": voice_list_voices()}))
            return 0
        if args.command == "voice-get-voice":
            print(json.dumps({"voice": voice_get_voice()}))
            return 0
        if args.command == "voice-set-voice":
            voice_set_voice(args.voice_id, args.speaker)
            print(json.dumps({"ok": True, "voice_id": args.voice_id, "speaker": args.speaker}))
            return 0
        raise DbusError("unknown command")
    except (DbusError, WsError) as exc:
        print(json.dumps({"error": str(exc)}))
        return 1


if __name__ == "__main__":
    sys.exit(main())
