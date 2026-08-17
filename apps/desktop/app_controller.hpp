// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "firewall_access.hpp"

#include "jamlink/audio/soundcheck_audio_service.hpp"
#include "jamlink/control/readiness_tracker.hpp"
#include "jamlink/control/session_conductor.hpp"
#include "jamlink/network/peer_audio_transport.hpp"
#include "jamlink/preferences/preferences_store.hpp"
#include "private_room_directory.hpp"
#include "update_manager.hpp"

#include <QJsonObject>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <vector>

namespace jamlink::desktop {

class AppController final : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppController)
    QML_UNCREATABLE("Created by the JamLink application")
    Q_PROPERTY(QString currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(bool visualFixture READ visualFixture CONSTANT)
    Q_PROPERTY(QString visualPrivateRoomFixture READ visualPrivateRoomFixture CONSTANT)
    Q_PROPERTY(bool restoredPreferences READ restoredPreferences CONSTANT)
    Q_PROPERTY(bool devicesAvailable READ devicesAvailable NOTIFY setupChanged)
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY setupChanged)
    Q_PROPERTY(QString audioStatus READ audioStatus NOTIFY setupChanged)
    Q_PROPERTY(bool allReady READ allReady NOTIFY setupChanged)
    Q_PROPERTY(QString readinessLabel READ readinessLabel NOTIFY setupChanged)
    Q_PROPERTY(QString setupMessage READ setupMessage NOTIFY setupChanged)
    Q_PROPERTY(QString saveMessage READ saveMessage NOTIFY saveMessageChanged)

    Q_PROPERTY(QStringList instrumentDevices READ instrumentDevices CONSTANT)
    Q_PROPERTY(QStringList voiceDevices READ voiceDevices CONSTANT)
    Q_PROPERTY(QStringList outputDevices READ outputDevices CONSTANT)
    Q_PROPERTY(int instrumentDeviceIndex READ instrumentDeviceIndex WRITE setInstrumentDeviceIndex NOTIFY setupChanged)
    Q_PROPERTY(int voiceDeviceIndex READ voiceDeviceIndex WRITE setVoiceDeviceIndex NOTIFY setupChanged)
    Q_PROPERTY(int outputDeviceIndex READ outputDeviceIndex WRITE setOutputDeviceIndex NOTIFY setupChanged)

    Q_PROPERTY(QStringList sampleRates READ sampleRates NOTIFY setupChanged)
    Q_PROPERTY(QStringList bufferSizes READ bufferSizes NOTIFY setupChanged)
    Q_PROPERTY(int sampleRateIndex READ sampleRateIndex WRITE setSampleRateIndex NOTIFY setupChanged)
    Q_PROPERTY(int bufferSizeIndex READ bufferSizeIndex WRITE setBufferSizeIndex NOTIFY setupChanged)

    Q_PROPERTY(double instrumentMonitorGain READ instrumentMonitorGain WRITE setInstrumentMonitorGain NOTIFY setupChanged)
    Q_PROPERTY(double voiceMonitorGain READ voiceMonitorGain WRITE setVoiceMonitorGain NOTIFY setupChanged)
    Q_PROPERTY(bool instrumentMonitorEnabled READ instrumentMonitorEnabled WRITE setInstrumentMonitorEnabled NOTIFY setupChanged)
    Q_PROPERTY(bool voiceMonitorEnabled READ voiceMonitorEnabled WRITE setVoiceMonitorEnabled NOTIFY setupChanged)
    Q_PROPERTY(double instrumentLevel READ instrumentLevel NOTIFY setupChanged)
    Q_PROPERTY(double voiceLevel READ voiceLevel NOTIFY setupChanged)
    Q_PROPERTY(double outputLevel READ outputLevel NOTIFY setupChanged)
    Q_PROPERTY(double instrumentPeakHold READ instrumentPeakHold NOTIFY setupChanged)
    Q_PROPERTY(double voicePeakHold READ voicePeakHold NOTIFY setupChanged)
    Q_PROPERTY(double outputPeakHold READ outputPeakHold NOTIFY setupChanged)
    Q_PROPERTY(bool instrumentInputClipped READ instrumentInputClipped NOTIFY setupChanged)
    Q_PROPERTY(bool voiceInputClipped READ voiceInputClipped NOTIFY setupChanged)
    Q_PROPERTY(bool instrumentSendClipped READ instrumentSendClipped NOTIFY setupChanged)
    Q_PROPERTY(bool voiceSendClipped READ voiceSendClipped NOTIFY setupChanged)
    Q_PROPERTY(bool outputClipped READ outputClipped NOTIFY setupChanged)
    Q_PROPERTY(QString instrumentSignalStatus READ instrumentSignalStatus NOTIFY setupChanged)
    Q_PROPERTY(QString voiceSignalStatus READ voiceSignalStatus NOTIFY setupChanged)
    Q_PROPERTY(QString outputSignalStatus READ outputSignalStatus NOTIFY setupChanged)
    Q_PROPERTY(QString instrumentSignalGuidance READ instrumentSignalGuidance NOTIFY setupChanged)
    Q_PROPERTY(QString voiceSignalGuidance READ voiceSignalGuidance NOTIFY setupChanged)
    Q_PROPERTY(QString outputSignalGuidance READ outputSignalGuidance NOTIFY setupChanged)
    Q_PROPERTY(bool roomActive READ roomActive NOTIFY roomChanged)
    Q_PROPERTY(bool peerConnected READ peerConnected NOTIFY roomChanged)
    Q_PROPERTY(QString roomStatus READ roomStatus NOTIFY roomChanged)
    Q_PROPERTY(QString inviteCode READ inviteCode NOTIFY roomChanged)
    Q_PROPERTY(bool privateRoomCodesAvailable READ privateRoomCodesAvailable CONSTANT)
    Q_PROPERTY(bool privateRoomBusy READ privateRoomBusy NOTIFY roomChanged)
    Q_PROPERTY(bool privateRoomWaiting READ privateRoomWaiting NOTIFY roomChanged)
    Q_PROPERTY(QString privateRoomCode READ privateRoomCode NOTIFY roomChanged)
    Q_PROPERTY(QString privateRoomStatus READ privateRoomStatus NOTIFY roomChanged)
    Q_PROPERTY(QVariantList waitingRoomRequests READ waitingRoomRequests NOTIFY roomChanged)
    Q_PROPERTY(bool automaticPortMapping READ automaticPortMapping NOTIFY roomChanged)
    Q_PROPERTY(QString connectionPreflightStatus READ connectionPreflightStatus NOTIFY roomChanged)
    Q_PROPERTY(QString connectionPreflightDetail READ connectionPreflightDetail NOTIFY roomChanged)
    Q_PROPERTY(bool connectionPreflightReady READ connectionPreflightReady NOTIFY roomChanged)
    Q_PROPERTY(int roomPort READ roomPort NOTIFY roomChanged)
    Q_PROPERTY(int roundTripMilliseconds READ roundTripMilliseconds NOTIFY roomChanged)
    Q_PROPERTY(double remoteLevel READ remoteLevel NOTIFY roomChanged)
    Q_PROPERTY(double remoteInstrumentLevel READ remoteInstrumentLevel NOTIFY roomChanged)
    Q_PROPERTY(double remoteVoiceLevel READ remoteVoiceLevel NOTIFY roomChanged)
    Q_PROPERTY(bool remoteInstrumentClipped READ remoteInstrumentClipped NOTIFY roomChanged)
    Q_PROPERTY(bool remoteVoiceClipped READ remoteVoiceClipped NOTIFY roomChanged)
    Q_PROPERTY(double remoteInstrumentGain READ remoteInstrumentGain WRITE setRemoteInstrumentGain NOTIFY roomChanged)
    Q_PROPERTY(double remoteVoiceGain READ remoteVoiceGain WRITE setRemoteVoiceGain NOTIFY roomChanged)
    Q_PROPERTY(bool remoteInstrumentMuted READ remoteInstrumentMuted WRITE setRemoteInstrumentMuted NOTIFY roomChanged)
    Q_PROPERTY(bool remoteVoiceMuted READ remoteVoiceMuted WRITE setRemoteVoiceMuted NOTIFY roomChanged)
    Q_PROPERTY(QString connectionQuality READ connectionQuality NOTIFY roomChanged)
    Q_PROPERTY(QString networkDiagnostics READ networkDiagnostics NOTIFY roomChanged)
    Q_PROPERTY(QString packetSummary READ packetSummary NOTIFY roomChanged)
    Q_PROPERTY(bool sendMuted READ sendMuted WRITE setSendMuted NOTIFY roomChanged)

    Q_PROPERTY(QString recordingDirectory READ recordingDirectory WRITE setRecordingDirectory NOTIFY settingsChanged)
    Q_PROPERTY(int preferredUdpPort READ preferredUdpPort WRITE setPreferredUdpPort NOTIFY settingsChanged)
    Q_PROPERTY(bool automaticRouterMapping READ automaticRouterMapping WRITE setAutomaticRouterMapping NOTIFY settingsChanged)
    Q_PROPERTY(int latencyMode READ latencyMode WRITE setLatencyMode NOTIFY settingsChanged)
    Q_PROPERTY(QString latencyModeDetail READ latencyModeDetail NOTIFY settingsChanged)
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY updateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(bool updateBusy READ updateBusy NOTIFY updateChanged)
    Q_PROPERTY(double updateProgress READ updateProgress NOTIFY updateChanged)

    Q_PROPERTY(QString profileId READ profileId CONSTANT)
    Q_PROPERTY(QString profileHandle READ profileHandle WRITE setProfileHandle NOTIFY profileChanged)
    Q_PROPERTY(QString profileDisplayName READ profileDisplayName WRITE setProfileDisplayName NOTIFY profileChanged)
    Q_PROPERTY(QString profileAvatarId READ profileAvatarId WRITE setProfileAvatarId NOTIFY profileChanged)
    Q_PROPERTY(QUrl profileCustomAvatarSource READ profileCustomAvatarSource NOTIFY profileChanged)
    Q_PROPERTY(QString profilePrimaryInstrument READ profilePrimaryInstrument WRITE setProfilePrimaryInstrument NOTIFY profileChanged)
    Q_PROPERTY(QString profileGenres READ profileGenres WRITE setProfileGenres NOTIFY profileChanged)
    Q_PROPERTY(QString profileBio READ profileBio WRITE setProfileBio NOTIFY profileChanged)
    Q_PROPERTY(QString profileRegion READ profileRegion WRITE setProfileRegion NOTIFY profileChanged)
    Q_PROPERTY(QStringList profileAvatarIds READ profileAvatarIds CONSTANT)
    Q_PROPERTY(QStringList profileAvatarLabels READ profileAvatarLabels CONSTANT)
    Q_PROPERTY(QStringList profileInstrumentOptions READ profileInstrumentOptions CONSTANT)

    Q_PROPERTY(QString remoteDisplayName READ remoteDisplayName NOTIFY roomChanged)
    Q_PROPERTY(QString remoteHandle READ remoteHandle NOTIFY roomChanged)
    Q_PROPERTY(QString remoteAvatarId READ remoteAvatarId NOTIFY roomChanged)
    Q_PROPERTY(QString remotePrimaryInstrument READ remotePrimaryInstrument NOTIFY roomChanged)
    Q_PROPERTY(QVariantList roomParticipants READ roomParticipants NOTIFY roomChanged)
    Q_PROPERTY(int roomParticipantCount READ roomParticipantCount NOTIFY roomChanged)
    Q_PROPERTY(QVariantList chatMessages READ chatMessages NOTIFY chatChanged)
    Q_PROPERTY(int unreadChatCount READ unreadChatCount NOTIFY chatChanged)

    Q_PROPERTY(QString bufferSizeExplanation READ bufferSizeExplanation NOTIFY setupChanged)
    Q_PROPERTY(QString sampleRateExplanation READ sampleRateExplanation NOTIFY setupChanged)
    // One coherent answer about what the musician should understand, so the
    // interface never has to reason across a dozen subsystem states itself.
    Q_PROPERTY(QString sessionHeadline READ sessionHeadline NOTIFY roomChanged)
    Q_PROPERTY(QString sessionExplanation READ sessionExplanation NOTIFY roomChanged)
    Q_PROPERTY(QString sessionActionLabel READ sessionActionLabel NOTIFY roomChanged)
    Q_PROPERTY(bool sessionActionEnabled READ sessionActionEnabled NOTIFY roomChanged)
    Q_PROPERTY(bool sessionPlayable READ sessionPlayable NOTIFY roomChanged)
    Q_PROPERTY(QString sessionPhase READ sessionPhase NOTIFY roomChanged)
    Q_PROPERTY(QString monitorPathSummary READ monitorPathSummary NOTIFY setupChanged)
    Q_PROPERTY(QString settingsSessionNotice READ settingsSessionNotice NOTIFY roomChanged)
    Q_PROPERTY(bool asioActive READ asioActive NOTIFY setupChanged)

    Q_PROPERTY(bool firewallNeedsAttention READ firewallNeedsAttention NOTIFY firewallChanged)
    Q_PROPERTY(bool firewallFixable READ firewallFixable NOTIFY firewallChanged)
    Q_PROPERTY(bool firewallBusy READ firewallBusy NOTIFY firewallChanged)
    Q_PROPERTY(QString firewallMessage READ firewallMessage NOTIFY firewallChanged)
    Q_PROPERTY(QString firewallActionMessage READ firewallActionMessage NOTIFY firewallChanged)

    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingElapsed READ recordingElapsed NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingMessage READ recordingMessage NOTIFY recordingChanged)
    Q_PROPERTY(QString recordingLocation READ recordingLocation NOTIFY recordingChanged)

    Q_PROPERTY(bool tunerActive READ tunerActive WRITE setTunerActive NOTIFY tunerChanged)
    Q_PROPERTY(bool tunerMutesInstrument READ tunerMutesInstrument WRITE setTunerMutesInstrument NOTIFY tunerChanged)
    Q_PROPERTY(bool tunerDetected READ tunerDetected NOTIFY tunerChanged)
    Q_PROPERTY(QString tunerNote READ tunerNote NOTIFY tunerChanged)
    Q_PROPERTY(int tunerOctave READ tunerOctave NOTIFY tunerChanged)
    Q_PROPERTY(double tunerCents READ tunerCents NOTIFY tunerChanged)
    Q_PROPERTY(double tunerFrequency READ tunerFrequency NOTIFY tunerChanged)
    Q_PROPERTY(double tunerLevel READ tunerLevel NOTIFY tunerChanged)

    Q_PROPERTY(int preferredWindowX READ preferredWindowX CONSTANT)
    Q_PROPERTY(int preferredWindowY READ preferredWindowY CONSTANT)
    Q_PROPERTY(int preferredWindowWidth READ preferredWindowWidth CONSTANT)
    Q_PROPERTY(int preferredWindowHeight READ preferredWindowHeight CONSTANT)
    Q_PROPERTY(bool hasPreferredWindowPosition READ hasPreferredWindowPosition CONSTANT)

