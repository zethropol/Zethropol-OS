/****************************************************************************
** Meta object code from reading C++ file 'ChatClient.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.12.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/ChatClient.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ChatClient.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.12.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN9zethropol4chat10ChatClientE_t {};
} // unnamed namespace

template <> constexpr inline auto zethropol::chat::ChatClient::qt_create_metaobjectdata<qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "zethropol::chat::ChatClient",
        "statusChanged",
        "",
        "busyChanged",
        "errorChanged",
        "settingsChanged",
        "sessionsChanged",
        "messagesChanged",
        "onDbusMessageReceived",
        "sessionId",
        "messageId",
        "role",
        "contentPreview",
        "executable",
        "policyAuthority",
        "onDbusSessionCleared",
        "reason",
        "refreshStatus",
        "clearExistingError",
        "refreshSessions",
        "loadActiveHistory",
        "createSession",
        "title",
        "selectSession",
        "sendMessage",
        "userMessage",
        "maxTokens",
        "clearActiveSession",
        "clearAllHistory",
        "clearError",
        "connected",
        "busy",
        "isSending",
        "sendingStatusText",
        "runtimeState",
        "vramLocked",
        "activeVramMb",
        "hardwareProfile",
        "activeModelId",
        "loopbackOnly",
        "externalTelemetryDisabled",
        "commandExecutionDisabled",
        "storageEncrypted",
        "storagePermissionsStrict",
        "maxContextTokens",
        "maxOutputTokens",
        "blockedCommandAttempts",
        "blockedInjectionAttempts",
        "statusBanner",
        "errorMessage",
        "includeSystemContext",
        "activeSessionId",
        "sessions",
        "QVariantList",
        "messages"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'statusChanged'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'busyChanged'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'errorChanged'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'settingsChanged'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'sessionsChanged'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'messagesChanged'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onDbusMessageReceived'
        QtMocHelpers::SlotData<void(const QString &, const QString &, const QString &, const QString &, bool, const QString &)>(8, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 9 }, { QMetaType::QString, 10 }, { QMetaType::QString, 11 }, { QMetaType::QString, 12 },
            { QMetaType::Bool, 13 }, { QMetaType::QString, 14 },
        }}),
        // Slot 'onDbusSessionCleared'
        QtMocHelpers::SlotData<void(const QString &, const QString &)>(15, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 9 }, { QMetaType::QString, 16 },
        }}),
        // Method 'refreshStatus'
        QtMocHelpers::MethodData<bool(bool)>(17, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::Bool, 18 },
        }}),
        // Method 'refreshStatus'
        QtMocHelpers::MethodData<bool()>(17, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Bool),
        // Method 'refreshSessions'
        QtMocHelpers::MethodData<bool(bool)>(19, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::Bool, 20 },
        }}),
        // Method 'refreshSessions'
        QtMocHelpers::MethodData<bool()>(19, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Bool),
        // Method 'createSession'
        QtMocHelpers::MethodData<QString(const QString &)>(21, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 22 },
        }}),
        // Method 'createSession'
        QtMocHelpers::MethodData<QString()>(21, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::QString),
        // Method 'selectSession'
        QtMocHelpers::MethodData<bool(const QString &)>(23, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 9 },
        }}),
        // Method 'sendMessage'
        QtMocHelpers::MethodData<bool(const QString &, int)>(24, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 25 }, { QMetaType::Int, 26 },
        }}),
        // Method 'sendMessage'
        QtMocHelpers::MethodData<bool(const QString &)>(24, 2, QMC::AccessPublic | QMC::MethodCloned, QMetaType::Bool, {{
            { QMetaType::QString, 25 },
        }}),
        // Method 'clearActiveSession'
        QtMocHelpers::MethodData<bool()>(27, 2, QMC::AccessPublic, QMetaType::Bool),
        // Method 'clearAllHistory'
        QtMocHelpers::MethodData<bool()>(28, 2, QMC::AccessPublic, QMetaType::Bool),
        // Method 'clearError'
        QtMocHelpers::MethodData<void()>(29, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'connected'
        QtMocHelpers::PropertyData<bool>(30, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'busy'
        QtMocHelpers::PropertyData<bool>(31, QMetaType::Bool, QMC::DefaultPropertyFlags, 1),
        // property 'isSending'
        QtMocHelpers::PropertyData<bool>(32, QMetaType::Bool, QMC::DefaultPropertyFlags, 1),
        // property 'sendingStatusText'
        QtMocHelpers::PropertyData<QString>(33, QMetaType::QString, QMC::DefaultPropertyFlags, 1),
        // property 'runtimeState'
        QtMocHelpers::PropertyData<QString>(34, QMetaType::QString, QMC::DefaultPropertyFlags, 0),
        // property 'vramLocked'
        QtMocHelpers::PropertyData<bool>(35, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'activeVramMb'
        QtMocHelpers::PropertyData<qulonglong>(36, QMetaType::ULongLong, QMC::DefaultPropertyFlags, 0),
        // property 'hardwareProfile'
        QtMocHelpers::PropertyData<QString>(37, QMetaType::QString, QMC::DefaultPropertyFlags, 0),
        // property 'activeModelId'
        QtMocHelpers::PropertyData<QString>(38, QMetaType::QString, QMC::DefaultPropertyFlags, 0),
        // property 'loopbackOnly'
        QtMocHelpers::PropertyData<bool>(39, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'externalTelemetryDisabled'
        QtMocHelpers::PropertyData<bool>(40, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'commandExecutionDisabled'
        QtMocHelpers::PropertyData<bool>(41, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'storageEncrypted'
        QtMocHelpers::PropertyData<bool>(42, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'storagePermissionsStrict'
        QtMocHelpers::PropertyData<bool>(43, QMetaType::Bool, QMC::DefaultPropertyFlags, 0),
        // property 'maxContextTokens'
        QtMocHelpers::PropertyData<int>(44, QMetaType::Int, QMC::DefaultPropertyFlags, 0),
        // property 'maxOutputTokens'
        QtMocHelpers::PropertyData<int>(45, QMetaType::Int, QMC::DefaultPropertyFlags, 0),
        // property 'blockedCommandAttempts'
        QtMocHelpers::PropertyData<qulonglong>(46, QMetaType::ULongLong, QMC::DefaultPropertyFlags, 0),
        // property 'blockedInjectionAttempts'
        QtMocHelpers::PropertyData<qulonglong>(47, QMetaType::ULongLong, QMC::DefaultPropertyFlags, 0),
        // property 'statusBanner'
        QtMocHelpers::PropertyData<QString>(48, QMetaType::QString, QMC::DefaultPropertyFlags, 0),
        // property 'errorMessage'
        QtMocHelpers::PropertyData<QString>(49, QMetaType::QString, QMC::DefaultPropertyFlags, 2),
        // property 'includeSystemContext'
        QtMocHelpers::PropertyData<bool>(50, QMetaType::Bool, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet, 3),
        // property 'activeSessionId'
        QtMocHelpers::PropertyData<QString>(51, QMetaType::QString, QMC::DefaultPropertyFlags, 4),
        // property 'sessions'
        QtMocHelpers::PropertyData<QVariantList>(52, 0x80000000 | 53, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 4),
        // property 'messages'
        QtMocHelpers::PropertyData<QVariantList>(54, 0x80000000 | 53, QMC::DefaultPropertyFlags | QMC::EnumOrFlag, 5),
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ChatClient, qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject zethropol::chat::ChatClient::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>.metaTypes,
    nullptr
} };

void zethropol::chat::ChatClient::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ChatClient *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->statusChanged(); break;
        case 1: _t->busyChanged(); break;
        case 2: _t->errorChanged(); break;
        case 3: _t->settingsChanged(); break;
        case 4: _t->sessionsChanged(); break;
        case 5: _t->messagesChanged(); break;
        case 6: _t->onDbusMessageReceived((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4])),(*reinterpret_cast<std::add_pointer_t<bool>>(_a[5])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[6]))); break;
        case 7: _t->onDbusSessionCleared((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 8: { bool _r = _t->refreshStatus((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 9: { bool _r = _t->refreshStatus();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 10: { bool _r = _t->refreshSessions((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 11: { bool _r = _t->refreshSessions();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 12: { QString _r = _t->createSession((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 13: { QString _r = _t->createSession();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 14: { bool _r = _t->selectSession((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 15: { bool _r = _t->sendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 16: { bool _r = _t->sendMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 17: { bool _r = _t->clearActiveSession();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 18: { bool _r = _t->clearAllHistory();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 19: _t->clearError(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::statusChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::busyChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::errorChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::settingsChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::sessionsChanged, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (ChatClient::*)()>(_a, &ChatClient::messagesChanged, 5))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<bool*>(_v) = _t->connected(); break;
        case 1: *reinterpret_cast<bool*>(_v) = _t->busy(); break;
        case 2: *reinterpret_cast<bool*>(_v) = _t->isSending(); break;
        case 3: *reinterpret_cast<QString*>(_v) = _t->sendingStatusText(); break;
        case 4: *reinterpret_cast<QString*>(_v) = _t->runtimeState(); break;
        case 5: *reinterpret_cast<bool*>(_v) = _t->vramLocked(); break;
        case 6: *reinterpret_cast<qulonglong*>(_v) = _t->activeVramMb(); break;
        case 7: *reinterpret_cast<QString*>(_v) = _t->hardwareProfile(); break;
        case 8: *reinterpret_cast<QString*>(_v) = _t->activeModelId(); break;
        case 9: *reinterpret_cast<bool*>(_v) = _t->loopbackOnly(); break;
        case 10: *reinterpret_cast<bool*>(_v) = _t->externalTelemetryDisabled(); break;
        case 11: *reinterpret_cast<bool*>(_v) = _t->commandExecutionDisabled(); break;
        case 12: *reinterpret_cast<bool*>(_v) = _t->storageEncrypted(); break;
        case 13: *reinterpret_cast<bool*>(_v) = _t->storagePermissionsStrict(); break;
        case 14: *reinterpret_cast<int*>(_v) = _t->maxContextTokens(); break;
        case 15: *reinterpret_cast<int*>(_v) = _t->maxOutputTokens(); break;
        case 16: *reinterpret_cast<qulonglong*>(_v) = _t->blockedCommandAttempts(); break;
        case 17: *reinterpret_cast<qulonglong*>(_v) = _t->blockedInjectionAttempts(); break;
        case 18: *reinterpret_cast<QString*>(_v) = _t->statusBanner(); break;
        case 19: *reinterpret_cast<QString*>(_v) = _t->errorMessage(); break;
        case 20: *reinterpret_cast<bool*>(_v) = _t->includeSystemContext(); break;
        case 21: *reinterpret_cast<QString*>(_v) = _t->activeSessionId(); break;
        case 22: *reinterpret_cast<QVariantList*>(_v) = _t->sessions(); break;
        case 23: *reinterpret_cast<QVariantList*>(_v) = _t->messages(); break;
        default: break;
        }
    }
    if (_c == QMetaObject::WriteProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 20: _t->setIncludeSystemContext(*reinterpret_cast<bool*>(_v)); break;
        default: break;
        }
    }
}

const QMetaObject *zethropol::chat::ChatClient::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *zethropol::chat::ChatClient::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9zethropol4chat10ChatClientE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int zethropol::chat::ChatClient::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 20)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 20;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 20)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 20;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 24;
    }
    return _id;
}

// SIGNAL 0
void zethropol::chat::ChatClient::statusChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void zethropol::chat::ChatClient::busyChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void zethropol::chat::ChatClient::errorChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void zethropol::chat::ChatClient::settingsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void zethropol::chat::ChatClient::sessionsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void zethropol::chat::ChatClient::messagesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}
QT_WARNING_POP
