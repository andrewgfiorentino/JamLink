// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <string>

namespace jamlink::desktop {
namespace {

constexpr std::uint64_t fnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnvPrime = 1'099'511'628'211ULL;

std::uint64_t appendHash(std::uint64_t hash, const QByteArray& bytes) noexcept {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnvPrime;
    }
    return hash;
}

} // namespace

AppController::AppController(
    std::filesystem::path preferencePath,
    bool visualFixture,
    QString initialPage,
    std::uint32_t widthOverride,
    std::uint32_t heightOverride,
    QObject* parent)
    : QObject(parent),
      store_(std::move(preferencePath)),
      currentPage_(std::move(initialPage)),
      visualFixture_(visualFixture) {
    const bool automaticPage = currentPage_ == QStringLiteral("auto");
    if (!automaticPage && currentPage_ != QStringLiteral("home")
        && currentPage_ != QStringLiteral("soundcheck")
        && currentPage_ != QStringLiteral("settings")) {
        currentPage_ = QStringLiteral("home");
    }

    if (visualFixture_) {
        instrumentOptions_ = {
            {QStringLiteral("fixture:interface"),
             QStringLiteral("Focusrite Scarlett 2i2 — Input 1"),
             QStringLiteral("input:1"), {}},
            {QStringLiteral("fixture:interface:alt"),
             QStringLiteral("Focusrite Scarlett 2i2 — Input 2"),
             QStringLiteral("input:2"), {}}};
        voiceOptions_ = {
            {QStringLiteral("fixture:voice"), QStringLiteral("Shure SM7B"),
             QStringLiteral("capture:1"), {}}};
        outputOptions_ = {
            {QStringLiteral("fixture:interface"),
             QStringLiteral("Focusrite Scarlett 2i2 — Output 1–2"),
             QStringLiteral("output:1"), QStringLiteral("output:2")}};
        devicesAvailable_ = true;
    } else {
        instrumentOptions_ = {{QString(), QStringLiteral("No audio backend available"), {}, {}}};
        voiceOptions_ = {{QString(), QStringLiteral("No audio backend available"), {}, {}}};
        outputOptions_ = {{QString(), QStringLiteral("No audio backend available"), {}, {}}};
    }

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(350);
    connect(&saveTimer_, &QTimer::timeout, this, &AppController::persistNow);
    loadPreferences(widthOverride, heightOverride);
    if (automaticPage) {
        currentPage_ = restoredSetupAvailable_
            ? QStringLiteral("home")
            : QStringLiteral("soundcheck");
    }
}

QString AppController::currentPage() const { return currentPage_; }

void AppController::setCurrentPage(const QString& page) {
    if (page != QStringLiteral("home") && page != QStringLiteral("soundcheck")
        && page != QStringLiteral("settings")) {
        return;
    }
    if (page != currentPage_) {
        currentPage_ = page;
        emit currentPageChanged();
    }
}

bool AppController::visualFixture() const noexcept { return visualFixture_; }
bool AppController::restoredPreferences() const noexcept { return restoredPreferences_; }
bool AppController::devicesAvailable() const noexcept { return devicesAvailable_; }
bool AppController::allReady() const noexcept { return readiness_.allVerified(); }
QString AppController::readinessLabel() const {
    if (!devicesAvailable_) {
        return QStringLiteral("Offline");
    }
    return allReady() ? QStringLiteral("Ready") : QStringLiteral("Check again");
}
QString AppController::setupMessage() const { return setupMessage_; }
QString AppController::saveMessage() const { return saveMessage_; }

QStringList AppController::instrumentDevices() const { return displayNames(instrumentOptions_); }
QStringList AppController::voiceDevices() const { return displayNames(voiceOptions_); }
QStringList AppController::outputDevices() const { return displayNames(outputOptions_); }
int AppController::instrumentDeviceIndex() const noexcept { return instrumentIndex_; }
int AppController::voiceDeviceIndex() const noexcept { return voiceIndex_; }
int AppController::outputDeviceIndex() const noexcept { return outputIndex_; }

void AppController::setInstrumentDeviceIndex(int index) {
    if (!validIndex(index, instrumentOptions_.size()) || index == instrumentIndex_) {
        return;
    }
    instrumentIndex_ = index;
    invalidateReadiness();
}

void AppController::setVoiceDeviceIndex(int index) {
    if (!validIndex(index, voiceOptions_.size()) || index == voiceIndex_) {
        return;
    }
    voiceIndex_ = index;
    invalidateReadiness();
}

void AppController::setOutputDeviceIndex(int index) {
    if (!validIndex(index, outputOptions_.size()) || index == outputIndex_) {
        return;
    }
    outputIndex_ = index;
    invalidateReadiness();
}

