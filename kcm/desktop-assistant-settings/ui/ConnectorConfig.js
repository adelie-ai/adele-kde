.pragma library

// Pure view<->config mapping for the connections editor (adele-kde#1, #114).
//
// Factored out of ConnectionEditor.qml so the per-connector config assembly and
// the edit-prefill parse can be unit-tested without instantiating the editor
// (which needs the C++ `kcm` context object and so is only compile-probed
// headless — the same approach as AudioDevices.js / VoiceBackends.js).
//
// The daemon's `ConnectionConfigView` is a tagged enum whose non-secret fields
// diverge per connector. `buildConfig` is the create/save direction: an editor
// field bag -> the view-shaped object sent to create_connection /
// update_connection. `parseConfig` is the edit direction: the daemon's echoed
// non-secret config -> the editor field bag, applying per-connector enum
// defaults. `defaultFields` seeds the create form. Together they round-trip a
// connection through the editor.
//
// Secrets never travel through this mapping — only the api_key_env *name* does.

// Trim any value to a string ("" for null/undefined).
function _s(v) {
    return String(v === undefined || v === null ? "" : v).trim()
}

// Stringify an optional numeric config field for an editor text box ("" when
// the config omits it), without trimming a genuine 0 away.
function _num(v) {
    return (v === undefined || v === null) ? "" : String(v)
}

// Per-connector auth_mode default: Azure authenticates with a key, Google talks
// to Vertex AI. Other connectors have no auth_mode field.
function _defaultAuthMode(connectorType) {
    var t = String(connectorType || "").toLowerCase()
    if (t === "azure") return "api_key"
    if (t === "google") return "vertex"
    return ""
}

// Build the `ConnectionConfigView`-shaped object for `create_connection` /
// `update_connection` from the editor's field bag. Returns null for an unknown
// connector type so the editor can surface its "unsupported type" path. Blank
// fields are omitted so the daemon applies its own defaults and the config
// round-trips cleanly.
function buildConfig(connectorType, fields) {
    var t = String(connectorType || "").toLowerCase()
    var f = fields || {}
    var cfg = { type: t }
    function put(key, val) {
        var s = _s(val)
        if (s.length > 0) cfg[key] = s
    }

    if (t === "anthropic" || t === "openai" || t === "openrouter") {
        put("base_url", f.baseUrl)
        put("api_key_env", f.apiKeyEnv)
    } else if (t === "azure") {
        put("base_url", f.baseUrl)
        put("api_key_env", f.apiKeyEnv)
        put("api_surface", f.apiSurface)
        put("auth_mode", f.authMode)
        // api_version is a classic-surface-only knob; the v1 GA surface is
        // versionless, so never emit a (possibly stale) version under it.
        if (_s(f.apiSurface) === "classic") put("api_version", f.apiVersion)
    } else if (t === "google") {
        put("base_url", f.baseUrl)
        put("api_key_env", f.apiKeyEnv)
        put("project", f.project)
        put("location", f.location)
        put("auth_mode", f.authMode)
        put("credentials_path", f.credentialsPath)
    } else if (t === "bedrock") {
        put("aws_profile", f.awsProfile)
        put("region", f.region)
        put("base_url", f.baseUrl)
    } else if (t === "ollama") {
        put("base_url", f.baseUrl)
        // Only send keep_warm when enabled so the field round-trips cleanly.
        if (f.keepWarm === true) cfg.keep_warm = true
    } else {
        return null
    }

    // Streaming stall budgets and the context hard cap apply to every
    // connector: blank -> omit (use the connector default), a positive integer
    // -> override. Zero and negatives are treated as "unset".
    var connect = parseInt(_s(f.connectTimeout), 10)
    if (!isNaN(connect) && connect > 0) cfg.connect_timeout_secs = connect
    var stream = parseInt(_s(f.streamTimeout), 10)
    if (!isNaN(stream) && stream > 0) cfg.stream_timeout_secs = stream
    var maxCtx = parseInt(_s(f.maxContextTokens), 10)
    if (!isNaN(maxCtx) && maxCtx > 0) cfg.max_context_tokens = maxCtx

    return cfg
}

// Pre-fill the editor's field bag from the daemon's echoed non-secret config
// (`ConnectionView.config`). Applies the per-connector enum defaults when the
// stored config omits them (older connections predate the Azure/Google knobs).
function parseConfig(connectorType, config) {
    var c = config || {}
    return {
        baseUrl: _s(c.base_url),
        apiKeyEnv: _s(c.api_key_env),
        // Azure GA default is the v1 surface when unspecified.
        apiSurface: c.api_surface ? _s(c.api_surface) : "v1",
        apiVersion: _s(c.api_version),
        project: _s(c.project),
        location: _s(c.location),
        credentialsPath: _s(c.credentials_path),
        authMode: c.auth_mode ? _s(c.auth_mode) : _defaultAuthMode(connectorType),
        awsProfile: _s(c.aws_profile),
        region: _s(c.region),
        keepWarm: c.keep_warm === true,
        connectTimeout: _num(c.connect_timeout_secs),
        streamTimeout: _num(c.stream_timeout_secs),
        maxContextTokens: _num(c.max_context_tokens),
    }
}

// Field bag for a brand-new connection of the given type: everything blank
// except the seeded per-connector enum defaults the create form shows.
function defaultFields(connectorType) {
    return {
        baseUrl: "",
        apiKeyEnv: "",
        apiSurface: "v1",
        apiVersion: "",
        project: "",
        location: "",
        credentialsPath: "",
        authMode: _defaultAuthMode(connectorType),
        awsProfile: "",
        region: "",
        keepWarm: false,
        connectTimeout: "",
        streamTimeout: "",
        maxContextTokens: "",
    }
}
