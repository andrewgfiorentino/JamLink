// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QCoreApplication>

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
    void setPeerAudioExchange(jamlink::network::IPeerAudioExchange*) noexcept override {}
    [[nodiscard]] jamlink::audio::SoundcheckAudioTelemetry telemetry() const noexcept override {
        return {jamlink::audio::SoundcheckAudioState::NoEndpoints};
    }
};

class DeterministicAudioService final : public jamlink::audio::ISoundcheckAudioService {
public:
    [[nodiscard]] jamlink::audio::SoundcheckDeviceInventory enumerate() override {
        const jamlink::audio::SoundcheckEndpointOption input{
            "test:capture", "Test Capture — Input 1", 0U, 0U, false,
            48'000U, {128U, 256U}};
        const jamlink::audio::SoundcheckEndpointOption output{
            "test:render", "Test Render — Output 1–2", 0U, 1U, true,
            48'000U, {128U, 256U}};
        return {{input}, {output}};
    }
    [[nodiscard]] bool start(
        const jamlink::audio::SoundcheckAudioConfiguration& configuration) override {
        lastConfiguration = configuration;
        ++startCount;
        current = {
            jamlink::audio::SoundcheckAudioState::Running,
            0.25F, 0.5F, 0.375F, 48'000U, configuration.requestedBufferFrames,
            0U, 0U, false};
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
    void setPeerAudioExchange(jamlink::network::IPeerAudioExchange*) noexcept override {}
    [[nodiscard]] jamlink::audio::SoundcheckAudioTelemetry telemetry() const noexcept override {
        return current;
    }

    jamlink::audio::SoundcheckAudioConfiguration lastConfiguration;
    jamlink::audio::SoundcheckAudioTelemetry current;
    jamlink::audio::TunerReading tuner;
    bool tunerEnabled{false};
    std::size_t startCount{0U};
    std::size_t outputTestCount{0U};
    float lastInstrumentGain{0.0F};
    float lastVoiceGain{0.0F};
    bool lastInstrumentEnabled{false};
    bool lastVoiceEnabled{false};
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
    bool passed = expect(!first.restoredPreferences(), "first launch starts without restored state");
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
    first.saveSoundcheck();
    passed = expect(first.allReady(), "explicit Sound Check save verifies current selections")
        && passed;
    first.updateWindowPlacement(120, 80, 532, 534);
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
    passed = expect(second.hasPreferredWindowPosition(), "window placement flag restores")
        && passed;
    passed = expect(second.preferredWindowX() == 120 && second.preferredWindowY() == 80,
                    "window position restores")
        && passed;
    passed = expect(second.preferredWindowWidth() == 532
                        && second.preferredWindowHeight() == 534,
                    "reference-size window geometry restores")
        && passed;

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

    std::filesystem::remove_all(directory, cleanupError);
    std::cout << (passed ? "[PASS] desktop controller persistence and readiness\n"
                         : "[FAIL] desktop controller persistence and readiness\n");
    return passed ? 0 : 1;
}
