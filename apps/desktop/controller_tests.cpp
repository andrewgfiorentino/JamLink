// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QCoreApplication>

#include <cmath>
#include <filesystem>
#include <iostream>

namespace {

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
        productionPath, false, QStringLiteral("auto"), 0U, 0U);
    productionFirst.persistNow();
    jamlink::desktop::AppController productionSecond(
        productionPath, false, QStringLiteral("auto"), 0U, 0U);
    passed = expect(productionSecond.restoredPreferences(),
                    "production preferences restore syntactically")
        && passed;
    passed = expect(productionSecond.currentPage() == QStringLiteral("soundcheck"),
                    "unavailable production backend never skips Sound Check")
        && passed;

    std::filesystem::remove_all(directory, cleanupError);
    std::cout << (passed ? "[PASS] desktop controller persistence and readiness\n"
                         : "[FAIL] desktop controller persistence and readiness\n");
    return passed ? 0 : 1;
}
