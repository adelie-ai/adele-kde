#pragma once

#include <KQuickConfigModule>
#include <QJSValue>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <functional>

class QDBusMessage;
class QDBusServiceWatcher;
class QFile;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

class DesktopAssistantKcm : public KQuickConfigModule {
    Q_OBJECT
    Q_PROPERTY(QString buildStamp READ buildStamp CONSTANT)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool gitEnabled READ gitEnabled WRITE setGitEnabled NOTIFY gitEnabledChanged)
    Q_PROPERTY(QString gitRemoteUrl READ gitRemoteUrl WRITE setGitRemoteUrl NOTIFY gitRemoteUrlChanged)
    Q_PROPERTY(QString gitRemoteName READ gitRemoteName WRITE setGitRemoteName NOTIFY gitRemoteNameChanged)
    Q_PROPERTY(bool gitPushOnUpdate READ gitPushOnUpdate WRITE setGitPushOnUpdate NOTIFY gitPushOnUpdateChanged)
    Q_PROPERTY(QString dbUrl READ dbUrl WRITE setDbUrl NOTIFY dbUrlChanged)
    Q_PROPERTY(int dbMaxConnections READ dbMaxConnections WRITE setDbMaxConnections NOTIFY dbMaxConnectionsChanged)
    Q_PROPERTY(QStringList connectionNames READ connectionNames NOTIFY connectionNamesChanged)
    Q_PROPERTY(QString defaultConnectionName READ defaultConnectionName WRITE setDefaultConnectionName NOTIFY defaultConnectionNameChanged)
    Q_PROPERTY(QString selectedConnectionName READ selectedConnectionName WRITE setSelectedConnectionName NOTIFY selectedConnectionNameChanged)
    Q_PROPERTY(QString selectedConnectionTransport READ selectedConnectionTransport NOTIFY selectedConnectionTransportChanged)
    Q_PROPERTY(QString selectedConnectionDbusService READ selectedConnectionDbusService WRITE setSelectedConnectionDbusService NOTIFY selectedConnectionDbusServiceChanged)
    Q_PROPERTY(QString selectedConnectionWsUrl READ selectedConnectionWsUrl WRITE setSelectedConnectionWsUrl NOTIFY selectedConnectionWsUrlChanged)
    Q_PROPERTY(QString selectedConnectionWsSubject READ selectedConnectionWsSubject WRITE setSelectedConnectionWsSubject NOTIFY selectedConnectionWsSubjectChanged)
    Q_PROPERTY(bool selectedConnectionRemovable READ selectedConnectionRemovable NOTIFY selectedConnectionRemovableChanged)
    Q_PROPERTY(bool btDreamingEnabled READ btDreamingEnabled WRITE setBtDreamingEnabled NOTIFY btDreamingEnabledChanged)
    Q_PROPERTY(int btDreamingIntervalSecs READ btDreamingIntervalSecs WRITE setBtDreamingIntervalSecs NOTIFY btDreamingIntervalSecsChanged)
    Q_PROPERTY(int btArchiveAfterDays READ btArchiveAfterDays WRITE setBtArchiveAfterDays NOTIFY btArchiveAfterDaysChanged)
    Q_PROPERTY(bool wsAuthPasswordEnabled READ wsAuthPasswordEnabled WRITE setWsAuthPasswordEnabled NOTIFY wsAuthMethodsChanged)
    Q_PROPERTY(bool wsAuthOidcEnabled READ wsAuthOidcEnabled WRITE setWsAuthOidcEnabled NOTIFY wsAuthMethodsChanged)
    Q_PROPERTY(QString oidcIssuer READ oidcIssuer WRITE setOidcIssuer NOTIFY oidcIssuerChanged)
    Q_PROPERTY(QString oidcAuthEndpoint READ oidcAuthEndpoint WRITE setOidcAuthEndpoint NOTIFY oidcAuthEndpointChanged)
    Q_PROPERTY(QString oidcTokenEndpoint READ oidcTokenEndpoint WRITE setOidcTokenEndpoint NOTIFY oidcTokenEndpointChanged)
    Q_PROPERTY(QString oidcClientId READ oidcClientId WRITE setOidcClientId NOTIFY oidcClientIdChanged)
    Q_PROPERTY(QString oidcScopes READ oidcScopes WRITE setOidcScopes NOTIFY oidcScopesChanged)
    // Voice page (adele-kde#30). The voice daemon (repo adelie-ai/voice) owns a
    // distinct bus name and exposes only Enable + voice selection over D-Bus;
    // STT/sensitivity/device settings live in its TOML config and autostart is
    // a systemd user unit, so those are handled file/process-side here.
    Q_PROPERTY(bool voiceServiceAvailable READ voiceServiceAvailable NOTIFY voiceChanged)
    Q_PROPERTY(bool voiceEnabled READ voiceEnabled WRITE setVoiceEnabled NOTIFY voiceChanged)
    Q_PROPERTY(QVariantList voiceList READ voiceList NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceCurrentId READ voiceCurrentId NOTIFY voiceChanged)
    Q_PROPERTY(int voiceCurrentSpeaker READ voiceCurrentSpeaker NOTIFY voiceChanged)
    // Tri-state autostart: -1 unknown/unit-not-installed, 0 disabled/masked, 1 enabled.
    Q_PROPERTY(int voiceAutostart READ voiceAutostart NOTIFY voiceChanged)
    Q_PROPERTY(QString sttLanguage READ sttLanguage WRITE setSttLanguage NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString sttModelPath READ sttModelPath WRITE setSttModelPath NOTIFY voiceConfigChanged)
    // Whisper STT model download state (adele-kde#44). The Voice tab's model
    // selector can fetch a missing catalog model into the models dir. Exposed so
    // QML can show progress, disable the controls in flight, and surface errors.
    //   * sttDownloadActive — a download is currently running.
    //   * sttDownloadProgress — 0..100 (-1 = indeterminate / not started).
    //   * sttDownloadError — last failure message ("" when none).
    //   * sttDownloadingFile — basename of the model being fetched ("" when idle).
    Q_PROPERTY(bool sttDownloadActive READ sttDownloadActive NOTIFY sttDownloadChanged)
    Q_PROPERTY(int sttDownloadProgress READ sttDownloadProgress NOTIFY sttDownloadChanged)
    Q_PROPERTY(QString sttDownloadError READ sttDownloadError NOTIFY sttDownloadChanged)
    Q_PROPERTY(QString sttDownloadingFile READ sttDownloadingFile NOTIFY sttDownloadChanged)
    Q_PROPERTY(double wakeSensitivity READ wakeSensitivity WRITE setWakeSensitivity NOTIFY voiceConfigChanged)
    // Wake-word calibration (#121): the daemon takes over the mic, has the user
    // say "Hey Adele" a few times, and sets the sensitivity from the measured
    // scores. `calibrationActive` gates the button; `calibrationStatus` carries
    // the live prompt and the final result for the UI to show.
    Q_PROPERTY(bool calibrationActive READ calibrationActive NOTIFY calibrationChanged)
    Q_PROPERTY(QString calibrationStatus READ calibrationStatus NOTIFY calibrationChanged)
    Q_PROPERTY(QString inputDevice READ inputDevice WRITE setInputDevice NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString outputDevice READ outputDevice WRITE setOutputDevice NOTIFY voiceConfigChanged)
    // Voice tuning knobs (adele-kde#37): endpointing + wake-word forward-compat.
    // All TOML-backed (config.toml), applied via applyVoiceChanges() (D-Bus
    // Reload when the daemon supports it, else a service restart).
    //   [vad] speech_threshold / silence_duration_ms, [assistant] followup_timeout_ms.
    Q_PROPERTY(double vadSpeechThreshold READ vadSpeechThreshold WRITE setVadSpeechThreshold NOTIFY voiceConfigChanged)
    Q_PROPERTY(int vadSilenceDurationMs READ vadSilenceDurationMs WRITE setVadSilenceDurationMs NOTIFY voiceConfigChanged)
    Q_PROPERTY(int followupTimeoutMs READ followupTimeoutMs WRITE setFollowupTimeoutMs NOTIFY voiceConfigChanged)
    // Forward-compat keys for voice#50/#51 (wake_word.eager + listening_cue).
    // They map to real config keys today, so writing them is safe even before
    // the daemon consumes them.
    Q_PROPERTY(bool wakeEager READ wakeEager WRITE setWakeEager NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString listeningCue READ listeningCue WRITE setListeningCue NOTIFY voiceConfigChanged)
    // Enumerated audio devices for the input/output selectors (adele-kde#37).
    // Each entry is a {value, label} map; the first is always "Follow system
    // default" (value "default"). Refreshed by loadAudioDevices().
    Q_PROPERTY(QVariantList inputDeviceOptions READ inputDeviceOptions NOTIFY audioDevicesChanged)
    Q_PROPERTY(QVariantList outputDeviceOptions READ outputDeviceOptions NOTIFY audioDevicesChanged)
    // Pluggable TTS backend (adele-kde#33): tts.backend in config.toml, plus the
    // per-backend keys the GUI exposes. Applied on the next service (re)start,
    // not hot over D-Bus — hence the Restart button (restartVoiceService()).
    Q_PROPERTY(QString ttsBackend READ ttsBackend WRITE setTtsBackend NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString kokoroLang READ kokoroLang WRITE setKokoroLang NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString piperModelPath READ piperModelPath WRITE setPiperModelPath NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString pollyEngine READ pollyEngine WRITE setPollyEngine NOTIFY voiceConfigChanged)
    Q_PROPERTY(QString pollyRegion READ pollyRegion WRITE setPollyRegion NOTIFY voiceConfigChanged)
    // Personality (adele-kde#42). Seven discrete 0..4 traits that set the
    // assistant's global disposition. They live ONLY on the daemon's aggregate
    // config (org.desktopAssistant.Settings GetConfig/SetConfig) as per-trait
    // u32 fields — there are no granular get/set_personality D-Bus methods — so
    // load() reads them from GetConfig and each setter writes them back via a
    // SetConfig patch (set_personality_<trait>=true). Hot-applied, no Apply
    // button. All share one NOTIFY (personalityChanged) so the page can resync
    // every slider from one handler after a reload.
    Q_PROPERTY(int personalityProfessionalism READ personalityProfessionalism WRITE setPersonalityProfessionalism NOTIFY personalityChanged)
    Q_PROPERTY(int personalityWarmth READ personalityWarmth WRITE setPersonalityWarmth NOTIFY personalityChanged)
    Q_PROPERTY(int personalityDirectness READ personalityDirectness WRITE setPersonalityDirectness NOTIFY personalityChanged)
    Q_PROPERTY(int personalityEnthusiasm READ personalityEnthusiasm WRITE setPersonalityEnthusiasm NOTIFY personalityChanged)
    Q_PROPERTY(int personalityHumor READ personalityHumor WRITE setPersonalityHumor NOTIFY personalityChanged)
    Q_PROPERTY(int personalitySarcasm READ personalitySarcasm WRITE setPersonalitySarcasm NOTIFY personalityChanged)
    Q_PROPERTY(int personalityPretentiousness READ personalityPretentiousness WRITE setPersonalityPretentiousness NOTIFY personalityChanged)

