#pragma once
#include <QDBusArgument>
#include <QVariantList>
#include <QVariantMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace ZethropolUpdateWire {
enum class PackageReplyPurpose { PendingList, TransactionPlan };

inline bool decodePackages(const QVariant &value, QVariantList *result, QString *error,
                           PackageReplyPurpose purpose)
{
    if (!result || value.metaType() != QMetaType::fromType<QDBusArgument>()) {
        if (error)
            *error = QStringLiteral("Update D-Bus yanıtı beklenen dizi türünde değil.");
        return false;
    }

    const QDBusArgument argument = value.value<QDBusArgument>();
    if (argument.currentSignature() != QStringLiteral("a(sssstsss)")) {
        if (error)
            *error = QStringLiteral("Update D-Bus yanıt imzası beklenen a(sssstsss) biçiminde değil.");
        return false;
    }

    QVariantList packages;
    argument.beginArray();
    while (!argument.atEnd()) {
        QString name;
        QString currentVersion;
        QString newVersion;
        QString repository;
        qulonglong downloadSize = 0;
        QString filename, sha256, metadataHash;

        argument.beginStructure();
        argument >> name >> currentVersion >> newVersion >> repository >> downloadSize >> filename >> sha256 >> metadataHash;
        argument.endStructure();

        QStringList invalidFields;
        if (name.trimmed().isEmpty()) invalidFields << QStringLiteral("package_name");
        if (currentVersion.trimmed().isEmpty()) invalidFields << QStringLiteral("current_version");
        if (newVersion.trimmed().isEmpty()) invalidFields << QStringLiteral("new_version");
        if (repository.trimmed().isEmpty()) invalidFields << QStringLiteral("repository");
        if (filename.isEmpty() || filename == QStringLiteral(".") || filename == QStringLiteral("..")
            || filename.contains(QLatin1Char('/')) || filename.contains(QLatin1Char('\\')))
            invalidFields << QStringLiteral("filename");
        const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
        if (!digest.match(sha256).hasMatch()) invalidFields << QStringLiteral("sha256");
        // Informational listing intentionally carries no repository snapshot
        // digest. Only PreviewPlan binds metadata to an authenticated plan.
        // Preserve an empty digest; never manufacture an installation identity.
        if (!(purpose == PackageReplyPurpose::PendingList && metadataHash.isEmpty())
            && !digest.match(metadataHash).hasMatch())
            invalidFields << QStringLiteral("repository_metadata_sha256");
        if (!invalidFields.isEmpty()) {
            if (error)
                *error = QStringLiteral("Update D-Bus paket kaydı %1 (%2): zorunlu alan eksik veya geçersiz: %3. Liste kabul edilmedi.")
                    .arg(packages.size() + 1).arg(name.isEmpty() ? QStringLiteral("<adsız>") : name,
                                                invalidFields.join(QStringLiteral(", ")));
            argument.endArray();
            return false;
        }

        QVariantMap item;
        item.insert(QStringLiteral("package_name"), name);
        item.insert(QStringLiteral("current_version"), currentVersion);
        item.insert(QStringLiteral("new_version"), newVersion);
        item.insert(QStringLiteral("repository"), repository);
        item.insert(QStringLiteral("download_size"), downloadSize);
        item.insert(QStringLiteral("filename"), filename);
        item.insert(QStringLiteral("sha256"), sha256);
        item.insert(QStringLiteral("repository_metadata_sha256"), metadataHash);
        packages.append(item);
    }
    argument.endArray();

    *result = packages;
    return true;
}

inline bool decodePlanPackages(const QVariant &value, QVariantList *result, QString *error)
{
    return decodePackages(value, result, error, PackageReplyPurpose::TransactionPlan);
}

inline bool decodePendingPackages(const QVariant &value, QVariantList *result, QString *error)
{
    return decodePackages(value, result, error, PackageReplyPurpose::PendingList);
}

inline bool decodePlan(const QVariantList &arguments, QVariantList *packages,
                       QVariantList *removals, QString *hash, QString *error)
{
    auto reject = [error] { if (error) *error = QStringLiteral("Invalid or unsupported replacement-aware transaction plan."); return false; };
    if (!packages || !removals || !hash || arguments.size() != 3
        || arguments.at(1).metaType() != QMetaType::fromType<QDBusArgument>()
        || arguments.at(2).metaType() != QMetaType::fromType<QString>()
        || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(arguments.at(2).toString()).hasMatch())
        return reject();
    QVariantList additions, effects;
    if (!decodePlanPackages(arguments.at(0), &additions, error))
        return false;
    QSet<QString> targets, removed;
    for (const QVariant &value : additions) {
        const QString name = value.toMap().value(QStringLiteral("package_name")).toString();
        if (targets.contains(name)) return reject();
        targets.insert(name);
    }
    const QDBusArgument wire = arguments.at(1).value<QDBusArgument>();
    if (wire.currentSignature() != QStringLiteral("a(ssss)")) return reject();
    wire.beginArray();
    while (!wire.atEnd()) {
        QString name, version, reason, replacement;
        wire.beginStructure();
        wire >> name >> version >> reason >> replacement;
        wire.endStructure();
        if (name.trimmed().isEmpty() || version.trimmed().isEmpty()
            || reason != QStringLiteral("replacement") || !targets.contains(replacement)
            || targets.contains(name) || removed.contains(name)) {
            wire.endArray();
            return reject();
        }
        removed.insert(name);
        effects.append(QVariantMap{{QStringLiteral("package_name"), name},
            {QStringLiteral("current_version"), version}, {QStringLiteral("reason"), reason},
            {QStringLiteral("replacement"), replacement}});
    }
    wire.endArray();
    *packages = additions;
    *removals = effects;
    *hash = arguments.at(2).toString();
    return true;
}
}
