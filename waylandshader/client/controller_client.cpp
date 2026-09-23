#include "controller_client.h"

#include <QDBusError>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <cmath>
#include <utility>

namespace WaylandShader {
namespace {
    const QString service = qEnvironmentVariable("WAYLANDSHADER_DBUS_SERVICE", QStringLiteral("org.waylandshader.Niri"));
    const QString effectPath = QStringLiteral("/WaylandShader");
    const QString effectInterface = QStringLiteral("org.waylandshader.Effect");

    ControllerError messageError(const QDBusMessage& message)
    {
        if (message.type() != QDBusMessage::ErrorMessage) {
            return { };
        }
        const QDBusError error(message);
        const bool unavailable = error.type() == QDBusError::ServiceUnknown
            || error.type() == QDBusError::UnknownObject || error.type() == QDBusError::UnknownMethod
            || error.type() == QDBusError::UnknownInterface;
        const auto kind = unavailable ? ControllerError::Unavailable
            : error.name().startsWith(QStringLiteral("org.waylandshader.Error."))
            ? ControllerError::Rejected
            : ControllerError::Transport;
        return { kind, error.name() + QStringLiteral(": ") + error.message() };
    }

    ControllerError booleanReply(const QDBusMessage& message)
    {
        if (const auto error = messageError(message)) {
            return error;
        }
        if (message.type() != QDBusMessage::ReplyMessage || message.arguments().size() != 1
            || message.arguments().front().metaType().id() != QMetaType::Bool) {
            return { ControllerError::Protocol, QStringLiteral("The compositor returned a malformed boolean reply.") };
        }
        if (!message.arguments().front().toBool()) {
            return { ControllerError::Rejected, QStringLiteral("The compositor rejected the request (returned false).") };
        }
        return { };
    }

    bool parseDevice(const QJsonValue& value, ControllerGpuDevice& device)
    {
        const auto object = value.toObject();
        for (const auto* field : { "node", "path", "name" }) {
            if (!object.value(QLatin1String(field)).isString())
                return false;
        }
        device = { object.value(QStringLiteral("node")).toString(), object.value(QStringLiteral("path")).toString(),
            object.value(QStringLiteral("name")).toString() };
        return value.isObject() && !device.path.isEmpty();
    }

    bool parseNullableString(const QJsonValue& value, std::optional<QString>& text)
    {
        if (value.isString())
            text = value.toString();
        return value.isString() || value.isNull();
    }

    // Returns a description of the first invalid field, or an empty string.
    QString parseGpu(const QJsonValue& value, ControllerGpu& gpu)
    {
        if (!value.isObject())
            return QStringLiteral("gpu must be an object.");
        const auto object = value.toObject();
        const auto active = object.value(QStringLiteral("active"));
        if (!active.isNull() && !parseDevice(active, gpu.active.emplace()))
            return QStringLiteral("gpu active must be null or a device with node/path/name strings.");
        if (!parseNullableString(object.value(QStringLiteral("startup")), gpu.startup)
            || !parseNullableString(object.value(QStringLiteral("configured")), gpu.configured))
            return QStringLiteral("gpu startup/configured must be strings or null.");
        if (!object.value(QStringLiteral("fallback")).isBool() || !object.value(QStringLiteral("pending")).isBool())
            return QStringLiteral("gpu fallback/pending must be boolean.");
        gpu.fallback = object.value(QStringLiteral("fallback")).toBool();
        gpu.pending = object.value(QStringLiteral("pending")).toBool();
        if (!object.value(QStringLiteral("devices")).isArray())
            return QStringLiteral("gpu devices must be an array.");
        QSet<QString> paths;
        for (const auto& entry : object.value(QStringLiteral("devices")).toArray()) {
            ControllerGpuDevice device;
            if (!parseDevice(entry, device) || paths.contains(device.path))
                return QStringLiteral("gpu device must have node/path/name strings and a unique nonempty path.");
            paths.insert(device.path);
            gpu.devices.push_back(std::move(device));
        }
        if (!object.value(QStringLiteral("outputs")).isObject())
            return QStringLiteral("gpu outputs must be an object.");
        const auto outputs = object.value(QStringLiteral("outputs")).toObject();
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (!it.value().isString())
                return QStringLiteral("gpu outputs values must be strings.");
            gpu.outputs.insert(it.key(), it.value().toString());
        }
        if (!object.value(QStringLiteral("preference")).isObject())
            return QStringLiteral("gpu preference must be an object.");
        const auto preference = object.value(QStringLiteral("preference")).toObject();
        for (const auto* field : { "state", "detail", "include" }) {
            if (!preference.value(QLatin1String(field)).isString())
                return QStringLiteral("gpu preference ") + QString::fromLatin1(field) + QStringLiteral(" must be a string.");
        }
        if (!parseNullableString(preference.value(QStringLiteral("config")), gpu.preference.config)
            || !parseNullableString(preference.value(QStringLiteral("file")), gpu.preference.file))
            return QStringLiteral("gpu preference config/file must be strings or null.");
        gpu.preference.state = preference.value(QStringLiteral("state")).toString();
        gpu.preference.detail = preference.value(QStringLiteral("detail")).toString();
        gpu.preference.include = preference.value(QStringLiteral("include")).toString();
        return { };
    }

