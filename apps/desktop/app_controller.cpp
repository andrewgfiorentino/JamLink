// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>
#include <QVariantMap>

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

QString boundedSingleLine(const QString& value, qsizetype maximum) {
    QString result = value.normalized(QString::NormalizationForm_KC);
    result.replace(QLatin1Char('\r'), QLatin1Char(' '));
    result.replace(QLatin1Char('\n'), QLatin1Char(' '));
    result = result.simplified();
    return result.left(maximum);
}

QString normalizedHandle(const QString& value) {
    QString source = value.trimmed().normalized(QString::NormalizationForm_KC);
    if (source.startsWith(QLatin1Char('@'))) {
        source.remove(0, 1);
    }
    source = source.toCaseFolded();
    QString result;
    result.reserve(std::min<qsizetype>(source.size(), 24));
    for (const QChar character : source) {
        if (character.isLetterOrNumber() || character == QLatin1Char('_')
            || character == QLatin1Char('.') || character == QLatin1Char('-')) {
            result.push_back(character);
            if (result.size() == 24) {
                break;
            }
        }
    }
    return result;
}

const QStringList& avatarIds() {
    static const QStringList values{
        QStringLiteral("avatar:guitar-electric"),
        QStringLiteral("avatar:guitar-acoustic"),
        QStringLiteral("avatar:bass"),
        QStringLiteral("avatar:drums"),
        QStringLiteral("avatar:keys"),
        QStringLiteral("avatar:vocals"),
        QStringLiteral("avatar:synth"),
        QStringLiteral("avatar:listener")};
    return values;
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
      updateManager_(
          QStringLiteral(JAMLINK_VERSION_STRING),
          QStringLiteral(JAMLINK_RELEASE_CHANNEL_STRING),
          visualFixture,
          this),
      currentPage_(std::move(initialPage)),
      visualFixture_(visualFixture),
      audioService_(std::move(audioService)) {
    connect(&updateManager_, &UpdateManager::changed, this, &AppController::updateChanged);
    const bool automaticPage = currentPage_ == QStringLiteral("auto");
    if (!automaticPage && currentPage_ != QStringLiteral("home")
        && currentPage_ != QStringLiteral("soundcheck")
        && currentPage_ != QStringLiteral("settings")
        && currentPage_ != QStringLiteral("room")
        && currentPage_ != QStringLiteral("tuner")
        && currentPage_ != QStringLiteral("profile")) {
        currentPage_ = QStringLiteral("home");
    }
    tunerActive_ = currentPage_ == QStringLiteral("tuner");

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
    if (!visualFixture_ && devicesAvailable_
        && (!restoredPreferences_ || restoredSetupAvailable_)) {
        audioRestartTimer_.start(0);
    }
    if (!visualFixture_) {
        QTimer::singleShot(1'500, &updateManager_, &UpdateManager::checkNow);
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
        && page != QStringLiteral("settings") && page != QStringLiteral("room")
        && page != QStringLiteral("tuner") && page != QStringLiteral("profile")) {
        return;
    }
    if (page != currentPage_) {
        // Leaving the tuner always releases the tuner mute, so a user cannot
        // navigate away and silently stay muted to the room.
        if (currentPage_ == QStringLiteral("tuner")) {
            setTunerActive(false);
        }
        currentPage_ = page;
        if (page == QStringLiteral("tuner")) {
            setTunerActive(true);
        }
        emit currentPageChanged();
    }
}

bool AppController::tunerActive() const noexcept { return tunerActive_; }

void AppController::setTunerActive(bool active) {
    if (tunerActive_ == active) {
        return;
    }
    tunerActive_ = active;
    if (audioService_) {
        audioService_->setTunerEnabled(active);
    }
    applyTunerMute();
    if (!active) {
        tunerReading_ = {};
    }
    emit tunerChanged();
}

bool AppController::tunerMutesInstrument() const noexcept {
    return preferences_.tunerMutesInstrument;
}

void AppController::setTunerMutesInstrument(bool muted) {
    if (preferences_.tunerMutesInstrument == muted) {
        return;
    }
    preferences_.tunerMutesInstrument = muted;
    applyTunerMute();
    scheduleSave();
    emit tunerChanged();
}

// "Give me a second to tune" — the instrument stops reaching the room while
// voice keeps flowing, which is only possible because they are separate
// streams on the wire.
void AppController::applyTunerMute() {
    if (!peerTransport_) {
        return;
    }
    peerTransport_->setLocalStreamMuted(
        jamlink::network::AudioStreamId::Instrument,
        tunerActive_ && preferences_.tunerMutesInstrument);
}

QString AppController::recordingDirectory() const {
    if (!preferences_.recordingDirectory.empty()) {
        return QString::fromStdString(preferences_.recordingDirectory);
    }
    return QString::fromStdWString(defaultRecordingDirectory().wstring());
}

void AppController::setRecordingDirectory(const QString& directory) {
    const std::string chosen = directory.trimmed().toStdString();
    if (preferences_.recordingDirectory == chosen) {
        return;
    }
    preferences_.recordingDirectory = chosen;
    scheduleSave();
    emit settingsChanged();
}

std::filesystem::path AppController::defaultRecordingDirectory() const {
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty()) {
        return store_.path().parent_path() / "Recordings";
    }
    return std::filesystem::path(music.toStdWString()) / L"JamLink";
}

void AppController::openRecordingFolder() {
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(recordingDirectory().toStdWString()), error);
    static_cast<void>(QDesktopServices::openUrl(
        QUrl::fromLocalFile(recordingDirectory())));
}

