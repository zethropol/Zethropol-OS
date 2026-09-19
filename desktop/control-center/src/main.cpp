#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>

class HardwareBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit HardwareBridge(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&process, &QProcess::finished, this, &HardwareBridge::readOutput);
        process.setProgram(QStringLiteral("/home/Zevor/Zethropol-OS/services/hardware/target/debug/hardware"));
        process.setArguments({QStringLiteral("--json")});
        process.start();
    }

    QVariantMap state() const { return m_state; }

signals:
    void stateChanged();

private slots:
    void readOutput()
    {
        const QByteArray output = process.readAllStandardOutput();
        const QJsonDocument document = QJsonDocument::fromJson(output);
        if (!document.isObject()) return;
        m_state = document.object().toVariantMap();
        emit stateChanged();
    }

private:
    QProcess process;
    QVariantMap m_state;
};

#include "main.moc"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    HardwareBridge hardwareBridge;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("HardwareBridge"), &hardwareBridge);

    const QUrl url(QStringLiteral("qrc:/qt/qml/Zethropol/ControlCenter/qml/Main.qml"));

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
