import QtQuick
import QtTest 1.0

import "../../kcm/desktop-assistant-settings/ui/ConnectorConfig.js" as ConnectorConfig

// Unit tests for the connections editor's view<->config mapping
// (adele-kde#1, #114).
//
// ConnectionEditor.qml builds the `ConnectionConfigView`-shaped object it sends
// to create_connection / update_connection from its editor fields
// (`ConnectorConfig.buildConfig`), and pre-fills those fields from the daemon's
// echoed non-secret config when editing (`ConnectorConfig.parseConfig`). The
// editor itself needs the C++ `kcm` context object and can't be instantiated
// headless (it's compile-probed in tst_QmlComponentsLoad), but this mapping is
// pure logic, so we pin it directly — the same approach as tst_AudioDevices /
// tst_VoiceBackends.
//
// The contract under test, per connector:
//   * buildConfig emits exactly the fields the matching ConnectionConfigView
//     variant carries, trimming whitespace and omitting blanks;
//   * the api_surface / auth_mode enums round-trip, with the classic-only
//     api_version emitted only under the classic surface;
//   * parseConfig applies the per-connector enum defaults (Azure api_key + v1,
//     Google vertex) when the stored config omits them;
//   * a full create -> save -> from_view round-trip preserves the fields;
//   * an unknown connector type yields null (the editor's "unsupported" path).
TestCase {
    id: testCase
    name: "ConnectorConfig"

    // --- buildConfig: OpenAI-compatible family --------------------------------

    function test_buildConfig_openai_emits_base_url_and_key_env() {
        var cfg = ConnectorConfig.buildConfig("openai", {
            baseUrl: "https://gw.example/v1", apiKeyEnv: "OPENAI_API_KEY",
        })
        compare(cfg.type, "openai")
        compare(cfg.base_url, "https://gw.example/v1")
        compare(cfg.api_key_env, "OPENAI_API_KEY")
        verify(cfg.api_surface === undefined)
    }

    function test_buildConfig_openrouter_shares_openai_shape() {
        var cfg = ConnectorConfig.buildConfig("openrouter", {
            apiKeyEnv: "OPENROUTER_API_KEY",
        })
        compare(cfg.type, "openrouter")
        compare(cfg.api_key_env, "OPENROUTER_API_KEY")
        verify(cfg.base_url === undefined)
    }

    // --- buildConfig: Azure ---------------------------------------------------

    function test_buildConfig_azure_v1_shape() {
        var cfg = ConnectorConfig.buildConfig("azure", {
            baseUrl: "https://res.openai.azure.com",
            apiKeyEnv: "AZURE_OPENAI_API_KEY",
            apiSurface: "v1", authMode: "api_key", apiVersion: "",
        })
        compare(cfg.type, "azure")
        compare(cfg.base_url, "https://res.openai.azure.com")
        compare(cfg.api_key_env, "AZURE_OPENAI_API_KEY")
        compare(cfg.api_surface, "v1")
        compare(cfg.auth_mode, "api_key")
        // v1 GA is versionless — api_version must not travel under it.
        verify(cfg.api_version === undefined)
    }

    function test_buildConfig_azure_classic_includes_api_version() {
        var cfg = ConnectorConfig.buildConfig("azure", {
            baseUrl: "https://res.openai.azure.com",
            apiSurface: "classic", authMode: "entra", apiVersion: "2024-10-21",
        })
        compare(cfg.api_surface, "classic")
        compare(cfg.auth_mode, "entra")
        compare(cfg.api_version, "2024-10-21")
    }

    function test_buildConfig_azure_v1_drops_api_version_even_when_set() {
        // A stale api_version left over from the classic surface must not leak
        // once the user switches back to v1.
        var cfg = ConnectorConfig.buildConfig("azure", {
            apiSurface: "v1", authMode: "api_key", apiVersion: "2024-10-21",
        })
        verify(cfg.api_version === undefined)
    }

    // --- buildConfig: Google --------------------------------------------------

    function test_buildConfig_google_vertex_shape() {
        var cfg = ConnectorConfig.buildConfig("google", {
            project: "my-proj", location: "us-central1",
            authMode: "vertex", credentialsPath: "/etc/sa.json", baseUrl: "",
        })
        compare(cfg.type, "google")
        compare(cfg.project, "my-proj")
        compare(cfg.location, "us-central1")
        compare(cfg.auth_mode, "vertex")
        compare(cfg.credentials_path, "/etc/sa.json")
        // Vertex leaves base_url blank — it's derived from project/location.
        verify(cfg.base_url === undefined)
    }

    function test_buildConfig_google_api_key_mode() {
        var cfg = ConnectorConfig.buildConfig("google", {
            authMode: "api_key", apiKeyEnv: "GOOGLE_API_KEY",
        })
        compare(cfg.auth_mode, "api_key")
        compare(cfg.api_key_env, "GOOGLE_API_KEY")
        verify(cfg.project === undefined)
    }

    // --- buildConfig: Bedrock / Ollama ---------------------------------------

    function test_buildConfig_bedrock_shape() {
        var cfg = ConnectorConfig.buildConfig("bedrock", {
            awsProfile: "adele", region: "us-east-1",
        })
        compare(cfg.type, "bedrock")
        compare(cfg.aws_profile, "adele")
        compare(cfg.region, "us-east-1")
    }

    function test_buildConfig_ollama_keep_warm_only_when_true() {
        var on = ConnectorConfig.buildConfig("ollama", {
            baseUrl: "http://localhost:11434", keepWarm: true,
        })
        compare(on.keep_warm, true)
        var off = ConnectorConfig.buildConfig("ollama", {
            baseUrl: "http://localhost:11434", keepWarm: false,
        })
        verify(off.keep_warm === undefined)
    }

    // --- buildConfig: shared numeric knobs -----------------------------------

    function test_buildConfig_shared_numeric_positive_included() {
        var cfg = ConnectorConfig.buildConfig("openai", {
            connectTimeout: "45", streamTimeout: "90", maxContextTokens: "16384",
        })
        compare(cfg.connect_timeout_secs, 45)
        compare(cfg.stream_timeout_secs, 90)
        compare(cfg.max_context_tokens, 16384)
    }

    function test_buildConfig_shared_numeric_blank_zero_negative_omitted() {
        var cfg = ConnectorConfig.buildConfig("openai", {
            connectTimeout: "", streamTimeout: "0", maxContextTokens: "-5",
        })
        verify(cfg.connect_timeout_secs === undefined)
        verify(cfg.stream_timeout_secs === undefined)
        verify(cfg.max_context_tokens === undefined)
    }

    // --- buildConfig: hygiene -------------------------------------------------

    function test_buildConfig_trims_whitespace() {
        var cfg = ConnectorConfig.buildConfig("azure", {
            baseUrl: "  https://res.openai.azure.com  ",
            apiKeyEnv: " AZURE_OPENAI_API_KEY ", apiSurface: " v1 ",
        })
        compare(cfg.base_url, "https://res.openai.azure.com")
        compare(cfg.api_key_env, "AZURE_OPENAI_API_KEY")
        compare(cfg.api_surface, "v1")
    }

    function test_buildConfig_unknown_type_returns_null() {
        verify(ConnectorConfig.buildConfig("does-not-exist", {}) === null)
    }

    // --- parseConfig: enum defaults ------------------------------------------

    function test_parseConfig_azure_defaults_v1_and_api_key() {
        var f = ConnectorConfig.parseConfig("azure", {})
        compare(f.apiSurface, "v1")
        compare(f.authMode, "api_key")
    }

    function test_parseConfig_google_defaults_vertex() {
        var f = ConnectorConfig.parseConfig("google", {})
        compare(f.authMode, "vertex")
    }

    function test_parseConfig_honours_stored_values() {
        var f = ConnectorConfig.parseConfig("azure", {
            api_surface: "classic", auth_mode: "entra", api_version: "2024-10-21",
        })
        compare(f.apiSurface, "classic")
        compare(f.authMode, "entra")
        compare(f.apiVersion, "2024-10-21")
    }

    function test_parseConfig_numeric_fields_stringified() {
        var f = ConnectorConfig.parseConfig("openai", {
            connect_timeout_secs: 30, stream_timeout_secs: 60, max_context_tokens: 8192,
        })
        compare(f.connectTimeout, "30")
        compare(f.streamTimeout, "60")
        compare(f.maxContextTokens, "8192")
    }

    // --- defaultFields (create form seeding) ---------------------------------

    function test_defaultFields_azure_seeds_v1_and_api_key() {
        var f = ConnectorConfig.defaultFields("azure")
        compare(f.apiSurface, "v1")
        compare(f.authMode, "api_key")
    }

    function test_defaultFields_google_seeds_vertex() {
        compare(ConnectorConfig.defaultFields("google").authMode, "vertex")
    }

    // --- create -> save -> from_view round-trips -----------------------------

    function test_roundtrip_azure_classic_entra() {
        var fields = {
            baseUrl: "https://res.openai.azure.com",
            apiKeyEnv: "AZURE_OPENAI_API_KEY",
            apiSurface: "classic", authMode: "entra", apiVersion: "2024-10-21",
            connectTimeout: "45", streamTimeout: "", maxContextTokens: "",
        }
        var back = ConnectorConfig.parseConfig("azure",
                        ConnectorConfig.buildConfig("azure", fields))
        compare(back.baseUrl, "https://res.openai.azure.com")
        compare(back.apiKeyEnv, "AZURE_OPENAI_API_KEY")
        compare(back.apiSurface, "classic")
        compare(back.authMode, "entra")
        compare(back.apiVersion, "2024-10-21")
        compare(back.connectTimeout, "45")
    }

    function test_roundtrip_google_vertex() {
        var fields = {
            project: "my-proj", location: "us-central1",
            authMode: "vertex", credentialsPath: "/etc/sa.json",
            maxContextTokens: "32768",
        }
        var back = ConnectorConfig.parseConfig("google",
                        ConnectorConfig.buildConfig("google", fields))
        compare(back.project, "my-proj")
        compare(back.location, "us-central1")
        compare(back.authMode, "vertex")
        compare(back.credentialsPath, "/etc/sa.json")
        compare(back.maxContextTokens, "32768")
    }
}
