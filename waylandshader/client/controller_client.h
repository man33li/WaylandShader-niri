#pragma once

#include "output_settings.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <optional>

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
    QString transfer; // "", "same-gpu", "gpu-copy" or "cpu-copy"
    QString bypass; // nonempty: why this monitor is shown unfiltered
};

struct ControllerGpuDevice {
    QString node;
    QString path;
    QString name;
};

struct ControllerGpu {
    std::optional<ControllerGpuDevice> active;
    std::optional<QString> startup;
    bool fallback = false;
    std::optional<QString> configured;
    bool pending = false;
    QVector<ControllerGpuDevice> devices;
    QMap<QString, QString> outputs; // output name -> render node of the GPU driving it
    struct {
        QString state;
        QString detail;
        std::optional<QString> config;
        std::optional<QString> file;
        QString include;
    } preference;
};

struct ControllerStatus {
    bool enabled = false;
    bool active = false;
    bool loading = false;
    QString preset;
    QString requestedPreset;
    QStringList recentPresets;
    QString error;
    QVector<ControllerParameter> parameters;
    QVector<ControllerOutput> outputs;
    std::optional<ControllerGpu> gpu; // absent on backends without render GPU selection
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
    void setRenderDevice(const QString& device, Reply reply);

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
