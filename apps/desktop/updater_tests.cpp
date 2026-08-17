// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "update_manager.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace {

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    bool passed = true;
    const QByteArray releases = R"json([
      {
        "tag_name":"v9.0.0",
        "draft":false,
        "published_at":"2026-08-11T00:00:00Z",
        "assets":[
          {"name":"JamLink-9.0.0-windows-x64.zip","browser_download_url":"https://example/stable.zip"},
          {"name":"JamLink-9.0.0-windows-x64.zip.sha256","browser_download_url":"https://example/stable.sha256"}
        ]
      },
      {
        "tag_name":"v0.3.2-test",
        "draft":false,
        "published_at":"2026-08-13T00:00:00Z",
        "assets":[
          {"name":"JamLink-0.3.2-test-windows-x64.zip","browser_download_url":"https://example/test-032.zip","digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
          {"name":"JamLink-0.3.2-test-windows-x64.zip.sha256","browser_download_url":"https://example/test-032.sha256"}
        ]
      },
      {
        "tag_name":"v0.3.1-test",
        "draft":false,
        "published_at":"2026-08-11T00:00:00Z",
        "assets":[
          {"name":"JamLink-0.3.1-test-windows-x64.zip","browser_download_url":"https://example/test.zip","digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
          {"name":"JamLink-0.3.1-test-windows-x64.zip.sha256","browser_download_url":"https://example/test.sha256"}
        ]
      }
    ])json";
    const auto candidate = jamlink::desktop::selectUpdateCandidate(
        releases, QStringLiteral("0.3.0"), QStringLiteral("test"));
    passed = expect(candidate.version == QStringLiteral("0.3.2"),
                    "0.3.0 tester selects the latest updater-compatible release") && passed;
    passed = expect(candidate.archiveUrl == QUrl(QStringLiteral("https://example/test-032.zip")),
                    "archive URL is selected by exact package name") && passed;
    passed = expect(candidate.apiDigest.size() == 64,
                    "GitHub API digest is retained") && passed;
    const auto none = jamlink::desktop::selectUpdateCandidate(
        releases, QStringLiteral("0.3.2"), QStringLiteral("test"));
    passed = expect(none.version.isEmpty(), "current version is not offered again") && passed;
    const auto previousBuild = jamlink::desktop::selectUpdateCandidate(
        releases, QStringLiteral("0.3.1"), QStringLiteral("test"));
    passed = expect(previousBuild.version == QStringLiteral("0.3.2"),
                    "0.3.1 tester is offered the launch-prompt release") && passed;

    const QByteArray digest = jamlink::desktop::parseSha256(
        QByteArray(64, 'a') + "  JamLink.zip\n");
    passed = expect(digest == QByteArray(64, 'a'), "checksum sidecar is parsed") && passed;
    passed = expect(jamlink::desktop::parseSha256("not-a-hash").isEmpty(),
                    "malformed checksum is rejected") && passed;

    QTemporaryDir directory;
    QFile payload(directory.filePath(QStringLiteral("payload.bin")));
    passed = expect(payload.open(QIODevice::WriteOnly), "temporary payload opens") && passed;
    payload.write("JamLink updater test vector");
    payload.close();
    passed = expect(jamlink::desktop::verifyFileSha256(
        payload.fileName(),
        QByteArrayLiteral("05600edbc7023bf6341177a7f4d4df9a7d7a5b51bddbbb687fe52e74287538db")),
        "known SHA-256 is accepted") && passed;
    passed = expect(!jamlink::desktop::verifyFileSha256(payload.fileName(), QByteArray(64, '0')),
                    "wrong SHA-256 is rejected") && passed;

    if (passed) {
        std::cout << "[PASS] update selection, channel isolation, and SHA-256 verification\n";
    }
    return passed ? 0 : 1;
}
