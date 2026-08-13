// SPDX-License-Identifier: GPL-3.0-or-later

#include "private_room_directory.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QUrl>

#include <utility>

#ifndef JAMLINK_DIRECTORY_URL_STRING
#define JAMLINK_DIRECTORY_URL_STRING ""
#endif

namespace jamlink::desktop {

PrivateRoomDirectory::PrivateRoomDirectory(QObject* parent)
    : QObject(parent), network_(this), heartbeatTimer_(this), pollTimer_(this) {
    baseUrl_ = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("JAMLINK_DIRECTORY_URL"),
        QString::fromUtf8(JAMLINK_DIRECTORY_URL_STRING)).trimmed();
    while (baseUrl_.endsWith('/')) {
        baseUrl_.chop(1);
    }
    const QUrl url(baseUrl_);
    const bool localHttp = url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("127.0.0.1")
            || url.host() == QStringLiteral("localhost"));
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("https") && !localHttp)) {
        baseUrl_.clear();
    } else {
        status_ = QStringLiteral("Choose a unique private room name");
    }
    heartbeatTimer_.setInterval(25'000);
    // A host and guest may share one NAT/rate-limit identity. Two-second
    // polling keeps their combined waiting traffic comfortably bounded.
    pollTimer_.setInterval(2'000);
    connect(&heartbeatTimer_, &QTimer::timeout, this, &PrivateRoomDirectory::heartbeat);
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (hosting_) {
            pollHost();
        } else if (waiting_) {
            pollGuest();
        }
    });
}

bool PrivateRoomDirectory::available() const noexcept { return !baseUrl_.isEmpty(); }
bool PrivateRoomDirectory::busy() const noexcept { return busy_; }
bool PrivateRoomDirectory::active() const noexcept { return active_; }
bool PrivateRoomDirectory::hosting() const noexcept { return hosting_; }
bool PrivateRoomDirectory::waiting() const noexcept { return waiting_; }
QString PrivateRoomDirectory::roomCode() const { return roomCode_; }
QString PrivateRoomDirectory::status() const { return status_; }
QVariantList PrivateRoomDirectory::waitingRequests() const { return waitingRequests_; }

void PrivateRoomDirectory::create(
    const QString& code,
    const QString& directInvite,
    const QJsonObject& compatibility) {
    if (!available() || busy_ || directInvite.isEmpty()) {
        setStatus(QStringLiteral("Private room name service is unavailable"));
        return;
    }
    roomCode_ = normalizedCode(code);
    if (roomCode_.isEmpty()) {
        setStatus(QStringLiteral("Use 4–32 letters, numbers, or hyphens"));
        return;
    }
    busy_ = true;
    const quint64 generation = ++generation_;
    emit changed();
    QJsonObject payload = compatibility;
    payload.insert(QStringLiteral("code"), roomCode_);
    payload.insert(QStringLiteral("invite_code"), directInvite);
    send("POST", QStringLiteral("/v1/private-rooms"), payload, {},
        [this, generation](int statusCode, const QJsonObject& response) {
            if (generation != generation_) {
                return;
            }
            busy_ = false;
            if (statusCode == 201) {
                ownerToken_ = response.value(QStringLiteral("owner_token")).toString();
                roomCode_ = response.value(QStringLiteral("code")).toString(roomCode_);
                hosting_ = !ownerToken_.isEmpty();
                waiting_ = false;
                if (hosting_) {
                    heartbeatTimer_.start();
                    pollTimer_.start();
                    setStatus(QStringLiteral("Private room %1 is ready").arg(roomCode_));
                    return;
                }
            }
            setStatus(statusCode == 409
                ? QStringLiteral("That private room name is already in use")
                : QStringLiteral("Could not register the private room name"));
            roomCode_.clear();
        });
}

void PrivateRoomDirectory::requestJoin(
    const QString& code,
    const QJsonObject& compatibility,
    const QString& displayName,
    const QString& instrument,
    const QString& avatarId) {
    if (!available() || busy_) {
        setStatus(QStringLiteral("Private room name service is unavailable"));
        return;
    }
    roomCode_ = normalizedCode(code);
    if (roomCode_.isEmpty()) {
        setStatus(QStringLiteral("Enter a valid private room name"));
        return;
    }
    busy_ = true;
    active_ = true;
    const quint64 generation = ++generation_;
    emit changed();
    QJsonObject payload = compatibility;
    payload.insert(QStringLiteral("display_name"), displayName);
    payload.insert(QStringLiteral("primary_instrument"), instrument);
    payload.insert(QStringLiteral("avatar_id"), avatarId);
    send("POST", QStringLiteral("/v1/private-rooms/%1/requests").arg(roomCode_),
        payload, {}, [this, generation](int statusCode, const QJsonObject& response) {
            if (generation != generation_) {
                return;
            }
            busy_ = false;
            if (statusCode == 201) {
                requestId_ = response.value(QStringLiteral("request_id")).toString();
                requestToken_ = response.value(QStringLiteral("request_token")).toString();
                waiting_ = !requestId_.isEmpty() && !requestToken_.isEmpty();
                hosting_ = false;
                if (waiting_) {
                    pollTimer_.start();
                    setStatus(QStringLiteral("Waiting for the host to let you in"));
                    return;
                }
            }
            if (statusCode == 404) {
                setStatus(QStringLiteral("No active private room has that name"));
            } else if (statusCode == 409) {
                const QString error = response.value(QStringLiteral("error")).toString();
                setStatus(error == QStringLiteral("room_already_admitted")
                    ? QStringLiteral("That private room already admitted its friend")
                    : QStringLiteral("This room requires the latest matching JamLink build"));
            } else if (statusCode == 429) {
                setStatus(QStringLiteral("That room's waiting list is full; try again shortly"));
            } else {
                setStatus(QStringLiteral("Could not request access to that room"));
            }
        });
}