int AppController::preferredUdpPort() const noexcept {
    return static_cast<int>(preferences_.preferredUdpPort);
}

void AppController::setPreferredUdpPort(int port) {
    // Zero means "ask the operating system", which is the right default. Ports
    // below 1024 are privileged and would fail to bind.
    const auto bounded = static_cast<std::uint32_t>(
        port <= 0 ? 0 : std::clamp(port, 1'024, 65'535));
    if (preferences_.preferredUdpPort == bounded) {
        return;
    }
    preferences_.preferredUdpPort = bounded;
    scheduleSave();
    emit settingsChanged();
}

bool AppController::automaticRouterMapping() const noexcept {
    return preferences_.automaticPortMapping;
}

void AppController::setAutomaticRouterMapping(bool enabled) {
    if (preferences_.automaticPortMapping == enabled) {
        return;
    }
    preferences_.automaticPortMapping = enabled;
    scheduleSave();
    emit settingsChanged();
}

int AppController::latencyMode() const noexcept {
    return static_cast<int>(preferences_.latencyMode);
}

void AppController::setLatencyMode(int mode) {
    const auto bounded = static_cast<std::uint32_t>(std::clamp(mode, 0, 2));
    if (preferences_.latencyMode == bounded) {
        return;
    }
    preferences_.latencyMode = bounded;
    if (peerTransport_) {
        peerTransport_->setLatencyPreference(
            static_cast<jamlink::network::LatencyPreference>(bounded));
    }
    scheduleSave();
    emit settingsChanged();
}

QString AppController::latencyModeDetail() const {
    switch (preferences_.latencyMode) {
    case 0U:
        return QStringLiteral(
            "Smallest receive buffer. Best on a good wired path; a rough "
            "network will be audible as dropouts.");
    case 2U:
        return QStringLiteral(
            "Largest receive buffer. Rides out an unstable network, at delay "
            "that eventually makes playing in time impossible.");
    default:
        return QStringLiteral(
            "Follows measured jitter and keeps the buffer only as deep as the "
            "network actually needs.");
    }
}

QString AppController::applicationVersion() const {
    return QStringLiteral(JAMLINK_VERSION_STRING);
}

QString AppController::qtVersion() const { return QStringLiteral(QT_VERSION_STR); }

QString AppController::updateStatus() const { return updateManager_.status(); }
bool AppController::updateAvailable() const noexcept { return updateManager_.updateAvailable(); }
bool AppController::updateBusy() const noexcept { return updateManager_.busy(); }
double AppController::updateProgress() const noexcept { return updateManager_.progress(); }
void AppController::checkForUpdates() { updateManager_.checkNow(); }
void AppController::installUpdate() {
    if (!roomActive()) {
        updateManager_.downloadAndInstall();
    }
}

UpdateManager* AppController::updateManager() noexcept { return &updateManager_; }