public:
    AppController(
        std::filesystem::path preferencePath,
        bool visualFixture,
        QString initialPage,
        std::uint32_t widthOverride,
        std::uint32_t heightOverride,
        QObject* parent = nullptr,
        std::unique_ptr<jamlink::audio::ISoundcheckAudioService> audioService = {});
    ~AppController() override;

    [[nodiscard]] QString currentPage() const;
    void setCurrentPage(const QString& page);
    [[nodiscard]] bool visualFixture() const noexcept;
    [[nodiscard]] QString visualPrivateRoomFixture() const;
    [[nodiscard]] bool restoredPreferences() const noexcept;
    [[nodiscard]] bool devicesAvailable() const noexcept;
    [[nodiscard]] bool audioActive() const noexcept;
    [[nodiscard]] QString audioStatus() const;
    [[nodiscard]] bool allReady() const noexcept;
    [[nodiscard]] QString readinessLabel() const;
    [[nodiscard]] QString setupMessage() const;
    [[nodiscard]] QString saveMessage() const;

    [[nodiscard]] QStringList instrumentDevices() const;
    [[nodiscard]] QStringList voiceDevices() const;
    [[nodiscard]] QStringList outputDevices() const;
    [[nodiscard]] int instrumentDeviceIndex() const noexcept;
    [[nodiscard]] int voiceDeviceIndex() const noexcept;
    [[nodiscard]] int outputDeviceIndex() const noexcept;
    void setInstrumentDeviceIndex(int index);
    void setVoiceDeviceIndex(int index);
    void setOutputDeviceIndex(int index);

    [[nodiscard]] QStringList sampleRates() const;
    [[nodiscard]] QStringList bufferSizes() const;
    [[nodiscard]] int sampleRateIndex() const noexcept;
    [[nodiscard]] int bufferSizeIndex() const noexcept;
    void setSampleRateIndex(int index);
    void setBufferSizeIndex(int index);

    [[nodiscard]] double instrumentMonitorGain() const noexcept;
    [[nodiscard]] double voiceMonitorGain() const noexcept;
    [[nodiscard]] bool instrumentMonitorEnabled() const noexcept;
    [[nodiscard]] bool voiceMonitorEnabled() const noexcept;
    void setInstrumentMonitorGain(double gain);
    void setVoiceMonitorGain(double gain);
    void setInstrumentMonitorEnabled(bool enabled);
    void setVoiceMonitorEnabled(bool enabled);

    [[nodiscard]] double instrumentLevel() const noexcept;
    [[nodiscard]] double voiceLevel() const noexcept;
    [[nodiscard]] double outputLevel() const noexcept;
    [[nodiscard]] double instrumentPeakHold() const noexcept;
    [[nodiscard]] double voicePeakHold() const noexcept;
    [[nodiscard]] double outputPeakHold() const noexcept;
    [[nodiscard]] bool instrumentInputClipped() const noexcept;
    [[nodiscard]] bool voiceInputClipped() const noexcept;
    [[nodiscard]] bool instrumentSendClipped() const noexcept;
    [[nodiscard]] bool voiceSendClipped() const noexcept;
    [[nodiscard]] bool outputClipped() const noexcept;
    [[nodiscard]] QString instrumentSignalStatus() const;
    [[nodiscard]] QString voiceSignalStatus() const;
    [[nodiscard]] QString outputSignalStatus() const;
    [[nodiscard]] QString instrumentSignalGuidance() const;
    [[nodiscard]] QString voiceSignalGuidance() const;
    [[nodiscard]] QString outputSignalGuidance() const;
    [[nodiscard]] bool roomActive() const noexcept;
    [[nodiscard]] bool peerConnected() const noexcept;
    [[nodiscard]] QString roomStatus() const;
    [[nodiscard]] QString inviteCode() const;
    [[nodiscard]] bool privateRoomCodesAvailable() const noexcept;
    [[nodiscard]] bool privateRoomBusy() const noexcept;
    [[nodiscard]] bool privateRoomWaiting() const noexcept;
    [[nodiscard]] QString privateRoomCode() const;
    [[nodiscard]] QString privateRoomStatus() const;
    [[nodiscard]] QVariantList waitingRoomRequests() const;
    [[nodiscard]] bool automaticPortMapping() const noexcept;
    [[nodiscard]] QString connectionPreflightStatus() const;
    [[nodiscard]] QString connectionPreflightDetail() const;
    [[nodiscard]] bool connectionPreflightReady() const noexcept;
    [[nodiscard]] int roomPort() const noexcept;
    [[nodiscard]] int roundTripMilliseconds() const noexcept;
    [[nodiscard]] double remoteLevel() const noexcept;
    [[nodiscard]] double remoteInstrumentLevel() const noexcept;
    [[nodiscard]] double remoteVoiceLevel() const noexcept;
    [[nodiscard]] bool remoteInstrumentClipped() const noexcept;
    [[nodiscard]] bool remoteVoiceClipped() const noexcept;
    [[nodiscard]] double remoteInstrumentGain() const noexcept;
    [[nodiscard]] double remoteVoiceGain() const noexcept;
    [[nodiscard]] bool remoteInstrumentMuted() const noexcept;
    [[nodiscard]] bool remoteVoiceMuted() const noexcept;
    void setRemoteInstrumentGain(double gain);
    void setRemoteVoiceGain(double gain);
    void setRemoteInstrumentMuted(bool muted);
    void setRemoteVoiceMuted(bool muted);
    [[nodiscard]] QString connectionQuality() const;
    [[nodiscard]] QString networkDiagnostics() const;
    [[nodiscard]] QString packetSummary() const;
    [[nodiscard]] bool sendMuted() const noexcept;
    void setSendMuted(bool muted);

    [[nodiscard]] QString recordingDirectory() const;
    void setRecordingDirectory(const QString& directory);
    [[nodiscard]] int preferredUdpPort() const noexcept;
    void setPreferredUdpPort(int port);
    [[nodiscard]] bool automaticRouterMapping() const noexcept;
    void setAutomaticRouterMapping(bool enabled);
    [[nodiscard]] int latencyMode() const noexcept;
    void setLatencyMode(int mode);
    [[nodiscard]] QString latencyModeDetail() const;
    [[nodiscard]] QString applicationVersion() const;
    [[nodiscard]] QString qtVersion() const;
    [[nodiscard]] QString updateStatus() const;
    [[nodiscard]] bool updateAvailable() const noexcept;
    [[nodiscard]] bool updateBusy() const noexcept;
    [[nodiscard]] double updateProgress() const noexcept;
    [[nodiscard]] UpdateManager* updateManager() noexcept;

    [[nodiscard]] QString profileId() const;
    [[nodiscard]] QString profileHandle() const;
    void setProfileHandle(const QString& value);
    [[nodiscard]] QString profileDisplayName() const;
    void setProfileDisplayName(const QString& value);
    [[nodiscard]] QString profileAvatarId() const;
    void setProfileAvatarId(const QString& value);
    [[nodiscard]] QUrl profileCustomAvatarSource() const;
    [[nodiscard]] QString profilePrimaryInstrument() const;
    void setProfilePrimaryInstrument(const QString& value);
    [[nodiscard]] QString profileGenres() const;
    void setProfileGenres(const QString& value);
    [[nodiscard]] QString profileBio() const;
    void setProfileBio(const QString& value);
    [[nodiscard]] QString profileRegion() const;
    void setProfileRegion(const QString& value);
    [[nodiscard]] QStringList profileAvatarIds() const;
    [[nodiscard]] QStringList profileAvatarLabels() const;
    [[nodiscard]] QStringList profileInstrumentOptions() const;

    [[nodiscard]] QString remoteDisplayName() const;
    [[nodiscard]] QString remoteHandle() const;
    [[nodiscard]] QString remoteAvatarId() const;
    [[nodiscard]] QString remotePrimaryInstrument() const;
    [[nodiscard]] QVariantList roomParticipants() const;
    [[nodiscard]] int roomParticipantCount() const noexcept;
    [[nodiscard]] QVariantList chatMessages() const;
    [[nodiscard]] int unreadChatCount() const noexcept;

    [[nodiscard]] QString bufferSizeExplanation() const;
    [[nodiscard]] QString sampleRateExplanation() const;
    [[nodiscard]] QString sessionHeadline() const;
    [[nodiscard]] QString sessionExplanation() const;
    [[nodiscard]] QString sessionActionLabel() const;
    [[nodiscard]] bool sessionActionEnabled() const noexcept;
    [[nodiscard]] bool sessionPlayable() const noexcept;
    [[nodiscard]] QString sessionPhase() const;
    // Invokes whatever the guidance is currently offering, so the interface
    // does not need to know which action it is.
    Q_INVOKABLE void takeSessionAction();
    [[nodiscard]] QString monitorPathSummary() const;
    [[nodiscard]] QString settingsSessionNotice() const;
    [[nodiscard]] bool asioActive() const noexcept;

    [[nodiscard]] bool firewallNeedsAttention() const noexcept;
    [[nodiscard]] bool firewallFixable() const noexcept;
    [[nodiscard]] bool firewallBusy() const noexcept;
    [[nodiscard]] QString firewallMessage() const;
    [[nodiscard]] QString firewallActionMessage() const;

    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] QString recordingElapsed() const;
    [[nodiscard]] QString recordingMessage() const;
    [[nodiscard]] QString recordingLocation() const;

    [[nodiscard]] bool tunerActive() const noexcept;
    void setTunerActive(bool active);
    [[nodiscard]] bool tunerMutesInstrument() const noexcept;
    void setTunerMutesInstrument(bool muted);
    [[nodiscard]] bool tunerDetected() const noexcept;
    [[nodiscard]] QString tunerNote() const;
    [[nodiscard]] int tunerOctave() const noexcept;
    [[nodiscard]] double tunerCents() const noexcept;
    [[nodiscard]] double tunerFrequency() const noexcept;
    [[nodiscard]] double tunerLevel() const noexcept;

    [[nodiscard]] int preferredWindowX() const noexcept;
    [[nodiscard]] int preferredWindowY() const noexcept;
    [[nodiscard]] int preferredWindowWidth() const noexcept;
    [[nodiscard]] int preferredWindowHeight() const noexcept;
    [[nodiscard]] bool hasPreferredWindowPosition() const noexcept;

    Q_INVOKABLE void navigate(const QString& page);
    Q_INVOKABLE void closeTuner();
    // Settings is reachable from inside a room, so it has to remember where it
    // was opened from rather than always dropping the musician back to Home.
    Q_INVOKABLE void openSettings();
    Q_INVOKABLE void closeSettings();
    Q_INVOKABLE void saveSoundcheck();
    Q_INVOKABLE void testOutput();
    Q_INVOKABLE void retryAudio();
    Q_INVOKABLE void clearInstrumentClipping();
    Q_INVOKABLE void clearVoiceClipping();
    Q_INVOKABLE void clearOutputClipping();
    Q_INVOKABLE void testInstrumentClipping();
    Q_INVOKABLE void testVoiceClipping();
    Q_INVOKABLE void testOutputClipping();
    Q_INVOKABLE void hostSession();
    Q_INVOKABLE QString generatePrivateInviteCode() const;
    Q_INVOKABLE void hostInviteCodeSession(const QString& roomCode);
    Q_INVOKABLE void joinSession(const QString& inviteCode);
    Q_INVOKABLE void decideWaitingRequest(const QString& requestId, bool admit);
    Q_INVOKABLE void leaveSession();
    Q_INVOKABLE void copyInvite();
    Q_INVOKABLE bool setCustomAvatar(const QUrl& source);
    Q_INVOKABLE void clearCustomAvatar();
    Q_INVOKABLE bool sendChatMessage(const QString& text);
    Q_INVOKABLE void markChatRead();
    Q_INVOKABLE void setRoomParticipantStreamGain(
        const QString& participantId, const QString& stream, double gain);
    Q_INVOKABLE void setRoomParticipantStreamMuted(
        const QString& participantId, const QString& stream, bool muted);
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void openRecordingFolder();
    Q_INVOKABLE void checkFirewall();
    Q_INVOKABLE void fixFirewall();
    Q_INVOKABLE void updateWindowPlacement(int x, int y, int width, int height);
    Q_INVOKABLE void persistNow();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void installUpdate();

