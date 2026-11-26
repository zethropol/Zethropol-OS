#pragma once

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class QNetworkReply;

namespace zethropol::tv {

struct EpgProgram
{
    QString channelId;
    QDateTime startUtc;
    QDateTime stopUtc;
    QString title;
    QString description;
    QString category;
};

struct EpgParseResult
{
    QHash<QString, QVector<EpgProgram>> programsByChannel;
    QHash<QString, QString> displayNameToChannelId;
    int totalPrograms = 0;
    QString errorMessage;
};

class EpgManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool hasEpgData READ hasEpgData NOTIFY epgChanged)
    Q_PROPERTY(bool hasCurrentProgram READ hasCurrentProgram NOTIFY epgChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY epgChanged)
    Q_PROPERTY(QString currentDescription READ currentDescription NOTIFY epgChanged)
    Q_PROPERTY(QString currentTimeRange READ currentTimeRange NOTIFY epgChanged)
    Q_PROPERTY(double currentProgress READ currentProgress NOTIFY epgChanged)
    Q_PROPERTY(int currentRemainingMinutes READ currentRemainingMinutes NOTIFY epgChanged)
    Q_PROPERTY(QVariantList upcomingPrograms READ upcomingPrograms NOTIFY epgChanged)
    Q_PROPERTY(QString fallbackMessage READ fallbackMessage CONSTANT)
    Q_PROPERTY(QString recommendedXmltvUrl READ recommendedXmltvUrl CONSTANT)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    static constexpr qint64 kTwoHourWindowSecs = 2 * 60 * 60;

    explicit EpgManager(bool loadDemoEpg = true, QObject *parent = nullptr);
    ~EpgManager() override;

    static EpgParseResult parseXmltvContent(const QByteArray &rawBytes);
    static QDateTime parseXmltvTimestamp(const QString &raw);

    bool hasEpgData() const { return m_hasEpgData; }
    bool hasCurrentProgram() const { return m_hasCurrentProgram; }
    QString currentTitle() const { return m_currentTitle; }
    QString currentDescription() const { return m_currentDescription; }
    QString currentTimeRange() const { return m_currentTimeRange; }
    double currentProgress() const { return m_currentProgress; }
    int currentRemainingMinutes() const { return m_currentRemainingMinutes; }
    QVariantList upcomingPrograms() const { return m_upcomingPrograms; }
    QString fallbackMessage() const { return QStringLiteral("Yayın akışı bilgisi bekleniyor"); }
    QString recommendedXmltvUrl() const { return QStringLiteral("https://epgshare01.online/epgshare01/epg_ripper_TR1.xml.gz"); }
    bool loading() const { return m_loading; }
    QString statusMessage() const { return m_statusMessage; }

    Q_INVOKABLE void setActiveChannel(const QString &tvgId, const QString &channelName);
    Q_INVOKABLE bool importXmltvFromFile(const QString &pathOrFileUrl);
    Q_INVOKABLE void importXmltvFromUrl(const QString &url);
    Q_INVOKABLE void refresh();

    /// Deterministic evaluation hook for unit tests and scheduled refresh.
    void refreshForTime(const QDateTime &nowUtc);

    /// Directly load parsed XMLTV bytes (used by tests and demo schedule).
    bool loadXmltvBytes(const QByteArray &rawBytes, const QString &sourceLabel);

signals:
    void epgChanged();
    void loadingChanged();
    void statusMessageChanged();

private:
    void startRemoteXmltvFetch(const QString &url, int redirectDepth);
    void loadDemoSchedule();
    QString resolveChannelKey(const QString &tvgId, const QString &channelName) const;
    void setLoading(bool loading);
    void setStatusMessage(const QString &msg);

    QHash<QString, QVector<EpgProgram>> m_programsByChannel;
    QHash<QString, QString> m_displayNameToChannelId;

    QString m_activeTvgId;
    QString m_activeChannelName;

    bool m_hasEpgData = false;
    bool m_hasCurrentProgram = false;
    QString m_currentTitle;
    QString m_currentDescription;
    QString m_currentTimeRange;
    double m_currentProgress = 0.0;
    int m_currentRemainingMinutes = 0;
    QVariantList m_upcomingPrograms;

    bool m_loading = false;
    QString m_statusMessage;

    QTimer m_refreshTimer;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_activeReply;
    QByteArray m_downloadBuffer;
    bool m_downloadOverflow = false;
};

} // namespace zethropol::tv