QString AppController::profileId() const {
    return QString::fromStdString(preferences_.profile.profileId);
}

QString AppController::profileHandle() const {
    return QString::fromStdString(preferences_.profile.handle);
}

void AppController::setProfileHandle(const QString& value) {
    const std::string normalized = normalizedHandle(value).toStdString();
    if (preferences_.profile.handle == normalized) {
        return;
    }
    preferences_.profile.handle = normalized;
    scheduleSave();
    emit profileChanged();
}

QString AppController::profileDisplayName() const {
    return QString::fromStdString(preferences_.profile.displayName);
}

void AppController::setProfileDisplayName(const QString& value) {
    QString normalized = boundedSingleLine(value, 48);
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("Musician");
    }
    const std::string stored = normalized.toStdString();
    if (preferences_.profile.displayName == stored) {
        return;
    }
    preferences_.profile.displayName = stored;
    scheduleSave();
    emit profileChanged();
}

QString AppController::profileAvatarId() const {
    return QString::fromStdString(preferences_.profile.avatarId);
}

void AppController::setProfileAvatarId(const QString& value) {
    if (!avatarIds().contains(value)) {
        return;
    }
    const std::string stored = value.toStdString();
    if (preferences_.profile.avatarId == stored) {
        return;
    }
    preferences_.profile.avatarId = stored;
    scheduleSave();
    emit profileChanged();
}

QUrl AppController::profileCustomAvatarSource() const {
    if (preferences_.profile.avatarId != "avatar:custom"
        || preferences_.profile.customAvatarPath.empty()) {
        return {};
    }
    const QString path = QString::fromUtf8(preferences_.profile.customAvatarPath);
    if (!QFileInfo(path).isFile()) {
        return {};
    }
    return QUrl::fromLocalFile(path);
}

QString AppController::profilePrimaryInstrument() const {
    return QString::fromStdString(preferences_.profile.primaryInstrument);
}

void AppController::setProfilePrimaryInstrument(const QString& value) {
    if (!profileInstrumentOptions().contains(value)) {
        return;
    }
    const std::string stored = value.toStdString();
    if (preferences_.profile.primaryInstrument == stored) {
        return;
    }
    preferences_.profile.primaryInstrument = stored;
    scheduleSave();
    emit profileChanged();
}

QString AppController::profileGenres() const {
    return QString::fromStdString(preferences_.profile.genres);
}

void AppController::setProfileGenres(const QString& value) {
    const std::string stored = boundedSingleLine(value, 96).toStdString();
    if (preferences_.profile.genres == stored) {
        return;
    }
    preferences_.profile.genres = stored;
    scheduleSave();
    emit profileChanged();
}

QString AppController::profileBio() const {
    return QString::fromStdString(preferences_.profile.bio);
}

void AppController::setProfileBio(const QString& value) {
    QString normalized = value.normalized(QString::NormalizationForm_KC);
    normalized.replace(QLatin1Char('\r'), QString());
    normalized = normalized.left(240);
    const std::string stored = normalized.toStdString();
    if (preferences_.profile.bio == stored) {
        return;
    }
    preferences_.profile.bio = stored;
    scheduleSave();
    emit profileChanged();
}

QString AppController::profileRegion() const {
    return QString::fromStdString(preferences_.profile.region);
}

void AppController::setProfileRegion(const QString& value) {
    const std::string stored = boundedSingleLine(value, 48).toStdString();
    if (preferences_.profile.region == stored) {
        return;
    }
    preferences_.profile.region = stored;
    scheduleSave();
    emit profileChanged();
}

QStringList AppController::profileAvatarIds() const { return avatarIds(); }

QStringList AppController::profileAvatarLabels() const {
    return {
        QStringLiteral("Electric guitar"), QStringLiteral("Acoustic guitar"),
        QStringLiteral("Bass"), QStringLiteral("Drums"), QStringLiteral("Keys"),
        QStringLiteral("Vocals"), QStringLiteral("Synth"), QStringLiteral("Listener")};
}

QStringList AppController::profileInstrumentOptions() const {
    return {
        QStringLiteral("Guitar"), QStringLiteral("Bass"), QStringLiteral("Drums"),
        QStringLiteral("Keys"), QStringLiteral("Vocals"), QStringLiteral("Synth"),
        QStringLiteral("Multi-instrumentalist"), QStringLiteral("Listener")};
}

