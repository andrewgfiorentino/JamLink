// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QCoreApplication>
#include <QColor>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {

class EmptyAudioService final : public jamlink::audio::ISoundcheckAudioService {
public:
    [[nodiscard]] jamlink::audio::SoundcheckDeviceInventory enumerate() override { return {}; }
    [[nodiscard]] bool start(
        const jamlink::audio::SoundcheckAudioConfiguration&) override { return false; }
    void stop() noexcept override {}
    void setMonitorControls(float, bool, float, bool) noexcept override {}
    void requestOutputTest() noexcept override {}
    void setTunerEnabled(bool) noexcept override {}
    [[nodiscard]] jamlink::audio::TunerReading tunerReading() override { return {}; }
    [[nodiscard]] bool startRecording(
        const std::filesystem::path&, const std::string&) override { return false; }
    void stopRecording() noexcept override {}
    [[nodiscard]] jamlink::record::RecorderTelemetry recorderTelemetry()
        const noexcept override { return {}; }
    void setPeerAudioExchange(jamlink::network::IPeerAudioExchange*) noexcept override {}
    [[nodiscard]] jamlink::audio::SoundcheckAudioTelemetry telemetry() const noexcept override {
        return {jamlink::audio::SoundcheckAudioState::NoEndpoints};
    }
};

