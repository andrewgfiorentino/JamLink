// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>

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
    QObject* parent,
    std::unique_ptr<jamlink::audio::ISoundcheckAudioService> audioService)
    : QObject(parent),
      store_(std::move(preferencePath)),
      currentPage_(std::move(initialPage)),
      visualFixture_(visualFixture),
      audioService_(std::move(audioService)) {
    const bool automaticPage = currentPage_ == QStringLiteral("auto");
    if (!automaticPage && currentPage_ != QStringLiteral("home")
        && currentPage_ != QStringLiteral("soundcheck")
        && currentPage_ != QStringLiteral("settings")
        && currentPage_ != QStringLiteral("room")) {
        currentPage_ = QStringLiteral("home");
    }

    if (visualFixture_) {
        sampleRateValues_ = {44'100U, 48'000U, 96'000U};
        bufferSizeValues_ = {64U, 128U, 256U};
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
        if (!audioService_) {
            audioService_ = jamlink::audio::createPlatformSoundcheckAudioService();
        }
        loadDeviceInventory();
    }

    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(350);
    connect(&saveTimer_, &QTimer::timeout, this, &AppController::persistNow);
    audioRestartTimer_.setSingleShot(true);
    audioRestartTimer_.setInterval(180);
    connect(&audioRestartTimer_, &QTimer::timeout, this, &AppController::retryAudio);
    telemetryTimer_.setInterval(33);
    connect(&telemetryTimer_, &QTimer::timeout, this, &AppController::pollAudioTelemetry);
    loadPreferences(widthOverride, heightOverride);
    if (automaticPage) {
        currentPage_ = restoredSetupAvailable_
            ? QStringLiteral("home")
            : QStringLiteral("soundcheck");
    }
    if (!visualFixture_ && devicesAvailable_) {
        audioRestartTimer_.start(0);
    }
}

AppController::~AppController() {
    if (audioService_) {
        audioService_->stop();
        audioService_->setPeerAudioExchange(nullptr);
    }
    if (peerTransport_) {
        peerTransport_->stop();
    }
}

QString AppController::currentPage() const { return currentPage_; }

void AppController::setCurrentPage(const QString& page) {
    if (page != QStringLiteral("home") && page != QStringLiteral("soundcheck")
        && page != QStringLiteral("settings") && page != QStringLiteral("room")) {
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
bool AppController::audioActive() const noexcept {
    return visualFixture_
        || audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::Running;
}
QString AppController::audioStatus() const {
    if (visualFixture_) {
        return QStringLiteral("Deterministic visual fixture");
    }
    if (audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::Running) {
        return QStringLiteral("WASAPI Shared · %1 kHz · %2 frames")
            .arg(audioTelemetry_.outputSampleRate / 1'000.0, 0, 'g', 3)
            .arg(audioTelemetry_.outputBufferFrames);
    }
    const QString state = audioStateText(audioTelemetry_.state);
    return audioTelemetry_.nativeError == 0
        ? state
        : state + QStringLiteral(" · error 0x%1").arg(
            static_cast<std::uint32_t>(audioTelemetry_.nativeError),
            8, 16, QChar('0'));
}
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
    scheduleAudioRestart();
}

void AppController::setVoiceDeviceIndex(int index) {
    if (!validIndex(index, voiceOptions_.size()) || index == voiceIndex_) {
        return;
    }
    voiceIndex_ = index;
    invalidateReadiness();
    scheduleAudioRestart();
}

void AppController::setOutputDeviceIndex(int index) {
    if (!validIndex(index, outputOptions_.size()) || index == outputIndex_) {
        return;
    }
    outputIndex_ = index;
    updateOutputCapabilities();
    invalidateReadiness();
    scheduleAudioRestart();
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
    scheduleAudioRestart();
}

void AppController::setBufferSizeIndex(int index) {
    if (!validIndex(index, bufferSizeValues_.size()) || index == bufferSizeIndex_) {
        return;
    }
    bufferSizeIndex_ = index;
    invalidateReadiness();
    scheduleAudioRestart();
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
    if (audioService_) {
        audioService_->setMonitorControls(
            preferences_.instrumentMonitorGain,
            preferences_.instrumentMonitorEnabled,
            preferences_.voiceMonitorGain,
            preferences_.voiceMonitorEnabled);
    }
    scheduleSave();
    emit setupChanged();
}

void AppController::setVoiceMonitorGain(double gain) {
    const auto bounded = static_cast<float>(std::clamp(gain, 0.0, 1.0));
    if (preferences_.voiceMonitorGain == bounded) {
        return;
    }
    preferences_.voiceMonitorGain = bounded;
    if (audioService_) {
        audioService_->setMonitorControls(
            preferences_.instrumentMonitorGain,
            preferences_.instrumentMonitorEnabled,
            preferences_.voiceMonitorGain,
            preferences_.voiceMonitorEnabled);
    }
    scheduleSave();
    emit setupChanged();
}

void AppController::setInstrumentMonitorEnabled(bool enabled) {
    if (preferences_.instrumentMonitorEnabled == enabled) {
        return;
    }
    preferences_.instrumentMonitorEnabled = enabled;
    if (audioService_) {
        audioService_->setMonitorControls(
            preferences_.instrumentMonitorGain,
            preferences_.instrumentMonitorEnabled,
            preferences_.voiceMonitorGain,
            preferences_.voiceMonitorEnabled);
    }
    scheduleSave();
    emit setupChanged();
}

void AppController::setVoiceMonitorEnabled(bool enabled) {
    if (preferences_.voiceMonitorEnabled == enabled) {
        return;
    }
    preferences_.voiceMonitorEnabled = enabled;
    if (enabled && !visualFixture_) {
        setupMessage_ = QStringLiteral(
            "Voice monitoring is live; use headphones to prevent speaker feedback");
    }
    if (audioService_) {
        audioService_->setMonitorControls(
            preferences_.instrumentMonitorGain,
            preferences_.instrumentMonitorEnabled,
            preferences_.voiceMonitorGain,
            preferences_.voiceMonitorEnabled);
    }
    scheduleSave();
    emit setupChanged();
}

double AppController::instrumentLevel() const noexcept {
    return visualFixture_ ? 0.78 : audioTelemetry_.instrumentPeak;
}
double AppController::voiceLevel() const noexcept {
    return visualFixture_ ? 0.55 : audioTelemetry_.voicePeak;
}
double AppController::outputLevel() const noexcept {
    return visualFixture_ ? 0.50 : audioTelemetry_.outputPeak;
}

bool AppController::roomActive() const noexcept { return peerTransport_ != nullptr; }
bool AppController::peerConnected() const noexcept {
    return peerTelemetry_.state == jamlink::network::PeerConnectionState::Connected;
}
QString AppController::roomStatus() const {
    if (!peerTransport_) {
        return QStringLiteral("No room session");
    }
    if (peerConnected()) {
        return QStringLiteral("Connected · encrypted direct audio · %1")
            .arg(connectionQuality());
    }
    if (peerTelemetry_.state == jamlink::network::PeerConnectionState::WaitingForPeer
        && !peerTelemetry_.automaticPortMapping) {
        return QStringLiteral(
            "Waiting for your friend · automatic router mapping unavailable");
    }
    return peerStateText(peerTelemetry_.state);
}
QString AppController::inviteCode() const { return inviteCode_; }
bool AppController::automaticPortMapping() const noexcept {
    return peerTelemetry_.automaticPortMapping;
}
int AppController::roomPort() const noexcept {
    return peerTransport_ ? static_cast<int>(peerTransport_->localPort()) : 0;
}
int AppController::roundTripMilliseconds() const noexcept {
    return static_cast<int>((peerTelemetry_.roundTripMicroseconds + 500U) / 1'000U);
}
double AppController::remoteLevel() const noexcept {
    return std::max(
        peerTelemetry_.streams[instrumentStream].peak,
        peerTelemetry_.streams[voiceStream].peak);
}
double AppController::remoteInstrumentLevel() const noexcept {
    return peerTelemetry_.streams[instrumentStream].peak;
}
double AppController::remoteVoiceLevel() const noexcept {
    return peerTelemetry_.streams[voiceStream].peak;
}

double AppController::remoteInstrumentGain() const noexcept { return remoteInstrumentGain_; }
double AppController::remoteVoiceGain() const noexcept { return remoteVoiceGain_; }
bool AppController::remoteInstrumentMuted() const noexcept { return remoteInstrumentMuted_; }
bool AppController::remoteVoiceMuted() const noexcept { return remoteVoiceMuted_; }

void AppController::setRemoteInstrumentGain(double gain) {
    applyRemoteStream(
        jamlink::network::AudioStreamId::Instrument, remoteInstrumentGain_, gain);
}
void AppController::setRemoteVoiceGain(double gain) {
    applyRemoteStream(jamlink::network::AudioStreamId::Voice, remoteVoiceGain_, gain);
}
void AppController::setRemoteInstrumentMuted(bool muted) {
    if (remoteInstrumentMuted_ == muted) {
        return;
    }
    remoteInstrumentMuted_ = muted;
    if (peerTransport_) {
        peerTransport_->setRemoteStreamMuted(
            jamlink::network::AudioStreamId::Instrument, muted);
    }
    emit roomChanged();
}
void AppController::setRemoteVoiceMuted(bool muted) {
    if (remoteVoiceMuted_ == muted) {
        return;
    }
    remoteVoiceMuted_ = muted;
    if (peerTransport_) {
        peerTransport_->setRemoteStreamMuted(jamlink::network::AudioStreamId::Voice, muted);
    }
    emit roomChanged();
}

void AppController::applyRemoteStream(
    jamlink::network::AudioStreamId stream,
    double& stored,
    double gain) {
    const double bounded = std::clamp(gain, 0.0, 1.0);
    if (std::abs(stored - bounded) < 0.0005) {
        return;
    }
    stored = bounded;
    if (peerTransport_) {
        peerTransport_->setRemoteStreamGain(stream, static_cast<float>(bounded));
    }
    emit roomChanged();
}

// Buffering latency is measured; one-way delay is estimated from half the
// round trip. The two are reported separately so an estimate is never shown as
// a measurement.
QString AppController::connectionQuality() const {
    if (!peerConnected()) {
        return QStringLiteral("Not connected");
    }
    const auto& instrument = peerTelemetry_.streams[instrumentStream];
    const double oneWayMilliseconds =
        static_cast<double>(peerTelemetry_.roundTripMicroseconds) / 2'000.0;
    const double bufferMilliseconds = instrument.bufferedFrames == 0U
        ? 0.0
        : static_cast<double>(instrument.bufferedFrames) / 48.0;
    const double playableMilliseconds = oneWayMilliseconds + bufferMilliseconds;
    const double concealRatio = peerTelemetry_.packetsReceived == 0U
        ? 0.0
        : static_cast<double>(instrument.packetsConcealed)
            / static_cast<double>(peerTelemetry_.packetsReceived);

    QString grade;
    if (concealRatio > 0.05 || playableMilliseconds > 60.0) {
        grade = QStringLiteral("Conversation only");
    } else if (concealRatio > 0.02 || playableMilliseconds > 40.0) {
        grade = QStringLiteral("Poor");
    } else if (concealRatio > 0.005 || playableMilliseconds > 25.0) {
        grade = QStringLiteral("Playable");
    } else if (playableMilliseconds > 15.0) {
        grade = QStringLiteral("Good");
    } else {
        grade = QStringLiteral("Excellent");
    }
    return QStringLiteral("%1 · about %2 ms one way · %3 ms buffer")
        .arg(grade)
        .arg(oneWayMilliseconds, 0, 'f', 1)
        .arg(bufferMilliseconds, 0, 'f', 1);
}

QString AppController::networkDiagnostics() const {
    if (!peerTransport_) {
        return QStringLiteral("No room session");
    }
    const auto& instrument = peerTelemetry_.streams[instrumentStream];
    const auto& voice = peerTelemetry_.streams[voiceStream];
    return QStringLiteral(
               "round trip %1 ms measured · jitter %2 ms\n"
               "instrument concealed %3 · late %4 · buffer %5 ms\n"
               "voice concealed %6 · late %7 · buffer %8 ms")
        .arg(static_cast<double>(peerTelemetry_.roundTripMicroseconds) / 1'000.0, 0, 'f', 1)
        .arg(static_cast<double>(instrument.jitterMicroseconds) / 1'000.0, 0, 'f', 1)
        .arg(instrument.packetsConcealed)
        .arg(instrument.packetsLate)
        .arg(static_cast<double>(instrument.bufferedFrames) / 48.0, 0, 'f', 1)
        .arg(voice.packetsConcealed)
        .arg(voice.packetsLate)
        .arg(static_cast<double>(voice.bufferedFrames) / 48.0, 0, 'f', 1);
}

QString AppController::packetSummary() const {
    return QStringLiteral("%1 received · %2 sent · %3 rejected")
        .arg(peerTelemetry_.packetsReceived)
        .arg(peerTelemetry_.packetsSent)
        .arg(peerTelemetry_.packetsRejected);
}
bool AppController::sendMuted() const noexcept { return sendMuted_; }
void AppController::setSendMuted(bool muted) {
    if (sendMuted_ == muted) {
        return;
    }
    sendMuted_ = muted;
    if (peerTransport_) {
        peerTransport_->setSendMuted(muted);
    }
    emit roomChanged();
}

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
    if (devicesAvailable_ && audioActive()) {
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
        setupMessage_ = QStringLiteral("Private setup verified and saved for this run");
    } else if (devicesAvailable_) {
        setupMessage_ = QStringLiteral("Start the private monitor before verifying this setup");
    } else {
        setupMessage_ = QStringLiteral("No compatible Windows audio endpoints are available");
    }
    persistNow();
    emit setupChanged();
}

void AppController::testOutput() {
    if (audioService_ && audioActive()) {
        audioService_->requestOutputTest();
        setupMessage_ = QStringLiteral("Playing a quiet 440 Hz output test for one second");
    } else {
        setupMessage_ = QStringLiteral("The private monitor must be active to test the output");
    }
    emit setupChanged();
}

void AppController::retryAudio() {
    if (!visualFixture_) {
        if (!instrumentOptions_.empty() && !voiceOptions_.empty() && !outputOptions_.empty()) {
            applySelectionsToPreferences();
        }
        loadDeviceInventory();
        if (devicesAvailable_) {
            instrumentIndex_ = resolveDevice(instrumentOptions_, preferences_.instrument);
            voiceIndex_ = resolveDevice(voiceOptions_, preferences_.voice);
            outputIndex_ = resolveDevice(outputOptions_, preferences_.output);
            updateOutputCapabilities();
            sampleRateIndex_ = resolveScalar(sampleRateValues_, preferences_.sampleRate);
            bufferSizeIndex_ = resolveScalar(bufferSizeValues_, preferences_.bufferFrames);
            updateReadinessConfiguration();
            restartAudio();
        } else {
            audioTelemetry_ = {};
            audioTelemetry_.state = jamlink::audio::SoundcheckAudioState::NoEndpoints;
            setupMessage_ = QStringLiteral("Connect an input and output, then retry audio");
        }
        emit setupChanged();
    }
}

void AppController::hostSession() {
    if (visualFixture_ || !audioService_ || !audioActive() || !allReady()) {
        setupMessage_ = QStringLiteral("Verify the real private audio setup before hosting");
        setCurrentPage(QStringLiteral("soundcheck"));
        emit setupChanged();
        return;
    }
    leaveSession();
    auto transport = jamlink::network::createPlatformPeerAudioTransport();
    if (!transport) {
        setupMessage_ = QStringLiteral("This build has no Windows peer transport");
        emit setupChanged();
        return;
    }
    const std::string invite = transport->host();
    peerTelemetry_ = transport->telemetry();
    if (invite.empty()) {
        setupMessage_ = peerStateText(peerTelemetry_.state);
        emit setupChanged();
        return;
    }
    audioService_->stop();
    audioService_->setPeerAudioExchange(transport.get());
    // Carry the listener's remote mix preferences into the new session rather
    // than silently resetting them on every join.
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Instrument,
        static_cast<float>(remoteInstrumentGain_));
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Voice, static_cast<float>(remoteVoiceGain_));
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Instrument, remoteInstrumentMuted_);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Voice, remoteVoiceMuted_);
    peerTransport_ = std::move(transport);
    inviteCode_ = QString::fromStdString(invite);
    sendMuted_ = false;
    restartAudio();
    setCurrentPage(QStringLiteral("room"));
    emit roomChanged();
}

void AppController::joinSession(const QString& inviteCode) {
    if (visualFixture_ || !audioService_ || !audioActive() || !allReady()) {
        setupMessage_ = QStringLiteral("Verify the real private audio setup before joining");
        setCurrentPage(QStringLiteral("soundcheck"));
        emit setupChanged();
        return;
    }
    leaveSession();
    auto transport = jamlink::network::createPlatformPeerAudioTransport();
    if (!transport || !transport->join(inviteCode.trimmed().toStdString())) {
        if (transport) {
            peerTelemetry_ = transport->telemetry();
            setupMessage_ = peerStateText(peerTelemetry_.state);
        } else {
            setupMessage_ = QStringLiteral("This build has no Windows peer transport");
        }
        emit setupChanged();
        return;
    }
    audioService_->stop();
    audioService_->setPeerAudioExchange(transport.get());
    // Carry the listener's remote mix preferences into the new session rather
    // than silently resetting them on every join.
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Instrument,
        static_cast<float>(remoteInstrumentGain_));
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Voice, static_cast<float>(remoteVoiceGain_));
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Instrument, remoteInstrumentMuted_);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Voice, remoteVoiceMuted_);
    peerTransport_ = std::move(transport);
    inviteCode_.clear();
    sendMuted_ = false;
    restartAudio();
    setCurrentPage(QStringLiteral("room"));
    emit roomChanged();
}