QString AppController::remoteDisplayName() const {
    return remoteParticipant_.displayName.empty()
        ? QStringLiteral("Friend")
        : QString::fromStdString(remoteParticipant_.displayName);
}

QString AppController::remoteHandle() const {
    return QString::fromStdString(remoteParticipant_.handle);
}

QString AppController::remoteAvatarId() const {
    return QString::fromStdString(remoteParticipant_.avatarId);
}

QString AppController::remotePrimaryInstrument() const {
    return remoteParticipant_.primaryInstrument.empty()
        ? QStringLiteral("Musician")
        : QString::fromStdString(remoteParticipant_.primaryInstrument);
}

QVariantList AppController::chatMessages() const { return chatMessages_; }
int AppController::unreadChatCount() const noexcept { return unreadChatCount_; }

bool AppController::setCustomAvatar(const QUrl& source) {
    if (!source.isLocalFile()) {
        setupMessage_ = QStringLiteral("Choose a local PNG, JPEG, or WebP image");
        emit setupChanged();
        return false;
    }
    const QString sourcePath = source.toLocalFile();
    const QFileInfo sourceInfo(sourcePath);
    constexpr qint64 maximumInputBytes = 10LL * 1'024LL * 1'024LL;
    if (!sourceInfo.isFile() || sourceInfo.size() <= 0
        || sourceInfo.size() > maximumInputBytes) {
        setupMessage_ = QStringLiteral("Avatar images must be smaller than 10 MB");
        emit setupChanged();
        return false;
    }
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    constexpr qint64 maximumPixels = 16LL * 1'024LL * 1'024LL;
    if (!sourceSize.isValid() || sourceSize.width() > 4'096
        || sourceSize.height() > 4'096
        || static_cast<qint64>(sourceSize.width()) * sourceSize.height() > maximumPixels) {
        setupMessage_ = QStringLiteral("Avatar dimensions are too large");
        emit setupChanged();
        return false;
    }
    QSize decodedSize = sourceSize;
    decodedSize.scale(512, 512, Qt::KeepAspectRatioByExpanding);
    reader.setScaledSize(decodedSize);
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        setupMessage_ = QStringLiteral("That image could not be decoded safely");
        emit setupChanged();
        return false;
    }
    const int side = std::min(decoded.width(), decoded.height());
    const QRect crop(
        (decoded.width() - side) / 2, (decoded.height() - side) / 2, side, side);
    const QImage thumbnail = decoded.copy(crop).scaled(
        256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGBA8888);
    const std::filesystem::path destination =
        store_.path().parent_path() / "profile-avatar.png";
    QSaveFile output(QString::fromStdWString(destination.wstring()));
    if (!output.open(QIODevice::WriteOnly)
        || !thumbnail.save(&output, "PNG") || !output.commit()) {
        setupMessage_ = QStringLiteral("The sanitized avatar could not be saved");
        emit setupChanged();
        return false;
    }
    preferences_.profile.customAvatarPath =
        QString::fromStdWString(destination.wstring()).toUtf8().toStdString();
    preferences_.profile.avatarId = "avatar:custom";
    scheduleSave();
    emit profileChanged();
    return true;
}

void AppController::clearCustomAvatar() {
    if (preferences_.profile.avatarId != "avatar:custom") {
        return;
    }
    preferences_.profile.avatarId = "avatar:guitar-electric";
    scheduleSave();
    emit profileChanged();
}

bool AppController::recording() const noexcept { return recorderTelemetry_.recording; }

QString AppController::recordingElapsed() const {
    const std::uint32_t seconds = recorderTelemetry_.elapsedSeconds;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60U)
        .arg(seconds % 60U, 2, 10, QLatin1Char('0'));
}

QString AppController::recordingMessage() const {
    if (recorderTelemetry_.failed) {
        return QStringLiteral("Recording could not be written to disk");
    }
    if (!recorderTelemetry_.recording) {
        return recordingLocation_.isEmpty()
            ? QStringLiteral("Records your parts and your friend's separately")
            : QStringLiteral("Saved to %1").arg(recordingLocation_);
    }
    if (recorderTelemetry_.droppedFrames > 0U) {
        // Never quietly ship a take with holes in it.
        return QStringLiteral("Recording · disk fell behind, this take has gaps");
    }
    return QStringLiteral("Recording · 4 separate tracks");
}

