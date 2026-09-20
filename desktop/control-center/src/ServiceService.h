#pragma once

#include <QObject>
#include <QVariantList>

class ServiceService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList services READ services NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(int active READ active NOTIFY stateChanged)
    Q_PROPERTY(int failed READ failed NOTIFY stateChanged)

public:
    explicit ServiceService(QObject *parent = nullptr);

    QVariantList services() const;
    int total() const;
    int active() const;
    int failed() const;

public slots:
    void refresh();

signals:
    void stateChanged();

private:
    QVariantList m_services;
    int m_total = 0;
    int m_active = 0;
    int m_failed = 0;
};
