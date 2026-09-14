#pragma once

#include "output_settings.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

namespace WaylandShader {

struct ControllerParameter {
    QString name;
    QString description;
    double minimum;
    double maximum;
    double step;
    double defaultValue;
    double value;
};

struct ControllerOutput {
    QString id;
    QString name;
    OutputSettings settings;
    bool shaderActive = false;
    bool colorActive = false;
};

struct ControllerStatus {
    bool enabled = false;
    bool active = false;
    bool loading = false;
    QString preset;
    QString requestedPreset;
    QString error;
    QVector<ControllerParameter> parameters;
    QVector<ControllerOutput> outputs;
    QJsonObject json;
};

struct ControllerError {
    enum Kind { None,
        Unavailable,
        Rejected,
        Protocol,
        Transport } kind
        = None;
    QString message;
    explicit operator bool() const { return kind != None; }
};

class ControllerClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const ControllerError&)>;
    explicit ControllerClient(QObject* parent = nullptr);
    const ControllerStatus& status() const { return m_status; }
    bool hasStatus() const { return m_hasStatus; }
    void ensureAvailable(Reply reply);
    void requestStatus(Reply reply);
    void loadPreset(const QString& absolutePath, Reply reply);
    void setEnabled(bool enabled, Reply reply);
    void setParameter(const QString& name, double value, Reply reply);
    void setOutputShaderEnabled(const QString& id, bool enabled, Reply reply);
    void setOutputColorEnabled(const QString& id, bool enabled, Reply reply);
    void setOutputGamma(const QString& id, double gamma, Reply reply);
    void setOutputSaturation(const QString& id, double saturation, Reply reply);

signals:
    void statusChanged(const WaylandShader::ControllerStatus& status);
    void transportError(const WaylandShader::ControllerError& error);

private slots:
    void receiveStatus(const QString& json);

private:
    using MessageReply = std::function<void(const QDBusMessage&)>;
    void call(const QString& path, const QString& interface, const QString& method,
        const QVariantList& arguments, MessageReply reply);
    void mutation(const QString& method, const QVariantList& arguments, Reply reply);
    ControllerError acceptStatus(const QDBusMessage& message);
    ControllerError acceptJson(const QString& json);
    QDBusConnection m_bus;
    ControllerStatus m_status;
    bool m_hasStatus = false;
    bool m_subscribed = false;
};

} // namespace WaylandShader
