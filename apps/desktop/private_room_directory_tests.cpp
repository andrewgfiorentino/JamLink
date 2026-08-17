// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "private_room_directory.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace {

bool waitUntil(const std::function<bool()>& predicate, int milliseconds = 6'000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (predicate()) {
            return true;
        }
        QThread::msleep(10U);
    }
    return predicate();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "expected Python executable and directory service script\n";
        return EXIT_FAILURE;
    }

    QTemporaryDir temporary;
    QProcess service;
    service.setProgram(QString::fromLocal8Bit(argv[1]));
    service.setArguments({
        QString::fromLocal8Bit(argv[2]),
        QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
        QStringLiteral("--port"), QStringLiteral("0"),
        QStringLiteral("--database"), temporary.filePath(QStringLiteral("directory.sqlite3")),
    });
    service.start();
    QByteArray output;
    QElapsedTimer startup;
    startup.start();
    while (startup.elapsed() < 5'000 && service.state() != QProcess::NotRunning) {
        static_cast<void>(service.waitForReadyRead(250));
        output += service.readAllStandardOutput();
        if (output.contains("listening")) {
            break;
        }
    }
    const QRegularExpression portExpression(QStringLiteral("listening on 127\\.0\\.0\\.1:(\\d+)"));
    const QRegularExpressionMatch portMatch =
        portExpression.match(QString::fromUtf8(output));
    if (!portMatch.hasMatch()) {
        std::cerr << "directory service did not start\n";
        service.kill();
        return EXIT_FAILURE;
    }
    const QString serviceUrl = QStringLiteral("http://127.0.0.1:%1")
        .arg(portMatch.captured(1));
    qputenv("JAMLINK_DIRECTORY_URL", serviceUrl.toUtf8());

    jamlink::desktop::PrivateRoomDirectory host;
    jamlink::desktop::PrivateRoomDirectory guest;
    if (!host.acceptsCode(QStringLiteral("andrew_mike"))
        || !host.acceptsCode(QString(64, QLatin1Char('A')))
        || host.acceptsCode(QStringLiteral("abc"))
        || host.acceptsCode(QStringLiteral("not a code"))
        || host.acceptsCode(QStringLiteral("JAMLINK"))) {
        std::cerr << "temporary invite-code validation is incorrect\n";
        service.kill();
        return EXIT_FAILURE;
    }
    const QJsonObject compatibility{
        {QStringLiteral("application_version"), QStringLiteral("0.3.4")},
        {QStringLiteral("build_identity"), QString(40, QLatin1Char('a'))},
        {QStringLiteral("release_channel"), QStringLiteral("test")},
        {QStringLiteral("media_protocol"), 2},
        {QStringLiteral("control_protocol"), 1},
    };
    const QString directInvite = QStringLiteral("JL1|203.0.113.20|45000|%1")
        .arg(QString(64, QLatin1Char('b')));
    host.create(QStringLiteral("andrew_mike"), directInvite, compatibility);
    if (!waitUntil([&host] { return host.hosting(); })) {
        std::cerr << "host failed to register private room: "
                  << host.status().toStdString() << '\n';
        service.kill();
        return EXIT_FAILURE;
    }
    if (host.roomCode() != QStringLiteral("andrew_mike")) {
        std::cerr << "host invite-code spelling was not preserved\n";
        service.kill();
        return EXIT_FAILURE;
    }

    QString resolvedInvite;
    QObject::connect(&guest, &jamlink::desktop::PrivateRoomDirectory::inviteResolved,
        [&resolvedInvite](const QString& invite) { resolvedInvite = invite; });
    guest.requestJoin(
        QStringLiteral("ANDREW_MIKE"), compatibility,
        QStringLiteral("Mike"), QStringLiteral("Bass"), QStringLiteral("avatar:bass"));
    if (!waitUntil([&guest] { return guest.waiting(); }) || !resolvedInvite.isEmpty()) {
        std::cerr << "guest did not enter media-isolated waiting state\n";
        service.kill();
        return EXIT_FAILURE;
    }
    if (!waitUntil([&host] { return !host.waitingRequests().isEmpty(); })) {
        std::cerr << "host did not receive waiting request\n";
        service.kill();
        return EXIT_FAILURE;
    }
    const QString requestId = host.waitingRequests().front().toMap()
        .value(QStringLiteral("request_id")).toString();
    host.decide(requestId, true);
    if (!waitUntil([&resolvedInvite] { return !resolvedInvite.isEmpty(); })
        || resolvedInvite != directInvite) {
        std::cerr << "approved request did not resolve the exact direct invite\n";
        service.kill();
        return EXIT_FAILURE;
    }

    host.stop();
    guest.stop();
    service.terminate();
    if (!service.waitForFinished(2'000)) {
        service.kill();
        static_cast<void>(service.waitForFinished(2'000));
    }
    std::cout << "private room directory client flow passed\n";
    return EXIT_SUCCESS;
}