    QString installationHint()
    {
        return QStringLiteral(" Run niri-waylandshader in a separate nested session or select its dedicated login session. "
                              "Stock niri will not be restarted or replaced.");
    }
} // namespace

ControllerClient::ControllerClient(QObject* parent)
    : QObject(parent)
    , m_bus(QDBusConnection::sessionBus())
{
    m_subscribed = m_bus.connect(service, effectPath, effectInterface, QStringLiteral("statusChanged"),
        this, SLOT(receiveStatus(QString)));
    auto* watcher = new QDBusServiceWatcher(service, m_bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
        [this](const QString&, const QString& oldOwner, const QString& newOwner) {
            if (!oldOwner.isEmpty() && oldOwner != newOwner) {
                m_hasStatus = false;
                emit transportError({ ControllerError::Unavailable,
                    QStringLiteral("The compositor D-Bus service disappeared or restarted. Reconnect to continue.") });
            }
        });
}

void ControllerClient::call(const QString& path, const QString& interface, const QString& method,
    const QVariantList& arguments, MessageReply reply)
{
    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [watcher, reply = std::move(reply)]() {
            const auto result = watcher->reply();
            watcher->deleteLater();
            reply(result);
        });
}

void ControllerClient::ensureAvailable(Reply reply)
{
    if (!m_bus.isConnected() || !m_subscribed) {
        reply({ ControllerError::Unavailable,
            QStringLiteral("Cannot connect to the session D-Bus or subscribe to WaylandShader status.") });
        return;
    }
    requestStatus([reply = std::move(reply)](const ControllerError& error) {
        if (error.kind == ControllerError::Unavailable) {
            reply({ error.kind, error.message + installationHint() });
        } else {
            reply(error);
        }
    });
}

ControllerError ControllerClient::acceptStatus(const QDBusMessage& message)
{
    if (const auto error = messageError(message)) {
        return error;
    }
    if (message.type() != QDBusMessage::ReplyMessage || message.arguments().size() != 1
        || message.arguments().front().metaType().id() != QMetaType::QString) {
        return { ControllerError::Protocol, QStringLiteral("The compositor returned a malformed status reply (expected JSON string).") };
    }
    return acceptJson(message.arguments().front().toString());
}

