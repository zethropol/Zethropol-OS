#pragma once

#include <QAbstractListModel>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkReply;

namespace zethropol::tv {

struct ChannelEntry
{
    QString id;
    int number = 0;
    QString name;
    QString group;
    QString logoUrl;
    QString streamUrl;
    QString sourcePageUrl;
    QStringList fallbackUrls;
    QString tvgId;
    bool favorite = false;
};

struct M3uParseResult
{
    QVector<ChannelEntry> channels;
    int rejectedCount = 0;
    QString errorMessage;
};

class ChannelManager : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QString selectedCategory READ selectedCategory WRITE setSelectedCategory NOTIFY selectedCategoryChanged)
    Q_PROPERTY(QStringList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(int totalChannelCount READ totalChannelCount NOTIFY totalChannelCountChanged)
    Q_PROPERTY(int filteredChannelCount READ filteredChannelCount NOTIFY filteredChannelCountChanged)
    Q_PROPERTY(QString currentChannelId READ currentChannelId NOTIFY currentChannelChanged)
    Q_PROPERTY(int currentChannelNumber READ currentChannelNumber NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelName READ currentChannelName NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelGroup READ currentChannelGroup NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelStreamUrl READ currentChannelStreamUrl NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelSourcePageUrl READ currentChannelSourcePageUrl NOTIFY currentChannelChanged)
    Q_PROPERTY(QStringList currentChannelFallbackUrls READ currentChannelFallbackUrls NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelLogoUrl READ currentChannelLogoUrl NOTIFY currentChannelChanged)
    Q_PROPERTY(QString currentChannelTvgId READ currentChannelTvgId NOTIFY currentChannelChanged)
    Q_PROPERTY(bool currentChannelFavorite READ currentChannelFavorite NOTIFY currentChannelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(int lastRejectedUrlCount READ lastRejectedUrlCount NOTIFY lastRejectedUrlCountChanged)

public:
    enum Roles {
        ChannelIdRole = Qt::UserRole + 1,
        NumberRole,
        NameRole,
        GroupRole,
        LogoUrlRole,
        StreamUrlRole,
        SourcePageUrlRole,
        FallbackUrlsRole,
        TvgIdRole,
        FavoriteRole,
        ActiveRole,
    };

    explicit ChannelManager(const QString &configDirOverride = QString(),
                            QObject *parent = nullptr);
    ~ChannelManager() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    static M3uParseResult parseM3uContent(const QByteArray &rawBytes);

    QString searchQuery() const { return m_searchQuery; }
    void setSearchQuery(const QString &query);

    QString selectedCategory() const { return m_selectedCategory; }
    void setSelectedCategory(const QString &category);

    QStringList categories() const { return m_categories; }
    int totalChannelCount() const { return m_allChannels.size(); }
    int filteredChannelCount() const { return m_filteredIndices.size(); }

    QString currentChannelId() const;
    int currentChannelNumber() const;
    QString currentChannelName() const;
    QString currentChannelGroup() const;
    QString currentChannelStreamUrl() const;
    QString currentChannelSourcePageUrl() const;
    QStringList currentChannelFallbackUrls() const;
    QString currentChannelLogoUrl() const;
    QString currentChannelTvgId() const;
    bool currentChannelFavorite() const;

    bool loading() const { return m_loading; }
    QString statusMessage() const { return m_statusMessage; }
    int lastRejectedUrlCount() const { return m_lastRejectedUrlCount; }

    Q_INVOKABLE bool selectChannel(int filteredIndex);
    Q_INVOKABLE bool selectChannelById(const QString &channelId);
    Q_INVOKABLE bool nextChannel();
    Q_INVOKABLE bool previousChannel();
    Q_INVOKABLE void toggleFavorite(const QString &channelId);
    Q_INVOKABLE bool importM3uFromFile(const QString &pathOrFileUrl, bool replaceExisting = false);
    Q_INVOKABLE void importM3uFromUrl(const QString &url, bool replaceExisting = false);
    Q_INVOKABLE bool addSingleChannel(const QString &name,
                                      const QString &urlOrPage,
                                      const QString &group = QString(),
                                      const QString &logoUrl = QString());
    Q_INVOKABLE void resetToDefaultChannels();

    QString stateFilePath() const;

signals:
    void searchQueryChanged();
    void selectedCategoryChanged();
    void categoriesChanged();
    void totalChannelCountChanged();
    void filteredChannelCountChanged();
    void currentChannelChanged();
    void loadingChanged();
    void statusMessageChanged();
    void lastRejectedUrlCountChanged();
    void channelSelected(const QString &streamUrl, const QString &name, const QString &tvgId);

private:
    void startRemoteM3uFetch(const QString &url, bool replaceExisting, int redirectDepth);
    void applyParsedChannels(const M3uParseResult &parsed,
                             bool replaceExisting,
                             const QString &sourceLabel);
    void loadDefaultChannels();
    void loadPersistedState();
    void savePersistedState() const;
    void rebuildCategories();
    void rebuildFilter();
    int findAllIndexById(const QString &id) const;
    void setLoading(bool loading);
    void setStatusMessage(const QString &msg);

    QString m_configDir;
    QVector<ChannelEntry> m_allChannels;
    QVector<int> m_filteredIndices;
    QSet<QString> m_favoriteIds;
    QStringList m_categories;
    QString m_searchQuery;
    QString m_selectedCategory = QStringLiteral("Tümü");
    QString m_currentChannelId;
    bool m_loading = false;
    QString m_statusMessage;
    int m_lastRejectedUrlCount = 0;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_activeReply;
    QByteArray m_downloadBuffer;
    bool m_downloadOverflow = false;
};

} // namespace zethropol::tv