QString AppController::recordingLocation() const { return recordingLocation_; }

void AppController::toggleRecording() {
    if (visualFixture_ || !audioService_) {
        return;
    }
    if (recorderTelemetry_.recording) {
        audioService_->stopRecording();
        recorderTelemetry_ = audioService_->recorderTelemetry();
        emit recordingChanged();
        return;
    }
    if (!audioActive()) {
        setupMessage_ = QStringLiteral("Start the audio setup before recording");
        emit setupChanged();
        return;
    }

    const std::filesystem::path directory(recordingDirectory().toStdWString());
    // A sortable, human-readable folder per take.
    const QString name = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH-mm-ss"));
    if (!audioService_->startRecording(directory, name.toStdString())) {
        setupMessage_ = QStringLiteral("Recording could not be started");
        emit setupChanged();
        return;
    }
    recordingLocation_ = QString::fromStdWString((directory / name.toStdWString()).wstring());
    recorderTelemetry_ = audioService_->recorderTelemetry();
    emit recordingChanged();
}

bool AppController::tunerDetected() const noexcept { return tunerReading_.detected; }
QString AppController::tunerNote() const {
    if (!tunerReading_.detected) {
        return QStringLiteral("—");
    }
    return QString::fromUtf8(
        jamlink::audio::InstrumentTuner::noteName(tunerReading_.midiNote).data(),
        static_cast<int>(
            jamlink::audio::InstrumentTuner::noteName(tunerReading_.midiNote).size()));
}
int AppController::tunerOctave() const noexcept {
    return tunerReading_.detected
        ? jamlink::audio::InstrumentTuner::noteOctave(tunerReading_.midiNote)
        : 0;
}
double AppController::tunerCents() const noexcept {
    return tunerReading_.detected ? tunerReading_.cents : 0.0;
}
double AppController::tunerFrequency() const noexcept { return tunerReading_.frequency; }
double AppController::tunerLevel() const noexcept {
    return static_cast<double>(tunerReading_.level);
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
        const bool asio = validIndex(outputIndex_, outputOptions_.size())
            && outputOptions_[static_cast<std::size_t>(outputIndex_)].serviceOption.backend
                == jamlink::audio::SoundcheckBackend::Asio;
        return QStringLiteral("%1 · %2 kHz · %3 frames%4")
            .arg(asio ? QStringLiteral("ASIO") : QStringLiteral("WASAPI Shared"))
            .arg(audioTelemetry_.outputSampleRate / 1'000.0, 0, 'g', 3)
            .arg(audioTelemetry_.outputBufferFrames)
            .arg(audioTelemetry_.secondaryVoiceActive
                ? QString() : QStringLiteral(" · microphone reconnecting"));
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
    if (audioService_ && audioActive()) {
        const auto result = audioService_->tryReplaceVoiceEndpoint(
            voiceOptions_[static_cast<std::size_t>(voiceIndex_)].serviceOption);
        if (result == jamlink::audio::VoiceEndpointChangeResult::Applied) {
            audioTelemetry_ = audioService_->telemetry();
            setupMessage_ = QStringLiteral(
                "Microphone switched; ASIO instrument and output stayed active");
            emit setupChanged();
            return;
        }
        if (result == jamlink::audio::VoiceEndpointChangeResult::Failed) {
            audioTelemetry_ = audioService_->telemetry();
            setupMessage_ = QStringLiteral(
                "That microphone could not start; ASIO instrument and output remain active");
            emit setupChanged();
            return;
        }
    }
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

double AppController::remoteInstrumentGain() const noexcept {
    return static_cast<double>(preferences_.remoteInstrumentGain);
}
double AppController::remoteVoiceGain() const noexcept {
    return static_cast<double>(preferences_.remoteVoiceGain);
}
bool AppController::remoteInstrumentMuted() const noexcept { return remoteInstrumentMuted_; }
bool AppController::remoteVoiceMuted() const noexcept { return remoteVoiceMuted_; }

void AppController::setRemoteInstrumentGain(double gain) {
    applyRemoteStream(
        jamlink::network::AudioStreamId::Instrument, preferences_.remoteInstrumentGain, gain);
}
void AppController::setRemoteVoiceGain(double gain) {
    applyRemoteStream(
        jamlink::network::AudioStreamId::Voice, preferences_.remoteVoiceGain, gain);
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
    float& stored,
    double gain) {
    const auto bounded = static_cast<float>(std::clamp(gain, 0.0, 1.0));
    if (std::abs(stored - bounded) < 0.0005F) {
        return;
    }
    stored = bounded;
    if (peerTransport_) {
        peerTransport_->setRemoteStreamGain(stream, bounded);
    }
    // How loud a friend sits in the mix is exactly the kind of thing worth
    // remembering between sessions.
    scheduleSave();
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
    if (devicesAvailable_ && audioActive() && audioTelemetry_.secondaryVoiceActive) {
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
    transport->setLocalParticipant(localParticipant());
    transport->setLatencyPreference(
        static_cast<jamlink::network::LatencyPreference>(preferences_.latencyMode));
    const std::string invite = transport->host(
        static_cast<std::uint16_t>(preferences_.preferredUdpPort),
        preferences_.automaticPortMapping);
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
        jamlink::network::AudioStreamId::Instrument, preferences_.remoteInstrumentGain);
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Voice, preferences_.remoteVoiceGain);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Instrument, remoteInstrumentMuted_);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Voice, remoteVoiceMuted_);
    peerTransport_ = std::move(transport);
    applyTunerMute();
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
    if (transport) {
        transport->setLocalParticipant(localParticipant());
        transport->setLatencyPreference(
            static_cast<jamlink::network::LatencyPreference>(preferences_.latencyMode));
    }
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
        jamlink::network::AudioStreamId::Instrument, preferences_.remoteInstrumentGain);
    transport->setRemoteStreamGain(
        jamlink::network::AudioStreamId::Voice, preferences_.remoteVoiceGain);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Instrument, remoteInstrumentMuted_);
    transport->setRemoteStreamMuted(
        jamlink::network::AudioStreamId::Voice, remoteVoiceMuted_);
    peerTransport_ = std::move(transport);
    applyTunerMute();
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
    remoteParticipant_ = {};
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