signals:
    void currentPageChanged();
    void setupChanged();
    void saveMessageChanged();
    void roomChanged();
    void tunerChanged();
    void recordingChanged();
    void firewallChanged();
    void settingsChanged();
    void profileChanged();
    void chatChanged();
    void updateChanged();

private:
    struct DeviceOption final {
        QString stableId;
        QString displayName;
        QString primaryChannelId;
        QString secondaryChannelId;
        jamlink::audio::SoundcheckEndpointOption serviceOption;
    };

    [[nodiscard]] static QStringList displayNames(const std::vector<DeviceOption>& options);
    [[nodiscard]] static int resolveDevice(
        const std::vector<DeviceOption>& options,
        const jamlink::preferences::AudioSelection& selection);
    [[nodiscard]] static bool selectionAvailable(
        const std::vector<DeviceOption>& options,
        const jamlink::preferences::AudioSelection& selection);
    [[nodiscard]] static int resolveScalar(
        const std::vector<std::uint32_t>& values,
        std::uint32_t value) noexcept;
    [[nodiscard]] static std::uint64_t fingerprint(
        const DeviceOption& option,
        std::uint32_t sampleRate,
        std::uint32_t bufferFrames);
    [[nodiscard]] static bool validIndex(int index, std::size_t size) noexcept;

    void loadPreferences(
        std::uint32_t widthOverride,
        std::uint32_t heightOverride);
    void loadDeviceInventory();
    void chooseInitialAudioDefaults();
    void updateOutputCapabilities();
    void applySelectionsToPreferences();
    void updateReadinessConfiguration();
    void invalidateReadiness();
    void scheduleSave();
    void scheduleAudioRestart();
    void restartAudio();
    void reportAudioDeviceDropouts();
    void installBufferSizeOptions(std::vector<std::uint32_t> deviceValues);
    // The frame count actually in use, resolving the automatic setting to the
    // size it has settled on.
    [[nodiscard]] std::uint32_t effectiveBufferFrames() const noexcept;
    [[nodiscard]] bool automaticBufferSize() const noexcept;
    void considerAutomaticBufferStep(std::uint64_t dropoutsSinceLastReport);
    void pollAudioTelemetry();
    [[nodiscard]] static QString audioStateText(
        jamlink::audio::SoundcheckAudioState state);
    [[nodiscard]] static QString signalStatus(
        const jamlink::audio::SignalHealthTelemetry& health,
        bool active);
    [[nodiscard]] static QString peerStateText(
        jamlink::network::PeerConnectionState state);
    void applyRemoteStream(
        jamlink::network::AudioStreamId stream,
        float& stored,
        double gain);
    void applyLocalSendMutes();
    [[nodiscard]] double bufferLatencyMilliseconds(std::uint32_t frames) const noexcept;
    [[nodiscard]] std::uint32_t currentSampleRate() const noexcept;
    void updateQualityWindow();

    // Rolling view of the friend's instrument stream, so the connection grade
    // describes the last few seconds rather than the whole session.
    static constexpr qint64 qualityWindowMilliseconds = 8'000;
    struct QualitySample final {
        qint64 elapsedMilliseconds{0};
        std::uint64_t packetsAccepted{0U};
        std::uint64_t packetsConcealed{0U};
    };
    struct QualityWindow final {
        bool hasRate{false};
        bool stalled{false};
        double concealRatio{0.0};
    };
    [[nodiscard]] jamlink::network::PeerParticipantInfo localParticipant() const;
    [[nodiscard]] QJsonObject roomCompatibility() const;
    void joinDirectSession(const QString& inviteCode);
    void processRoomControlEvents();
    void appendChatEntry(
        const QString& sender,
        const QString& handle,
        const QString& text,
        std::uint64_t timestampMilliseconds,
        bool own,
        bool system);
    [[nodiscard]] std::filesystem::path defaultRecordingDirectory() const;

    static constexpr std::size_t instrumentStream =
        static_cast<std::size_t>(jamlink::network::AudioStreamId::Instrument);
    static constexpr std::size_t voiceStream =
        static_cast<std::size_t>(jamlink::network::AudioStreamId::Voice);

    jamlink::preferences::PreferencesStore store_;
    PrivateRoomDirectory roomDirectory_;
    UpdateManager updateManager_;
    jamlink::preferences::UserPreferences preferences_;
    jamlink::control::ReadinessTracker readiness_;
    QTimer saveTimer_;
    QTimer audioRestartTimer_;
    // Local audio drop-outs already reported, so the log records a rate rather
    // than repeating a running total at the telemetry poll rate.
    std::uint64_t reportedAudioDropouts_{0U};
    // Where the automatic buffer size has settled. It only ever climbs within a
    // session: stepping back down on a quiet stretch would oscillate, and the
    // musician can always choose a size by hand.
    std::uint32_t autoBufferFrames_{0U};
    bool autoBufferRaised_{false};
    qint64 audioDropoutReportMilliseconds_{0};
    QTimer telemetryTimer_;
    QString currentPage_;
    QString tunerReturnPage_{QStringLiteral("home")};
    QString settingsReturnPage_{QStringLiteral("home")};
    QString setupMessage_;
    QString saveMessage_;
    bool visualFixture_{false};
    bool visualClipFixture_{false};
    bool visualRoomFixture_{false};
    QString visualPrivateRoomFixture_;
    QVariantList visualWaitingRoomRequests_;
    bool visualRecordingFixture_{false};
    int visualRoomParticipantCount_{2};
    bool restoredPreferences_{false};
    bool restoredSetupAvailable_{false};
    bool devicesAvailable_{false};
    jamlink::control::SessionConductor conductor_;
    jamlink::control::MusicianGuidance guidance_{};
    // A peer that was here and is gone is recoverable; one that never arrived
    // is not the same situation and must not read as one.
    bool peerHasConnected_{false};
    // Their identity outlives a drop, so a blip does not empty the room of
    // someone who is about to come back.
    bool peerReconnecting_{false};
    bool buildIncompatible_{false};
    std::uint64_t conductorDropoutBaseline_{0U};
    void refreshSessionGuidance();
    std::unique_ptr<jamlink::audio::ISoundcheckAudioService> audioService_;
    jamlink::audio::SoundcheckAudioTelemetry audioTelemetry_;
    std::unique_ptr<jamlink::network::IPeerAudioTransport> peerTransport_;
    jamlink::network::PeerTransportTelemetry peerTelemetry_;
    std::deque<QualitySample> qualitySamples_;
    QualityWindow qualityWindow_;
    QElapsedTimer qualityClock_;
    jamlink::network::ConnectionPreflightResult connectionPreflight_;
    QString inviteCode_;
    bool sendMuted_{false};
    // What this machine stops transmitting, per stream. Deliberately not
    // persisted: like the room-wide mute these reset on entering and leaving a
    // room, so nobody arrives silently muted without knowing it.
    bool instrumentSendMuted_{false};
    bool voiceSendMuted_{false};
    bool remoteInstrumentMuted_{false};
    bool remoteVoiceMuted_{false};
    jamlink::audio::TunerReading tunerReading_;
    bool tunerActive_{false};
    FirewallAccess firewall_;
    QString firewallActionMessage_;
    bool firewallBusy_{false};
    jamlink::record::RecorderTelemetry recorderTelemetry_;
    QString recordingLocation_;
    jamlink::network::PeerParticipantInfo remoteParticipant_;
    std::vector<jamlink::network::PeerParticipantInfo> remoteParticipants_;
    QVariantList chatMessages_;
    int unreadChatCount_{0};

    std::vector<DeviceOption> instrumentOptions_;
    std::vector<DeviceOption> voiceOptions_;
    std::vector<DeviceOption> outputOptions_;
    std::vector<std::uint32_t> sampleRateValues_{48'000U};
    std::vector<std::uint32_t> bufferSizeValues_{128U};
    int instrumentIndex_{0};
    int voiceIndex_{0};
    int outputIndex_{0};
    int sampleRateIndex_{1};
    int bufferSizeIndex_{1};
};

} // namespace jamlink::desktop
