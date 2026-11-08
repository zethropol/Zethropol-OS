#pragma once

#include <QObject>
#include <QVariantList>

class ServiceService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList services READ services NOTIFY stateChanged)
    Q_PROPERTY(QVariantList failedServices READ failedServices NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(int active READ active NOTIFY stateChanged)
    Q_PROPERTY(int failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString actionStatus READ actionStatus NOTIFY stateChanged)
    Q_PROPERTY(QString failureDetailsName READ failureDetailsName NOTIFY stateChanged)
    Q_PROPERTY(QString failureDetailsText READ failureDetailsText NOTIFY stateChanged)
    Q_PROPERTY(bool failureDetailsBusy READ failureDetailsBusy NOTIFY stateChanged)

public:
    explicit ServiceService(QObject *parent = nullptr);

    QVariantList services() const;
    QVariantList failedServices() const;
    int total() const;
    int active() const;
    int failed() const;
    bool busy() const;
    QString actionStatus() const;
    QString failureDetailsName() const;
    QString failureDetailsText() const;
    bool failureDetailsBusy() const;

public slots:
    void refresh();
    void loadFailureDetails(const QString &name);
    void startService(const QString &name);
    void stopService(const QString &name);
    void restartService(const QString &name);
    void enableService(const QString &name);
    void disableService(const QString &name);

signals:
    void stateChanged();

private:
    bool serviceExists(const QString &name) const;
    void runAction(const QString &name, const QString &action);

    QVariantList m_services;
    int m_total = 0;
    int m_active = 0;
    int m_failed = 0;
    bool m_busy = false;
    QString m_actionStatus;
    QString m_failureDetailsName;
    QString m_failureDetailsText;
    bool m_failureDetailsBusy = false;
};
