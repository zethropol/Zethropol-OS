#include "ApplicationService.h"
#include "CompatibilityManager.h"
#include "HardwareServiceClient.h"
#include "NotificationService.h"
#include "ProcessService.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "UpdateInstaller.h"
#include "UpdateScanner.h"
#include "core/zethropol_monitoring_client.h"
#include "core/zethropol_power_client.h"

#include <QDBusConnection>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>

namespace {

QString collectTexts(QQuickItem *root)
{
    if (!root)
        return {};
    QString out;
    const QVariant textProp = root->property("text");
    if (textProp.isValid() && textProp.typeId() == QMetaType::QString) {
        const QString s = textProp.toString();
        if (!s.isEmpty())
            out += s + QLatin1Char('\n');
    }
    for (QQuickItem *child : root->childItems())
        out += collectTexts(child);
    return out;
}

class MockMonitoringBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
public:
    QVariantMap state() const { return m_state; }
    void setState(const QVariantMap &s)
    {
        m_state = s;
        emit stateChanged();
    }
signals:
    void stateChanged();
private:
    QVariantMap m_state;
};

} // namespace

class TabRegressionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(QDBusConnection::systemBus().isConnected());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
        qmlRegisterSingletonType(
            QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/SystemState.qml")),
            "Zethropol.ControlCenter", 1, 0, "SystemState");
    }

    void allTwelveTabsLoadSafelyWithMissingServicesAndEmptyPath()
    {
        QTemporaryDir emptyPathDir;
        QVERIFY(emptyPathDir.isValid());
        const QByteArray previousPath = qgetenv("PATH");
        const QByteArray previousConfig = qgetenv("XDG_CONFIG_HOME");
        const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("PATH", emptyPathDir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", emptyPathDir.path().toUtf8());
        const auto restoreEnv = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (hadConfig)
                qputenv("XDG_CONFIG_HOME", previousConfig);
            else
                qunsetenv("XDG_CONFIG_HOME");
        });

        HardwareServiceClient hardware;
        UpdateScanner scanner;
        UpdateInstaller installer(&scanner);
        RecoveryService recovery;
        SecurityService security;
        ServiceService services;
        ApplicationService applications;
        ProcessService processes;
        NotificationService notifications;
        ZethropolPowerClient power;
        ZethropolMonitoringClient monitoring;
        CompatibilityManager compatibility;

        // Wait for isolated scanner to finish with missing tools (fail-closed)
        QTRY_VERIFY_WITH_TIMEOUT(!scanner.busy(), 5000);
        QVERIFY(!installer.liveExecutionEnabled());
        QVERIFY(!installer.officialRepoReady());

        // Recovery must fail-closed to backend_unavailable on isolated bus without Recovery daemon
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("backend_unavailable"));
        QVERIFY(!recovery.available());
        QCOMPARE(recovery.snapshotCount(), 0);

        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &hardware},
                 {"UpdateBridge", &scanner},
                 {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery},
                 {"SecurityBridge", &security},
                 {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications},
                 {"ProcessBridge", &processes},
                 {"NotificationBridge", &notifications},
                 {"PowerBridge", &power},
                 {"MonitoringBridge", &monitoring},
                 {"CompatibilityBridge", &compatibility}}) {
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        }

        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QCOMPARE(pages->property("count").toInt(), 13);

        const QStringList expectedTabTitles = {
            QStringLiteral("Overview"),
            QStringLiteral("Hardware"),
            QStringLiteral("Diagnostics"),
            QStringLiteral("System"),
            QStringLiteral("Performance"),
            QStringLiteral("Power"),
            QStringLiteral("Updates"),
            QStringLiteral("Recovery"),
            QStringLiteral("Security"),
            QStringLiteral("Services"),
            QStringLiteral("Applications"),
            QStringLiteral("Processes"),
            QStringLiteral("Compatibility"),
        };

        for (int index = 0; index < 13; ++index) {
            QVERIFY2(pages->setProperty("currentIndex", index),
                     qPrintable(QStringLiteral("Failed to switch to tab %1 (%2)").arg(index).arg(expectedTabTitles[index])));
            QTest::qWait(30);
            QCOMPARE(pages->property("currentIndex").toInt(), index);

            QQuickItem *currentPage = nullptr;
            const auto childItems = pages->childItems();
            if (index < childItems.size())
                currentPage = childItems.at(index);
            QVERIFY2(currentPage != nullptr,
                     qPrintable(QStringLiteral("Missing page item for tab %1").arg(expectedTabTitles[index])));

            const QString pageTexts = collectTexts(currentPage);
            QVERIFY2(!pageTexts.contains(QStringLiteral("NaN")),
                     qPrintable(QStringLiteral("NaN found on tab %1").arg(expectedTabTitles[index])));
            QVERIFY2(!pageTexts.contains(QStringLiteral("undefined")),
                     qPrintable(QStringLiteral("undefined found on tab %1").arg(expectedTabTitles[index])));

            switch (index) {
            case 0: // Overview
                QVERIFY(pageTexts.contains(QStringLiteral("Overview")));
                QVERIFY(pageTexts.contains(QStringLiteral("Unknown")));
                QVERIFY(pageTexts.contains(QStringLiteral("System data unavailable")));
                break;
            case 1: { // Hardware
                auto *hwPage = window->findChild<QQuickItem *>(QStringLiteral("hardwarePage"));
                QVERIFY(hwPage);
                QVERIFY(pageTexts.contains(QStringLiteral("Unavailable")));
                break;
            }
            case 2: // Diagnostics
                QVERIFY(pageTexts.contains(QStringLiteral("Diagnostics")));
                QVERIFY(pageTexts.contains(QStringLiteral("Unknown")));
                break;
            case 3: // System
                QVERIFY(pageTexts.contains(QStringLiteral("Unknown")));
                break;
            case 4: // Performance
                QVERIFY(pageTexts.contains(QStringLiteral("Download: Unknown")));
                QVERIFY(pageTexts.contains(QStringLiteral("Upload: Unknown")));
                QVERIFY(!pageTexts.contains(QStringLiteral("Download: 0.00 Mbps")));
                break;
            case 5: // Power
                QVERIFY(pageTexts.contains(QStringLiteral("Unknown")));
                QVERIFY(pageTexts.contains(QStringLiteral("N/A")));
                QVERIFY(pageTexts.contains(QStringLiteral("Not available")));
                break;
            case 6: { // Updates
                auto *updatePage = window->findChild<QQuickItem *>(QStringLiteral("updateReportPage"));
                auto *startBtn = window->findChild<QObject *>(QStringLiteral("startUpdateInstallation"));
                QVERIFY(updatePage);
                QVERIFY(startBtn);
                QVERIFY(!startBtn->property("enabled").toBool());
                QVERIFY(pageTexts.contains(QStringLiteral("Güncelleme raporu")));
                break;
            }
            case 7: // Recovery
                QVERIFY(pageTexts.contains(QStringLiteral("Recovery")));
                break;
            case 8: // Security
                QVERIFY(!pageTexts.isEmpty());
                break;
            case 9: // Services
                QVERIFY(!pageTexts.isEmpty());
                break;
            case 10: // Applications
                QVERIFY(pageTexts.contains(QStringLiteral("applications detected")));
                break;
            case 11: // Processes
                QVERIFY(!pageTexts.isEmpty());
                break;
            case 12: { // Compatibility
                auto *compatPage = window->findChild<QQuickItem *>(QStringLiteral("compatibilityPage"));
                QVERIFY(compatPage);
                QVERIFY(pageTexts.contains(QStringLiteral("Windows Uyumluluğu")));
                QVERIFY(pageTexts.contains(QStringLiteral("zethropol-compatibility")));
                break;
            }
            }
        }

        QVERIFY2(warnings.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected QML warnings count: %1").arg(warnings.count())));
    }

    void performanceTabHandlesMissingInvalidAndZeroMetricsSafely()
    {
        QTemporaryDir emptyPathDir;
        QVERIFY(emptyPathDir.isValid());
        const QByteArray previousPath = qgetenv("PATH");
        const QByteArray previousConfig = qgetenv("XDG_CONFIG_HOME");
        const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("PATH", emptyPathDir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", emptyPathDir.path().toUtf8());
        const auto restoreEnv = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (hadConfig)
                qputenv("XDG_CONFIG_HOME", previousConfig);
            else
                qunsetenv("XDG_CONFIG_HOME");
        });

        HardwareServiceClient hardware;
        UpdateScanner scanner;
        UpdateInstaller installer(&scanner);
        RecoveryService recovery;
        SecurityService security;
        ServiceService services;
        ApplicationService applications;
        ProcessService processes;
        NotificationService notifications;
        ZethropolPowerClient power;
        MockMonitoringBridge monitoring;
        CompatibilityManager compatibility;

        QTRY_VERIFY_WITH_TIMEOUT(!scanner.busy(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);

        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &hardware},
                 {"UpdateBridge", &scanner},
                 {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery},
                 {"SecurityBridge", &security},
                 {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications},
                 {"ProcessBridge", &processes},
                 {"NotificationBridge", &notifications},
                 {"PowerBridge", &power},
                 {"MonitoringBridge", &monitoring},
                 {"CompatibilityBridge", &compatibility}}) {
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        }

        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QVERIFY(pages->setProperty("currentIndex", 4));
        QTest::qWait(30);
        QQuickItem *perfPage = pages->childItems().at(4);
        QVERIFY(perfPage);

        // 1. Empty state {} -> safe fallback, no NaN/undefined
        QString texts = collectTexts(perfPage);
        QVERIFY(!texts.contains(QStringLiteral("NaN")));
        QVERIFY(!texts.contains(QStringLiteral("undefined")));
        QVERIFY(texts.contains(QStringLiteral("VRAM: Unknown")));
        QVERIFY(texts.contains(QStringLiteral("Usage: Unknown")));

        // 2. Explicit null values -> safe fallback
        monitoring.setState({
            {QStringLiteral("gpu_vram_used_gb"), QVariant::fromValue(nullptr)},
            {QStringLiteral("gpu_vram_total_gb"), QVariant::fromValue(nullptr)},
            {QStringLiteral("storage_used_gb"), QVariant::fromValue(nullptr)},
            {QStringLiteral("storage_capacity_gb"), QVariant::fromValue(nullptr)},
        });
        QTest::qWait(20);
        texts = collectTexts(perfPage);
        QVERIFY(!texts.contains(QStringLiteral("NaN")));
        QVERIFY(texts.contains(QStringLiteral("VRAM: Unknown")));
        QVERIFY(texts.contains(QStringLiteral("Usage: Unknown")));

        // 3. Invalid numeric values (NaN, Infinity, non-numeric string) -> safe fallback
        monitoring.setState({
            {QStringLiteral("gpu_vram_used_gb"), std::numeric_limits<double>::quiet_NaN()},
            {QStringLiteral("gpu_vram_total_gb"), std::numeric_limits<double>::infinity()},
            {QStringLiteral("storage_used_gb"), QStringLiteral("invalid")},
            {QStringLiteral("storage_capacity_gb"), 500.0},
        });
        QTest::qWait(20);
        texts = collectTexts(perfPage);
        QVERIFY(!texts.contains(QStringLiteral("NaN")));
        QVERIFY(!texts.contains(QStringLiteral("Infinity")));
        QVERIFY(texts.contains(QStringLiteral("VRAM: Unknown")));
        QVERIFY(texts.contains(QStringLiteral("Usage: Unknown")));

        // 4. Valid real zero and valid finite numbers -> formatted accurately
        monitoring.setState({
            {QStringLiteral("gpu_vram_used_gb"), 0.0},
            {QStringLiteral("gpu_vram_total_gb"), 8.0},
            {QStringLiteral("storage_used_gb"), 0.0},
            {QStringLiteral("storage_capacity_gb"), 512.0},
        });
        QTest::qWait(20);
        texts = collectTexts(perfPage);
        QVERIFY(texts.contains(QStringLiteral("VRAM: 0.00 / 8.00 GB")));
        QVERIFY(texts.contains(QStringLiteral("Usage: 0.0 / 512.0 GB")));

        QVERIFY2(warnings.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected QML warnings count: %1").arg(warnings.count())));
    }

    void processesTabColumnAlignmentAndMemoryDisplay()
    {
        QTemporaryDir emptyPathDir;
        QVERIFY(emptyPathDir.isValid());
        const QByteArray previousPath = qgetenv("PATH");
        const QByteArray previousConfig = qgetenv("XDG_CONFIG_HOME");
        const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("PATH", emptyPathDir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", emptyPathDir.path().toUtf8());
        const auto restoreEnv = qScopeGuard([&] {
            qputenv("PATH", previousPath);
            if (hadConfig)
                qputenv("XDG_CONFIG_HOME", previousConfig);
            else
                qunsetenv("XDG_CONFIG_HOME");
        });

        HardwareServiceClient hardware;
        UpdateScanner scanner;
        UpdateInstaller installer(&scanner);
        RecoveryService recovery;
        SecurityService security;
        ServiceService services;
        ApplicationService applications;
        ProcessService processes;
        NotificationService notifications;
        ZethropolPowerClient power;
        ZethropolMonitoringClient monitoring;
        CompatibilityManager compatibility;

        QTRY_VERIFY_WITH_TIMEOUT(!scanner.busy(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);

        // Real ProcessService must read non-zero RAM/RSS from /proc
        QVERIFY(processes.totalMemoryBytes() > 0);
        QVERIFY(processes.usedMemoryBytes() > 0);
        QVERIFY(processes.usedMemoryFormatted() != QStringLiteral("0 KB"));
        QVERIFY(!processes.processes().isEmpty());

        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &hardware},
                 {"UpdateBridge", &scanner},
                 {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery},
                 {"SecurityBridge", &security},
                 {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications},
                 {"ProcessBridge", &processes},
                 {"NotificationBridge", &notifications},
                 {"PowerBridge", &power},
                 {"MonitoringBridge", &monitoring},
                 {"CompatibilityBridge", &compatibility}}) {
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        }

        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QVERIFY(pages->setProperty("currentIndex", 11)); // Processes tab
        QTest::qWait(80);

        auto *procPage = window->findChild<QQuickItem *>(QStringLiteral("processesPage"));
        auto *listView = window->findChild<QQuickItem *>(QStringLiteral("processListView"));
        auto *vScroll = window->findChild<QQuickItem *>(QStringLiteral("processVerticalScrollBar"));
        auto *hScroll = window->findChild<QQuickItem *>(QStringLiteral("processTableHorizontalScroll"));
        QVERIFY(procPage && listView && vScroll && hScroll);

        const QString pageTexts = collectTexts(procPage);
        QVERIFY(pageTexts.contains(processes.usedMemoryFormatted()));
        QVERIFY(!pageTexts.contains(QStringLiteral("NaN")));
        QVERIFY(!pageTexts.contains(QStringLiteral("undefined")));

        auto verifyColumnAlignment = [&]() {
            const QStringList headerNames = {
                QStringLiteral("headerColName"), QStringLiteral("headerColPid"),
                QStringLiteral("headerColCpu"), QStringLiteral("headerColRam"),
                QStringLiteral("headerColUser"), QStringLiteral("headerColStatus"),
                QStringLiteral("headerColAction")
            };
            const QStringList rowNames = {
                QStringLiteral("rowColName"), QStringLiteral("rowColPid"),
                QStringLiteral("rowColCpu"), QStringLiteral("rowColRam"),
                QStringLiteral("rowColUser"), QStringLiteral("rowColStatus"),
                QStringLiteral("rowColAction")
            };

            // Find first realized row delegate inside ListView contentItem
            auto *contentItem = listView->property("contentItem").value<QQuickItem *>();
            QVERIFY(contentItem);
            QQuickItem *firstRow = nullptr;
            for (QQuickItem *child : contentItem->childItems()) {
                if (child->objectName() == QStringLiteral("processRowDelegate") && child->isVisible()) {
                    firstRow = child;
                    break;
                }
            }
            QVERIFY(firstRow);

            for (int i = 0; i < headerNames.size(); ++i) {
                auto *hdr = procPage->findChild<QQuickItem *>(headerNames[i]);
                auto *cell = firstRow->findChild<QQuickItem *>(rowNames[i]);
                QVERIFY2(hdr && cell, qPrintable(QStringLiteral("Missing col %1").arg(headerNames[i])));
                const qreal hdrX = hdr->mapToScene(QPointF(0, 0)).x();
                const qreal cellX = cell->mapToScene(QPointF(0, 0)).x();
                QVERIFY2(qAbs(hdrX - cellX) <= 1.0,
                         qPrintable(QStringLiteral("Column %1 X mismatch: header=%2 row=%3")
                                        .arg(headerNames[i]).arg(hdrX).arg(cellX)));
                QVERIFY2(qAbs(hdr->width() - cell->width()) <= 1.0,
                         qPrintable(QStringLiteral("Column %1 width mismatch: header=%2 row=%3")
                                        .arg(headerNames[i]).arg(hdr->width()).arg(cell->width())));
            }
        };

        // 1. Normal window width (1280x800) with vertical scrollbar active
        window->resize(1280, 800);
        QTest::qWait(80);
        QVERIFY(vScroll->property("visible").toBool());
        verifyColumnAlignment();

        // 2. Narrow window width (700x600) -> horizontal scroll activates without column drift
        window->resize(700, 600);
        QTest::qWait(80);
        QVERIFY(hScroll->property("contentWidth").toReal() > hScroll->width());
        verifyColumnAlignment();

        QVERIFY2(warnings.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected QML warnings count: %1").arg(warnings.count())));
    }
};

QTEST_MAIN(TabRegressionTest)
#include "TabRegressionTest.moc"
