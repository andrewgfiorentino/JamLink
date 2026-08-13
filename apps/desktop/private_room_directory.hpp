// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

namespace jamlink::desktop {

class PrivateRoomDirectory final : public QObject {
    Q_OBJECT

public:
    explicit PrivateRoomDirectory(QObject* parent = nullptr);

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool hosting() const noexcept;
    [[nodiscard]] bool waiting() const noexcept;
    [[nodiscard]] QString roomCode() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] QVariantList waitingRequests() const;

    void create(
        const QString& code,
        const QString& directInvite,
        const QJsonObject& compatibility);
    void requestJoin(
        const QString& code,
        const QJsonObject& compatibility,
        const QString& displayName,
        const QString& instrument,
        const QString& avatarId);
    void decide(const QString& requestId, bool admit);
    void stop();

signals:
    void changed();
    void inviteResolved(QString directInvite);

private:
    using ResponseHandler = std::function<void(int, const QJsonObject&)>;

    void send(
        const QByteArray& method,
        const QString& path,
        const QJsonObject& body,
        const QString& bearerToken,
        ResponseHandler handler);
    void heartbeat();
    void pollHost();
    void pollGuest();
    void setStatus(QString value);
    [[nodiscard]] QString normalizedCode(const QString& code) const;

    QNetworkAccessManager network_;
    QTimer heartbeatTimer_;
    QTimer pollTimer_;
    QString baseUrl_;
    QString roomCode_;
    QString ownerToken_;
    QString requestId_;
    QString requestToken_;
    QString status_{QStringLiteral("Private room names unavailable")};
    QVariantList waitingRequests_;
    bool busy_{false};
    bool active_{false};
    bool hosting_{false};
    bool waiting_{false};
    bool heartbeatInFlight_{false};
    bool pollInFlight_{false};
    quint64 generation_{0U};
};

} // namespace jamlink::desktop
