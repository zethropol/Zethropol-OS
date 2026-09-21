#pragma once

#include <QObject>
#include <QVariantList>
#include <QString>

class ApplicationService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList applications READ applications NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(bool removing READ removing NOTIFY stateChanged)
    Q_PROPERTY(QString removeStatus READ removeStatus NOTIFY stateChanged)

public:
    explicit ApplicationService(QObject *parent = nullptr);

    QVariantList applications() const;
    int total() const;
    bool removing() const;
    QString removeStatus() const;

public slots:
    void refresh();
    void removeApplication(const QString &package);

signals:
    void stateChanged();

private:
    QVariantList m_applications;
    int m_total = 0;
    bool m_removing = false;
    QString m_removeStatus;
};
