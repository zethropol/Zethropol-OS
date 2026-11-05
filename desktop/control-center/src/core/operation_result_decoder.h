#pragma once
#include "update_state_decoder.h"
#include <QRegularExpression>
#include <QStringList>

namespace ZethropolUpdateWire {
inline bool decodeOperationResult(const QVariantList &arguments, QVariantMap *result, QString *error)
{
    QVariantList fields = arguments;
    if (fields.size() == 1 && fields.first().metaType() == QMetaType::fromType<QDBusArgument>()) {
        const auto wire = fields.first().value<QDBusArgument>();
        if (wire.currentSignature() != QStringLiteral("(bsssa{sv})")) fields.clear();
        else {
            bool success;
            QString code, message, id;
            QVariantMap data;
            wire.beginStructure();
            wire >> success >> code >> message >> id >> data;
            wire.endStructure();
            fields = {success, code, message, id, data};
        }
    }
    bool valid = result && fields.size() == 5;
    if (valid) {
        valid = fields[0].metaType().id() == QMetaType::Bool;
        for (int i = 1; i <= 3; ++i) valid = valid && fields[i].metaType().id() == QMetaType::QString;
        const auto &data = fields[4];
        valid = valid && (data.metaType() == QMetaType::fromType<QVariantMap>()
            || (data.metaType() == QMetaType::fromType<QDBusArgument>()
                && data.value<QDBusArgument>().currentSignature() == QStringLiteral("a{sv}")));
    }
    if (valid) {
        const QString id = fields[3].toString();
        valid = !fields[1].toString().isEmpty()
            && (id.isEmpty() || QRegularExpression(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")).match(id).hasMatch())
            && (!fields[0].toBool() || (fields[1].toString() == QStringLiteral("started") && !id.isEmpty()));
    }
    if (!valid) {
        if (error) *error = QStringLiteral("invalid_trigger_reply: expected bsssa{sv} or (bsssa{sv}); start outcome is unknown. Do not retry automatically.");
        return false;
    }
    *result = {{QStringLiteral("success"), fields[0]}, {QStringLiteral("code"), fields[1]},
        {QStringLiteral("message"), fields[2]}, {QStringLiteral("operation_id"), fields[3]},
        {QStringLiteral("data"), decodeMap(fields[4])}};
    return true;
}
inline bool triggerRejectedBeforeStart(const QString &name, const QString &message)
{
    if (QStringList{QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"),
        QStringLiteral("org.freedesktop.DBus.Error.AuthFailed"), QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown"),
        QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner"), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
        QStringLiteral("org.freedesktop.DBus.Error.UnknownInterface"), QStringLiteral("org.freedesktop.DBus.Error.UnknownObject")}.contains(name)) return true;
    for (const QString &code : {QStringLiteral("Expired"), QStringLiteral("NoPreview"), QStringLiteral("AuthorizationDenied"),
         QStringLiteral("InvalidIdentity"), QStringLiteral("IdentityMismatch"), QStringLiteral("InvalidToken"),
         QStringLiteral("InvalidPlan"), QStringLiteral("PlanChanged"), QStringLiteral("OperationInProgress")})
        if (message == QStringLiteral("Update broker rejected the request: ") + code) return true;
    return false;
}
}