jamlink::network::PeerParticipantInfo AppController::localParticipant() const {
    jamlink::network::PeerParticipantInfo participant;
    participant.profileId = preferences_.profile.profileId;
    participant.handle = preferences_.profile.handle;
    participant.displayName = preferences_.profile.displayName;
    participant.avatarId = preferences_.profile.avatarId;
    participant.primaryInstrument = preferences_.profile.shareInstrument
        ? preferences_.profile.primaryInstrument : "Musician";
    participant.applicationVersion = JAMLINK_VERSION_STRING;
    participant.buildIdentity = JAMLINK_BUILD_IDENTITY_STRING;
    participant.releaseChannel = JAMLINK_RELEASE_CHANNEL_STRING;
    return participant;
}

bool AppController::sendChatMessage(const QString& text) {
    if (!peerTransport_ || !peerConnected()) {
        return false;
    }
    QString normalized = text.normalized(QString::NormalizationForm_C);
    normalized.replace(QLatin1Char('\r'), QString());
    normalized = normalized.trimmed();
    const QByteArray encoded = normalized.toUtf8();
    if (encoded.isEmpty()
        || encoded.size() > static_cast<qsizetype>(jamlink::network::maximumChatMessageBytes)
        || !peerTransport_->sendChatMessage(encoded.toStdString())) {
        return false;
    }
    appendChatEntry(
        profileDisplayName(), profileHandle(), normalized,
        static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()), true, false);
    emit chatChanged();
    return true;
}

void AppController::markChatRead() {
    if (unreadChatCount_ == 0) {
        return;
    }
    unreadChatCount_ = 0;
    emit chatChanged();
}

