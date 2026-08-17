// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QByteArray>
#include <QClipboard>
#include "jamlink/diagnostics/session_log.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QRandomGenerator>
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

bool exactBuildIdentityReady(const QString& value) noexcept {
    if (value.size() != 40) {
        return false;
    }
    for (const QChar character : value) {
        const QChar lower = character.toLower();
        if (!character.isDigit()
            && (lower < QLatin1Char('a') || lower > QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
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
      roomDirectory_(this),
      updateManager_(
          QStringLiteral(JAMLINK_VERSION_STRING),
          QStringLiteral(JAMLINK_RELEASE_CHANNEL_STRING),
          visualFixture,
          this),
      currentPage_(std::move(initialPage)),
      visualFixture_(visualFixture),
      visualClipFixture_(visualFixture && qEnvironmentVariableIsSet("JAMLINK_VISUAL_CLIP")),
      visualPrivateRoomFixture_(visualFixture
          ? qEnvironmentVariable("JAMLINK_VISUAL_PRIVATE_ROOM") : QString()),
      visualRecordingFixture_(visualFixture
          && qEnvironmentVariableIsSet("JAMLINK_VISUAL_RECORDING")),
      audioService_(std::move(audioService)) {
    connect(&updateManager_, &UpdateManager::changed, this, &AppController::updateChanged);
    connect(&roomDirectory_, &PrivateRoomDirectory::changed,
        this, &AppController::roomChanged);
    connect(&roomDirectory_, &PrivateRoomDirectory::inviteResolved,
        this, &AppController::joinDirectSession);
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
        installBufferSizeOptions({64U, 128U, 256U});
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
    // Primed here so the interface is never handed an empty answer before the
    // first telemetry tick, which on a machine with no audio device may never
    // arrive at all.
    refreshSessionGuidance();
    telemetryTimer_.setInterval(33);
    connect(&telemetryTimer_, &QTimer::timeout, this, &AppController::pollAudioTelemetry);
    loadPreferences(widthOverride, heightOverride);
    if (automaticPage) {
        currentPage_ = restoredSetupAvailable_
            ? QStringLiteral("home")
            : QStringLiteral("soundcheck");
    }
    if (visualFixture_ && currentPage_ == QStringLiteral("room")) {
        visualRoomFixture_ = true;
        bool participantCountValid = false;
        const int requestedParticipantCount = qEnvironmentVariableIntValue(
            "JAMLINK_VISUAL_ROOM_SIZE", &participantCountValid);
        if (participantCountValid) {
            visualRoomParticipantCount_ = std::clamp(requestedParticipantCount, 2, 8);
        }
        peerTelemetry_.state = visualPrivateRoomFixture_.startsWith(QStringLiteral("host"))
            ? jamlink::network::PeerConnectionState::WaitingForPeer
            : jamlink::network::PeerConnectionState::Connected;
        peerTelemetry_.roundTripMicroseconds = 15'000U;
        peerTelemetry_.packetsSent = 2'840U;
        peerTelemetry_.packetsReceived = 2'836U;
        peerTelemetry_.streams[instrumentStream].peak = 0.72F;
        peerTelemetry_.streams[instrumentStream].playing = true;
        peerTelemetry_.streams[voiceStream].peak = 0.48F;
        peerTelemetry_.streams[voiceStream].playing = true;
        if (visualPrivateRoomFixture_.startsWith(QStringLiteral("host"))) {
            peerTelemetry_.udpBound = true;
            peerTelemetry_.publicAddressDiscovery =
                jamlink::network::PublicAddressDiscoveryState::Succeeded;
            peerTelemetry_.portMapping = jamlink::network::PortMappingState::Succeeded;
            peerTelemetry_.reachability =
                jamlink::network::ReachabilityAssessment::LikelyReachable;
            connectionPreflight_ = jamlink::network::evaluateConnectionPreflight({
                true, true, true, true,
                peerTelemetry_.publicAddressDiscovery,
                peerTelemetry_.portMapping,
                peerTelemetry_.reachability});
        }
        remoteParticipant_.profileId = "fixture:friend";
        remoteParticipant_.handle = "mike";
        remoteParticipant_.displayName = "Mike";
        remoteParticipant_.avatarId = "avatar:guitar-electric";
        remoteParticipant_.primaryInstrument = "Guitar";
        if (visualPrivateRoomFixture_.startsWith(QStringLiteral("host"))) {
            visualWaitingRoomRequests_.push_back(QVariantMap{
                {QStringLiteral("request_id"), QStringLiteral("fixture-request")},
                {QStringLiteral("display_name"), QStringLiteral("Mike <img>")},
                {QStringLiteral("primary_instrument"), QStringLiteral("Guitar & Voice")},
                {QStringLiteral("avatar_id"), QStringLiteral("avatar:guitar-electric")},
            });
        }
    }
    if (visualFixture_ && currentPage_ == QStringLiteral("tuner")) {
        tunerReading_ = {true, 329.25, 64, -2.0, 0.58F, 0.94F};
    }
    if (!visualFixture_ && devicesAvailable_
        && (!restoredPreferences_ || restoredSetupAvailable_)) {
        audioRestartTimer_.start(0);
    }
    if (!visualFixture_) {
        // Known before the first invite is created, so a musician is told what
        // to fix rather than discovering it when nobody can reach them.
        checkFirewall();
    }
    if (!visualFixture_) {
        // The first event-loop turn gives QML time to display, then version
        // discovery begins without requiring the user to visit Settings.
        QTimer::singleShot(0, &updateManager_, &UpdateManager::checkNow);
    }
}

AppController::~AppController() {
    roomDirectory_.stop();
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
        if (page == QStringLiteral("tuner")) {
            tunerReturnPage_ = currentPage_ == QStringLiteral("room")
                ? QStringLiteral("room") : QStringLiteral("home");
        }
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
    applyLocalSendMutes();
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
    applyLocalSendMutes();
    scheduleSave();
    emit tunerChanged();
}

// The single writer of the per-stream outgoing mutes.
//
// Two separate intents land on the same flag: "give me a second to tune",
// which stops the instrument reaching the room while voice keeps flowing, and
// the musician's own channel mutes. Each has to be OR-ed in here rather than
// written directly, because whichever wrote last would otherwise silently undo
// the other -- closing the tuner would have un-muted a guitar the player had
// deliberately muted, with nothing on screen changing to say so.
void AppController::applyLocalSendMutes() {
    if (!peerTransport_) {
        return;
    }
    peerTransport_->setLocalStreamMuted(
        jamlink::network::AudioStreamId::Instrument,
        instrumentSendMuted_
            || (tunerActive_ && preferences_.tunerMutesInstrument));
    peerTransport_->setLocalStreamMuted(
        jamlink::network::AudioStreamId::Voice, voiceSendMuted_);
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

QVariantList AppController::roomParticipants() const {
    QVariantList participants;
    participants.reserve(visualRoomFixture_ ? visualRoomParticipantCount_ : 2);

    QVariantMap local;
    local.insert(QStringLiteral("participantId"), profileId());
    local.insert(QStringLiteral("displayName"), QStringLiteral("You"));
    local.insert(QStringLiteral("avatarId"), profileAvatarId());
    local.insert(QStringLiteral("customAvatarSource"), profileCustomAvatarSource());
    local.insert(QStringLiteral("instrument"), profilePrimaryInstrument());
    local.insert(QStringLiteral("local"), true);
    local.insert(QStringLiteral("present"), roomActive());
    const bool localInstrumentMuted = sendMuted_ || instrumentSendMuted_
        || (tunerActive_ && preferences_.tunerMutesInstrument);
    const bool localVoiceMuted = sendMuted_ || voiceSendMuted_;
    local.insert(QStringLiteral("stateLabel"),
        localInstrumentMuted && localVoiceMuted ? QStringLiteral("MUTED")
                                                : QStringLiteral("LIVE"));
    local.insert(QStringLiteral("accent"), QStringLiteral("#2ac59a"));
    local.insert(QStringLiteral("surface"), QStringLiteral("#0e1819"));
    local.insert(QStringLiteral("instrumentLevel"), instrumentLevel());
    local.insert(QStringLiteral("voiceLevel"), voiceLevel());
    local.insert(QStringLiteral("instrumentGain"), instrumentMonitorGain());
    local.insert(QStringLiteral("voiceGain"), voiceMonitorGain());
    local.insert(QStringLiteral("instrumentMuted"), localInstrumentMuted);
    local.insert(QStringLiteral("voiceMuted"), localVoiceMuted);
    local.insert(QStringLiteral("controlsEnabled"), true);
    // A send mute needs a transport to act on, so the switch is only offered
    // once there is one rather than sitting there doing nothing.
    local.insert(QStringLiteral("canMute"), peerTransport_ != nullptr);
    local.insert(QStringLiteral("instrumentMutedByPeer"), false);
    local.insert(QStringLiteral("voiceMutedByPeer"), false);
    participants.push_back(local);

    QVariantMap remote;
    const QString remoteId = remoteParticipant_.profileId.empty()
        ? QStringLiteral("room:waiting-friend")
        : QString::fromStdString(remoteParticipant_.profileId);
    remote.insert(QStringLiteral("participantId"), remoteId);
    remote.insert(QStringLiteral("displayName"), peerConnected()
        ? remoteDisplayName() : QStringLiteral("Friend"));
    remote.insert(QStringLiteral("avatarId"), remoteAvatarId().isEmpty()
        ? QStringLiteral("avatar:listener") : remoteAvatarId());
    remote.insert(QStringLiteral("customAvatarSource"), QUrl());
    remote.insert(QStringLiteral("instrument"), peerConnected()
        ? remotePrimaryInstrument() : QStringLiteral("Instrument"));
    remote.insert(QStringLiteral("local"), false);
    remote.insert(QStringLiteral("present"), peerConnected());
    remote.insert(QStringLiteral("stateLabel"), peerConnected()
        ? QStringLiteral("HERE") : QStringLiteral("WAITING"));
    remote.insert(QStringLiteral("accent"), QStringLiteral("#a667e8"));
    remote.insert(QStringLiteral("surface"), QStringLiteral("#15131b"));
    remote.insert(QStringLiteral("instrumentLevel"), remoteInstrumentLevel());
    remote.insert(QStringLiteral("voiceLevel"), remoteVoiceLevel());
    remote.insert(QStringLiteral("instrumentGain"), remoteInstrumentGain());
    remote.insert(QStringLiteral("voiceGain"), remoteVoiceGain());
    remote.insert(QStringLiteral("instrumentMuted"), remoteInstrumentMuted_);
    remote.insert(QStringLiteral("voiceMuted"), remoteVoiceMuted_);
    remote.insert(QStringLiteral("controlsEnabled"), peerConnected());
    remote.insert(QStringLiteral("canMute"), peerConnected());
    // Kept separate from instrumentMuted, which is this machine's own choice to
    // silence them. Folding the two together would leave a switch that says
    // "off" and cannot turn the audio back on.
    remote.insert(QStringLiteral("instrumentMutedByPeer"),
        peerConnected() && peerTelemetry_.streams[instrumentStream].mutedByPeer);
    remote.insert(QStringLiteral("voiceMutedByPeer"),
        peerConnected() && peerTelemetry_.streams[voiceStream].mutedByPeer);
    participants.push_back(remote);

    if (!visualRoomFixture_ && remoteParticipants_.size() > 1U) {
        static const std::array<const char*, 6U> accents{
            "#e7b84b", "#3f9ee8", "#e26f9f", "#dd7b45", "#72c16c", "#4cc4c0"};
        for (std::size_t index = 1U; index < remoteParticipants_.size(); ++index) {
            const auto& participant = remoteParticipants_[index];
            QVariantMap additional;
            additional.insert(QStringLiteral("participantId"),
                QString::fromStdString(participant.profileId));
            additional.insert(QStringLiteral("displayName"),
                QString::fromStdString(participant.displayName));
            additional.insert(QStringLiteral("avatarId"),
                QString::fromStdString(participant.avatarId));
            additional.insert(QStringLiteral("customAvatarSource"), QUrl());
            additional.insert(QStringLiteral("instrument"),
                QString::fromStdString(participant.primaryInstrument));
            additional.insert(QStringLiteral("local"), false);
            additional.insert(QStringLiteral("present"), true);
            additional.insert(QStringLiteral("stateLabel"), QStringLiteral("HERE"));
            additional.insert(QStringLiteral("accent"),
                QString::fromLatin1(accents[(index - 1U) % accents.size()]));
            additional.insert(QStringLiteral("surface"), QStringLiteral("#13171c"));
            additional.insert(QStringLiteral("instrumentLevel"), 0.0);
            additional.insert(QStringLiteral("voiceLevel"), 0.0);
            additional.insert(QStringLiteral("instrumentGain"), 1.0);
            additional.insert(QStringLiteral("voiceGain"), 1.0);
            additional.insert(QStringLiteral("instrumentMuted"), false);
            additional.insert(QStringLiteral("voiceMuted"), false);
            additional.insert(QStringLiteral("controlsEnabled"), false);
            additional.insert(QStringLiteral("canMute"), false);
            additional.insert(QStringLiteral("instrumentMutedByPeer"), false);
            additional.insert(QStringLiteral("voiceMutedByPeer"), false);
            participants.push_back(additional);
        }
    }

    if (visualRoomFixture_) {
        static const std::array<const char*, 6U> names{
            "Chris", "Sam", "Riley", "Jordan", "Taylor", "Casey"};
        static const std::array<const char*, 6U> instruments{
            "Drums", "Bass", "Keys", "Vocals", "Guitar", "Synth"};
        static const std::array<const char*, 6U> avatars{
            "avatar:drums", "avatar:bass", "avatar:keys", "avatar:vocals",
            "avatar:guitar-acoustic", "avatar:synth"};
        static const std::array<const char*, 6U> accents{
            "#e7b84b", "#3f9ee8", "#e26f9f", "#dd7b45", "#72c16c", "#4cc4c0"};
        for (int index = 2; index < visualRoomParticipantCount_; ++index) {
            const std::size_t fixtureIndex = static_cast<std::size_t>(index - 2);
            QVariantMap fixture;
            fixture.insert(QStringLiteral("participantId"),
                QStringLiteral("fixture:participant-%1").arg(index + 1));
            fixture.insert(QStringLiteral("displayName"),
                QString::fromLatin1(names[fixtureIndex]));
            fixture.insert(QStringLiteral("avatarId"),
                QString::fromLatin1(avatars[fixtureIndex]));
            fixture.insert(QStringLiteral("customAvatarSource"), QUrl());
            fixture.insert(QStringLiteral("instrument"),
                QString::fromLatin1(instruments[fixtureIndex]));
            fixture.insert(QStringLiteral("local"), false);
            fixture.insert(QStringLiteral("present"), true);
            fixture.insert(QStringLiteral("stateLabel"), QStringLiteral("HERE"));
            fixture.insert(QStringLiteral("accent"),
                QString::fromLatin1(accents[fixtureIndex]));
            fixture.insert(QStringLiteral("surface"), QStringLiteral("#13171c"));
            fixture.insert(QStringLiteral("instrumentLevel"), 0.31 + index * 0.07);
            fixture.insert(QStringLiteral("voiceLevel"), 0.18 + index * 0.04);
            fixture.insert(QStringLiteral("instrumentGain"), 0.72);
            fixture.insert(QStringLiteral("voiceGain"), 0.58);
            fixture.insert(QStringLiteral("instrumentMuted"), false);
            fixture.insert(QStringLiteral("voiceMuted"), false);
            // Fixture-only people prove layout scaling. They deliberately do
            // not expose working controls until a corresponding transport
            // session exists.
            fixture.insert(QStringLiteral("controlsEnabled"), false);
            fixture.insert(QStringLiteral("canMute"), false);
            fixture.insert(QStringLiteral("instrumentMutedByPeer"), false);
            fixture.insert(QStringLiteral("voiceMutedByPeer"), false);
            participants.push_back(fixture);
        }
    }
    return participants;
}

int AppController::roomParticipantCount() const noexcept {
    return visualRoomFixture_ ? visualRoomParticipantCount_
                              : std::max(2, 1 + static_cast<int>(remoteParticipants_.size()));
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

bool AppController::firewallNeedsAttention() const noexcept {
    return firewall_.state == FirewallRuleState::Missing
        || firewall_.state == FirewallRuleState::StalePath
        || firewall_.state == FirewallRuleState::PublicNetwork;
}

QString AppController::firewallMessage() const {
    switch (firewall_.state) {
    case FirewallRuleState::Missing:
        return QStringLiteral(
            "Windows Firewall needs permission for JamLink to receive musicians.");
    case FirewallRuleState::StalePath:
        return QStringLiteral(
            "Windows Firewall still points at an older copy of JamLink, so this "
            "build cannot receive musicians until it is repaired.");
    case FirewallRuleState::PublicNetwork:
        return QStringLiteral(
            "This network is marked Public, so Windows blocks incoming audio. "
            "Set it to Private in Windows network settings, then check again.");
    case FirewallRuleState::Allowed:
        return QStringLiteral("Windows Firewall allows JamLink to receive musicians.");
    case FirewallRuleState::NotEnforced:
        return QStringLiteral("Windows Firewall is off for this network.");
    case FirewallRuleState::Unknown:
        break;
    }
    return QStringLiteral("Windows Firewall state could not be read.");
}

// PublicNetwork is deliberately not fixable from here. Widening a listening
// rule onto a network the user marked Public is their decision, not something
// to do quietly behind one button.
bool AppController::firewallFixable() const noexcept {
    return firewall_.state == FirewallRuleState::Missing
        || firewall_.state == FirewallRuleState::StalePath;
}

void AppController::checkFirewall() {
    if (visualFixture_) {
        return;
    }
    firewall_ = queryFirewallAccess();
    JAMLINK_LOG("firewall", "state " + std::to_string(static_cast<int>(firewall_.state))
        + " on the " + firewall_.activeProfile.toStdString() + " profile, "
        + std::to_string(firewall_.matchingRules) + " matching rules, "
        + std::to_string(firewall_.staleRules) + " stale");
    emit firewallChanged();
}

void AppController::fixFirewall() {
    if (visualFixture_ || !firewallFixable()) {
        return;
    }
    firewallBusy_ = true;
    emit firewallChanged();
    const bool requested = requestFirewallRule();
    firewallBusy_ = false;
    // Always re-read. The helper's return value says what it attempted, not
    // what Windows now believes.
    firewall_ = queryFirewallAccess();
    if (firewall_.state == FirewallRuleState::Allowed) {
        firewallActionMessage_ = QStringLiteral("JamLink can now receive musicians.");
    } else if (!requested) {
        firewallActionMessage_ = QStringLiteral(
            "Permission was not granted, so incoming audio is still blocked.");
    } else {
        firewallActionMessage_ = QStringLiteral(
            "The rule was changed but Windows still reports JamLink as blocked.");
    }
    emit firewallChanged();
}

QString AppController::firewallActionMessage() const { return firewallActionMessage_; }
bool AppController::firewallBusy() const noexcept { return firewallBusy_; }

bool AppController::recording() const noexcept {
    return visualRecordingFixture_ || recorderTelemetry_.recording;
}

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
QString AppController::visualPrivateRoomFixture() const {
    return visualPrivateRoomFixture_;
}
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
bool AppController::allReady() const noexcept {
    return readiness_.allVerified()
        && !instrumentInputClipped()
        && !voiceInputClipped()
        && !instrumentSendClipped()
        && !voiceSendClipped()
        && !outputClipped();
}
QString AppController::readinessLabel() const {
    if (!devicesAvailable_) {
        return QStringLiteral("Offline");
    }
    if (instrumentInputClipped() || voiceInputClipped()
        || instrumentSendClipped() || voiceSendClipped() || outputClipped()) {
        return QStringLiteral("Clipping");
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

// A number of samples means nothing to most musicians. What they can act on is
// the delay it causes and whether it is likely to hold up, so both are said
// plainly and the raw number is kept for anyone who wants it.
double AppController::bufferLatencyMilliseconds(std::uint32_t frames) const noexcept {
    const std::uint32_t rate = currentSampleRate();
    if (rate == 0U || frames == 0U) {
        return 0.0;
    }
    // One buffer in and one buffer out: what a player actually hears.
    return static_cast<double>(frames) * 2'000.0 / static_cast<double>(rate);
}

std::uint32_t AppController::currentSampleRate() const noexcept {
    if (!sampleRateValues_.empty() && validIndex(sampleRateIndex_, sampleRateValues_.size())) {
        return sampleRateValues_[static_cast<std::size_t>(sampleRateIndex_)];
    }
    return 48'000U;
}

QStringList AppController::bufferSizes() const {
    QStringList result;
    result.reserve(static_cast<qsizetype>(bufferSizeValues_.size()));
    for (const auto value : bufferSizeValues_) {
        if (value == 0U) {
            result.push_back(QStringLiteral("Auto · lowest that stays clean"));
            continue;
        }
        const double milliseconds = bufferLatencyMilliseconds(value);
        QString verdict;
        // Thresholds chosen from what playing together actually tolerates:
        // under about 6 ms is indistinguishable from an amp in the room, and
        // past roughly 25 ms a player starts fighting their own timing.
        if (milliseconds <= 6.0) {
            verdict = QStringLiteral("lowest delay");
        } else if (milliseconds <= 12.0) {
            verdict = QStringLiteral("low delay");
        } else if (milliseconds <= 25.0) {
            verdict = QStringLiteral("safe");
        } else {
            verdict = QStringLiteral("noticeable delay");
        }
        result.push_back(QStringLiteral("%1 · %2 ms · %3")
            .arg(value)
            .arg(milliseconds, 0, 'f', 1)
            .arg(verdict));
    }
    return result;
}

QString AppController::bufferSizeExplanation() const {
    if (!validIndex(bufferSizeIndex_, bufferSizeValues_.size())) {
        return QStringLiteral("Choose a buffer size to see the delay it causes.");
    }
    if (automaticBufferSize()) {
        // The figure has to be the size actually in use, not the setting's
        // name, or the one number a player needs is the one number missing.
        return QStringLiteral(
            "Currently %1 frames, about %2 ms between playing a note and hearing "
            "it back. JamLink opens your device at the smallest size it offers "
            "and moves up only when the device reports that it dropped audio. %3")
            .arg(effectiveBufferFrames())
            .arg(bufferLatencyMilliseconds(effectiveBufferFrames()), 0, 'f', 1)
            .arg(autoBufferRaised_
                ? QStringLiteral("It has had to move up during this session.")
                : QStringLiteral("It has not had to move up so far."));
    }
    const std::uint32_t frames = effectiveBufferFrames();
    const double milliseconds = bufferLatencyMilliseconds(frames);
    const QString delay = QStringLiteral(
        "About %1 ms between playing a note and hearing it back through JamLink.")
        .arg(milliseconds, 0, 'f', 1);
    const QString guidance = milliseconds <= 6.0
        ? QStringLiteral(
            " That is as close to playing through an amp in the room as this gets. "
            "If you hear crackling, choose the next size up.")
        : milliseconds <= 12.0
            ? QStringLiteral(
                " Comfortable for playing together. Go smaller if your computer copes.")
            : milliseconds <= 25.0
                ? QStringLiteral(
                    " Reliable, but you may feel it. Go smaller if there is no crackling.")
                : QStringLiteral(
                    " Large enough to be distracting while playing. Choose a smaller "
                    "size unless you need it to stop crackling.");
    return delay + guidance;
}

QString AppController::sampleRateExplanation() const {
    // Offering other rates would be a false choice: JamLink sends at 48 kHz, so
    // anything else would add a conversion stage and increase delay rather than
    // reduce it. Better to say why than to present a control that cannot help.
    const bool shared = !asioActive();
    return shared
        ? QStringLiteral(
            "Windows fixes this for shared audio. Change it in Windows sound "
            "settings if you need to. JamLink sends at 48 kHz either way.")
        : QStringLiteral(
            "JamLink records, sends, and plays at 48 kHz. Staying here avoids a "
            "conversion step, so it is both the cleanest and the fastest choice.");
}

bool AppController::asioActive() const noexcept {
    if (!validIndex(outputIndex_, outputOptions_.size())) {
        return false;
    }
    return outputOptions_[static_cast<std::size_t>(outputIndex_)].serviceOption.backend
        == jamlink::audio::SoundcheckBackend::Asio;
}

// Gathers what each subsystem already knows and lets the conductor decide what
// it means. Nothing is judged here; this is only collection.
void AppController::refreshSessionGuidance() {
    jamlink::control::SessionEvidence evidence;

    evidence.sessionRequested = roomActive();
    evidence.joiningRatherThanHosting = !inviteCode_.isEmpty() && !privateRoomWaiting();

    evidence.audioDevicesPresent = devicesAvailable_;
    evidence.audioRunning = audioActive();
    evidence.soundCheckVerified = allReady();
    // A device that was working and vanished, which is recoverable and must not
    // read the same as never having had one.
    evidence.audioDeviceLost = devicesAvailable_
        && (audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::DeviceInvalidated
            || audioTelemetry_.state == jamlink::audio::SoundcheckAudioState::DeviceUnavailable);
    evidence.inputClipping = instrumentInputClipped() || voiceInputClipped();
    const std::uint64_t dropouts = audioTelemetry_.underruns + audioTelemetry_.overruns;
    evidence.recentAudioDropouts = dropouts > conductorDropoutBaseline_
        ? dropouts - conductorDropoutBaseline_ : 0U;

    evidence.udpBound = peerTelemetry_.udpBound;
    evidence.firewallBlocking = firewall_.state == FirewallRuleState::Missing
        || firewall_.state == FirewallRuleState::StalePath;
    evidence.portMappingRefused =
        peerTelemetry_.portMapping == jamlink::network::PortMappingState::Failed;
    evidence.publicAddressKnown = peerTelemetry_.publicAddressDiscovery
        == jamlink::network::PublicAddressDiscoveryState::Succeeded;
    evidence.canJoinButNotHost = connectionPreflight_.outcome
        == jamlink::network::ConnectionPreflightOutcome::JoinOnly;

    evidence.peerAuthenticated = peerConnected();
    if (evidence.peerAuthenticated) {
        peerHasConnected_ = true;
    }
    evidence.peerWasConnected = peerHasConnected_;
    evidence.buildCompatible = !buildIncompatible_;
    evidence.transportFailed =
        peerTelemetry_.state == jamlink::network::PeerConnectionState::SocketFailed;

    // The distinction a socket cannot make: audio is actually moving.
    evidence.mediaProgressing = peerConnected() && !qualityWindow_.stalled;
    evidence.concealedPerThousand =
        static_cast<std::uint32_t>(qualityWindow_.concealRatio * 1'000.0);
    const auto& instrument = peerTelemetry_.streams[instrumentStream];
    evidence.receiveBufferMilliseconds =
        static_cast<std::uint32_t>(instrument.bufferedFrames / 48U);
    evidence.roundTripMeasured = peerTelemetry_.roundTripMeasured;
    evidence.roundTripMilliseconds =
        static_cast<std::uint32_t>(roundTripMilliseconds());

    evidence.recording = recording();

    guidance_ = conductor_.update(
        evidence, static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
}

QString AppController::sessionHeadline() const {
    return QString::fromUtf8(
        guidance_.headline.data(), static_cast<qsizetype>(guidance_.headline.size()));
}

QString AppController::sessionExplanation() const {
    return QString::fromUtf8(
        guidance_.explanation.data(), static_cast<qsizetype>(guidance_.explanation.size()));
}

QString AppController::sessionActionLabel() const {
    return QString::fromUtf8(
        guidance_.actionLabel.data(), static_cast<qsizetype>(guidance_.actionLabel.size()));
}

bool AppController::sessionActionEnabled() const noexcept { return guidance_.actionEnabled; }
bool AppController::sessionPlayable() const noexcept { return guidance_.playable; }

QString AppController::sessionPhase() const {
    const auto name = jamlink::control::phaseName(guidance_.phase);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

// The interface offers one button and does not need to know what it does.
void AppController::takeSessionAction() {
    using jamlink::control::GuidanceAction;
    switch (guidance_.action) {
    case GuidanceAction::FinishSoundCheck:
        navigate(QStringLiteral("soundcheck"));
        break;
    case GuidanceAction::ReconnectAudioDevice:
        retryAudio();
        break;
    case GuidanceAction::LowerInputGain:
    case GuidanceAction::ChooseLargerBuffer:
        openSettings();
        break;
    case GuidanceAction::FixFirewall:
        fixFirewall();
        break;
    case GuidanceAction::SendInvite:
    case GuidanceAction::WaitForFriend:
    case GuidanceAction::AskFriendToHost:
        copyInvite();
        break;
    case GuidanceAction::Retry:
        restartAudio();
        break;
    case GuidanceAction::ReviewRecording:
        navigate(QStringLiteral("home"));
        break;
    case GuidanceAction::LeaveRoom:
        leaveSession();
        break;
    case GuidanceAction::None:
        break;
    }
}

QString AppController::monitorPathSummary() const {
    if (!audioActive()) {
        return QStringLiteral("Audio is not running.");
    }
    if (audioTelemetry_.outputBufferFrames == 0U) {
        // The device has not reported its running buffer, so there is no figure
        // to give. Zero would read as instant, which is the opposite of true.
        return QStringLiteral(
            "Waiting for the audio device to report the buffer it settled on.");
    }
    const double milliseconds =
        static_cast<double>(audioTelemetry_.outputBufferFrames) * 2'000.0
        / static_cast<double>(currentSampleRate());
    // A device that is dropping blocks is the one thing that matters more than
    // the figure above, because it is audible and the remedy is a setting on
    // this screen.
    const QString dropouts = audioTelemetry_.underruns + audioTelemetry_.overruns > 0U
        ? QStringLiteral(
              " Your device has dropped %1 block(s) of audio, which is heard as "
              "clicks or crackle. Choose a larger buffer size.")
              .arg(audioTelemetry_.underruns + audioTelemetry_.overruns)
        : QString();
    if (asioActive()) {
        return QStringLiteral(
            "ASIO · about %1 ms to hear yourself. Your guitar goes straight from "
            "the input to your headphones in one step.%2")
            .arg(milliseconds, 0, 'f', 1)
            .arg(dropouts);
    }
    // Shared mode has a floor Windows imposes and a conversion JamLink cannot
    // avoid, so the honest advice is to change backend or use the interface.
    return QStringLiteral(
        "Windows shared audio · about %1 ms to hear yourself. Windows will not go "
        "lower than this. For less delay, choose an ASIO device above, or turn "
        "JamLink's monitor off and use your interface's own monitoring.%2")
        .arg(milliseconds, 0, 'f', 1)
        .arg(dropouts);
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
double AppController::instrumentPeakHold() const noexcept {
    return visualFixture_
        ? (visualClipFixture_ ? 1.0 : 0.82)
        : audioTelemetry_.instrumentInput.peakHold;
}
double AppController::voicePeakHold() const noexcept {
    return visualFixture_ ? 0.61 : audioTelemetry_.voiceInput.peakHold;
}
double AppController::outputPeakHold() const noexcept {
    return visualFixture_ ? 0.58 : audioTelemetry_.monitorMix.peakHold;
}
bool AppController::instrumentInputClipped() const noexcept {
    return visualFixture_ ? visualClipFixture_ : audioTelemetry_.instrumentInput.clipped;
}
bool AppController::voiceInputClipped() const noexcept {
    return !visualFixture_ && audioTelemetry_.voiceInput.clipped;
}
bool AppController::instrumentSendClipped() const noexcept {
    return !visualFixture_ && audioTelemetry_.instrumentSend.clipped;
}
bool AppController::voiceSendClipped() const noexcept {
    return !visualFixture_ && audioTelemetry_.voiceSend.clipped;
}
bool AppController::outputClipped() const noexcept {
    return !visualFixture_ && audioTelemetry_.monitorMix.clipped;
}

QString AppController::signalStatus(
    const jamlink::audio::SignalHealthTelemetry& health,
    bool active) {
    if (!active) {
        return QStringLiteral("UNAVAILABLE");
    }
    if (health.clipped || health.invalidSamples != 0U) {
        if (health.diagnosticClip) {
            return QStringLiteral("TEST CLIP");
        }
        if (health.nearFullScaleRisk) {
            return QStringLiteral("CLIP RISK");
        }
        return QStringLiteral("CLIPPING");
    }
    if (health.currentPeak < 1.0e-5F) {
        return QStringLiteral("NO SIGNAL");
    }
    if (health.currentPeak < 0.01F) {
        return QStringLiteral("TOO QUIET");
    }
    if (health.currentPeak < 0.501187F) {
        return QStringLiteral("GOOD");
    }
    if (health.currentPeak < 0.891251F) {
        return QStringLiteral("HOT");
    }
    return QStringLiteral("NEAR CLIP");
}

QString AppController::instrumentSignalStatus() const {
    if (visualFixture_) {
        return visualClipFixture_ ? QStringLiteral("CLIPPING") : QStringLiteral("HOT");
    }
    return signalStatus(audioTelemetry_.instrumentInput, audioActive());
}
QString AppController::voiceSignalStatus() const {
    if (visualFixture_) {
        return QStringLiteral("GOOD");
    }
    return signalStatus(audioTelemetry_.voiceInput, audioActive());
}
QString AppController::outputSignalStatus() const {
    if (visualFixture_) {
        return QStringLiteral("GOOD");
    }
    return signalStatus(audioTelemetry_.monitorMix, audioActive());
}

QString AppController::instrumentSignalGuidance() const {
    if (audioTelemetry_.instrumentInput.clipped
        && audioTelemetry_.instrumentInput.diagnosticClip) {
        return QStringLiteral("INDICATOR TEST · Click the red CLIP control to reset.");
    }
    if (audioTelemetry_.instrumentInput.clipped
        && audioTelemetry_.instrumentInput.nearFullScaleRisk) {
        const QString device = validIndex(instrumentIndex_, instrumentOptions_.size())
            ? instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)].displayName
            : QStringLiteral("your interface input");
        return QStringLiteral(
            "INPUT AT FULL SCALE · Lower %1 hardware gain, reset, and try again.")
            .arg(device);
    }
    if (instrumentInputClipped()) {
        const QString device = validIndex(instrumentIndex_, instrumentOptions_.size())
            ? instrumentOptions_[static_cast<std::size_t>(instrumentIndex_)].displayName
            : QStringLiteral("your interface input");
        return QStringLiteral("INPUT CLIPPED · Lower %1 hardware gain, reset, and try again.")
            .arg(device);
    }
    if (instrumentSendClipped()) {
        return QStringLiteral("SEND LEVEL CLIPPED · Lower this JamLink level.");
    }
    return QStringLiteral("Play your loudest chords; green or amber is healthy.");
}
QString AppController::voiceSignalGuidance() const {
    if (audioTelemetry_.voiceInput.clipped
        && audioTelemetry_.voiceInput.diagnosticClip) {
        return QStringLiteral("INDICATOR TEST · Click the red CLIP control to reset.");
    }
    if (audioTelemetry_.voiceInput.clipped
        && audioTelemetry_.voiceInput.nearFullScaleRisk) {
        const QString device = validIndex(voiceIndex_, voiceOptions_.size())
            ? voiceOptions_[static_cast<std::size_t>(voiceIndex_)].displayName
            : QStringLiteral("your microphone input");
        return QStringLiteral(
            "INPUT AT FULL SCALE · Lower %1 hardware gain, reset, and try again.")
            .arg(device);
    }
    if (voiceInputClipped()) {
        const QString device = validIndex(voiceIndex_, voiceOptions_.size())
            ? voiceOptions_[static_cast<std::size_t>(voiceIndex_)].displayName
            : QStringLiteral("your microphone input");
        return QStringLiteral("INPUT CLIPPED · Lower %1 hardware gain, reset, and try again.")
            .arg(device);
    }
    if (voiceSendClipped()) {
        return QStringLiteral("SEND LEVEL CLIPPED · Lower this JamLink level.");
    }
    return QStringLiteral("Speak or sing at your loudest expected level.");
}
QString AppController::outputSignalGuidance() const {
    if (audioTelemetry_.monitorMix.clipped
        && audioTelemetry_.monitorMix.diagnosticClip) {
        return QStringLiteral("INDICATOR TEST · Click the red CLIP control to reset.");
    }
    return outputClipped()
        ? QStringLiteral(
            "MONITOR MIX TOO HOT · Reduce one or more listening levels, then reset.")
        : QStringLiteral("Monitor mix has headroom.");
}

bool AppController::roomActive() const noexcept {
    return peerTransport_ != nullptr || roomDirectory_.active() || visualRoomFixture_;
}
bool AppController::peerConnected() const noexcept {
    return peerTelemetry_.state == jamlink::network::PeerConnectionState::Connected;
}
QString AppController::roomStatus() const {
    if (visualRoomFixture_) {
        return QStringLiteral("Connected · encrypted small-room session · Excellent");
    }
    if (!peerTransport_) {
        if (roomDirectory_.active()) {
            return roomDirectory_.status();
        }
        return QStringLiteral("No room session");
    }
    if (peerConnected()) {
        return QStringLiteral("Connected · encrypted direct audio · %1")
            .arg(connectionQuality());
    }
    if (peerTelemetry_.state == jamlink::network::PeerConnectionState::WaitingForPeer
        && connectionPreflight_.outcome
            != jamlink::network::ConnectionPreflightOutcome::NotRun) {
        return QStringLiteral("Waiting for your friend · %1")
            .arg(connectionPreflightStatus());
    }
    return peerStateText(peerTelemetry_.state);
}
QString AppController::inviteCode() const {
    if (!visualPrivateRoomFixture_.isEmpty()) {
        return QStringLiteral("THEWONDERYEARS");
    }
    return roomDirectory_.hosting() && !roomDirectory_.roomCode().isEmpty()
        ? roomDirectory_.roomCode() : inviteCode_;
}
bool AppController::privateRoomCodesAvailable() const noexcept {
    return roomDirectory_.available()
        || visualPrivateRoomFixture_ == QStringLiteral("create");
}
bool AppController::privateRoomBusy() const noexcept { return roomDirectory_.busy(); }
bool AppController::privateRoomWaiting() const noexcept {
    return visualPrivateRoomFixture_ == QStringLiteral("guest-waiting")
        || roomDirectory_.waiting();
}
QString AppController::privateRoomCode() const {
    return visualPrivateRoomFixture_.isEmpty()
        ? roomDirectory_.roomCode() : QStringLiteral("THEWONDERYEARS");
}
QString AppController::privateRoomStatus() const { return roomDirectory_.status(); }
QVariantList AppController::waitingRoomRequests() const {
    return visualWaitingRoomRequests_.isEmpty()
        ? roomDirectory_.waitingRequests() : visualWaitingRoomRequests_;
}
bool AppController::automaticPortMapping() const noexcept {
    return peerTelemetry_.automaticPortMapping;
}
QString AppController::connectionPreflightStatus() const {
    using jamlink::network::ConnectionPreflightOutcome;
    switch (connectionPreflight_.outcome) {
    case ConnectionPreflightOutcome::Ready:
        return QStringLiteral("Ready");
    case ConnectionPreflightOutcome::DirectMayNeedHelp:
        return QStringLiteral("Direct connection may need help");
    case ConnectionPreflightOutcome::JoinOnly:
        return QStringLiteral("You can join, but not host");
    case ConnectionPreflightOutcome::RelayRequired:
        return QStringLiteral("Relay required");
    case ConnectionPreflightOutcome::Blocked:
        return QStringLiteral("Action needed");
    case ConnectionPreflightOutcome::NotRun:
        break;
    }
    return QStringLiteral("Connection check not run");
}
QString AppController::connectionPreflightDetail() const {
    using jamlink::network::ConnectionPreflightAction;
    using jamlink::network::PortMappingState;
    using jamlink::network::PublicAddressDiscoveryState;
    switch (connectionPreflight_.action) {
    case ConnectionPreflightAction::None:
        if (connectionPreflight_.outcome
            == jamlink::network::ConnectionPreflightOutcome::Ready) {
            return QStringLiteral(
                "Public address and automatic router mapping found. This indicates likely "
                "reachability; it is not a measured Internet connection. Your friend’s "
                "exact build is verified during the encrypted join.");
        }
        break;
    case ConnectionPreflightAction::FinishSoundCheck:
        return QStringLiteral("Finish Sound Check before starting a private jam.");
    case ConnectionPreflightAction::UseCurrentBuild:
        return QStringLiteral(
            "Install the current packaged JamLink build before hosting. Your friend’s exact "
            "build is verified during the encrypted join.");
    case ConnectionPreflightAction::ChooseAnotherUdpPort:
        return QStringLiteral(
            "JamLink could not open UDP port %1. Choose another port or allow JamLink through "
            "Windows Firewall.").arg(preferences_.preferredUdpPort);
    case ConnectionPreflightAction::EnableMappingOrForwardPort: {
        const QString publicCheck = peerTelemetry_.publicAddressDiscovery
                == PublicAddressDiscoveryState::Succeeded
            ? QStringLiteral("A public address was found")
            : peerTelemetry_.publicAddressDiscovery
                    == PublicAddressDiscoveryState::Failed
                ? QStringLiteral("Public-address discovery did not respond")
                : QStringLiteral("Public-address discovery was not run");
        const QString mappingCheck = peerTelemetry_.portMapping
                == PortMappingState::Failed
            ? QStringLiteral("automatic router mapping failed")
            : QStringLiteral("automatic router mapping is off");
        return QStringLiteral(
            "%1, but %2. Turn mapping on or forward UDP port %3. Internet reachability was "
            "not measured.")
            .arg(publicCheck, mappingCheck)
            .arg(roomPort() > 0 ? roomPort() : preferences_.preferredUdpPort);
    }
    case ConnectionPreflightAction::AskFriendToHost:
        return QStringLiteral(
            "Your router would not open a port for JamLink, so your friend is unlikely to "
            "reach an invite you create. Only the person who makes the invite needs an open "
            "port, so ask your friend to create it and paste it here instead. Forwarding UDP "
            "port %1 on your router is the alternative. Internet reachability was not "
            "measured.")
            .arg(roomPort() > 0 ? roomPort() : preferences_.preferredUdpPort);
    case ConnectionPreflightAction::CheckFirewall:
        return QStringLiteral(
            "The direct path may be blocked. Allow JamLink through Windows Firewall and "
            "forward the shown UDP port. Internet reachability was not measured.");
    case ConnectionPreflightAction::ConfigureRelay:
        return QStringLiteral(
            "Direct traversal was assessed as unavailable. A relay is not configured in this "
            "build, so no relay connection was attempted.");
    }
    return QStringLiteral(
        "Start a private jam to check audio, build identity, UDP, public address, and router "
        "mapping.");
}
bool AppController::connectionPreflightReady() const noexcept {
    return connectionPreflight_.outcome
        == jamlink::network::ConnectionPreflightOutcome::Ready;
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
bool AppController::remoteInstrumentClipped() const noexcept {
    return peerTelemetry_.streams[instrumentStream].sourceClipped;
}
bool AppController::remoteVoiceClipped() const noexcept {
    return peerTelemetry_.streams[voiceStream].sourceClipped;
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
    const bool haveRoundTrip = peerTelemetry_.roundTripMeasured;
    const double oneWayMilliseconds =
        static_cast<double>(peerTelemetry_.roundTripMicroseconds) / 2'000.0;
    const double bufferMilliseconds = instrument.bufferedFrames == 0U
        ? 0.0
        : static_cast<double>(instrument.bufferedFrames) / 48.0;
    const double playableMilliseconds = oneWayMilliseconds + bufferMilliseconds;

    // A stream that has stopped arriving parks playout past the last packet, so
    // its buffer reads zero and nothing is booked as concealment. Graded on
    // those numbers alone a silent link scores better than a working one, so
    // the absence of arrivals has to be stated outright.
    if (qualityWindow_.stalled) {
        // A muted stream sends nothing, which is indistinguishable from a
        // broken one unless the sender says so. It does, on the periodic
        // control packet, so this no longer reports a deliberate choice as a
        // fault and sends both of them looking for a connection problem.
        const auto& stalledVoice = peerTelemetry_.streams[voiceStream];
        if (instrument.mutedByPeer && stalledVoice.mutedByPeer) {
            return QStringLiteral("Your friend is muted · still connected");
        }
        if (instrument.mutedByPeer) {
            return QStringLiteral("Your friend muted their guitar · still connected");
        }
        return QStringLiteral("No audio from your friend · still connected");
    }

    QString grade;
    if (!qualityWindow_.hasRate) {
        grade = QStringLiteral("Measuring");
    } else if (qualityWindow_.concealRatio > 0.05 || playableMilliseconds > 60.0) {
        grade = QStringLiteral("Conversation only");
    } else if (qualityWindow_.concealRatio > 0.02 || playableMilliseconds > 40.0) {
        grade = QStringLiteral("Poor");
    } else if (qualityWindow_.concealRatio > 0.005 || playableMilliseconds > 25.0) {
        grade = QStringLiteral("Playable");
    } else if (playableMilliseconds > 15.0) {
        grade = QStringLiteral("Good");
    } else {
        grade = QStringLiteral("Excellent");
    }
    if (!haveRoundTrip) {
        return QStringLiteral("%1 · round trip not measured yet · %2 ms buffer")
            .arg(grade)
            .arg(bufferMilliseconds, 0, 'f', 1);
    }
    return QStringLiteral("%1 · about %2 ms one way · %3 ms buffer")
        .arg(grade)
        .arg(oneWayMilliseconds, 0, 'f', 1)
        .arg(bufferMilliseconds, 0, 'f', 1);
}

// The grade has to describe the last few seconds. Lifetime totals go deaf: an
// hour of clean play makes the denominator so large that a fresh dropout cannot
// move it at all.
void AppController::updateQualityWindow() {
    if (!peerTransport_ || !peerConnected()) {
        qualityWindow_ = {};
        qualitySamples_.clear();
        return;
    }
    if (!qualityClock_.isValid()) {
        qualityClock_.start();
    }
    const auto& instrument = peerTelemetry_.streams[instrumentStream];
    qualitySamples_.push_back(
        {qualityClock_.elapsed(), instrument.packetsAccepted, instrument.packetsConcealed});
    while (qualitySamples_.size() > 2U
           && qualitySamples_.back().elapsedMilliseconds - qualitySamples_.front().elapsedMilliseconds
               > qualityWindowMilliseconds) {
        qualitySamples_.pop_front();
    }

    const auto& oldest = qualitySamples_.front();
    const auto& newest = qualitySamples_.back();
    const qint64 span = newest.elapsedMilliseconds - oldest.elapsedMilliseconds;
    const std::uint64_t accepted = newest.packetsAccepted - oldest.packetsAccepted;
    const std::uint64_t concealed = newest.packetsConcealed - oldest.packetsConcealed;

    QualityWindow window;
    // Long enough that a couple of missing packets do not swing the grade.
    if (span >= 2'000) {
        // Nothing arrived for two seconds while the session is still up.
        window.stalled = accepted == 0U;
        const std::uint64_t expected = accepted + concealed;
        if (expected > 0U) {
            window.hasRate = true;
            window.concealRatio =
                static_cast<double>(concealed) / static_cast<double>(expected);
        }
    }
    qualityWindow_ = window;
}

QString AppController::networkDiagnostics() const {
    if (!peerTransport_) {
        if (connectionPreflight_.outcome
            == jamlink::network::ConnectionPreflightOutcome::NotRun) {
            return QStringLiteral("No room session");
        }
        return QStringLiteral("%1 · %2")
            .arg(connectionPreflightStatus(), connectionPreflightDetail());
    }
    const auto& instrument = peerTelemetry_.streams[instrumentStream];
    const auto& voice = peerTelemetry_.streams[voiceStream];
    const QString roundTrip = peerTelemetry_.roundTripMeasured
        ? QStringLiteral("round trip %1 ms measured")
              .arg(static_cast<double>(peerTelemetry_.roundTripMicroseconds) / 1'000.0, 0, 'f', 1)
        : QStringLiteral("round trip not measured yet");
    return QStringLiteral(
               "%1 · jitter %2 ms\n"
               "instrument concealed %3 · late %4 · buffer %5 ms\n"
               "voice concealed %6 · late %7 · buffer %8 ms")
        .arg(roundTrip)
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

void AppController::openSettings() {
    if (currentPage_ != QStringLiteral("settings")) {
        settingsReturnPage_ = currentPage_;
    }
    setCurrentPage(QStringLiteral("settings"));
}

void AppController::closeSettings() {
    // Returning to a room that has since ended would show an empty one.
    setCurrentPage(
        settingsReturnPage_ == QStringLiteral("room") && roomActive()
            ? QStringLiteral("room") : QStringLiteral("home"));
}

// What changing a setting costs while two people are playing.
//
// The audio device can be swapped without ending the session: the transport is
// untouched by a restart, and the backend dispatcher re-applies the peer
// exchange, tuner state and monitor controls every time it starts, so even
// moving from shared Windows audio to ASIO keeps the friend hearing you. It is
// still a real interruption, and saying so is better than a musician wondering
// whether they have just dropped the room.
QString AppController::settingsSessionNotice() const {
    if (!roomActive()) {
        return {};
    }
    return QStringLiteral(
        "You are in a room. Levels, monitoring and mute take effect as you "
        "change them. Choosing a different input, output or buffer size "
        "restarts the audio device, which is a short silence for both of you — "
        "the session stays connected and no new invite is needed.");
}

void AppController::closeTuner() {
    setCurrentPage(tunerReturnPage_ == QStringLiteral("room")
        ? QStringLiteral("room") : QStringLiteral("home"));
}

void AppController::saveSoundcheck() {
    bool saved = false;
    if (instrumentInputClipped() || voiceInputClipped()
        || instrumentSendClipped() || voiceSendClipped() || outputClipped()) {
        setupMessage_ = QStringLiteral(
            "Clipping must be corrected and reset before this setup is Ready to Jam");
    } else if (devicesAvailable_ && audioActive() && audioTelemetry_.secondaryVoiceActive) {
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
        saved = true;
    } else if (devicesAvailable_) {
        setupMessage_ = QStringLiteral("Start the private monitor before verifying this setup");
    } else {
        setupMessage_ = QStringLiteral("No compatible Windows audio endpoints are available");
    }
    persistNow();
    emit setupChanged();
    if (saved) {
        setCurrentPage(QStringLiteral("home"));
    }
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

void AppController::clearInstrumentClipping() {
    if (visualFixture_) {
        visualClipFixture_ = false;
        emit setupChanged();
        return;
    }
    if (!audioService_) {
        return;
    }
    audioService_->clearSignalHealth(jamlink::audio::SignalHealthPath::InstrumentInput);
    audioService_->clearSignalHealth(jamlink::audio::SignalHealthPath::InstrumentSend);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::clearVoiceClipping() {
    if (!audioService_) {
        return;
    }
    audioService_->clearSignalHealth(jamlink::audio::SignalHealthPath::VoiceInput);
    audioService_->clearSignalHealth(jamlink::audio::SignalHealthPath::VoiceSend);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::clearOutputClipping() {
    if (!audioService_) {
        return;
    }
    audioService_->clearSignalHealth(jamlink::audio::SignalHealthPath::MonitorMix);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::testInstrumentClipping() {
    if (visualFixture_) {
        visualClipFixture_ = true;
        emit setupChanged();
        return;
    }
    if (!audioService_ || !audioActive()) {
        return;
    }
    audioService_->requestSignalHealthSelfTest(
        jamlink::audio::SignalHealthPath::InstrumentInput);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::testVoiceClipping() {
    if (!audioService_ || !audioActive()) {
        return;
    }
    audioService_->requestSignalHealthSelfTest(
        jamlink::audio::SignalHealthPath::VoiceInput);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::testOutputClipping() {
    if (!audioService_ || !audioActive()) {
        return;
    }
    audioService_->requestSignalHealthSelfTest(
        jamlink::audio::SignalHealthPath::MonitorMix);
    audioTelemetry_ = audioService_->telemetry();
    emit setupChanged();
}

void AppController::hostSession() {
    jamlink::network::ConnectionPreflightChecks checks;
    checks.audioReady = !visualFixture_ && audioService_ && audioActive() && allReady();
    checks.buildIdentityReady = exactBuildIdentityReady(
        QStringLiteral(JAMLINK_BUILD_IDENTITY_STRING));
    checks.protocolIdentityReady =
        jamlink::network::currentMediaProtocolVersion != 0U
        && jamlink::network::currentControlProtocolVersion != 0U;
    connectionPreflight_ = jamlink::network::evaluateConnectionPreflight(checks);
    if (!checks.audioReady) {
        setupMessage_ = QStringLiteral("Verify the real private audio setup before hosting");
        setCurrentPage(QStringLiteral("soundcheck"));
        emit setupChanged();
        emit roomChanged();
        return;
    }
    if (!checks.buildIdentityReady || !checks.protocolIdentityReady) {
        setupMessage_ = connectionPreflightDetail();
        emit setupChanged();
        emit roomChanged();
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
        true,
        preferences_.automaticPortMapping);
    peerTelemetry_ = transport->telemetry();
    checks.udpBindSucceeded = peerTelemetry_.udpBound;
    checks.publicAddress = peerTelemetry_.publicAddressDiscovery;
    checks.portMapping = peerTelemetry_.portMapping;
    checks.reachability = peerTelemetry_.reachability;
    connectionPreflight_ = jamlink::network::evaluateConnectionPreflight(checks);
    if (invite.empty()) {
        setupMessage_ = peerTelemetry_.state == jamlink::network::PeerConnectionState::SocketFailed
            ? connectionPreflightDetail() : peerStateText(peerTelemetry_.state);
        emit setupChanged();
        emit roomChanged();
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
    applyLocalSendMutes();
    inviteCode_ = QString::fromStdString(invite);
    sendMuted_ = false;
    instrumentSendMuted_ = false;
    voiceSendMuted_ = false;
    peerHasConnected_ = false;
    buildIncompatible_ = false;
    conductor_.reset();
    restartAudio();
    setCurrentPage(QStringLiteral("room"));
    emit roomChanged();
}

QString AppController::generatePrivateInviteCode() const {
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    constexpr qsizetype alphabetLength = static_cast<qsizetype>(sizeof(alphabet) - 1U);
    QString code;
    code.reserve(9);
    QRandomGenerator* const generator = QRandomGenerator::system();
    for (qsizetype index = 0; index < 8; ++index) {
        if (index == 4) {
            code.push_back(QLatin1Char('-'));
        }
        code.push_back(QLatin1Char(alphabet[generator->bounded(alphabetLength)]));
    }
    return code;
}

void AppController::hostInviteCodeSession(const QString& roomCode) {
    if (!roomDirectory_.available()) {
        setupMessage_ = QStringLiteral(
            "Temporary invite codes are unavailable; use the full direct invite");
        emit setupChanged();
        return;
    }
    if (!roomDirectory_.acceptsCode(roomCode)) {
        setupMessage_ = QStringLiteral(
            "Use 4-64 letters, numbers, hyphens, or underscores for the invite code");
        emit setupChanged();
        return;
    }
    hostSession();
    if (peerTransport_ && !inviteCode_.isEmpty()) {
        roomDirectory_.create(roomCode, inviteCode_, roomCompatibility());
    }
}

void AppController::joinSession(const QString& inviteCode) {
    if (visualFixture_ || !audioService_ || !audioActive() || !allReady()) {
        setupMessage_ = QStringLiteral("Verify the real private audio setup before joining");
        setCurrentPage(QStringLiteral("soundcheck"));
        emit setupChanged();
        return;
    }
    const QString normalized = inviteCode.trimmed();
    if (!normalized.startsWith(QStringLiteral("JL1|"), Qt::CaseInsensitive)) {
        if (!roomDirectory_.available()) {
            setupMessage_ = QStringLiteral(
                "Temporary invite codes are unavailable; paste the full direct invite");
            emit setupChanged();
            return;
        }
        if (!roomDirectory_.acceptsCode(normalized)) {
            setupMessage_ = QStringLiteral(
                "Enter a valid 4-64 character temporary invite code");
            emit setupChanged();
            return;
        }
        leaveSession();
        roomDirectory_.requestJoin(
            normalized,
            roomCompatibility(),
            profileDisplayName(),
            profilePrimaryInstrument(),
            profileAvatarId());
        setCurrentPage(QStringLiteral("room"));
        emit roomChanged();
        return;
    }
    joinDirectSession(normalized);
}

void AppController::joinDirectSession(const QString& inviteCode) {
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
    applyLocalSendMutes();
    inviteCode_.clear();
    sendMuted_ = false;
    instrumentSendMuted_ = false;
    voiceSendMuted_ = false;
    peerHasConnected_ = false;
    buildIncompatible_ = false;
    conductor_.reset();
    restartAudio();
    setCurrentPage(QStringLiteral("room"));
    emit roomChanged();
}

void AppController::leaveSession() {
    if (!peerTransport_ && !roomDirectory_.active() && !roomDirectory_.hosting()) {
        if (connectionPreflight_.outcome
            != jamlink::network::ConnectionPreflightOutcome::NotRun) {
            connectionPreflight_ = {};
            emit roomChanged();
        }
        return;
    }
    roomDirectory_.stop();
    if (audioService_) {
        audioService_->stop();
        audioService_->setPeerAudioExchange(nullptr);
    }
    if (peerTransport_) {
        peerTransport_->stop();
        peerTransport_.reset();
    }
    peerTelemetry_ = {};
    qualitySamples_.clear();
    qualityWindow_ = {};
    qualityClock_.invalidate();
    connectionPreflight_ = {};
    remoteParticipant_ = {};
    remoteParticipants_.clear();
    inviteCode_.clear();
    sendMuted_ = false;
    instrumentSendMuted_ = false;
    voiceSendMuted_ = false;
    peerHasConnected_ = false;
    buildIncompatible_ = false;
    conductor_.reset();
    if (audioService_ && devicesAvailable_) {
        restartAudio();
    }
    setCurrentPage(QStringLiteral("home"));
    emit roomChanged();
}

void AppController::copyInvite() {
    const QString visibleInvite = inviteCode();
    if (!visibleInvite.isEmpty()) {
        QGuiApplication::clipboard()->setText(visibleInvite);
        setupMessage_ = QStringLiteral("Invite copied");
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

QJsonObject AppController::roomCompatibility() const {
    return {
        {QStringLiteral("application_version"), QStringLiteral(JAMLINK_VERSION_STRING)},
        {QStringLiteral("build_identity"), QStringLiteral(JAMLINK_BUILD_IDENTITY_STRING)},
        {QStringLiteral("release_channel"), QStringLiteral(JAMLINK_RELEASE_CHANNEL_STRING)},
        {QStringLiteral("media_protocol"),
            static_cast<int>(jamlink::network::currentMediaProtocolVersion)},
        {QStringLiteral("control_protocol"),
            static_cast<int>(jamlink::network::currentControlProtocolVersion)},
    };
}

void AppController::decideWaitingRequest(const QString& requestId, bool admit) {
    roomDirectory_.decide(requestId, admit);
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

void AppController::setRoomParticipantStreamGain(
    const QString& participantId, const QString& stream, double gain) {
    if (participantId == profileId()) {
        if (stream == QStringLiteral("instrument")) {
            setInstrumentMonitorGain(gain);
        } else if (stream == QStringLiteral("voice")) {
            setVoiceMonitorGain(gain);
        }
        return;
    }
    if (remoteParticipant_.profileId.empty()
        || participantId != QString::fromStdString(remoteParticipant_.profileId)) {
        return;
    }
    if (stream == QStringLiteral("instrument")) {
        setRemoteInstrumentGain(gain);
    } else if (stream == QStringLiteral("voice")) {
        setRemoteVoiceGain(gain);
    }
}

void AppController::setRoomParticipantStreamMuted(
    const QString& participantId, const QString& stream, bool muted) {
    const bool instrument = stream == QStringLiteral("instrument");
    if (!instrument && stream != QStringLiteral("voice")) {
        return;
    }
    if (participantId == profileId()) {
        // The musician's own channel. This stops the stream reaching the other
        // person while capture, meters, peak hold, clip latching, the tuner tap
        // and recording all continue, because every one of those sits upstream
        // of the transport. It is a different control from the monitor
        // switches, which only decide whether you hear yourself.
        if (instrument) {
            instrumentSendMuted_ = muted;
        } else {
            voiceSendMuted_ = muted;
        }
        applyLocalSendMutes();
        emit roomChanged();
        return;
    }
    // Anything that is not this machine is the friend, and muting them is a
    // local playback decision that needs no agreement from anyone.
    //
    // This used to require the identifier to equal the one the peer announced.
    // A session that drops clears remoteParticipant_ and only a fresh join
    // event restores it, so after any blip the control stayed enabled and did
    // nothing at all: the switch moved, the guard rejected it, and the next
    // model refresh snapped it back.
    if (instrument) {
        setRemoteInstrumentMuted(muted);
    } else {
        setRemoteVoiceMuted(muted);
    }
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
    const auto rememberParticipant = [this](
        const jamlink::network::PeerParticipantInfo& participant) {
        if (participant.profileId.empty()) {
            return;
        }
        const auto existing = std::find_if(
            remoteParticipants_.begin(), remoteParticipants_.end(),
            [&participant](const auto& value) {
                return value.profileId == participant.profileId;
            });
        if (existing != remoteParticipants_.end()) {
            *existing = participant;
        } else if (remoteParticipants_.size() < 12U) {
            remoteParticipants_.push_back(participant);
        }
    };
    for (const auto& event : peerTransport_->takeControlEvents()) {
        const QString displayName = event.participant.displayName.empty()
            ? QStringLiteral("Friend")
            : QString::fromStdString(event.participant.displayName);
        switch (event.type) {
        case jamlink::network::RoomControlEventType::PeerJoined:
            remoteParticipant_ = event.participant;
            rememberParticipant(event.participant);
            appendChatEntry(
                QString(), QString(), displayName + QStringLiteral(" joined"),
                event.timestampMilliseconds, false, true);
            roomUpdated = true;
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::PeerLeft:
            std::erase_if(remoteParticipants_, [&event](const auto& participant) {
                return participant.profileId == event.participant.profileId;
            });
            if (remoteParticipant_.profileId == event.participant.profileId) {
                remoteParticipant_ = remoteParticipants_.empty()
                    ? jamlink::network::PeerParticipantInfo{}
                    : remoteParticipants_.front();
            }
            appendChatEntry(
                QString(), QString(), displayName + QStringLiteral(" left"),
                event.timestampMilliseconds, false, true);
            roomUpdated = true;
            chatUpdated = true;
            break;
        case jamlink::network::RoomControlEventType::ChatMessage:
            remoteParticipant_ = event.participant;
            rememberParticipant(event.participant);
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
            updateManager_.checkNow();
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
    installBufferSizeOptions(option.bufferFrameOptions);
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
        effectiveBufferFrames(),
        preferences_.instrumentMonitorGain,
        preferences_.voiceMonitorGain,
        preferences_.instrumentMonitorEnabled,
        preferences_.voiceMonitorEnabled};
    static_cast<void>(audioService_->start(configuration));
    audioTelemetry_ = audioService_->telemetry();
    {
        // Monitoring delay is decided almost entirely by which backend is in
        // use and how large the buffer is, so a complaint about it has to be
        // answerable from the log rather than from memory.
        const auto backendName = [](jamlink::audio::SoundcheckBackend backend) {
            return backend == jamlink::audio::SoundcheckBackend::Asio
                ? "ASIO" : "WASAPI shared";
        };
        const std::uint32_t rate = audioTelemetry_.outputSampleRate == 0U
            ? 48'000U : audioTelemetry_.outputSampleRate;
        const double monitorMilliseconds =
            static_cast<double>(audioTelemetry_.outputBufferFrames) * 2'000.0
            / static_cast<double>(rate);
        JAMLINK_LOG("audio", std::string("instrument ")
            + backendName(configuration.instrument.backend) + ", voice "
            + backendName(configuration.voice.backend) + ", output "
            + backendName(configuration.output.backend)
            + "; requested buffer "
            + (automaticBufferSize()
                ? "automatic (" + std::to_string(effectiveBufferFrames()) + ")"
                : std::to_string(effectiveBufferFrames()))
            + ", running buffer " + std::to_string(audioTelemetry_.outputBufferFrames)
            + " at " + std::to_string(rate)
            + " Hz, about " + std::to_string(static_cast<int>(monitorMilliseconds + 0.5))
            + " ms round trip through the local monitor");
    }
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

// Zero heads the list and means automatic. It is a real setting rather than a
// label: the device is opened at the smallest size it offers, and only a device
// that actually reports dropping audio moves it up.
void AppController::installBufferSizeOptions(std::vector<std::uint32_t> deviceValues) {
    std::sort(deviceValues.begin(), deviceValues.end());
    deviceValues.erase(
        std::remove(deviceValues.begin(), deviceValues.end(), 0U), deviceValues.end());
    deviceValues.erase(
        std::unique(deviceValues.begin(), deviceValues.end()), deviceValues.end());
    if (deviceValues.empty()) {
        deviceValues = {480U};
    }
    autoBufferFrames_ = deviceValues.front();
    autoBufferRaised_ = false;
    bufferSizeValues_.clear();
    bufferSizeValues_.reserve(deviceValues.size() + 1U);
    bufferSizeValues_.push_back(0U);
    bufferSizeValues_.insert(
        bufferSizeValues_.end(), deviceValues.begin(), deviceValues.end());
    bufferSizeIndex_ = std::clamp(
        bufferSizeIndex_, 0, static_cast<int>(bufferSizeValues_.size() - 1U));
}

bool AppController::automaticBufferSize() const noexcept {
    return validIndex(bufferSizeIndex_, bufferSizeValues_.size())
        && bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)] == 0U;
}

std::uint32_t AppController::effectiveBufferFrames() const noexcept {
    if (!validIndex(bufferSizeIndex_, bufferSizeValues_.size())) {
        return autoBufferFrames_;
    }
    const std::uint32_t selected =
        bufferSizeValues_[static_cast<std::size_t>(bufferSizeIndex_)];
    return selected == 0U ? autoBufferFrames_ : selected;
}

// Automatic means "the smallest size this machine can actually hold", and the
// only honest evidence for that is the device reporting that it could not.
// Stepping up is therefore driven by measured dropouts rather than by guessing
// from the hardware, and it never steps back down within a session: a quiet
// stretch is not proof the smaller size would have held, and oscillating
// between two sizes would restart the audio device each time.
void AppController::considerAutomaticBufferStep(std::uint64_t dropoutsSinceLastReport) {
    if (!automaticBufferSize() || dropoutsSinceLastReport == 0U) {
        return;
    }
    const auto next = std::find_if(
        bufferSizeValues_.begin(), bufferSizeValues_.end(),
        [this](std::uint32_t value) { return value > autoBufferFrames_; });
    if (next == bufferSizeValues_.end()) {
        JAMLINK_LOG("audio", "automatic buffer size is already at the largest this "
            "device offers (" + std::to_string(autoBufferFrames_)
            + " frames) and audio is still being dropped");
        return;
    }
    JAMLINK_LOG("audio", "automatic buffer size raised from "
        + std::to_string(autoBufferFrames_) + " to " + std::to_string(*next)
        + " frames after the device dropped audio");
    autoBufferFrames_ = *next;
    autoBufferRaised_ = true;
    setupMessage_ = QStringLiteral(
        "Audio was dropping, so the buffer moved up to %1. You will hear a "
        "short gap.").arg(autoBufferFrames_);
    emit setupChanged();
    scheduleAudioRestart();
}

void AppController::reportAudioDeviceDropouts() {
    // The counters existed from the beginning and were shown nowhere, so
    // "which buffer size should I use" had no answer but guesswork, and a
    // device that was quietly dropping blocks looked identical to a bad
    // network. A drop-out here is local: it is this machine failing to hand
    // over audio in time, not anything the connection did.
    const std::uint64_t dropouts =
        audioTelemetry_.underruns + audioTelemetry_.overruns;
    if (dropouts <= reportedAudioDropouts_) {
        return;
    }
    // Rate limited so a struggling device cannot fill the log at the telemetry
    // poll rate, and so the figure quoted is a rate rather than a running
    // total the reader has to differentiate by hand.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (audioDropoutReportMilliseconds_ != 0
        && now - audioDropoutReportMilliseconds_ < 5'000) {
        return;
    }
    const std::uint64_t since = dropouts - reportedAudioDropouts_;
    const qint64 elapsed = audioDropoutReportMilliseconds_ == 0
        ? 0 : now - audioDropoutReportMilliseconds_;
    reportedAudioDropouts_ = dropouts;
    audioDropoutReportMilliseconds_ = now;
    JAMLINK_LOG("audio", "device dropped " + std::to_string(since)
        + " block(s)" + (elapsed > 0
            ? " in " + std::to_string(elapsed / 1'000) + " s" : "")
        + "; running buffer " + std::to_string(audioTelemetry_.outputBufferFrames)
        + " frames. The buffer is too small for this machine, or another"
        + " program is competing for the device.");
    considerAutomaticBufferStep(since);
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
    reportAudioDeviceDropouts();
    refreshSessionGuidance();
    if (peerTransport_) {
        peerTelemetry_ = peerTransport_->telemetry();
        updateQualityWindow();
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
