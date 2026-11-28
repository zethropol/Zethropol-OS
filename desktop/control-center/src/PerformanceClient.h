#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class PerformanceClient : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY stateChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY stateChanged)
    Q_PROPERTY(QString cpuGovernor READ cpuGovernor NOTIFY stateChanged)
    Q_PROPERTY(QStringList availableGovernors READ availableGovernors NOTIFY stateChanged)
    Q_PROPERTY(QString gpuDpmLevel READ gpuDpmLevel NOTIFY stateChanged)

    Q_PROPERTY(bool thermalGuardActive READ thermalGuardActive NOTIFY stateChanged)
    Q_PROPERTY(QString thermalGuardReason READ thermalGuardReason NOTIFY stateChanged)
    Q_PROPERTY(double thermalCpuLimitC READ thermalCpuLimitC NOTIFY stateChanged)
    Q_PROPERTY(double thermalGpuLimitC READ thermalGpuLimitC NOTIFY stateChanged)

    Q_PROPERTY(bool gamingSessionActive READ gamingSessionActive NOTIFY stateChanged)
    Q_PROPERTY(QStringList detectedGamingProcesses READ detectedGamingProcesses NOTIFY stateChanged)
    Q_PROPERTY(QString recommendedProfile READ recommendedProfile NOTIFY stateChanged)
    Q_PROPERTY(QString gamingSessionAdvisory READ gamingSessionAdvisory NOTIFY stateChanged)
    Q_PROPERTY(bool silentAutoSwitchBlocked READ silentAutoSwitchBlocked NOTIFY stateChanged)

    Q_PROPERTY(QVariantMap readiness READ readiness NOTIFY stateChanged)
    Q_PROPERTY(bool signalConnected READ signalConnected CONSTANT)

public:
    explicit PerformanceClient(QObject *parent = nullptr);

    static bool isValidProfile(const QString &profile);
    static QVariantList defaultProfiles();

    bool available() const { return m_available; }
    bool busy() const { return m_busy; }
    QString statusMessage() const { return m_statusMessage; }
    QString lastError() const { return m_lastError; }

    QString activeProfile() const { return m_activeProfile; }
    QVariantList profiles() const { return m_profiles; }
    QString cpuGovernor() const { return m_cpuGovernor; }
    QStringList availableGovernors() const { return m_availableGovernors; }
    QString gpuDpmLevel() const { return m_gpuDpmLevel; }

    bool thermalGuardActive() const { return m_thermalGuardActive; }
    QString thermalGuardReason() const { return m_thermalGuardReason; }
    double thermalCpuLimitC() const { return m_thermalCpuLimitC; }
    double thermalGpuLimitC() const { return m_thermalGpuLimitC; }

    bool gamingSessionActive() const { return m_gamingSessionActive; }
    QStringList detectedGamingProcesses() const { return m_detectedGamingProcesses; }
    QString recommendedProfile() const { return m_recommendedProfile; }
    QString gamingSessionAdvisory() const { return m_gamingSessionAdvisory; }
    bool silentAutoSwitchBlocked() const { return m_silentAutoSwitchBlocked; }

    QVariantMap readiness() const { return m_readiness; }
    bool signalConnected() const { return m_signalConnected; }

    void setAutoRefreshEnabled(bool enabled);
    void applyProfileStateMap(const QVariantMap &data);
    void applyReadinessMap(const QVariantMap &data);

public slots:
    void refresh();
    void setProfile(const QString &profile, bool userConfirmed = false);

signals:
    void stateChanged();
    void operationFinished(const QString &operation, bool ok, const QString &message);

private slots:
    void requestProfileState();
    void requestGamingReadiness();
    void handleProfileStateReply();
    void handleGamingReadinessReply();
    void handleSetProfileReply();
    void handleProfileChangedSignal(const QVariantMap &state);

private:
    QTimer m_refreshTimer;
    bool m_loadingProfile = false;
    bool m_loadingReadiness = false;
    bool m_busy = false;
    bool m_available = false;
    bool m_signalConnected = false;

    QString m_statusMessage = QStringLiteral("Performans profili durumu okunuyor...");
    QString m_lastError;

    QString m_activeProfile = QStringLiteral("general");
    QVariantList m_profiles;
    QString m_cpuGovernor = QStringLiteral("Bilinmiyor");
    QStringList m_availableGovernors;
    QString m_gpuDpmLevel = QStringLiteral("Bilinmiyor");

    bool m_thermalGuardActive = false;
    QString m_thermalGuardReason = QStringLiteral("Termal sınırlar güvenli aralıkta.");
    double m_thermalCpuLimitC = 85.0;
    double m_thermalGpuLimitC = 82.0;

    bool m_gamingSessionActive = false;
    QStringList m_detectedGamingProcesses;
    QString m_recommendedProfile = QStringLiteral("general");
    QString m_gamingSessionAdvisory = QStringLiteral("Aktif oyun oturumu algılanmadı. Sessiz profil dayatması kapalıdır.");
    bool m_silentAutoSwitchBlocked = true;

    QVariantMap m_readiness;
};