public:
    DesktopAssistantKcm(QObject *parent, const KPluginMetaData &metaData, const QVariantList &args);

    QString buildStamp() const;

    QString statusText() const;

    bool gitEnabled() const;
    void setGitEnabled(bool value);

    QString gitRemoteUrl() const;
    void setGitRemoteUrl(const QString &value);

    QString gitRemoteName() const;
    void setGitRemoteName(const QString &value);

    bool gitPushOnUpdate() const;
    void setGitPushOnUpdate(bool value);

    QString dbUrl() const;
    void setDbUrl(const QString &value);

    int dbMaxConnections() const;
    void setDbMaxConnections(int value);

    QStringList connectionNames() const;

    QString defaultConnectionName() const;
    void setDefaultConnectionName(const QString &value);

    QString selectedConnectionName() const;
    void setSelectedConnectionName(const QString &value);

    QString selectedConnectionTransport() const;

    QString selectedConnectionDbusService() const;
    void setSelectedConnectionDbusService(const QString &value);

    QString selectedConnectionWsUrl() const;
    void setSelectedConnectionWsUrl(const QString &value);

    QString selectedConnectionWsSubject() const;
    void setSelectedConnectionWsSubject(const QString &value);

    bool selectedConnectionRemovable() const;

    bool btDreamingEnabled() const;
    void setBtDreamingEnabled(bool value);

    int btDreamingIntervalSecs() const;
    void setBtDreamingIntervalSecs(int value);

    int btArchiveAfterDays() const;
    void setBtArchiveAfterDays(int value);

    bool wsAuthPasswordEnabled() const;
    void setWsAuthPasswordEnabled(bool value);
    bool wsAuthOidcEnabled() const;
    void setWsAuthOidcEnabled(bool value);

    QString oidcIssuer() const;
    void setOidcIssuer(const QString &value);
    QString oidcAuthEndpoint() const;
    void setOidcAuthEndpoint(const QString &value);
    QString oidcTokenEndpoint() const;
    void setOidcTokenEndpoint(const QString &value);
    QString oidcClientId() const;
    void setOidcClientId(const QString &value);
    QString oidcScopes() const;
    void setOidcScopes(const QString &value);

    bool voiceServiceAvailable() const;
    bool voiceEnabled() const;
    void setVoiceEnabled(bool value);
    QVariantList voiceList() const;
    QString voiceCurrentId() const;
    int voiceCurrentSpeaker() const;
    int voiceAutostart() const;
    QString sttLanguage() const;
    void setSttLanguage(const QString &value);
    QString sttModelPath() const;
    void setSttModelPath(const QString &value);
    bool sttDownloadActive() const;
    int sttDownloadProgress() const;
    QString sttDownloadError() const;
    QString sttDownloadingFile() const;
    double wakeSensitivity() const;
    void setWakeSensitivity(double value);
    QString inputDevice() const;
    void setInputDevice(const QString &value);
    QString outputDevice() const;
    void setOutputDevice(const QString &value);
    double vadSpeechThreshold() const;
    void setVadSpeechThreshold(double value);
    int vadSilenceDurationMs() const;
    void setVadSilenceDurationMs(int value);
    int followupTimeoutMs() const;
    void setFollowupTimeoutMs(int value);
    bool wakeEager() const;
    void setWakeEager(bool value);
    QString listeningCue() const;
    void setListeningCue(const QString &value);
    QVariantList inputDeviceOptions() const;
    QVariantList outputDeviceOptions() const;
    QString ttsBackend() const;
    void setTtsBackend(const QString &value);
    QString kokoroLang() const;
    void setKokoroLang(const QString &value);
    QString piperModelPath() const;
    void setPiperModelPath(const QString &value);
    QString pollyEngine() const;
    void setPollyEngine(const QString &value);
    QString pollyRegion() const;
    void setPollyRegion(const QString &value);

    int personalityProfessionalism() const;
    void setPersonalityProfessionalism(int value);
    int personalityWarmth() const;
    void setPersonalityWarmth(int value);
    int personalityDirectness() const;
    void setPersonalityDirectness(int value);
    int personalityEnthusiasm() const;
    void setPersonalityEnthusiasm(int value);
    int personalityHumor() const;
    void setPersonalityHumor(int value);
    int personalitySarcasm() const;
    void setPersonalitySarcasm(int value);
    int personalityPretentiousness() const;
    void setPersonalityPretentiousness(int value);

    /// Re-probe the voice service (D-Bus) and re-read its TOML config + the
    /// autostart unit state. Called when the Voice tab loads so the page
    /// reflects current reality without a full KCM reload.
    Q_INVOKABLE void loadVoiceSettings();
    /// Set the active TTS voice (and speaker; -1 = default/single-speaker)
    /// over D-Bus. Affects both spoken replies and SayText immediately.
    Q_INVOKABLE void setVoice(const QString &voiceId, int speaker);
    /// Enable/disable the adele-voice systemd user unit (autostart at login).
    /// `enabled` true -> `systemctl --user enable`; false -> `disable`.
    Q_INVOKABLE void setVoiceAutostart(bool enabled);
    /// Restart the adele-voice systemd user unit so config-file changes (TTS
    /// backend, per-backend keys, STT, devices, sensitivity) take effect
    /// without leaving the page. Re-reads live state afterwards.
    Q_INVOKABLE void restartVoiceService();

    /// Apply config-file changes live without a manual restart (adele-kde#37):
    /// try the daemon's `Reload` D-Bus method (voice#52) first, and on
    /// UnknownMethod / any failure fall back to restarting the systemd user
    /// unit. Forward-compatible — works whether or not voice#52 has landed.
    Q_INVOKABLE void applyVoiceChanges();

    // --- Whisper STT model selector (adele-kde#44) ---------------------------
    /// Absolute path of the directory the voice daemon resolves Whisper models
    /// from: $XDG_DATA_HOME/adele-voice/models (fallback
    /// ~/.local/share/adele-voice/models). QML composes the absolute model_path
    /// it writes back as sttModelsDir() + "/" + <catalog file>.
    Q_INVOKABLE QString sttModelsDir() const;
    /// True when a Whisper model is present on disk. `fileOrPath` may be a bare
    /// catalog filename (resolved against sttModelsDir()) or an absolute/relative
    /// path (checked as given). Empty -> false.
    Q_INVOKABLE bool sttModelInstalled(const QString &fileOrPath) const;
    /// Download a catalog Whisper model into the models dir. `fileName` is the
    /// catalog basename (also the on-disk name); `url` is its download URL.
    /// Fetches to a temp file and atomically renames on success; updates the
    /// sttDownload* properties (progress/active/error) as it runs. A no-op if a
    /// download is already in flight. On success emits voiceConfigChanged() so
    /// the page re-evaluates presence (the "not downloaded" warning clears).
    Q_INVOKABLE void downloadSttModel(const QString &fileName, const QString &url);
    /// Cancel an in-flight model download (aborts the transfer; the partial temp
    /// file is removed). No-op when idle.
    Q_INVOKABLE void cancelSttModelDownload();

    /// Re-enumerate input/output audio devices (PipeWire/Pulse via pactl,
    /// falling back to ALSA card names) into inputDeviceOptions/
    /// outputDeviceOptions. Always includes "Follow system default" first.
    Q_INVOKABLE void loadAudioDevices();

    /// Sample the configured (or system-default) input device briefly and report
    /// a 0..1 peak level, so the page can nudge the user when the mic is too
    /// quiet for reliable wake-word detection (ties into voice#47).
    ///
    /// KDE-2 / #57, PR 5/5: this used to BLOCK the UI thread ~0.4–3s on a
    /// `parecord` capture and RETURN the level. It is now NON-BLOCKING and void —
    /// it spawns the capture async and emits inputLevelMeasured(level) when done
    /// (level == -1 when no level could be measured: no tool / no device). The
    /// Voice page sets micLevel from that signal instead of the return value.
    /// While a measurement is in flight statusText shows "Measuring…".
    Q_INVOKABLE void measureInputLevel();

    /// Start wake-word calibration on the running daemon (#121). The daemon takes
    /// over the mic, prompts the user to say the wake word several times, sets the
    /// sensitivity a margin below the weakest score, applies it live, and persists
    /// it. Progress + result surface via `calibrationStatus` / `calibrationActive`
    /// (and the sensitivity slider updates to the calibrated value on success).
    /// A no-op if a calibration is already running or the daemon isn't available.
    Q_INVOKABLE void calibrateWake();
    bool calibrationActive() const;
    QString calibrationStatus() const;

    /// Reset the wake-word knobs (sensitivity, eager, listening cue) to the
    /// documented defaults and persist them.
    Q_INVOKABLE void resetWakeDefaults();
    /// Reset the endpointing knobs (VAD threshold + silence, follow-up timeout)
    /// to the documented defaults and persist them.
    Q_INVOKABLE void resetEndpointingDefaults();
    /// Reset the input/output device selectors to "Follow system default".
    Q_INVOKABLE void resetDeviceDefaults();
    /// Reset every tuning knob this page owns (wake + endpointing + devices) to
    /// the documented defaults in one shot.
    Q_INVOKABLE void resetVoiceTuningDefaults();

    Q_INVOKABLE void load() override;
    Q_INVOKABLE void save() override;
    Q_INVOKABLE void defaults() override;
    Q_INVOKABLE void addRemoteConnection(const QString &name);
    Q_INVOKABLE void removeSelectedConnection();

    /// Dispatch a daemon Command (snake-cased variant) and invoke
    /// `callback(result, error)` exactly once. `payload` is the variant's
    /// inner object; pass an empty object for unit variants such as
    /// `list_connections` or `get_purposes`.
    ///
    /// This is the QML-facing entry point for the multi-connection pages
    /// (Connections, Purposes). Dispatched via the daemon's
    /// `org.desktopAssistant.Connections` D-Bus interface — the name is
    /// transport-neutral so we can swap implementations without churning
    /// every QML caller.
    Q_INVOKABLE void daemonCall(const QString &command, const QJSValue &payload, const QJSValue &callback);