ControllerError ControllerClient::acceptJson(const QString& json)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    const auto invalid = [](const QString& field) -> ControllerError {
        return { ControllerError::Protocol, QStringLiteral("Invalid WaylandShader status: ") + field };
    };
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return invalid(QStringLiteral("expected a JSON object: ") + parseError.errorString());
    }
    const auto object = document.object();
    for (const auto* field : { "enabled", "active", "loading" }) {
        if (!object.value(QLatin1String(field)).isBool()) {
            return invalid(QString::fromLatin1(field) + QStringLiteral(" must be boolean."));
        }
    }
    for (const auto* field : { "preset", "requestedPreset", "error" }) {
        if (!object.value(QLatin1String(field)).isString()) {
            return invalid(QString::fromLatin1(field) + QStringLiteral(" must be a string."));
        }
    }
    if (!object.value(QStringLiteral("parameters")).isArray()
        || !object.value(QStringLiteral("outputs")).isArray()) {
        return invalid(QStringLiteral("parameters and outputs must be arrays."));
    }
    ControllerStatus next;
    next.enabled = object.value(QStringLiteral("enabled")).toBool();
    next.active = object.value(QStringLiteral("active")).toBool();
    next.loading = object.value(QStringLiteral("loading")).toBool();
    next.preset = object.value(QStringLiteral("preset")).toString();
    next.requestedPreset = object.value(QStringLiteral("requestedPreset")).toString();
    next.error = object.value(QStringLiteral("error")).toString();
    const auto recent = object.value(QStringLiteral("recentPresets"));
    if (!recent.isUndefined() && !recent.isArray())
        return invalid(QStringLiteral("recentPresets must be an array."));
    for (const auto& entry : recent.toArray()) {
        auto path = entry.toString();
        if (!entry.isString() || path.isEmpty())
            return invalid(QStringLiteral("recent preset must be a nonempty string."));
        next.recentPresets.push_back(std::move(path));
    }
    QSet<QString> names;
    for (const auto& entry : object.value(QStringLiteral("parameters")).toArray()) {
        if (!entry.isObject()) {
            return invalid(QStringLiteral("parameter must be an object."));
        }
        const auto parameter = entry.toObject();
        if (!parameter.value(QStringLiteral("name")).isString()
            || parameter.value(QStringLiteral("name")).toString().isEmpty()
            || !parameter.value(QStringLiteral("description")).isString()) {
            return invalid(QStringLiteral("parameter name/description must be strings; name must not be empty."));
        }
        for (const auto* field : { "minimum", "maximum", "step", "default", "value" }) {
            const auto number = parameter.value(QLatin1String(field));
            if (!number.isDouble() || !std::isfinite(number.toDouble())) {
                return invalid(QStringLiteral("parameter ") + QString::fromLatin1(field)
                    + QStringLiteral(" must be finite numeric data."));
            }
        }
        ControllerParameter value { parameter.value(QStringLiteral("name")).toString(),
            parameter.value(QStringLiteral("description")).toString(),
            parameter.value(QStringLiteral("minimum")).toDouble(),
            parameter.value(QStringLiteral("maximum")).toDouble(),
            parameter.value(QStringLiteral("step")).toDouble(),
            parameter.value(QStringLiteral("default")).toDouble(),
            parameter.value(QStringLiteral("value")).toDouble() };
        if (names.contains(value.name) || value.minimum > value.maximum || value.step < 0
            || value.value < value.minimum || value.value > value.maximum
            || value.defaultValue < value.minimum || value.defaultValue > value.maximum) {
            return invalid(QStringLiteral("duplicate parameter or invalid bounds: ") + value.name);
        }
        names.insert(value.name);
        next.parameters.push_back(std::move(value));
    }
    QSet<QString> outputIds;
    for (const auto& entry : object.value(QStringLiteral("outputs")).toArray()) {
        if (!entry.isObject()) {
            return invalid(QStringLiteral("output must be an object."));
        }
        const auto output = entry.toObject();
        if (!output.value(QStringLiteral("id")).isString()
            || output.value(QStringLiteral("id")).toString().isEmpty()
            || !output.value(QStringLiteral("name")).isString()) {
            return invalid(QStringLiteral("output id/name must be strings; id must not be empty."));
        }
        for (const auto* field : { "shaderEnabled", "colorEnabled", "shaderActive", "colorActive" }) {
            if (!output.value(QLatin1String(field)).isBool()) {
                return invalid(QStringLiteral("output ") + QString::fromLatin1(field)
                    + QStringLiteral(" must be boolean."));
            }
        }
        for (const auto* field : { "gamma", "saturation" }) {
            const auto number = output.value(QLatin1String(field));
            if (!number.isDouble() || !std::isfinite(number.toDouble())) {
                return invalid(QStringLiteral("output ") + QString::fromLatin1(field)
                    + QStringLiteral(" must be finite numeric data."));
            }
        }
        for (const auto* field : { "transfer", "bypass" }) {
            const auto text = output.value(QLatin1String(field));
            if (!text.isUndefined() && !text.isString()) {
                return invalid(QStringLiteral("output ") + QString::fromLatin1(field)
                    + QStringLiteral(" must be a string."));
            }
        }
        ControllerOutput value;
        value.id = output.value(QStringLiteral("id")).toString();
        value.name = output.value(QStringLiteral("name")).toString();
        value.settings.shaderEnabled = output.value(QStringLiteral("shaderEnabled")).toBool();
        value.settings.colorEnabled = output.value(QStringLiteral("colorEnabled")).toBool();
        value.settings.gamma = output.value(QStringLiteral("gamma")).toDouble();
        value.settings.saturation = output.value(QStringLiteral("saturation")).toDouble();
        value.shaderActive = output.value(QStringLiteral("shaderActive")).toBool();
        value.colorActive = output.value(QStringLiteral("colorActive")).toBool();
        value.transfer = output.value(QStringLiteral("transfer")).toString();
        value.bypass = output.value(QStringLiteral("bypass")).toString();
        if (outputIds.contains(value.id)
            || value.settings.gamma < OutputSettings::MinimumGamma
            || value.settings.gamma > OutputSettings::MaximumGamma
            || value.settings.saturation < OutputSettings::MinimumSaturation
            || value.settings.saturation > OutputSettings::MaximumSaturation) {
            return invalid(QStringLiteral("duplicate output id or invalid bounds: ") + value.id);
        }
        outputIds.insert(value.id);
        next.outputs.push_back(std::move(value));
    }
    if (const auto gpu = object.value(QStringLiteral("gpu")); !gpu.isUndefined()) {
        next.gpu.emplace();
        if (const auto error = parseGpu(gpu, *next.gpu); !error.isEmpty())
            return invalid(error);
    }
    next.json = object;
    m_status = std::move(next);
    m_hasStatus = true;
    emit statusChanged(m_status);
    return { };
}

