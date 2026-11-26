#include "ChannelManager.h"
#include "EpgManager.h"
#include "MpvItem.h"
#include "StreamResolver.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtQml>
#include <QtTest>

#include <clocale>

using namespace zethropol::tv;

namespace {

QQuickItem *findVisualItemByObjectName(QQuickItem *root, const QString &name)
{
    if (!root) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findVisualItemByObjectName(child, name)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

class TvQmlSmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        std::setlocale(LC_NUMERIC, "C");
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        qmlRegisterType<MpvItem>("Zethropol.TV", 1, 0, "MpvItem");
    }

    void mainQmlLoadsWithPointingHandCursorSingleClickSwitchAndEpgFallback()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        ChannelManager channels(tempDir.path());
        EpgManager epg(true);
        StreamResolver resolver;

        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errs) { warnings.append(errs); });

        engine.rootContext()->setContextProperty(QStringLiteral("ChannelBridge"), &channels);
        engine.rootContext()->setContextProperty(QStringLiteral("EpgBridge"), &epg);
        engine.rootContext()->setContextProperty(QStringLiteral("StreamResolverBridge"), &resolver);

        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Zethropol/TV/qml/Main.qml")));
        QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
        QVERIFY2(warnings.isEmpty(), qPrintable(QStringLiteral("Unexpected QML warnings count: %1").arg(warnings.count())));

        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(win, 5000));
        QCoreApplication::processEvents();

        auto *sidebar = findVisualItemByObjectName(win->contentItem(), QStringLiteral("channelSidebar"));
        auto *epgBar = findVisualItemByObjectName(win->contentItem(), QStringLiteral("epgTimelineBar"));
        auto *osdBar = findVisualItemByObjectName(win->contentItem(), QStringLiteral("videoOsdBar"));
        auto *mpv = qobject_cast<MpvItem *>(findVisualItemByObjectName(win->contentItem(), QStringLiteral("tvMpvPlayer")));
        QVERIFY(sidebar != nullptr);
        QVERIFY(epgBar != nullptr);
        QVERIFY(osdBar != nullptr);
        QVERIFY(mpv != nullptr);
        QCOMPARE(mpv->mirrorVertically(), false);
        QCOMPARE(mpv->configuredAudioOutput(), QStringLiteral("pipewire,pulse,alsa"));
        QCOMPARE(mpv->configuredAudioClientName(), QStringLiteral("Zethropol TV"));
        QVERIFY(mpv->volume() > 0);
        QCOMPARE(mpv->muted(), false);
        QVERIFY(!mpv->audioStatus().isEmpty());

        // Verify horizontal category chips bar exists and stays overflow-safe
        auto *catFlick = findVisualItemByObjectName(sidebar, QStringLiteral("categoryChipsFlickable"));
        QVERIFY(catFlick != nullptr);
        QCOMPARE(catFlick->clip(), true);

        // Verify channel row delegate MouseArea has hoverEnabled=true and Qt.PointingHandCursor
        QQuickItem *row0Mouse = nullptr;
        QQuickItem *row1Mouse = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            (row0Mouse = findVisualItemByObjectName(sidebar, QStringLiteral("channelRowMouseArea_0"))) != nullptr
                && (row1Mouse = findVisualItemByObjectName(sidebar, QStringLiteral("channelRowMouseArea_1"))) != nullptr,
            3000);
        QCOMPARE(row0Mouse->property("hoverEnabled").toBool(), true);
        QCOMPARE(row0Mouse->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(row1Mouse->cursor().shape(), Qt::PointingHandCursor);

        // Single-click channel row 0 (TRT 1, has Turkish EPG) -> switches immediately and shows live EPG
        QSignalSpy selectSpy(&channels, &ChannelManager::channelSelected);
        const QPointF p0 = row0Mouse->mapToScene(QPointF(row0Mouse->width() * 0.35, row0Mouse->height() * 0.5));
        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, p0.toPoint());
        QCoreApplication::processEvents();

        QCOMPARE(selectSpy.count(), 1);
        QCOMPARE(channels.currentChannelNumber(), 1);
        QCOMPARE(channels.currentChannelName(), QStringLiteral("TRT 1"));
        QVERIFY(epg.hasEpgData());
        QVERIFY(epg.hasCurrentProgram());

        // Single-click channel row 1 (ATV, has Turkish EPG) -> switches immediately and shows live EPG
        const QPointF p1 = row1Mouse->mapToScene(QPointF(row1Mouse->width() * 0.35, row1Mouse->height() * 0.5));
        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, p1.toPoint());
        QCoreApplication::processEvents();

        QCOMPARE(selectSpy.count(), 2);
        QCOMPARE(channels.currentChannelNumber(), 2);
        QCOMPARE(channels.currentChannelName(), QStringLiteral("ATV"));
        QVERIFY(epg.hasEpgData());
        QVERIFY(epg.hasCurrentProgram());
        // Verify channel switching never locks volume to 0 or muted=true
        QVERIFY(mpv->volume() > 0);
        QCOMPARE(mpv->muted(), false);

        // Custom channel without EPG -> falls back gracefully with Turkish status message
        QVERIFY(channels.addSingleChannel(QStringLiteral("EPG Olmayan Kanal"),
                                          QStringLiteral("https://example.org/custom-noepg.m3u8"),
                                          QStringLiteral("Özel")));
        QCoreApplication::processEvents();

        QCOMPARE(selectSpy.count(), 3);
        QVERIFY(!epg.hasEpgData());

        auto *fallbackLbl = findVisualItemByObjectName(epgBar, QStringLiteral("epgFallbackLabel"));
        QVERIFY(fallbackLbl != nullptr);
        QCOMPARE(fallbackLbl->property("text").toString(), QStringLiteral("Yayın akışı bilgisi bekleniyor"));

        resolver.cancel();
        mpv->stop();
    }
};

QTEST_MAIN(TvQmlSmokeTest)
#include "TvQmlSmokeTest.moc"