Q_SIGNALS:
    void statusTextChanged();
    /// Relays the daemon's `Knowledge.EntriesChanged` D-Bus signal so the QML
    /// Knowledge page refreshes its list when entries change on any client or a
    /// maintenance pass (dream cycle) rewrites them.
    void knowledgeEntriesChanged();
    void gitEnabledChanged();
    void gitRemoteUrlChanged();
    void gitRemoteNameChanged();
    void gitPushOnUpdateChanged();
    void dbUrlChanged();
    void dbMaxConnectionsChanged();
    void connectionNamesChanged();
    void defaultConnectionNameChanged();
    void selectedConnectionNameChanged();
    void selectedConnectionTransportChanged();
    void selectedConnectionDbusServiceChanged();
    void selectedConnectionWsUrlChanged();
    void selectedConnectionWsSubjectChanged();
    void selectedConnectionRemovableChanged();
    void btDreamingEnabledChanged();
    void btDreamingIntervalSecsChanged();
    void btArchiveAfterDaysChanged();
    void wsAuthMethodsChanged();
    void oidcIssuerChanged();
    void oidcAuthEndpointChanged();
    void oidcTokenEndpointChanged();
    void oidcClientIdChanged();
    void oidcScopesChanged();
    // Coarse "voice service / live D-Bus state changed" vs. "TOML config field
    // changed" — kept separate so config-field text bindings don't churn when
    // only the daemon's reported state (enabled / voice list) refreshes.
    void voiceChanged();
    void voiceConfigChanged();
    // Whisper STT model download state changed (adele-kde#44): active/progress/
    // error/file. Separate from voiceConfigChanged so progress ticks don't churn
    // the config text bindings.
    void sttDownloadChanged();
    // Enumerated audio device lists changed (loadAudioDevices()).
    void audioDevicesChanged();
    // Any of the seven personality traits changed (adele-kde#42). One shared
    // signal so the page resyncs every slider from a single handler.
    void personalityChanged();
    // Async input-level measurement finished (KDE-2 / #57, PR 5/5). `level` is a
    // 0..1 peak, or -1 when none could be measured. The Voice page binds its
    // micLevel from this (measureInputLevel() is now void/non-blocking).
    void inputLevelMeasured(double level);
    // Wake-word calibration state changed (#121): active flag and/or status text.
    void calibrationChanged();

