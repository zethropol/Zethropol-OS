#pragma once

#include <QDBusArgument>
#include <QDBusVariant>
#include <QVariantMap>

namespace ZethropolUpdateWire {
// a{ss} is not a{sv}: reading a string as QVariant asks libdbus for a
// variant container and can crash. Keep the value's actual wire type.
inline QVariantMap decodeMap(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QVariantMap>())
        return value.toMap();
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return {};
    const auto argument = value.value<QDBusArgument>();
    const bool strings = argument.currentSignature() == QStringLiteral("a{ss}");
    if (!strings && argument.currentSignature() != QStringLiteral("a{sv}"))
        return {};
    QVariantMap result;
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        argument >> key;
        if (strings) {
            QString item;
            argument >> item;
            result.insert(key, item);
        } else {
            QDBusVariant item;
            argument >> item;
            result.insert(key, item.variant());
        }
        argument.endMapEntry();
    }
    argument.endMap();
    return result;
}

// The deployed service sends bbsu as four top-level arguments. Older peers
// and signals may wrap those fields in one (bbsu) structure.
inline bool decodeState(const QVariantList &arguments, QVariantMap *state, QString *error)
{
    QVariantList fields = arguments;
    if (arguments.size() == 1) {
        const QVariant &value = arguments.first();
        if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
            const auto argument = value.value<QDBusArgument>();
            if (argument.currentSignature() != QStringLiteral("(bbsu)"))
                fields.clear();
            else {
                bool checking = false, updating = false;
                QString lastCheck;
                uint count = 0;
                argument.beginStructure();
                argument >> checking >> updating >> lastCheck >> count;
                argument.endStructure();
                fields = {checking, updating, lastCheck, count};
            }
        } else if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
            const auto map = value.toMap();
            fields = {map.value(QStringLiteral("is_checking")),
                      map.value(QStringLiteral("is_updating")),
                      map.value(QStringLiteral("last_check_time")),
                      map.value(QStringLiteral("pending_updates_count"))};
        }
    }
    if (!state || fields.size() != 4
        || fields[0].metaType().id() != QMetaType::Bool
        || fields[1].metaType().id() != QMetaType::Bool
        || fields[2].metaType().id() != QMetaType::QString
        || fields[3].metaType().id() != QMetaType::UInt) {
        if (error)
            *error = QStringLiteral("Update durum yanıtı beklenen bbsu veya (bbsu) biçiminde değil.");
        return false;
    }
    *state = {{QStringLiteral("is_checking"), fields[0]},
              {QStringLiteral("is_updating"), fields[1]},
              {QStringLiteral("last_check_time"), fields[2]},
              {QStringLiteral("pending_updates_count"), fields[3]}};
    return true;
}
}