QStringList AppController::sampleRates() const {
    QStringList result;
    result.reserve(static_cast<qsizetype>(sampleRateValues_.size()));
    for (const auto value : sampleRateValues_) {
        result.push_back(QString::number(value / 1'000.0, 'g', 3) + QStringLiteral(" kHz"));
    }
    return result;
}

QStringList AppController::bufferSizes() const {
    QStringList result;
    result.reserve(static_cast<qsizetype>(bufferSizeValues_.size()));
    for (const auto value : bufferSizeValues_) {
        result.push_back(QString::number(value) + QStringLiteral(" samples"));
    }
    return result;
}

int AppController::sampleRateIndex() const noexcept { return sampleRateIndex_; }
int AppController::bufferSizeIndex() const noexcept { return bufferSizeIndex_; }

void AppController::setSampleRateIndex(int index) {
    if (!validIndex(index, sampleRateValues_.size()) || index == sampleRateIndex_) {
        return;
    }
    sampleRateIndex_ = index;
    invalidateReadiness();
}

void AppController::setBufferSizeIndex(int index) {
    if (!validIndex(index, bufferSizeValues_.size()) || index == bufferSizeIndex_) {
        return;
    }
    bufferSizeIndex_ = index;
    invalidateReadiness();
}

double AppController::instrumentMonitorGain() const noexcept {
    return preferences_.instrumentMonitorGain;
}
double AppController::voiceMonitorGain() const noexcept {
    return preferences_.voiceMonitorGain;
}
bool AppController::instrumentMonitorEnabled() const noexcept {
    return preferences_.instrumentMonitorEnabled;
}
bool AppController::voiceMonitorEnabled() const noexcept {
    return preferences_.voiceMonitorEnabled;
}

void AppController::setInstrumentMonitorGain(double gain) {
    const auto bounded = static_cast<float>(std::clamp(gain, 0.0, 1.0));
    if (preferences_.instrumentMonitorGain == bounded) {
        return;
    }
    preferences_.instrumentMonitorGain = bounded;
    scheduleSave();
    emit setupChanged();
}

void AppController::setVoiceMonitorGain(double gain) {
    const auto bounded = static_cast<float>(std::clamp(gain, 0.0, 1.0));
    if (preferences_.voiceMonitorGain == bounded) {
        return;
    }
    preferences_.voiceMonitorGain = bounded;
    scheduleSave();
    emit setupChanged();
}

void AppController::setInstrumentMonitorEnabled(bool enabled) {
    if (preferences_.instrumentMonitorEnabled == enabled) {
        return;
    }
    preferences_.instrumentMonitorEnabled = enabled;
    scheduleSave();
    emit setupChanged();
}

void AppController::setVoiceMonitorEnabled(bool enabled) {
    if (preferences_.voiceMonitorEnabled == enabled) {
        return;
    }
    preferences_.voiceMonitorEnabled = enabled;
    scheduleSave();
    emit setupChanged();
}

double AppController::instrumentLevel() const noexcept {
    return visualFixture_ ? 0.78 : 0.0;
}
double AppController::voiceLevel() const noexcept { return visualFixture_ ? 0.55 : 0.0; }
double AppController::outputLevel() const noexcept { return visualFixture_ ? 0.50 : 0.0; }

int AppController::preferredWindowX() const noexcept { return preferences_.window.x; }
int AppController::preferredWindowY() const noexcept { return preferences_.window.y; }
int AppController::preferredWindowWidth() const noexcept {
    return static_cast<int>(preferences_.window.width);
}
int AppController::preferredWindowHeight() const noexcept {
    return static_cast<int>(preferences_.window.height);
}
bool AppController::hasPreferredWindowPosition() const noexcept {
    return preferences_.window.hasPosition;
}

void AppController::navigate(const QString& page) { setCurrentPage(page); }

void AppController::saveSoundcheck() {
    if (devicesAvailable_) {
        updateReadinessConfiguration();
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Instrument,
            fingerprint(instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Voice,
            fingerprint(voiceOptions_[static_cast<std::size_t>(voiceIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Output,
            fingerprint(outputOptions_[static_cast<std::size_t>(outputIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
        setupMessage_ = QStringLiteral("Private setup saved for this run");
    } else {
        setupMessage_ = QStringLiteral("Audio backends are not available in this build");
    }
    persistNow();
    emit setupChanged();
}

void AppController::updateWindowPlacement(int x, int y, int width, int height) {
    if (width < 532 || height < 480) {
        return;
    }
    preferences_.window.x = x;
    preferences_.window.y = y;
    preferences_.window.width = static_cast<std::uint32_t>(width);
    preferences_.window.height = static_cast<std::uint32_t>(height);
    preferences_.window.hasPosition = true;
    scheduleSave();
}

void AppController::persistNow() {
    applySelectionsToPreferences();
    const auto result = store_.save(preferences_);
    const QString message = result.succeeded
        ? QStringLiteral("Saved")
        : QStringLiteral("Settings could not be saved safely");
    if (message != saveMessage_) {
        saveMessage_ = message;
        emit saveMessageChanged();
    }
}

QStringList AppController::displayNames(const std::vector<DeviceOption>& options) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(options.size()));
    for (const auto& option : options) {
        result.push_back(option.displayName);
    }
    return result;
}

int AppController::resolveDevice(
    const std::vector<DeviceOption>& options,
    const std::string& stableId) {
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (options[index].stableId.toStdString() == stableId) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

bool AppController::selectionAvailable(
    const std::vector<DeviceOption>& options,
    const jamlink::preferences::AudioSelection& selection) {
    return std::any_of(options.begin(), options.end(), [&selection](const auto& option) {
        return option.stableId.toStdString() == selection.deviceId
            && option.primaryChannelId.toStdString() == selection.primaryChannelId
            && option.secondaryChannelId.toStdString() == selection.secondaryChannelId;
    });
}

int AppController::resolveScalar(
    const std::vector<std::uint32_t>& values,
    std::uint32_t value) noexcept {
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), found));
}

std::uint64_t AppController::fingerprint(
    const DeviceOption& option,
    std::uint32_t sampleRate,
    std::uint32_t bufferFrames) {
    std::uint64_t value = fnvOffset;
    value = appendHash(value, option.stableId.toUtf8());
    value = appendHash(value, option.primaryChannelId.toUtf8());
    value = appendHash(value, option.secondaryChannelId.toUtf8());
    value = appendHash(value, QByteArray::number(sampleRate));
    return appendHash(value, QByteArray::number(bufferFrames));
}

bool AppController::validIndex(int index, std::size_t size) noexcept {
    return index >= 0 && static_cast<std::size_t>(index) < size;
}

void AppController::loadPreferences(
    std::uint32_t widthOverride,
    std::uint32_t heightOverride) {
    const auto loaded = store_.load();
    restoredPreferences_ =
        loaded.state == jamlink::preferences::PreferencesLoadState::Loaded;
    preferences_ = loaded.preferences;
    if (widthOverride >= 532U) {
        preferences_.window.width = widthOverride;
    }
    if (heightOverride >= 480U) {
        preferences_.window.height = heightOverride;
    }

    instrumentIndex_ = resolveDevice(instrumentOptions_, preferences_.instrument.deviceId);
    voiceIndex_ = resolveDevice(voiceOptions_, preferences_.voice.deviceId);
    outputIndex_ = resolveDevice(outputOptions_, preferences_.output.deviceId);
    sampleRateIndex_ = resolveScalar(sampleRateValues_, preferences_.sampleRate);
    bufferSizeIndex_ = resolveScalar(bufferSizeValues_, preferences_.bufferFrames);
    restoredSetupAvailable_ = restoredPreferences_ && devicesAvailable_
        && selectionAvailable(instrumentOptions_, preferences_.instrument)
        && selectionAvailable(voiceOptions_, preferences_.voice)
        && selectionAvailable(outputOptions_, preferences_.output)
        && std::find(sampleRateValues_.begin(), sampleRateValues_.end(),
                     preferences_.sampleRate) != sampleRateValues_.end()
        && std::find(bufferSizeValues_.begin(), bufferSizeValues_.end(),
                     preferences_.bufferFrames) != bufferSizeValues_.end();

    if (loaded.state == jamlink::preferences::PreferencesLoadState::RecoveredDefaults) {
        setupMessage_ = QStringLiteral("Recovered safe defaults; verify devices again");
    } else if (devicesAvailable_) {
        setupMessage_ = QStringLiteral("Only you can hear this private preview");
    } else {
        setupMessage_ = QStringLiteral("Audio backends are not available in this build");
    }
    updateReadinessConfiguration();
    if (visualFixture_) {
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Instrument,
            fingerprint(instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Voice,
            fingerprint(voiceOptions_[static_cast<std::size_t>(voiceIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
        static_cast<void>(readiness_.markVerified(
            jamlink::control::SetupComponent::Output,
            fingerprint(outputOptions_[static_cast<std::size_t>(outputIndex_)],
                        sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)])));
    }
}

void AppController::applySelectionsToPreferences() {
    const auto assign = [](jamlink::preferences::AudioSelection& selection,
                           const DeviceOption& option) {
        selection.deviceId = option.stableId.toStdString();
        selection.primaryChannelId = option.primaryChannelId.toStdString();
        selection.secondaryChannelId = option.secondaryChannelId.toStdString();
    };
    assign(preferences_.instrument,
           instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)]);
    assign(preferences_.voice, voiceOptions_[static_cast<std::size_t>(voiceIndex_)]);
    assign(preferences_.output, outputOptions_[static_cast<std::size_t>(outputIndex_)]);
    preferences_.sampleRate = sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)];
    preferences_.bufferFrames = bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)];
}

void AppController::updateReadinessConfiguration() {
    readiness_.setConfiguration(
        jamlink::control::SetupComponent::Instrument,
        fingerprint(instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)],
                    sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                    bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)]));
    readiness_.setConfiguration(
        jamlink::control::SetupComponent::Voice,
        fingerprint(voiceOptions_[static_cast<std::size_t>(voiceIndex_)],
                    sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                    bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)]));
    readiness_.setConfiguration(
        jamlink::control::SetupComponent::Output,
        fingerprint(outputOptions_[static_cast<std::size_t>(outputIndex_)],
                    sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)],
                    bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)]));
}

void AppController::invalidateReadiness() {
    updateReadinessConfiguration();
    setupMessage_ = QStringLiteral("Selection changed; verify this setup again");
    scheduleSave();
    emit setupChanged();
}

void AppController::scheduleSave() { saveTimer_.start(); }

} // namespace jamlink::desktop