void AppController::appendChatEntry(
    const QString& sender,
    const QString& handle,
    const QString& text,
    std::uint64_t timestampMilliseconds,
    bool own,
    bool system) {
    QVariantMap entry;
    entry.insert(QStringLiteral("sender"), sender);
    entry.insert(QStringLiteral("handle"), handle);
    entry.insert(QStringLiteral("text"), text);
    entry.insert(QStringLiteral("own"), own);
    entry.insert(QStringLiteral("system"), system);
    entry.insert(
        QStringLiteral("time"),
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestampMilliseconds))
            .toString(QStringLiteral("h:mm AP")));
    if (chatMessages_.size() >= 200) {
        chatMessages_.removeFirst();
    }
    chatMessages_.push_back(entry);
}

void AppController::processRoomControlEvents() {
    if (!peerTransport_) {
        return;
    }
    bool roomUpdated = false;
    bool chatUpdated = false;
    for (const auto& event : peerTransport_->takeControlEvents()) {
        const QString displayName = event.participant.displayName.empty()
            ? QStringLiteral("Friend")
            : QString::fromStdString(event.participant.displayName);
        switch (event.type) {
        case jamlink::network::RoomControlEventType::PeerJoined:
            remoteParticipant_ = event.participant;
            appendChatEntry(
                QString(), QString(), displayName + QStringLiteral(" joined"),
                event.timestampMilliseconds, false, true);
            roomUpdated = true;
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::PeerLeft:
            appendChatEntry(
                QString(), QString(), displayName + QStringLiteral(" left"),
                event.timestampMilliseconds, false, true);
            roomUpdated = true;
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::ChatMessage:
            remoteParticipant_ = event.participant;
            appendChatEntry(
                displayName, QString::fromStdString(event.participant.handle),
                QString::fromStdString(event.text), event.timestampMilliseconds,
                false, false);
            unreadChatCount_ = std::min(unreadChatCount_ + 1, 99);
            roomUpdated = true;
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::ChatDeliveryFailed:
            appendChatEntry(
                QString(), QString(), QStringLiteral("A message was not delivered"),
                event.timestampMilliseconds, false, true);
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::VersionMismatch:
            remoteParticipant_ = event.participant;
            setupMessage_ = QStringLiteral(
                "Your friend is using a different JamLink build; both people must update");
            roomUpdated = true;
            break;
        }
    }
    if (roomUpdated) {
        emit roomChanged();
    }
    if (chatUpdated) {
        emit chatChanged();
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

void AppController::chooseInitialAudioDefaults() {
    if (!devicesAvailable_) {
        return;
    }
    const auto driverScore = [](const DeviceOption& option) {
        if (option.serviceOption.backend != jamlink::audio::SoundcheckBackend::Asio) {
            return -1'000;
        }
        const QString name = option.displayName.toCaseFolded();
        int score = 100;
        for (const QString& professional : {
                 QStringLiteral("focusrite"), QStringLiteral("rme"),
                 QStringLiteral("motu"), QStringLiteral("presonus"),
                 QStringLiteral("universal audio"), QStringLiteral("audient"),
                 QStringLiteral("steinberg"), QStringLiteral("behringer"),
                 QStringLiteral("ssl"), QStringLiteral("apollo")}) {
            if (name.contains(professional)) {
                score += 100;
                break;
            }
        }
        if (name.contains(QStringLiteral("asio4all"))
            || name.contains(QStringLiteral("fl studio"))) {
            score -= 80;
        }
        return score;
    };

    int bestScore = -1'000;
    int bestInstrument = -1;
    int bestOutput = -1;
    for (std::size_t output = 0U; output < outputOptions_.size(); ++output) {
        const auto& outputOption = outputOptions_[output];
        for (std::size_t input = 0U; input < instrumentOptions_.size(); ++input) {
            const auto& inputOption = instrumentOptions_[input];
            if (inputOption.serviceOption.backendId.empty()
                || inputOption.serviceOption.backendId
                    != outputOption.serviceOption.backendId) {
                continue;
            }
            int score = driverScore(outputOption) + driverScore(inputOption);
            score += outputOption.serviceOption.hasSecondaryChannel ? 10 : 0;
            score += inputOption.serviceOption.primaryChannel == 1U ? 2 : 0;
            if (score > bestScore) {
                bestScore = score;
                bestInstrument = static_cast<int>(input);
                bestOutput = static_cast<int>(output);
            }
        }
    }
    if (bestInstrument < 0 || bestOutput < 0) {
        return;
    }
    instrumentIndex_ = bestInstrument;
    outputIndex_ = bestOutput;

    int bestVoice = -1;
    int bestVoiceScore = -1'000;
    const QString masterName = outputOptions_[static_cast<std::size_t>(bestOutput)]
        .displayName.toCaseFolded();
    for (std::size_t input = 0U; input < voiceOptions_.size(); ++input) {
        const auto& option = voiceOptions_[input];
        if (option.serviceOption.backend != jamlink::audio::SoundcheckBackend::WasapiShared) {
            continue;
        }
        const QString name = option.displayName.toCaseFolded();
        int score = 100;
        if (name.contains(QStringLiteral("microphone"))
            || name.contains(QStringLiteral("mic "))) {
            score += 20;
        }
        if (name.contains(QStringLiteral("loopback"))
            || name.contains(QStringLiteral("stereo mix"))) {
            score -= 100;
        }
        if (!masterName.isEmpty() && name.contains(masterName)) {
            score -= 20;
        }
        if (score > bestVoiceScore) {
            bestVoiceScore = score;
            bestVoice = static_cast<int>(input);
        }
    }
    voiceIndex_ = bestVoice >= 0 ? bestVoice : bestInstrument;
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
    if (preferences_.profile.profileId.empty()) {
        preferences_.profile.profileId = visualFixture_
            ? "fixture-profile"
            : QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        if (visualFixture_) {
            preferences_.profile.displayName = "Alex";
            preferences_.profile.handle = "alex";
            preferences_.profile.genres = "Rock / Alternative";
        }
        scheduleSave();
    }
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
    if (!restoredPreferences_) {
        chooseInitialAudioDefaults();
    }
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
        setupMessage_ = QStringLiteral("Starting selected private Windows audio monitor");
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
        const bool asio = configuration.output.backend == jamlink::audio::SoundcheckBackend::Asio;
        const bool hybrid = asio
            && configuration.voice.backend == jamlink::audio::SoundcheckBackend::WasapiShared;
        setupMessage_ = hybrid
            ? QStringLiteral("ASIO guitar/output active with synchronized USB microphone")
            : asio
                ? QStringLiteral("Native ASIO private monitor active")
                : QStringLiteral("Windows shared-audio private monitor active");
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
    const bool previousSecondary = audioTelemetry_.secondaryVoiceActive;
    audioTelemetry_ = audioService_->telemetry();
    if (previousState == jamlink::audio::SoundcheckAudioState::Running
        && audioTelemetry_.state != jamlink::audio::SoundcheckAudioState::Running) {
        setupMessage_ = audioStateText(audioTelemetry_.state);
        telemetryTimer_.stop();
        audioRestartTimer_.start(1'000);
    }
    if (audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::Running
        && previousSecondary != audioTelemetry_.secondaryVoiceActive) {
        if (!audioTelemetry_.secondaryVoiceActive) {
            readiness_.invalidate(jamlink::control::SetupComponent::Voice);
        }
        setupMessage_ = audioTelemetry_.secondaryVoiceActive
            ? QStringLiteral("USB microphone reconnected; ASIO stream stayed active")
            : QStringLiteral(
                "USB microphone disconnected; ASIO guitar/output remain active while it reconnects");
    }
    if (peerTransport_) {
        peerTelemetry_ = peerTransport_->telemetry();
        processRoomControlEvents();
        emit roomChanged();
    }
    if (tunerActive_) {
        tunerReading_ = audioService_->tunerReading();
        emit tunerChanged();
    }
    const auto previousRecorder = recorderTelemetry_;
    recorderTelemetry_ = audioService_->recorderTelemetry();
    if (previousRecorder.recording != recorderTelemetry_.recording
        || previousRecorder.elapsedSeconds != recorderTelemetry_.elapsedSeconds
        || previousRecorder.failed != recorderTelemetry_.failed
        || (previousRecorder.droppedFrames == 0U) != (recorderTelemetry_.droppedFrames == 0U)) {
        emit recordingChanged();
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
    case State::VersionMismatch:
        return QStringLiteral("Update required · this room uses a different JamLink build");
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