void PrivateRoomDirectory::decide(const QString& requestId, bool admit) {
    if (!hosting_ || ownerToken_.isEmpty() || requestId.isEmpty() || busy_) {
        return;
    }
    busy_ = true;
    const quint64 generation = generation_;
    emit changed();
    const QString action = admit ? QStringLiteral("admit") : QStringLiteral("deny");
    send("POST",
        QStringLiteral("/v1/private-rooms/%1/requests/%2/%3")
            .arg(roomCode_, requestId, action),
        {}, ownerToken_, [this, generation](int statusCode, const QJsonObject&) {
            if (generation != generation_) {
                return;
            }
            busy_ = false;
            if (statusCode != 200) {
                setStatus(QStringLiteral("Could not update that waiting request"));
                return;
            }
            pollHost();
        });
}

void PrivateRoomDirectory::stop() {
    if (hosting_ && !roomCode_.isEmpty() && !ownerToken_.isEmpty()) {
        send("DELETE", QStringLiteral("/v1/private-rooms/%1").arg(roomCode_),
            {}, ownerToken_, [](int, const QJsonObject&) {});
    }
    ++generation_;
    heartbeatTimer_.stop();
    pollTimer_.stop();
    roomCode_.clear();
    ownerToken_.clear();
    requestId_.clear();
    requestToken_.clear();
    waitingRequests_.clear();
    busy_ = false;
    active_ = false;
    hosting_ = false;
    waiting_ = false;
    heartbeatInFlight_ = false;
    pollInFlight_ = false;
    setStatus(available() ? QStringLiteral("Choose a unique private room name")
                          : QStringLiteral("Private room names unavailable"));
}

void PrivateRoomDirectory::send(
    const QByteArray& method,
    const QString& path,
    const QJsonObject& body,
    const QString& bearerToken,
    ResponseHandler handler) {
    QNetworkRequest request(QUrl(baseUrl_ + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(8'000);
    if (!bearerToken.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + bearerToken.toLatin1());
    }
    QNetworkReply* reply = nullptr;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (method == "GET") {
        reply = network_.get(request);
    } else {
        reply = network_.sendCustomRequest(request, method, payload);
    }
    connect(reply, &QNetworkReply::finished, this,
        [reply, handler = std::move(handler)]() mutable {
            const int statusCode = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            const QJsonObject response = document.isObject() ? document.object() : QJsonObject{};
            reply->deleteLater();
            handler(statusCode, response);
        });
}

void PrivateRoomDirectory::heartbeat() {
    if (!hosting_ || ownerToken_.isEmpty() || heartbeatInFlight_) {
        return;
    }
    heartbeatInFlight_ = true;
    const quint64 generation = generation_;
    send("POST", QStringLiteral("/v1/private-rooms/%1/heartbeat").arg(roomCode_),
        {}, ownerToken_, [this, generation](int statusCode, const QJsonObject&) {
            if (generation != generation_) {
                return;
            }
            heartbeatInFlight_ = false;
            if (statusCode != 200) {
                setStatus(QStringLiteral("Private room name lost; direct invite still works"));
            }
        });
}

void PrivateRoomDirectory::pollHost() {
    if (!hosting_ || ownerToken_.isEmpty() || pollInFlight_) {
        return;
    }
    pollInFlight_ = true;
    const quint64 generation = generation_;
    send("GET", QStringLiteral("/v1/private-rooms/%1/requests").arg(roomCode_),
        {}, ownerToken_, [this, generation](int statusCode, const QJsonObject& response) {
            if (generation != generation_) {
                return;
            }
            pollInFlight_ = false;
            if (statusCode != 200) {
                return;
            }
            QVariantList next;
            for (const QJsonValue value : response.value(QStringLiteral("requests")).toArray()) {
                next.push_back(value.toObject().toVariantMap());
            }
            if (next != waitingRequests_) {
                waitingRequests_ = std::move(next);
                emit changed();
            }
        });
}

void PrivateRoomDirectory::pollGuest() {
    if (!waiting_ || requestId_.isEmpty() || requestToken_.isEmpty() || pollInFlight_) {
        return;
    }
    pollInFlight_ = true;
    const quint64 generation = generation_;
    send("GET", QStringLiteral("/v1/private-requests/%1").arg(requestId_),
        {}, requestToken_, [this, generation](int statusCode, const QJsonObject& response) {
            if (generation != generation_) {
                return;
            }
            pollInFlight_ = false;
            if (statusCode != 200) {
                return;
            }
            const QString state = response.value(QStringLiteral("status")).toString();
            if (state == QStringLiteral("admitted")) {
                const QString invite = response.value(QStringLiteral("invite_code")).toString();
                waiting_ = false;
                pollTimer_.stop();
                setStatus(QStringLiteral("Host approved · connecting securely"));
                if (!invite.isEmpty()) {
                    emit inviteResolved(invite);
                }
            } else if (state == QStringLiteral("denied")) {
                waiting_ = false;
                pollTimer_.stop();
                setStatus(QStringLiteral("The host did not admit this request"));
            }
        });
}

void PrivateRoomDirectory::setStatus(QString value) {
    status_ = std::move(value);
    emit changed();
}

QString PrivateRoomDirectory::normalizedCode(const QString& code) const {
    QString normalized = code.trimmed().toUpper();
    if (normalized.size() < 4 || normalized.size() > 32) {
        return {};
    }
    for (const QChar character : normalized) {
        if (!(character >= u'A' && character <= u'Z')
            && !(character >= u'0' && character <= u'9')
            && character != u'-') {
            return {};
        }
    }
    return normalized;
}

} // namespace jamlink::desktop
