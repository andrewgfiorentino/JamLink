// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace jamlink::desktop {

struct UpdateCandidate final {
    QString version;
    QString tag;
    QUrl archiveUrl;
    QUrl checksumUrl;
    QByteArray apiDigest;
};

[[nodiscard]] UpdateCandidate selectUpdateCandidate(
    const QByteArray& releaseFeed,
    const QString& currentVersion,
    const QString& releaseChannel);
[[nodiscard]] QByteArray parseSha256(const QByteArray& checksumFile);
[[nodiscard]] bool verifyFileSha256(const QString& path, const QByteArray& expectedHex);

class UpdateManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)

public:
    UpdateManager(
        QString currentVersion,
        QString releaseChannel,
        bool visualFixture,
        QObject* parent = nullptr);
    ~UpdateManager() override;

    [[nodiscard]] QString status() const;
    [[nodiscard]] QString availableVersion() const;
    [[nodiscard]] bool updateAvailable() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] double progress() const noexcept;

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void downloadAndInstall();

signals:
    void changed();
    void restartRequested();

private:
    void setFailure(const QString& message);
    void beginArchiveDownload();
    void finishArchiveDownload();
    void stageAndRestart();

    QString currentVersion_;
    QString releaseChannel_;
    bool visualFixture_{false};
    QString status_{QStringLiteral("Updates have not been checked")};
    UpdateCandidate candidate_;
    bool busy_{false};
    double progress_{0.0};
    QByteArray expectedDigest_;
    QString updateDirectory_;
    QString archivePath_;
    std::unique_ptr<QNetworkAccessManager> network_;
    QNetworkReply* reply_{nullptr};
    std::unique_ptr<QFile> archiveFile_;
};

} // namespace jamlink::desktop