private Q_SLOTS:
    // Relays the daemon's `org.desktopAssistant.Voice.CalibrationProgress` D-Bus
    // signal into `calibrationStatus` while a calibration is in flight (#121).
    void onCalibrationProgress(uint captured, uint total, double score);

private:
    struct ConnectionProfile {
        QString name;
        QString transport;
        QString dbusService;
        QString wsUrl;
        QString wsSubject;
    };

    static QString dbusErrorMessage(const QDBusMessage &message);
    bool setStatusFromDbusError(const QDBusMessage &message);

    // --- Async D-Bus plumbing (KDE-2 / #57) ----------------------------------
    // Issue an async method call on a daemon interface and invoke `handler`
    // (on the UI thread) with the finished reply. The call is built with
    // QDBusMessage::createMethodCall + QDBusConnection::asyncCall (NEVER
    // QDBusInterface, whose constructor blocks on introspection), so nothing
    // ever stalls the System Settings UI thread. `timeoutMs` bounds the wait;
    // the watcher is parented to `this` and self-deletes on finish. `service`,
    // `path`, and `iface` default to the orchestrator Settings interface.
    void asyncSettingsCall(const QString &method,
                           const QVariantList &args,
                           int timeoutMs,
                           std::function<void(const QDBusMessage &)> handler,
                           const char *service = nullptr,
                           const char *path = nullptr,
                           const char *iface = nullptr);

    int connectionIndexByName(const QString &name) const;
    int selectedConnectionIndex() const;
    void loadWidgetConnectionSettings();
    bool saveWidgetConnectionSettings();
    void setSelectedConnectionByIndex(int index);
    void emitConnectionSelectionChanged();
    // Per-section immediate-save helpers. Each setter that previously
    // toggled setNeedsSave(true) now calls one of these directly so
    // changes hit the daemon as soon as the user makes them; there is no
    // Apply gesture (the daemon hot-reloads inside each Set* handler).
    //
    // Shared fire-and-forget setter push (KDE-2 / #57, PR 3/5): issues `method`
    // with `args` on the orchestrator Settings interface via asyncSettingsCall
    // and surfaces only an error reply into statusText — nothing blocks the UI
    // thread. Used by the per-section pushers below.
    void pushSetterAsync(const QString &method, const QVariantList &args);
    void pushPersistenceSettings();
    void pushDatabaseSettings();
    void pushBackendTasksSettings();
    void pushWsAuthSettings();
    // --- Personality (adele-kde#42) ------------------------------------------
    // Shared setter body for the seven traits: clamp to 0..4, store into `slot`,
    // and on a real change emit personalityChanged + push just that trait via a
    // SetConfig patch (set_personality_<trait>=true). `setField` is the
    // ConfigPatchArgs `set_personality_*` boolean to flip true for this trait.
    void setPersonalityTrait(int *slot, int value, const char *setField);
    // Push a single personality trait to the daemon via SetConfig with a
    // ConfigPatchArgs whose `set_personality_<setField after "set_">`=true and
    // matching value, all other set_* false. Mirrors the GetConfig/SetConfig
    // struct contract (desktop-assistant#226).
    void pushPersonalityTrait(const char *setField, int value);

    // --- Voice (adele-kde#30) -------------------------------------------------
    // The voice daemon exposes Enable + voice selection over D-Bus only; the
    // remaining knobs are file/process-backed:
    //   * STT language / wake sensitivity / input+output device live in the
    //     daemon's TOML config (~/.config/adele-voice/config.toml). We do a
    //     surgical, section-aware merge so we never clobber model paths or
    //     other fields the user set by hand. These take effect on the next
    //     daemon (re)start, which the page surfaces in its help text.
    //   * "Allow autostart" toggles the `adele-voice` systemd *user* unit.
    bool probeVoiceAvailable() const;
    void readVoiceConfig();
    bool writeVoiceConfig();
    // KDE-7 (#62): coalesce rapid config writes. A slider drag fires its setter
    // many times per second; without debouncing each tick did a full
    // read-merge-rewrite of config.toml (a write storm, and — before the atomic
    // QSaveFile — a torn-file risk). Instead the property setters call
    // scheduleVoiceConfigWrite(), which (re)arms a short single-shot timer; the
    // actual writeVoiceConfig() runs once the value settles. Paths that must
    // persist *now* before they act on the file (reset buttons, Apply/restart,
    // SetVoice) call flushVoiceConfigWrite() to cancel the pending timer and
    // write synchronously, so no debounced change is ever lost or applied late.
    void scheduleVoiceConfigWrite();
    bool flushVoiceConfigWrite();
    // Run `systemctl --user <args>` NON-BLOCKING (KDE-2 / #57, PR 5/5 — was a
    // synchronous QProcess that blocked the UI thread up to ~8s). Spawns a
    // QProcess parented to `this` and invokes `done(trimmedStdout, ok)` on the UI
    // thread from its finished/errorOccurred signal exactly once (ok == exit==0).
    // The process self-deletes on finish; if the KCM is destroyed first the
    // parent-child teardown cancels the callback.
    void runSystemctlUserAsync(const QStringList &args,
                               std::function<void(const QString &out, bool ok)> done);
    // Asynchronously re-probe the autostart unit state (`is-enabled`) and, when
    // it lands, update m_voiceAutostart (-1 unknown / 0 off / 1 on) + emit
    // voiceChanged. Replaces the old blocking int probeVoiceAutostart() (KDE-2 /
    // #57, PR 5/5).
    void probeVoiceAutostartAsync();
    // Apply config-file changes live. Tries the daemon's `Reload` D-Bus method
    // (voice#52) asynchronously (KDE-2 / #57, PR 4/5 — was a blocking
    // QDBusInterface::call) and invokes `done(true)` on success, `done(false)`
    // on UnknownMethod / unavailable / error so the caller can fall back to a
    // service restart. When the voice service isn't on the bus it calls
    // `done(false)` synchronously without issuing a call (so we never D-Bus
    // *activate* the daemon from a probe).
    void tryDaemonReload(std::function<void(bool)> done);
    // Enumerate audio devices for `direction` ("input"/"output"). Each entry is
    // a {value,label} map. Prefers `pactl`; falls back to ALSA card tokens from
    // `arecord -L` / `aplay -L`. Never includes the "default" sentinel — the
    // caller prepends "Follow system default".
    QVariantList enumerateAudioDevices(const QString &direction) const;

    // Ask the voice daemon (`adele-voice list-devices`) for the input devices it
    // can actually open — a probed, curated JSON list (adelie-ai/voice#74). Each
    // entry is a {value,label} map for devices reported `supported`; unsupported
    // ones (e.g. a sound server that isn't running) are dropped so the picker
    // never offers something capture would reject. Never includes the "default"
    // sentinel. On `*ok == false` the daemon was unavailable and the caller
    // should fall back to enumerateAudioDevices("input").
    QVariantList enumerateVoiceInputDevices(bool *ok) const;

    // --- Whisper STT model download (adele-kde#44) ---------------------------
    // Reset the sttDownload* state to idle and emit sttDownloadChanged.
    void resetSttDownloadState();
    // Tear down the in-flight download: abort+delete the reply, close+remove the
    // temp file. `keepError` preserves m_sttDownloadError (set by the caller).
    void cleanupSttDownload(bool keepError);

    static QString voiceConfigPath();

    // Monotonic generation counter bumped on every load(). Each async load
    // handler captures the value current when it was issued and ignores its
    // reply if a newer load() has since started, so a slow/stale reply from a
    // previous load can never clobber fresher state (KDE-2 / #57).
    quint64 m_loadGeneration = 0;

    // Monotonic generation counter bumped on every loadVoiceSettings() (KDE-2 /
    // #57, PR 4/5). The voice reads (GetEnabled/ListVoices/GetVoice) are now
    // async, and loadVoiceSettings() is re-fired frequently (each load(), the
    // service watcher, restart/apply). Each async voice handler captures the
    // value current when it was issued and drops its reply if a newer
    // loadVoiceSettings() has since started — so a stale reply (e.g. from a
    // daemon that just went away) can't clobber the fresh "unavailable" state.
    quint64 m_voiceLoadGeneration = 0;

    QString m_statusText;
    bool m_gitEnabled = false;
    QString m_gitRemoteUrl;
    QString m_gitRemoteName = QStringLiteral("origin");
    bool m_gitPushOnUpdate = true;
    QString m_dbUrl;
    int m_dbMaxConnections = 5;
    QVector<ConnectionProfile> m_connections;
    QString m_defaultConnectionName = QStringLiteral("local");
    QString m_selectedConnectionName = QStringLiteral("local");
    bool m_btDreamingEnabled = false;
    int m_btDreamingIntervalSecs = 3600;
    int m_btArchiveAfterDays = 0;
    // Pass-through backend-task LLM fields: loaded from
    // GetBackendTasksSettings and echoed back by pushBackendTasksSettings();
    // no UI binds them (the Purposes page owns model selection now).
    QString m_btLlmConnector;
    QString m_btLlmModel;
    QString m_btLlmBaseUrl;
    QStringList m_wsAuthMethods = {QStringLiteral("password")};
    QString m_oidcIssuer;
    QString m_oidcAuthEndpoint;
    QString m_oidcTokenEndpoint;
    QString m_oidcClientId;
    QString m_oidcScopes = QStringLiteral("openid profile email");

    // Voice (adele-kde#30) runtime + config state.
    // Watches org.desktopAssistant.Voice on the session bus so the page
    // re-probes when the daemon (re)appears — e.g. after "Restart voice
    // service" / "Apply now", where systemctl returns before the daemon has
    // re-acquired its bus name. Without this the voice picker latches disabled.
    QDBusServiceWatcher *m_voiceWatcher = nullptr;
    // Debounce timer for voice config writes (KDE-7 / #62). Lazily created on
    // first scheduleVoiceConfigWrite(); single-shot, re-armed on each call so a
    // burst of setter calls (a slider drag) collapses to one write.
    QTimer *m_voiceWriteDebounce = nullptr;
    bool m_voiceServiceAvailable = false;
    bool m_voiceEnabled = false;
    QVariantList m_voiceList;
    QString m_voiceCurrentId;
    int m_voiceCurrentSpeaker = -1;
    int m_voiceAutostart = -1;
    // Re-entrancy guard for the async measureInputLevel() (KDE-2 / #57, PR 5/5):
    // true while a `parecord` capture is in flight, so repeated "Test" clicks
    // don't spawn overlapping captures. Cleared when the measurement completes.
    bool m_inputLevelMeasuring = false;
    QString m_sttLanguage = QStringLiteral("en");
    QString m_sttModelPath;
    // Whisper model download (adele-kde#44). The QNAM is created lazily on first
    // download and reused; reply + temp file live only for the duration of a
    // single transfer.
    QNetworkAccessManager *m_sttNam = nullptr;
    QNetworkReply *m_sttReply = nullptr;
    QFile *m_sttTempFile = nullptr;
    QString m_sttDownloadTempPath;
    QString m_sttDownloadDestPath;
    QString m_sttDownloadingFile;
    QString m_sttDownloadError;
    int m_sttDownloadProgress = -1;
    bool m_sttDownloadActive = false;
    double m_wakeSensitivity = 0.5;
    // Wake-word calibration (#121): in-flight flag + the status/prompt text shown
    // in the Voice page while calibrating and after it finishes.
    bool m_calibrationActive = false;
    QString m_calibrationStatus;
    QString m_inputDevice = QStringLiteral("default");
    QString m_outputDevice = QStringLiteral("default");
    // Tuning knobs (adele-kde#37). Initial values mirror the daemon's
    // VadConfig / AssistantConfig defaults (repo adelie-ai/voice) — what the
    // daemon uses when the key is absent from config.toml. The Reset buttons
    // restore the *documented recommended* values (see the kVoiceDefault*
    // constants in the .cpp), which can differ from these absent-key fallbacks.
    double m_vadSpeechThreshold = 0.5;
    int m_vadSilenceDurationMs = 800;
    int m_followupTimeoutMs = 8000;
    bool m_wakeEager = false;
    QString m_listeningCue;
    // Enumerated device option lists ({value,label} maps), incl. the leading
    // "Follow system default" entry. Populated by loadAudioDevices().
    QVariantList m_inputDeviceOptions;
    QVariantList m_outputDeviceOptions;
    // TTS backend selection (adele-kde#33). Defaults mirror the daemon's
    // TtsConfig::default() (repo adelie-ai/voice, crates/daemon/src/config.rs):
    // Kokoro is the local default backend; polly_region is optional (omitted
    // from the file when empty, since the daemon types it as Option<String>).
    QString m_ttsBackend = QStringLiteral("kokoro");
    QString m_kokoroLang = QStringLiteral("en-us");
    QString m_piperModelPath;
    QString m_pollyEngine = QStringLiteral("neural");
    QString m_pollyRegion;
    // Persisted TTS voice selection per backend. SetVoice over D-Bus only
    // changes the running daemon; these are written to config.toml so the choice
    // survives a restart (otherwise the daemon reloads its default — "af_heart"
    // for Kokoro). Defaults mirror the daemon's TtsConfig::default().
    QString m_kokoroVoice = QStringLiteral("af_heart");
    QString m_pollyVoice = QStringLiteral("Joanna");

    // Personality traits (adele-kde#42), 0..4 each. Defaults match the daemon's
    // built-in disposition and are what we fall back to when GetConfig fails
    // (daemon down): Professionalism=4, Warmth=3, Directness=3, Enthusiasm=2,
    // Humor=2, Sarcasm=1, Pretentiousness=1.
    int m_personalityProfessionalism = 4;
    int m_personalityWarmth = 3;
    int m_personalityDirectness = 3;
    int m_personalityEnthusiasm = 2;
    int m_personalityHumor = 2;
    int m_personalitySarcasm = 1;
    int m_personalityPretentiousness = 1;
    // KDE-8 (#63): true only once a GetConfig reply has passed the ConfigData
    // signature validation, so the personality block's positional indices are
    // known to line up with the daemon's struct. pushPersonalityTrait refuses to
    // send its hand-encoded ConfigPatchArgs while this is false, since a schema
    // mismatch means the positional patch would write the wrong fields. Starts
    // false (no trustworthy read yet) and is set by load()'s validated read.
    bool m_personalitySchemaOk = false;
};
