// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/control/readiness_tracker.hpp"
#include "jamlink/preferences/preferences_store.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace jamlink::desktop {

class AppController final : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppController)
    QML_UNCREATABLE("Created by the JamLink application")
    Q_PROPERTY(QString currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(bool visualFixture READ visualFixture CONSTANT)
    Q_PROPERTY(bool restoredPreferences READ restoredPreferences CONSTANT)
    Q_PROPERTY(bool devicesAvailable READ devicesAvailable NOTIFY setupChanged)
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

    Q_PROPERTY(QStringList sampleRates READ sampleRates CONSTANT)
    Q_PROPERTY(QStringList bufferSizes READ bufferSizes CONSTANT)
    Q_PROPERTY(int sampleRateIndex READ sampleRateIndex WRITE setSampleRateIndex NOTIFY setupChanged)
    Q_PROPERTY(int bufferSizeIndex READ bufferSizeIndex WRITE setBufferSizeIndex NOTIFY setupChanged)

    Q_PROPERTY(double instrumentMonitorGain READ instrumentMonitorGain WRITE setInstrumentMonitorGain NOTIFY setupChanged)
    Q_PROPERTY(double voiceMonitorGain READ voiceMonitorGain WRITE setVoiceMonitorGain NOTIFY setupChanged)
    Q_PROPERTY(bool instrumentMonitorEnabled READ instrumentMonitorEnabled WRITE setInstrumentMonitorEnabled NOTIFY setupChanged)
    Q_PROPERTY(bool voiceMonitorEnabled READ voiceMonitorEnabled WRITE setVoiceMonitorEnabled NOTIFY setupChanged)
    Q_PROPERTY(double instrumentLevel READ instrumentLevel NOTIFY setupChanged)
    Q_PROPERTY(double voiceLevel READ voiceLevel NOTIFY setupChanged)
    Q_PROPERTY(double outputLevel READ outputLevel NOTIFY setupChanged)

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
        QObject* parent = nullptr);

    [[nodiscard]] QString currentPage() const;
    void setCurrentPage(const QString& page);
    [[nodiscard]] bool visualFixture() const noexcept;
    [[nodiscard]] bool restoredPreferences() const noexcept;
    [[nodiscard]] bool devicesAvailable() const noexcept;
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

    [[nodiscard]] int preferredWindowX() const noexcept;
    [[nodiscard]] int preferredWindowY() const noexcept;
    [[nodiscard]] int preferredWindowWidth() const noexcept;
    [[nodiscard]] int preferredWindowHeight() const noexcept;
    [[nodiscard]] bool hasPreferredWindowPosition() const noexcept;

    Q_INVOKABLE void navigate(const QString& page);
    Q_INVOKABLE void saveSoundcheck();
    Q_INVOKABLE void updateWindowPlacement(int x, int y, int width, int height);
    Q_INVOKABLE void persistNow();

signals:
    void currentPageChanged();
    void setupChanged();
    void saveMessageChanged();

private:
    struct DeviceOption final {
        QString stableId;
        QString displayName;
        QString primaryChannelId;
        QString secondaryChannelId;
    };

    [[nodiscard]] static QStringList displayNames(const std::vector<DeviceOption>& options);
    [[nodiscard]] static int resolveDevice(
        const std::vector<DeviceOption>& options,
        const std::string& stableId);
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
    void applySelectionsToPreferences();
    void updateReadinessConfiguration();
    void invalidateReadiness();
    void scheduleSave();

    jamlink::preferences::PreferencesStore store_;
    jamlink::preferences::UserPreferences preferences_;
    jamlink::control::ReadinessTracker readiness_;
    QTimer saveTimer_;
    QString currentPage_;
    QString setupMessage_;
    QString saveMessage_;
    bool visualFixture_{false};
    bool restoredPreferences_{false};
    bool restoredSetupAvailable_{false};
    bool devicesAvailable_{false};

    std::vector<DeviceOption> instrumentOptions_;
    std::vector<DeviceOption> voiceOptions_;
    std::vector<DeviceOption> outputOptions_;
    std::vector<std::uint32_t> sampleRateValues_{44'100U, 48'000U, 96'000U};
    std::vector<std::uint32_t> bufferSizeValues_{64U, 128U, 256U};
    int instrumentIndex_{0};
    int voiceIndex_{0};
    int outputIndex_{0};
    int sampleRateIndex_{1};
    int bufferSizeIndex_{1};
};

} // namespace jamlink::desktop
