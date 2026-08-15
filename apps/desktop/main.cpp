// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_controller.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include "jamlink/diagnostics/session_log.hpp"

#include <filesystem>

namespace {

std::filesystem::path preferencePath(const QString& overridePath) {
    if (!overridePath.isEmpty()) {
        return std::filesystem::path(overridePath.toStdWString());
    }
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path((directory + QStringLiteral("/preferences.jlpf")).toStdWString());
}

std::uint32_t dimension(const QCommandLineParser& parser, const QCommandLineOption& option) {
    bool valid = false;
    const auto value = parser.value(option).toUInt(&valid);
    return valid ? value : 0U;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("JamLink"));
    QCoreApplication::setOrganizationName(QStringLiteral("JamLink"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAMLINK_VERSION_STRING));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("JamLink desktop application"));
    parser.addHelpOption();
    const QCommandLineOption visualFixtureOption(QStringLiteral("visual-fixture"));
    const QCommandLineOption pageOption(
        QStringLiteral("page"), QStringLiteral("Initial page"), QStringLiteral("page"),
        QStringLiteral("auto"));
    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"), QStringLiteral("Capture and exit"), QStringLiteral("path"));
    const QCommandLineOption preferencesOption(
        QStringLiteral("preferences"), QStringLiteral("Preference file override"),
        QStringLiteral("path"));
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Window width override"), QStringLiteral("pixels"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Window height override"), QStringLiteral("pixels"));
    const QCommandLineOption minimumCaptureWidthOption(
        QStringLiteral("minimum-capture-width"),
        QStringLiteral("Minimum physical screenshot width"), QStringLiteral("pixels"));
    const QCommandLineOption minimumCaptureHeightOption(
        QStringLiteral("minimum-capture-height"),
        QStringLiteral("Minimum physical screenshot height"), QStringLiteral("pixels"));
    const QCommandLineOption expectedCaptureWidthOption(
        QStringLiteral("expected-capture-width"),
        QStringLiteral("Exact physical screenshot width"), QStringLiteral("pixels"));
    const QCommandLineOption expectedCaptureHeightOption(
        QStringLiteral("expected-capture-height"),
        QStringLiteral("Exact physical screenshot height"), QStringLiteral("pixels"));
    const QCommandLineOption resetPreferencesOption(QStringLiteral("reset-preferences"));
    const QCommandLineOption expectRestoredOption(QStringLiteral("expect-restored"));
    parser.addOptions({visualFixtureOption, pageOption, screenshotOption,
                       preferencesOption, widthOption, heightOption,
                       minimumCaptureWidthOption, minimumCaptureHeightOption,
                       expectedCaptureWidthOption, expectedCaptureHeightOption,
                       resetPreferencesOption, expectRestoredOption});
    parser.process(application);

    const auto resolvedPreferencePath = preferencePath(parser.value(preferencesOption));
    // Local only, and never uploaded. The first two-person test failed with no
    // record of which step broke, so a session now leaves evidence behind.
    if (!parser.isSet(visualFixtureOption)) {
        const auto logDirectory = QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation);
        if (!logDirectory.isEmpty()) {
            jamlink::diagnostics::SessionLog::instance().open(
                std::filesystem::path(logDirectory.toStdWString()));
            JAMLINK_LOG("app", "JamLink " JAMLINK_VERSION_STRING " started");
        }
    }
    if (parser.isSet(resetPreferencesOption)) {
        std::error_code removeError;
        std::filesystem::remove(resolvedPreferencePath, removeError);
        auto temporary = resolvedPreferencePath;
        temporary += ".tmp";
        std::filesystem::remove(temporary, removeError);
    }
    jamlink::desktop::AppController controller(
        resolvedPreferencePath,
        parser.isSet(visualFixtureOption),
        parser.value(pageOption),
        dimension(parser, widthOption),
        dimension(parser, heightOption));
    QObject::connect(
        controller.updateManager(), &jamlink::desktop::UpdateManager::restartRequested,
        &application, &QCoreApplication::quit);
    if (parser.isSet(expectRestoredOption) && !controller.restoredPreferences()) {
        return 5;
    }

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule(QStringLiteral("JamLinkDesktop"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 2;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (window == nullptr) {
        return 3;
    }
    const auto requestedWidth = dimension(parser, widthOption);
    const auto requestedHeight = dimension(parser, heightOption);
    if (requestedWidth >= 532U) {
        window->setWidth(static_cast<int>(requestedWidth));
    }
    if (requestedHeight >= 480U) {
        window->setHeight(static_cast<int>(requestedHeight));
    }

    const auto screenshotPath = parser.value(screenshotOption);
    if (!screenshotPath.isEmpty()) {
        const auto minimumCaptureWidth =
            std::max(532U, dimension(parser, minimumCaptureWidthOption));
        const auto minimumCaptureHeight =
            std::max(480U, dimension(parser, minimumCaptureHeightOption));
        const auto expectedCaptureWidth = dimension(parser, expectedCaptureWidthOption);
        const auto expectedCaptureHeight = dimension(parser, expectedCaptureHeightOption);
        QTimer::singleShot(650, &application,
                           [window, screenshotPath, minimumCaptureWidth,
                            minimumCaptureHeight, expectedCaptureWidth,
                            expectedCaptureHeight, &application] {
            const QImage image = window->grabWindow();
            const QFileInfo output(screenshotPath);
            QDir().mkpath(output.absolutePath());
            const bool widthValid = expectedCaptureWidth > 0U
                ? image.width() == static_cast<int>(expectedCaptureWidth)
                : image.width() >= static_cast<int>(minimumCaptureWidth);
            const bool heightValid = expectedCaptureHeight > 0U
                ? image.height() == static_cast<int>(expectedCaptureHeight)
                : image.height() >= static_cast<int>(minimumCaptureHeight);
            const bool dimensionsValid = widthValid && heightValid;
            application.exit(dimensionsValid && image.save(screenshotPath) ? 0 : 4);
        });
    }

    const int result = application.exec();
    controller.persistNow();
    return result;
}