class DeterministicAudioService final : public jamlink::audio::ISoundcheckAudioService {
public:
    [[nodiscard]] jamlink::audio::SoundcheckDeviceInventory enumerate() override {
        const jamlink::audio::SoundcheckEndpointOption asioInput{
            "asio:test", "Test ASIO · Input 2", 1U, 0U, false,
            48'000U, {64U, 128U, 256U}, jamlink::audio::SoundcheckBackend::Asio,
            "Test ASIO"};
        const jamlink::audio::SoundcheckEndpointOption voiceA{
            "test:voice-a", "USB Microphone A · Input 1", 0U, 0U, false,
            48'000U, {128U, 256U}};
        const jamlink::audio::SoundcheckEndpointOption voiceB{
            "test:voice-b", "USB Microphone B · Input 1", 0U, 0U, false,
            44'100U, {128U, 256U}};
        const jamlink::audio::SoundcheckEndpointOption output{
            "asio:test", "Test ASIO · Outputs 1–2", 0U, 1U, true,
            48'000U, {64U, 128U, 256U}, jamlink::audio::SoundcheckBackend::Asio,
            "Test ASIO"};
        return {{asioInput, voiceA, voiceB}, {output}};
    }
    [[nodiscard]] bool start(
        const jamlink::audio::SoundcheckAudioConfiguration& configuration) override {
        lastConfiguration = configuration;
        ++startCount;
        current = {
            jamlink::audio::SoundcheckAudioState::Running,
            0.25F, 0.5F, 0.375F, 48'000U, configuration.requestedBufferFrames,
            0U, 0U, false};
        current.instrumentInput = {0.25F, 0.15F, 0.4F};
        current.voiceInput = {0.5F, 0.25F, 0.6F};
        current.instrumentSend = {0.25F, 0.15F, 0.4F};
        current.voiceSend = {0.5F, 0.25F, 0.6F};
        current.monitorMix = {0.375F, 0.2F, 0.55F};
        if (injectInstrumentClipOnStart) {
            current.instrumentInput.clipped = true;
            current.instrumentInput.clipSamples = 3U;
            current.instrumentInput.clipEvents = 1U;
            current.instrumentInput.peakHold = 1.0F;
        }
        return true;
    }
    void stop() noexcept override {
        current.state = jamlink::audio::SoundcheckAudioState::Stopped;
    }
    void setMonitorControls(
        float instrumentGain,
        bool instrumentEnabled,
        float voiceGain,
        bool voiceEnabled) noexcept override {
        lastInstrumentGain = instrumentGain;
        lastVoiceGain = voiceGain;
        lastInstrumentEnabled = instrumentEnabled;
        lastVoiceEnabled = voiceEnabled;
    }
    void requestOutputTest() noexcept override { ++outputTestCount; }
    void setTunerEnabled(bool enabled) noexcept override { tunerEnabled = enabled; }
    [[nodiscard]] jamlink::audio::TunerReading tunerReading() override { return tuner; }
    [[nodiscard]] bool startRecording(
        const std::filesystem::path& directory, const std::string& name) override {
        lastRecordingDirectory = directory;
        lastRecordingName = name;
        ++recordingStartCount;
        recorder.recording = true;
        return true;
    }
    void stopRecording() noexcept override { recorder.recording = false; }
    [[nodiscard]] jamlink::record::RecorderTelemetry recorderTelemetry()
        const noexcept override { return recorder; }
    void setPeerAudioExchange(jamlink::network::IPeerAudioExchange*) noexcept override {}
    [[nodiscard]] jamlink::audio::VoiceEndpointChangeResult tryReplaceVoiceEndpoint(
        const jamlink::audio::SoundcheckEndpointOption& option) override {
        lastReplacement = option;
        ++replacementCount;
        return replacementResult;
    }
    void clearSignalHealth(jamlink::audio::SignalHealthPath path) noexcept override {
        ++clearHealthCount;
        switch (path) {
        case jamlink::audio::SignalHealthPath::InstrumentInput:
            current.instrumentInput = {};
            break;
        case jamlink::audio::SignalHealthPath::VoiceInput:
            current.voiceInput = {};
            break;
        case jamlink::audio::SignalHealthPath::InstrumentSend:
            current.instrumentSend = {};
            break;
        case jamlink::audio::SignalHealthPath::VoiceSend:
            current.voiceSend = {};
            break;
        case jamlink::audio::SignalHealthPath::MonitorMix:
            current.monitorMix = {};
            break;
        case jamlink::audio::SignalHealthPath::RecordingInstrument:
            current.recordingInstrument = {};
            break;
        case jamlink::audio::SignalHealthPath::RecordingVoice:
            current.recordingVoice = {};
            break;
        }
    }
    void requestSignalHealthSelfTest(
        jamlink::audio::SignalHealthPath path) noexcept override {
        ++selfTestHealthCount;
        jamlink::audio::SignalHealthTelemetry* health = nullptr;
        switch (path) {
        case jamlink::audio::SignalHealthPath::InstrumentInput:
            health = &current.instrumentInput;
            break;
        case jamlink::audio::SignalHealthPath::VoiceInput:
            health = &current.voiceInput;
            break;
        case jamlink::audio::SignalHealthPath::InstrumentSend:
            health = &current.instrumentSend;
            break;
        case jamlink::audio::SignalHealthPath::VoiceSend:
            health = &current.voiceSend;
            break;
        case jamlink::audio::SignalHealthPath::MonitorMix:
            health = &current.monitorMix;
            break;
        case jamlink::audio::SignalHealthPath::RecordingInstrument:
            health = &current.recordingInstrument;
            break;
        case jamlink::audio::SignalHealthPath::RecordingVoice:
            health = &current.recordingVoice;
            break;
        }
        if (health != nullptr) {
            health->clipped = true;
            health->diagnosticClip = true;
        }
    }
    [[nodiscard]] jamlink::audio::SoundcheckAudioTelemetry telemetry() const noexcept override {
        return current;
    }

    jamlink::audio::SoundcheckAudioConfiguration lastConfiguration;
    jamlink::audio::SoundcheckAudioTelemetry current;
    jamlink::audio::TunerReading tuner;
    jamlink::record::RecorderTelemetry recorder;
    std::filesystem::path lastRecordingDirectory;
    std::string lastRecordingName;
    std::size_t recordingStartCount{0U};
    bool tunerEnabled{false};
    std::size_t startCount{0U};
    std::size_t outputTestCount{0U};
    float lastInstrumentGain{0.0F};
    float lastVoiceGain{0.0F};
    bool lastInstrumentEnabled{false};
    bool lastVoiceEnabled{false};
    jamlink::audio::SoundcheckEndpointOption lastReplacement;
    jamlink::audio::VoiceEndpointChangeResult replacementResult{
        jamlink::audio::VoiceEndpointChangeResult::Applied};
    std::size_t replacementCount{0U};
    std::size_t clearHealthCount{0U};
    std::size_t selfTestHealthCount{0U};
    bool injectInstrumentClipOnStart{false};
};

bool near(double left, double right) {
    return std::abs(left - right) <= 1.0e-6;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto directory = std::filesystem::temp_directory_path()
        / "jamlink_desktop_controller_tests";
    const auto path = directory / "preferences.jlpf";
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);

    jamlink::desktop::AppController first(path, true, QStringLiteral("auto"), 0U, 0U);
    bool passed = true;
    const QRegularExpression randomInvitePattern(
        QStringLiteral("^[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{4}-"
                       "[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{4}$"));
    for (int index = 0; index < 32; ++index) {
        passed = expect(randomInvitePattern.match(first.generatePrivateInviteCode()).hasMatch(),
            "generated temporary invite codes are short and unambiguous") && passed;
    }
    passed = expect(!first.restoredPreferences(), "first launch starts without restored state")
        && passed;
    passed = expect(first.currentPage() == QStringLiteral("soundcheck"),
                    "first launch routes to private Sound Check")
        && passed;
    passed = expect(first.allReady(), "visual fixture begins verified") && passed;
    passed = expect(first.readinessLabel() == QStringLiteral("Ready"),
                    "verified fixture reports Ready")
        && passed;
    first.setInstrumentDeviceIndex(1);
    passed = expect(!first.allReady(), "device change invalidates readiness") && passed;
    passed = expect(first.readinessLabel() == QStringLiteral("Check again"),
                    "invalidated setup never reports Ready")
        && passed;
    first.setSampleRateIndex(2);
    first.setBufferSizeIndex(0);
    first.setInstrumentMonitorGain(0.44);
    first.setVoiceMonitorGain(0.31);
    first.setVoiceMonitorEnabled(false);
    // How loud a friend sits in the mix, and whether tuning mutes the room, are
    // settings a user should only ever choose once.
    first.setRemoteInstrumentGain(0.62);
    first.setRemoteVoiceGain(0.88);
    first.setTunerMutesInstrument(false);
    const QString generatedProfileId = first.profileId();
    passed = expect(!generatedProfileId.isEmpty(), "first launch generates a stable profile ID")
        && passed;
    first.setProfileDisplayName(QStringLiteral("Andrew"));
    first.setProfileHandle(QStringLiteral("@Andrew.F"));
    first.setProfilePrimaryInstrument(QStringLiteral("Guitar"));
    first.setProfileGenres(QStringLiteral("Rock, Alternative"));
    first.setProfileBio(QStringLiteral("Guitarist in Delaware"));
    first.setProfileAvatarId(QStringLiteral("avatar:guitar-acoustic"));
    first.saveSoundcheck();
    passed = expect(first.allReady(), "explicit Sound Check save verifies current selections")
        && passed;
    passed = expect(first.currentPage() == QStringLiteral("home"),
                    "successful Sound Check save returns to Home")
        && passed;
    first.updateWindowPlacement(120, 80, 532, 534);
    const auto avatarInput = directory / "avatar-input.png";
    QImage sourceAvatar(640, 480, QImage::Format_RGB32);
    sourceAvatar.fill(QColor(QStringLiteral("#6a35bd")));
    passed = expect(sourceAvatar.save(QString::fromStdWString(avatarInput.wstring()), "PNG"),
                    "avatar input fixture is written")
        && passed;
    passed = expect(first.setCustomAvatar(
                        QUrl::fromLocalFile(QString::fromStdWString(avatarInput.wstring()))),
                    "custom avatar is decoded and sanitized")
        && passed;
    QImageReader sanitized(first.profileCustomAvatarSource().toLocalFile());
    passed = expect(sanitized.size() == QSize(256, 256),
                    "custom avatar is cropped to a bounded square thumbnail")
        && passed;
    first.persistNow();

    jamlink::desktop::AppController second(path, true, QStringLiteral("auto"), 0U, 0U);
    passed = expect(second.restoredPreferences(), "second launch restores persisted state")
        && passed;
    passed = expect(second.currentPage() == QStringLiteral("home"),
                    "returning launch routes to Home")
        && passed;
    passed = expect(second.instrumentDeviceIndex() == 1, "stable device id resolves on restore")
        && passed;
    passed = expect(second.sampleRateIndex() == 2, "sample rate restores") && passed;
    passed = expect(second.bufferSizeIndex() == 0, "buffer size restores") && passed;
    passed = expect(near(second.instrumentMonitorGain(), 0.44), "instrument gain restores")
        && passed;
    passed = expect(near(second.voiceMonitorGain(), 0.31), "voice gain restores") && passed;
    passed = expect(!second.voiceMonitorEnabled(), "monitor mute restores") && passed;
    passed = expect(near(second.remoteInstrumentGain(), 0.62), "remote instrument level restores")
        && passed;
    passed = expect(near(second.remoteVoiceGain(), 0.88), "remote voice level restores")
        && passed;
    passed = expect(!second.tunerMutesInstrument(), "tuner mute preference restores") && passed;
    passed = expect(second.profileId() == generatedProfileId,
                    "stable profile ID survives restart")
        && passed;
    passed = expect(second.profileDisplayName() == QStringLiteral("Andrew")
                        && second.profileHandle() == QStringLiteral("andrew.f"),
                    "display name and normalized handle restore")
        && passed;
    passed = expect(second.profilePrimaryInstrument() == QStringLiteral("Guitar")
                        && second.profileGenres() == QStringLiteral("Rock, Alternative")
                        && second.profileBio() == QStringLiteral("Guitarist in Delaware"),
                    "musician profile details restore")
        && passed;
    passed = expect(second.profileAvatarId() == QStringLiteral("avatar:custom")
                        && second.profileCustomAvatarSource().isLocalFile(),
                    "sanitized custom avatar restores")
        && passed;
    passed = expect(second.hasPreferredWindowPosition(), "window placement flag restores")
        && passed;
    passed = expect(second.preferredWindowX() == 120 && second.preferredWindowY() == 80,
                    "window position restores")
        && passed;
    passed = expect(second.preferredWindowWidth() == 532
                        && second.preferredWindowHeight() == 534,
                    "reference-size window geometry restores")
        && passed;

    // Entering and leaving the tuner drives the instrument tap and the tuner
    // mute together, and leaving must always release the mute so a user cannot
    // navigate away and stay silently muted to the room.
    auto tunerService = std::make_unique<DeterministicAudioService>();
    auto* tunerServiceView = tunerService.get();
    // Its own preferences file, so this block does not inherit the tuner mute
    // the persistence checks above deliberately turned off.
    jamlink::desktop::AppController tuner(
        directory / "tuner.jlpf", false, QStringLiteral("home"), 0U, 0U, nullptr,
        std::move(tunerService));
    passed = expect(!tuner.tunerActive(), "tuner starts inactive") && passed;
    passed = expect(!tunerServiceView->tunerEnabled, "instrument tap starts off") && passed;
    passed = expect(tuner.tunerMutesInstrument(),
                    "tuner mutes the instrument to the room by default")
        && passed;

    tuner.navigate(QStringLiteral("tuner"));
    passed = expect(tuner.tunerActive(), "opening the tuner activates it") && passed;
    passed = expect(tunerServiceView->tunerEnabled, "opening the tuner taps the instrument")
        && passed;

    tunerServiceView->tuner = {true, 110.0, 45, -4.5, 0.4F, 0.95F};
    passed = expect(!tuner.tunerDetected(), "readings only arrive from the audio poll")
        && passed;

    tuner.closeTuner();
    passed = expect(!tuner.tunerActive(), "leaving the tuner deactivates it") && passed;
    passed = expect(!tunerServiceView->tunerEnabled, "leaving the tuner releases the tap")
        && passed;
    passed = expect(!tuner.tunerDetected(), "leaving the tuner clears the last reading")
        && passed;

    tuner.navigate(QStringLiteral("room"));
    tuner.navigate(QStringLiteral("tuner"));
    tuner.closeTuner();
    passed = expect(tuner.currentPage() == QStringLiteral("room"),
                    "closing an in-room tuner returns to the live room")
        && passed;

    // One button starts and stops a take, and it stays disabled until audio is
    // actually running so a press can never produce an empty folder.
    auto recordService = std::make_unique<DeterministicAudioService>();
    auto* recordServiceView = recordService.get();
    jamlink::desktop::AppController recorder(
        directory / "record.jlpf", false, QStringLiteral("home"), 0U, 0U, nullptr,
        std::move(recordService));
    passed = expect(!recorder.recording(), "recording starts off") && passed;
    recorder.toggleRecording();
    passed = expect(!recorder.recording(),
                    "record button does nothing before audio is running")
        && passed;
    passed = expect(recordServiceView->recordingStartCount == 0U,
                    "a take is never opened without a running audio device")
        && passed;

    // Let the deferred audio start run, which is what the real app does a beat
    // after launch.
    QCoreApplication::processEvents();
    passed = expect(recorder.audioActive(), "audio is running before the record test")
        && passed;

    recorder.toggleRecording();
    passed = expect(recorder.recording(), "record button starts a take") && passed;
    passed = expect(recordServiceView->recordingStartCount == 1U,
                    "record button reaches the audio service")
        && passed;
    passed = expect(!recordServiceView->lastRecordingName.empty(),
                    "each take gets its own named folder")
        && passed;
    passed = expect(recorder.recordingLocation().length() > 0,
                    "the user is told where the take went")
        && passed;
    recorder.toggleRecording();
    passed = expect(!recorder.recording(), "record button stops the take") && passed;

    auto stalePreferences = jamlink::preferences::PreferencesStore(path).load().preferences;
    stalePreferences.instrument.deviceId = "fixture:missing";
    passed = expect(jamlink::preferences::PreferencesStore(path).save(stalePreferences).succeeded,
                    "stale-ID fixture is written")
        && passed;
    jamlink::desktop::AppController stale(path, true, QStringLiteral("auto"), 0U, 0U);
    passed = expect(stale.restoredPreferences(), "stale preference file remains syntactically valid")
        && passed;
    passed = expect(stale.currentPage() == QStringLiteral("soundcheck"),
                    "missing stable device routes back to Sound Check")
        && passed;

    const auto productionPath = directory / "production-preferences.jlpf";
    jamlink::desktop::AppController productionFirst(
        productionPath, false, QStringLiteral("auto"), 0U, 0U, nullptr,
        std::make_unique<EmptyAudioService>());
    productionFirst.persistNow();
    jamlink::desktop::AppController productionSecond(
        productionPath, false, QStringLiteral("auto"), 0U, 0U, nullptr,
        std::make_unique<EmptyAudioService>());
    passed = expect(productionSecond.restoredPreferences(),
                    "production preferences restore syntactically")
        && passed;
    passed = expect(productionSecond.currentPage() == QStringLiteral("soundcheck"),
                    "unavailable production backend never skips Sound Check")
        && passed;

    const auto missingHardwarePath = directory / "missing-hardware-preferences.jlpf";
    jamlink::preferences::UserPreferences missingHardwarePreferences;
    missingHardwarePreferences.instrument = {"missing:asio", "input:1", ""};
    missingHardwarePreferences.voice = {"missing:usb-mic", "input:0", ""};
    missingHardwarePreferences.output = {"missing:asio", "output:0", "output:1"};
    passed = expect(jamlink::preferences::PreferencesStore(missingHardwarePath)
                        .save(missingHardwarePreferences).succeeded,
                    "missing-hardware production fixture is written")
        && passed;
    auto missingHardwareService = std::make_unique<DeterministicAudioService>();
    auto* missingHardwareServiceView = missingHardwareService.get();
    jamlink::desktop::AppController missingHardware(
        missingHardwarePath, false, QStringLiteral("auto"), 0U, 0U, nullptr,
        std::move(missingHardwareService));
    QCoreApplication::processEvents();
    passed = expect(missingHardware.currentPage() == QStringLiteral("soundcheck")
                        && missingHardwareServiceView->startCount == 0U,
                    "missing restored devices never silently fall back or auto-start")
        && passed;

    const auto activePath = directory / "active-audio-preferences.jlpf";
    auto deterministicService = std::make_unique<DeterministicAudioService>();
    auto* deterministicServiceView = deterministicService.get();
    jamlink::desktop::AppController active(
        activePath, false, QStringLiteral("auto"), 0U, 0U, nullptr,
        std::move(deterministicService));
    passed = expect(active.devicesAvailable(), "production inventory exposes real selections")
        && passed;
    active.retryAudio();
    passed = expect(active.audioActive(), "successful service start activates private monitor")
        && passed;
    passed = expect(deterministicServiceView->startCount == 1U,
                    "retry starts exactly one monitor session")
        && passed;
    passed = expect(near(active.instrumentLevel(), 0.25)
                        && near(active.voiceLevel(), 0.5)
                        && near(active.outputLevel(), 0.375),
                    "controller publishes service telemetry")
        && passed;
    active.setInstrumentMonitorGain(0.33);
    active.setVoiceMonitorEnabled(false);
    passed = expect(near(deterministicServiceView->lastInstrumentGain, 0.33)
                        && !deterministicServiceView->lastVoiceEnabled,
                    "monitor controls reach the service without restart")
        && passed;
    active.testOutput();
    passed = expect(deterministicServiceView->outputTestCount == 1U,
                    "output test reaches only the active local service")
        && passed;
    active.saveSoundcheck();
    passed = expect(active.allReady(), "active hybrid setup can be verified") && passed;
    passed = expect(active.currentPage() == QStringLiteral("home"),
                    "active hybrid setup advances after verification") && passed;
    passed = expect(active.instrumentDeviceIndex() == 0
                        && active.voiceDeviceIndex() == 1
                        && active.outputDeviceIndex() == 0,
                    "fresh setup favors an ASIO master with a separate WASAPI microphone")
        && passed;
    active.setVoiceDeviceIndex(2);
    passed = expect(deterministicServiceView->replacementCount == 1U
                        && deterministicServiceView->startCount == 1U,
                    "WASAPI microphone replacement does not restart the ASIO master")
        && passed;
    passed = expect(deterministicServiceView->lastReplacement.endpointId == "test:voice-b"
                        && active.audioActive() && !active.allReady(),
                    "microphone replacement invalidates readiness without stopping audio")
        && passed;
    deterministicServiceView->replacementResult =
        jamlink::audio::VoiceEndpointChangeResult::Failed;
    active.setVoiceDeviceIndex(1);
    passed = expect(deterministicServiceView->replacementCount == 2U
                        && deterministicServiceView->startCount == 1U
                        && active.audioActive(),
                    "failed replacement leaves the ASIO instrument/output stream active")
        && passed;

    auto clippedService = std::make_unique<DeterministicAudioService>();
    auto* clippedServiceView = clippedService.get();
    clippedServiceView->injectInstrumentClipOnStart = true;
    jamlink::desktop::AppController clipped(
        directory / "clipped-audio-preferences.jlpf", false,
        QStringLiteral("soundcheck"), 0U, 0U, nullptr, std::move(clippedService));
    clipped.retryAudio();
    passed = expect(clipped.instrumentInputClipped()
                        && clipped.readinessLabel() == QStringLiteral("Clipping"),
                    "input clip latch is visible and blocks Ready to Jam")
        && passed;
    clipped.saveSoundcheck();
    passed = expect(!clipped.allReady()
                        && clipped.currentPage() == QStringLiteral("soundcheck"),
                    "a clipped setup cannot verify or leave Sound Check") && passed;
    clipped.clearInstrumentClipping();
    passed = expect(!clipped.instrumentInputClipped()
                        && clippedServiceView->clearHealthCount == 2U,
                    "source reset clears both input and send latches")
        && passed;
    clipped.testInstrumentClipping();
    passed = expect(clipped.instrumentInputClipped()
                        && clipped.instrumentSignalStatus() == QStringLiteral("TEST CLIP")
                        && clipped.instrumentSignalGuidance().contains(
                            QStringLiteral("INDICATOR TEST"))
                        && clippedServiceView->selfTestHealthCount == 1U,
                    "silent self-test exercises the visible input clip latch")
        && passed;
    clipped.clearInstrumentClipping();
    passed = expect(!clipped.instrumentInputClipped()
                        && clippedServiceView->clearHealthCount == 4U,
                    "self-test clip resets through the same source controls")
        && passed;
    clipped.saveSoundcheck();
    passed = expect(clipped.allReady()
                        && clipped.currentPage() == QStringLiteral("home"),
                    "clean reset setup verifies and advances") && passed;

    qputenv("JAMLINK_VISUAL_ROOM_SIZE", QByteArrayLiteral("4"));
    jamlink::desktop::AppController fourMusicianRoom(
        directory / "four-musician-room.jlpf", true,
        QStringLiteral("room"), 0U, 0U);
    const QVariantList participants = fourMusicianRoom.roomParticipants();
    passed = expect(fourMusicianRoom.roomParticipantCount() == 4
                        && participants.size() == 4,
                    "room presentation expands from two to four musicians") && passed;
    passed = expect(participants.at(0).toMap().value(QStringLiteral("local")).toBool()
                        && participants.at(2).toMap()
                               .value(QStringLiteral("displayName")).toString()
                            == QStringLiteral("Chris"),
                    "scaled room keeps stable local-first participant ordering") && passed;
    qunsetenv("JAMLINK_VISUAL_ROOM_SIZE");

    qputenv("JAMLINK_VISUAL_PRIVATE_ROOM", QByteArrayLiteral("host-drawer"));
    jamlink::desktop::AppController preflightFixture(
        directory / "preflight.jlpf", true,
        QStringLiteral("room"), 532U, 728U);
    passed = expect(preflightFixture.connectionPreflightStatus() == QStringLiteral("Ready")
                        && preflightFixture.connectionPreflightReady(),
                    "host fixture exposes the musician-facing Ready preflight result")
        && passed;
    passed = expect(preflightFixture.connectionPreflightDetail().contains(
                        QStringLiteral("not a measured Internet connection"))
                        && preflightFixture.connectionPreflightDetail().contains(
                            QStringLiteral("verified during the encrypted join")),
                    "preflight copy separates inferred reachability from join-time identity")
        && passed;
    preflightFixture.leaveSession();
    passed = expect(!preflightFixture.connectionPreflightReady()
                        && preflightFixture.connectionPreflightStatus()
                            == QStringLiteral("Connection check not run"),
                    "leaving clears the preflight result instead of preserving stale Ready")
        && passed;
    qunsetenv("JAMLINK_VISUAL_PRIVATE_ROOM");

    std::filesystem::remove_all(directory, cleanupError);
    std::cout << (passed ? "[PASS] desktop controller persistence and readiness\n"
                         : "[FAIL] desktop controller persistence and readiness\n");
    return passed ? 0 : 1;
}
