#pragma once

#include "MonitoringSharedClient.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace zethropol::monitoring {

/// Read-only Qt6/QML D-Bus data bridge for `org.zethropol.Monitoring.v1`.
///
/// Exposed to QML via `import org.zethropol.monitoring 1.0` as `MonitoringBridge`.
/// All properties are strictly read-only; no privileged actions, subprocesses,
/// or direct Hardware queries exist.
class MonitoringBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool stateAvailable READ stateAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString readiness READ readiness NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap metrics READ metrics NOTIFY metricsChanged)
    Q_PROPERTY(QDateTime lastUpdate READ lastUpdate NOTIFY stateChanged)
    Q_PROPERTY(qint64 lastUpdateMs READ lastUpdateMs NOTIFY stateChanged)

public:
    explicit MonitoringBridge(QObject *parent = nullptr);
    explicit MonitoringBridge(std::shared_ptr<MonitoringSharedClient> sharedClient,
                              QObject *parent = nullptr);
    ~MonitoringBridge() override;

    bool connected() const;
    bool stateAvailable() const;
    QString readiness() const;
    QString errorCode() const;
    QString errorMessage() const;
    QVariantMap metrics() const;
    QDateTime lastUpdate() const;
    qint64 lastUpdateMs() const;

    /// Returns `true` iff `key` is present with a valid finite numeric metric
    /// (distinguishes absent sensors from genuine `0.0` measurements).
    Q_INVOKABLE bool hasMetric(const QString &key) const;

    /// Returns the validated `double` value for `key`, or invalid `QVariant`
    /// (`undefined` in QML) when the metric is absent.
    Q_INVOKABLE QVariant metricValue(const QString &key) const;

    std::shared_ptr<MonitoringSharedClient> sharedClient() const { return m_client; }

signals:
    void stateChanged();
    void metricsChanged();

private:
    void attachClient(std::shared_ptr<MonitoringSharedClient> client);

    std::shared_ptr<MonitoringSharedClient> m_client;
};

} // namespace zethropol::monitoring