void AppController::leaveSession() {
    if (!peerTransport_) {
        return;
    }
    if (audioService_) {
        audioService_->stop();
        audioService_->setPeerAudioExchange(nullptr);
    }
    peerTransport_->stop();
    peerTransport_.reset();
    peerTelemetry_ = {};
    inviteCode_.clear();
    sendMuted_ = false;
    if (audioService_ && devicesAvailable_) {
        restartAudio();
    }
    setCurrentPage(QStringLiteral("home"));
    emit roomChanged();
}

void AppController::copyInvite() {
    if (!inviteCode_.isEmpty()) {
        QGuiApplication::clipboard()->setText(inviteCode_);
        setupMessage_ = QStringLiteral("Invite code copied");
        emit setupChanged();
    }
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

void AppController::loadDeviceInventory() {
    instrumentOptions_.clear();
    voiceOptions_.clear();
    outputOptions_.clear();
    devicesAvailable_ = false;
    if (!audioService_) {
        instrumentOptions_ = {{QString(), QStringLiteral("Windows audio service unavailable"), {}, {}, {}}};
        voiceOptions_ = instrumentOptions_;
        outputOptions_ = instrumentOptions_;
        return;
    }

    const auto inventory = audioService_->enumerate();
    const auto inputOption = [](const jamlink::audio::SoundcheckEndpointOption& option) {
        return DeviceOption{
            QString::fromUtf8(option.endpointId),
            QString::fromUtf8(option.displayName),
            QStringLiteral("input:%1").arg(option.primaryChannel),
            {},
            option};
    };
    const auto outputOption = [](const jamlink::audio::SoundcheckEndpointOption& option) {
        return DeviceOption{
            QString::fromUtf8(option.endpointId),
            QString::fromUtf8(option.displayName),
            QStringLiteral("output:%1").arg(option.primaryChannel),
            option.hasSecondaryChannel
                ? QStringLiteral("output:%1").arg(option.secondaryChannel)
                : QString(),
            option};
    };
    instrumentOptions_.reserve(inventory.inputOptions.size());
    voiceOptions_.reserve(inventory.inputOptions.size());
    outputOptions_.reserve(inventory.outputOptions.size());
    for (const auto& option : inventory.inputOptions) {
        instrumentOptions_.push_back(inputOption(option));
        voiceOptions_.push_back(inputOption(option));
    }
    for (const auto& option : inventory.outputOptions) {
        outputOptions_.push_back(outputOption(option));
    }
    devicesAvailable_ = !instrumentOptions_.empty()
        && !voiceOptions_.empty() && !outputOptions_.empty();
    if (!devicesAvailable_) {
        instrumentOptions_ = {{QString(), QStringLiteral("No active Windows capture endpoint"), {}, {}, {}}};
        voiceOptions_ = instrumentOptions_;
        outputOptions_ = {{QString(), QStringLiteral("No active Windows output endpoint"), {}, {}, {}}};
    }
}

void AppController::updateOutputCapabilities() {
    if (visualFixture_ || !devicesAvailable_
        || !validIndex(outputIndex_, outputOptions_.size())) {
        return;
    }
    const auto& option = outputOptions_[static_cast<std::size_t>(outputIndex_)].serviceOption;
    sampleRateValues_ = {option.mixSampleRate == 0U ? 48'000U : option.mixSampleRate};
    bufferSizeValues_ = option.bufferFrameOptions;
    std::sort(bufferSizeValues_.begin(), bufferSizeValues_.end());
    bufferSizeValues_.erase(
        std::remove(bufferSizeValues_.begin(), bufferSizeValues_.end(), 0U),
        bufferSizeValues_.end());
    bufferSizeValues_.erase(
        std::unique(bufferSizeValues_.begin(), bufferSizeValues_.end()),
        bufferSizeValues_.end());
    if (bufferSizeValues_.empty()) {
        bufferSizeValues_ = {480U};
    }
    sampleRateIndex_ = 0;
    bufferSizeIndex_ = std::clamp(
        bufferSizeIndex_, 0, static_cast<int>(bufferSizeValues_.size() - 1U));
}

int AppController::resolveDevice(
    const std::vector<DeviceOption>& options,
    const jamlink::preferences::AudioSelection& selection) {
    for (std::size_t index = 0; index < options.size(); ++index) {
        if (options[index].stableId.toStdString() == selection.deviceId
            && options[index].primaryChannelId.toStdString() == selection.primaryChannelId
            && options[index].secondaryChannelId.toStdString() == selection.secondaryChannelId) {
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
    if (visualFixture_ && !restoredPreferences_) {
        preferences_.instrumentMonitorEnabled = true;
        preferences_.voiceMonitorEnabled = true;
    }
    if (widthOverride >= 532U) {
        preferences_.window.width = widthOverride;
    }
    if (heightOverride >= 480U) {
        preferences_.window.height = heightOverride;
    }

    instrumentIndex_ = resolveDevice(instrumentOptions_, preferences_.instrument);
    voiceIndex_ = resolveDevice(voiceOptions_, preferences_.voice);
    outputIndex_ = resolveDevice(outputOptions_, preferences_.output);
    updateOutputCapabilities();
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
    } else if (visualFixture_) {
        setupMessage_ = QStringLiteral("Deterministic visual fixture; no hardware is active");
    } else if (devicesAvailable_) {
        setupMessage_ = QStringLiteral("Starting private WASAPI Shared monitor");
    } else {
        setupMessage_ = QStringLiteral("Connect an input and output, then retry audio");
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

void AppController::scheduleAudioRestart() {
    if (visualFixture_ || !audioService_ || !devicesAvailable_) {
        return;
    }
    telemetryTimer_.stop();
    audioService_->stop();
    audioTelemetry_ = audioService_->telemetry();
    audioRestartTimer_.start();
}

void AppController::restartAudio() {
    if (visualFixture_ || !audioService_ || !devicesAvailable_
        || !validIndex(instrumentIndex_, instrumentOptions_.size())
        || !validIndex(voiceIndex_, voiceOptions_.size())
        || !validIndex(outputIndex_, outputOptions_.size())
        || !validIndex(bufferSizeIndex_, bufferSizeValues_.size())) {
        return;
    }
    audioService_->stop();
    setupMessage_ = QStringLiteral("Opening private Windows audio monitor…");
    emit setupChanged();
    const jamlink::audio::SoundcheckAudioConfiguration configuration{
        instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)].serviceOption,
        voiceOptions_[static_cast<std::size_t>(voiceIndex_)].serviceOption,
        outputOptions_[static_cast<std::size_t>(outputIndex_)].serviceOption,
        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)],
        preferences_.instrumentMonitorGain,
        preferences_.voiceMonitorGain,
        preferences_.instrumentMonitorEnabled,
        preferences_.voiceMonitorEnabled};
    static_cast<void>(audioService_->start(configuration));
    audioTelemetry_ = audioService_->telemetry();
    if (audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::Running) {
        setupMessage_ = QStringLiteral(
            "Private local monitor active; Windows shared-mode processing may apply");
        telemetryTimer_.start();
    } else {
        setupMessage_ = audioStateText(audioTelemetry_.state);
    }
    emit setupChanged();
}

void AppController::pollAudioTelemetry() {
    if (!audioService_) {
        return;
    }
    const auto previousState = audioTelemetry_.state;
    audioTelemetry_ = audioService_->telemetry();
    if (previousState == jamlink::audio::SoundcheckAudioState::Running
        && audioTelemetry_.state != jamlink::audio::SoundcheckAudioState::Running) {
        setupMessage_ = audioStateText(audioTelemetry_.state);
        telemetryTimer_.stop();
        audioRestartTimer_.start(1'000);
    }
    if (peerTransport_) {
        peerTelemetry_ = peerTransport_->telemetry();
        emit roomChanged();
    }
    emit setupChanged();
}

QString AppController::audioStateText(jamlink::audio::SoundcheckAudioState state) {
    using State = jamlink::audio::SoundcheckAudioState;
    switch (state) {
    case State::Stopped:
        return QStringLiteral("Private monitor stopped");
    case State::Starting:
        return QStringLiteral("Starting private monitor…");
    case State::Running:
        return QStringLiteral("Private monitor active");
    case State::NoEndpoints:
        return QStringLiteral("Select active input and output endpoints");
    case State::DeviceUnavailable:
        return QStringLiteral("A selected audio device is unavailable");
    case State::UnsupportedFormat:
        return QStringLiteral("A selected device uses an unsupported Windows format");
    case State::InitializationFailed:
        return QStringLiteral("Windows could not open this device combination");
    case State::DeviceInvalidated:
        return QStringLiteral("An audio device changed; retrying safely");
    }
    return QStringLiteral("Audio status unavailable");
}

QString AppController::peerStateText(jamlink::network::PeerConnectionState state) {
    using State = jamlink::network::PeerConnectionState;
    switch (state) {
    case State::Idle:
        return QStringLiteral("Room transport is idle");
    case State::Preparing:
        return QStringLiteral("Preparing secure invite…");
    case State::WaitingForPeer:
        return QStringLiteral("Invite ready · waiting for your friend");
    case State::Connecting:
        return QStringLiteral("Connecting with invite code…");
    case State::Connected:
        return QStringLiteral("Connected · encrypted direct audio");
    case State::InviteInvalid:
        return QStringLiteral("That invite code is invalid");
    case State::SocketFailed:
        return QStringLiteral("Windows could not open the room network port");
    case State::EncryptionFailed:
        return QStringLiteral("Secure room encryption could not start");
    case State::ConnectionLost:
        return QStringLiteral("Connection lost · retrying direct audio");
    }
    return QStringLiteral("Room status unavailable");
}

} // namespace jamlink::desktop
