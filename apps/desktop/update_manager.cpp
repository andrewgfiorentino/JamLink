// SPDX-License-Identifier: GPL-3.0-or-later

#include "update_manager.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <array>

namespace jamlink::desktop {
namespace {

constexpr auto releasesApi =
    "https://api.github.com/repos/andrewgfiorentino/JamLink/releases?per_page=20";

struct SemanticVersion final {
    std::array<int, 3U> parts{};
    bool valid{false};
};

[[nodiscard]] SemanticVersion semanticVersion(QString value) {
    if (value.startsWith(QLatin1Char('v'))) {
        value.remove(0, 1);
    }
    const qsizetype suffix = value.indexOf(QLatin1Char('-'));
    if (suffix >= 0) {
        value.truncate(suffix);
    }
    const auto pieces = value.split(QLatin1Char('.'));
    if (pieces.size() != 3) {
        return {};
    }
    SemanticVersion result;
    for (qsizetype index = 0; index < 3; ++index) {
        bool ok = false;
        result.parts[static_cast<std::size_t>(index)] = pieces[index].toInt(&ok);
        if (!ok || result.parts[static_cast<std::size_t>(index)] < 0) {
            return {};
        }
    }
    result.valid = true;
    return result;
}

[[nodiscard]] bool newer(const SemanticVersion& candidate, const SemanticVersion& current) {
    return candidate.valid && current.valid && candidate.parts > current.parts;
}

[[nodiscard]] bool channelMatches(const QString& tag, const QString& channel) {
    if (channel == QStringLiteral("stable")) {
        return !tag.contains(QLatin1Char('-'));
    }
    return tag.endsWith(QLatin1Char('-') + channel, Qt::CaseInsensitive);
}

[[nodiscard]] QNetworkRequest requestFor(const QUrl& url) {
    QNetworkRequest request(url);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("JamLink/%1").arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(30'000);
    return request;
}

} // namespace

UpdateCandidate selectUpdateCandidate(
    const QByteArray& releaseFeed,
    const QString& currentVersion,
    const QString& releaseChannel) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(releaseFeed, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return {};
    }
    const auto current = semanticVersion(currentVersion);
    UpdateCandidate best;
    SemanticVersion bestVersion;
    for (const auto& value : document.array()) {
        const auto release = value.toObject();
        if (release.value(QStringLiteral("draft")).toBool()
            || !release.value(QStringLiteral("published_at")).isString()) {
            continue;
        }
        const QString tag = release.value(QStringLiteral("tag_name")).toString();
        const auto version = semanticVersion(tag);
        if (!channelMatches(tag, releaseChannel) || !newer(version, current)
            || (bestVersion.valid && !newer(version, bestVersion))) {
            continue;
        }
        const QString versionText = QStringLiteral("%1.%2.%3")
            .arg(version.parts[0]).arg(version.parts[1]).arg(version.parts[2]);
        const QString suffix = releaseChannel == QStringLiteral("stable")
            ? QString() : QLatin1Char('-') + releaseChannel;
        const QString archiveName = QStringLiteral("JamLink-%1%2-windows-x64.zip")
            .arg(versionText, suffix);
        QUrl archiveUrl;
        QUrl checksumUrl;
        QByteArray digest;
        for (const auto& assetValue : release.value(QStringLiteral("assets")).toArray()) {
            const auto asset = assetValue.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (name == archiveName) {
                archiveUrl = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
                const QString apiDigest = asset.value(QStringLiteral("digest")).toString();
                if (apiDigest.startsWith(QStringLiteral("sha256:"))) {
                    digest = apiDigest.sliced(7).toLatin1().toLower();
                }
            } else if (name == archiveName + QStringLiteral(".sha256")) {
                checksumUrl = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
            }
        }
        if (!archiveUrl.isValid() || !checksumUrl.isValid()) {
            continue;
        }
        best = UpdateCandidate{versionText, tag, archiveUrl, checksumUrl, digest};
        bestVersion = version;
    }
    return best;
}

QByteArray parseSha256(const QByteArray& checksumFile) {
    static const QRegularExpression expression(QStringLiteral("^([0-9a-fA-F]{64})(?:\\s|$)"));
    if (checksumFile.size() > 1'024) {
        return {};
    }
    const auto match = expression.match(QString::fromLatin1(checksumFile).trimmed());
    return match.hasMatch() ? match.captured(1).toLatin1().toLower() : QByteArray{};
}

bool verifyFileSha256(const QString& path, const QByteArray& expectedHex) {
    QFile file(path);
    if (expectedHex.size() != 64 || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result().toHex() == expectedHex.toLower();
}

UpdateManager::UpdateManager(
    QString currentVersion,
    QString releaseChannel,
    bool visualFixture,
    QObject* parent)
    : QObject(parent),
      currentVersion_(std::move(currentVersion)),
      releaseChannel_(std::move(releaseChannel)),
      visualFixture_(visualFixture),
      network_(std::make_unique<QNetworkAccessManager>(this)) {
    if (visualFixture_) {
        status_ = QStringLiteral("JamLink is up to date");
    }
}

UpdateManager::~UpdateManager() = default;

QString UpdateManager::status() const { return status_; }
QString UpdateManager::availableVersion() const { return candidate_.version; }
bool UpdateManager::updateAvailable() const noexcept { return !candidate_.version.isEmpty(); }
bool UpdateManager::busy() const noexcept { return busy_; }
double UpdateManager::progress() const noexcept { return progress_; }

void UpdateManager::setFailure(const QString& message) {
    if (reply_ != nullptr) {
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (archiveFile_) {
        archiveFile_->close();
        archiveFile_.reset();
    }
    busy_ = false;
    progress_ = 0.0;
    status_ = message;
    emit changed();
}

void UpdateManager::checkNow() {
    if (busy_ || visualFixture_) {
        return;
    }
    busy_ = true;
    candidate_ = {};
    status_ = QStringLiteral("Checking for updates…");
    progress_ = 0.0;
    emit changed();
    reply_ = network_->get(requestFor(QUrl(QString::fromLatin1(releasesApi))));
    connect(reply_, &QNetworkReply::finished, this, [this] {
        auto* completed = reply_;
        reply_ = nullptr;
        if (completed->error() != QNetworkReply::NoError) {
            const QString detail = completed->errorString();
            completed->deleteLater();
            setFailure(QStringLiteral("Could not check for updates: %1").arg(detail));
            return;
        }
        const QByteArray response = completed->readAll();
        completed->deleteLater();
        candidate_ = selectUpdateCandidate(response, currentVersion_, releaseChannel_);
        busy_ = false;
        status_ = candidate_.version.isEmpty()
            ? QStringLiteral("JamLink is up to date")
            : QStringLiteral("JamLink %1 is ready").arg(candidate_.version);
        emit changed();
    });
}

void UpdateManager::downloadAndInstall() {
    if (busy_ || candidate_.version.isEmpty() || visualFixture_) {
        return;
    }
    busy_ = true;
    progress_ = 0.0;
    status_ = QStringLiteral("Verifying update information…");
    emit changed();
    reply_ = network_->get(requestFor(candidate_.checksumUrl));
    connect(reply_, &QNetworkReply::finished, this, [this] {
        auto* completed = reply_;
        reply_ = nullptr;
        if (completed->error() != QNetworkReply::NoError) {
            const QString detail = completed->errorString();
            completed->deleteLater();
            setFailure(QStringLiteral("Could not download the published checksum: %1").arg(detail));
            return;
        }
        expectedDigest_ = parseSha256(completed->readAll());
        completed->deleteLater();
        if (expectedDigest_.isEmpty()
            || (!candidate_.apiDigest.isEmpty() && candidate_.apiDigest != expectedDigest_)) {
            setFailure(QStringLiteral("The release checksum is invalid"));
            return;
        }
        beginArchiveDownload();
    });
}

void UpdateManager::beginArchiveDownload() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    updateDirectory_ = QDir(base).filePath(
        QStringLiteral("updates/%1-%2").arg(candidate_.tag, QUuid::createUuid().toString(QUuid::Id128)));
    if (!QDir().mkpath(updateDirectory_)) {
        setFailure(QStringLiteral("The update staging folder could not be created"));
        return;
    }
    archivePath_ = QDir(updateDirectory_).filePath(QStringLiteral("JamLink-update.zip"));
    archiveFile_ = std::make_unique<QFile>(archivePath_);
    if (!archiveFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setFailure(QStringLiteral("The update could not be staged"));
        return;
    }
    status_ = QStringLiteral("Downloading JamLink %1…").arg(candidate_.version);
    emit changed();
    reply_ = network_->get(requestFor(candidate_.archiveUrl));
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (archiveFile_ && reply_ != nullptr) {
            const QByteArray bytes = reply_->readAll();
            if (archiveFile_->write(bytes) != bytes.size()) {
                reply_->abort();
            }
        }
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        progress_ = total > 0 ? static_cast<double>(received) / static_cast<double>(total) : 0.0;
        emit changed();
    });
    connect(reply_, &QNetworkReply::finished, this, &UpdateManager::finishArchiveDownload);
}

