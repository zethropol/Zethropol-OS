#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>

class UpdateService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(QVariantList updates READ updates NOTIFY stateChanged)

public:
    explicit UpdateService(QObject *parent = nullptr);

    bool checking() const;
    bool available() const;
    int count() const;
    QVariantList updates() const;

public slots:
    void check();

signals:
    void stateChanged();

private slots:
    void readOutput();

private:
    QProcess process;
    bool m_checking = false;
    bool m_available = false;
    int m_count = 0;
    QVariantList m_updates;
};