void ControllerClient::receiveStatus(const QString& json)
{
    if (const auto error = acceptJson(json)) {
        m_hasStatus = false;
        emit transportError(error);
    }
}

void ControllerClient::requestStatus(Reply reply)
{
    call(effectPath, effectInterface, QStringLiteral("status"), { },
        [this, reply = std::move(reply)](const QDBusMessage& message) { reply(acceptStatus(message)); });
}

void ControllerClient::mutation(const QString& method, const QVariantList& arguments, Reply reply)
{
    call(effectPath, effectInterface, method, arguments,
        [reply = std::move(reply)](const QDBusMessage& message) { reply(booleanReply(message)); });
}

void ControllerClient::loadPreset(const QString& absolutePath, Reply reply)
{
    mutation(QStringLiteral("loadPreset"), { absolutePath }, std::move(reply));
}

void ControllerClient::setEnabled(bool enabled, Reply reply)
{
    mutation(QStringLiteral("setEnabled"), { enabled }, std::move(reply));
}

void ControllerClient::setParameter(const QString& name, double value, Reply reply)
{
    mutation(QStringLiteral("setParameter"), { name, value }, std::move(reply));
}

void ControllerClient::setOutputShaderEnabled(const QString& id, bool enabled, Reply reply)
{
    mutation(QStringLiteral("setOutputShaderEnabled"), { id, enabled }, std::move(reply));
}

void ControllerClient::setOutputColorEnabled(const QString& id, bool enabled, Reply reply)
{
    mutation(QStringLiteral("setOutputColorEnabled"), { id, enabled }, std::move(reply));
}

void ControllerClient::setOutputGamma(const QString& id, double gamma, Reply reply)
{
    mutation(QStringLiteral("setOutputGamma"), { id, gamma }, std::move(reply));
}

void ControllerClient::setOutputSaturation(const QString& id, double saturation, Reply reply)
{
    mutation(QStringLiteral("setOutputSaturation"), { id, saturation }, std::move(reply));
}

void ControllerClient::setRenderDevice(const QString& device, Reply reply)
{
    mutation(QStringLiteral("setRenderDevice"), { device }, std::move(reply));
}

} // namespace WaylandShader