void UpdateManager::finishArchiveDownload() {
    auto* completed = reply_;
    reply_ = nullptr;
    if (archiveFile_) {
        archiveFile_->flush();
        archiveFile_->close();
        archiveFile_.reset();
    }
    if (completed->error() != QNetworkReply::NoError) {
        const QString detail = completed->errorString();
        completed->deleteLater();
        setFailure(QStringLiteral("The update download failed: %1").arg(detail));
        return;
    }
    completed->deleteLater();
    status_ = QStringLiteral("Checking the downloaded update…");
    emit changed();
    if (!verifyFileSha256(archivePath_, expectedDigest_)) {
        QFile::remove(archivePath_);
        setFailure(QStringLiteral("The downloaded update did not pass its SHA-256 check"));
        return;
    }
    stageAndRestart();
}

void UpdateManager::stageAndRestart() {
    const QString installedHelper = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("JamLinkUpdater.exe"));
    const QString temporaryHelper = QDir(updateDirectory_)
        .filePath(QStringLiteral("JamLinkUpdater.exe"));
    if (!QFileInfo::exists(installedHelper)
        || (!QFile::copy(installedHelper, temporaryHelper) && !QFileInfo::exists(temporaryHelper))) {
        setFailure(QStringLiteral("The update helper is missing from this JamLink package"));
        return;
    }
    const QString target = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString stage = QDir(updateDirectory_).filePath(QStringLiteral("new-package"));
    const QStringList arguments{
        QStringLiteral("--archive"), archivePath_,
        QStringLiteral("--stage"), stage,
        QStringLiteral("--target"), target,
        QStringLiteral("--parent"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("--restart")};
    if (!QProcess::startDetached(temporaryHelper, arguments, updateDirectory_)) {
        setFailure(QStringLiteral("The update helper could not be started"));
        return;
    }
    status_ = QStringLiteral("Restarting to finish the update…");
    emit changed();
    emit restartRequested();
}

} // namespace jamlink::desktop
