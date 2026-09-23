#include "controller_client.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <charconv>
#include <cmath>

using namespace WaylandShader;

namespace {
const QString compositorName = QStringLiteral("niri");

// Keep the exact server bounds even for very small shader parameters, without
// filling the editor with hundreds of trailing decimal places.
class ParameterSpinBox : public QDoubleSpinBox {
public:
    explicit ParameterSpinBox(QWidget* parent)
        : QDoubleSpinBox(parent)
    {
        setDecimals(323);
        setLocale(QLocale::c());
    }

protected:
    QString textFromValue(double value) const override
    {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(value));
        return QString::fromLatin1(buffer, result.ptr - buffer);
    }
    double valueFromText(const QString& text) const override
    {
        return static_cast<float>(locale().toDouble(text));
    }
    QValidator::State validate(QString& text, int&) const override
    {
        bool ok = false;
        const float value = static_cast<float>(locale().toDouble(text, &ok));
        if (ok && std::isfinite(value) && value >= minimum() && value <= maximum()) {
            return QValidator::Acceptable;
        }
        // Intermediate permits negative numbers, exponents and replacing the
        // whole value. QAbstractSpinBox corrects incomplete/out-of-range edits.
        return QValidator::Intermediate;
    }
};

class SettingsWindow : public QWidget {
public:
    SettingsWindow()
    {
        setWindowTitle(QStringLiteral("WaylandShader — %1 Desktop Effects").arg(compositorName));
        resize(740, 620);
        auto* layout = new QVBoxLayout(this);
        auto* intro = new QLabel(QStringLiteral("Native desktop shader and color effects for %1").arg(compositorName), this);
        layout->addWidget(intro);
        auto* presetRow = new QHBoxLayout;
        m_preset = new QLineEdit(this);
        m_preset->setPlaceholderText(QStringLiteral("Select a RetroArch .slangp preset"));
        m_preset->setAccessibleName(QStringLiteral("Shader preset path"));
        auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
        m_recent = new QPushButton(QStringLiteral("Recent"), this);
        m_recent->setAccessibleName(QStringLiteral("Recently used shader presets"));
        m_recent->setToolTip(QStringLiteral("Choose a recently used preset, then click Load preset."));
        m_recent->setMenu(new QMenu(m_recent));
        m_recent->setEnabled(false);
        m_load = new QPushButton(QStringLiteral("Load preset"), this);
        presetRow->addWidget(m_preset, 1);
        presetRow->addWidget(browse);
        presetRow->addWidget(m_recent);
        presetRow->addWidget(m_load);
        layout->addLayout(presetRow);
        auto* controlRow = new QHBoxLayout;
        m_enabled = new QCheckBox(QStringLiteral("Enable desktop effects"), this);
        auto* reconnect = new QPushButton(QStringLiteral("Reconnect / refresh"), this);
        controlRow->addWidget(m_enabled);
        controlRow->addStretch();
        controlRow->addWidget(reconnect);
        layout->addLayout(controlRow);
        auto* monitorRow = new QHBoxLayout;
        auto* monitorLabel = new QLabel(QStringLiteral("Monitor:"), this);
        m_output = new QComboBox(this);
        m_output->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_output->setMinimumContentsLength(18);
        m_output->setPlaceholderText(QStringLiteral("No connected monitors"));
        monitorLabel->setBuddy(m_output);
        m_outputState = new QLabel(this);
        m_outputState->setTextFormat(Qt::PlainText);
        monitorRow->addWidget(monitorLabel);
        monitorRow->addWidget(m_output, 1);
        monitorRow->addWidget(m_outputState);
        layout->addLayout(monitorRow);
        auto* outputToggles = new QHBoxLayout;
        m_outputShader = new QCheckBox(QStringLiteral("Apply preset on this monitor"), this);
        m_outputColor = new QCheckBox(QStringLiteral("Enable color adjustments"), this);
        outputToggles->addWidget(m_outputShader);
        outputToggles->addWidget(m_outputColor);
        outputToggles->addStretch();
        layout->addLayout(outputToggles);
        auto* colorRow = new QHBoxLayout;
        auto* gammaLabel = new QLabel(QStringLiteral("Gamma:"), this);
        m_gamma = new QDoubleSpinBox(this);
        m_gamma->setRange(OutputSettings::MinimumGamma, OutputSettings::MaximumGamma);
        m_gamma->setDecimals(2);
        m_gamma->setSingleStep(0.05);
        m_gamma->setValue(1.0);
        m_gamma->setKeyboardTracking(false);
        m_gamma->setToolTip(QStringLiteral("1.00 is neutral; higher values brighten the image."));
        gammaLabel->setBuddy(m_gamma);
        auto* saturationLabel = new QLabel(QStringLiteral("Saturation:"), this);
        m_saturation = new QDoubleSpinBox(this);
        m_saturation->setRange(OutputSettings::MinimumSaturation, OutputSettings::MaximumSaturation);
        m_saturation->setDecimals(2);
        m_saturation->setSingleStep(0.05);
        m_saturation->setValue(1.0);
        m_saturation->setKeyboardTracking(false);
        m_saturation->setToolTip(QStringLiteral("0.00 is grayscale; 1.00 is neutral; 2.00 doubles saturation."));
        saturationLabel->setBuddy(m_saturation);
        colorRow->addWidget(gammaLabel);
        colorRow->addWidget(m_gamma, 1);
        colorRow->addSpacing(16);
        colorRow->addWidget(saturationLabel);
        colorRow->addWidget(m_saturation, 1);
        layout->addLayout(colorRow);
        m_gpuBox = new QGroupBox(QStringLiteral("Render GPU"), this);
        auto* gpuLayout = new QVBoxLayout(m_gpuBox);
        m_gpuActive = new QLabel(m_gpuBox);
        m_gpuActive->setWordWrap(true);
        m_gpuActive->setTextFormat(Qt::PlainText);
        m_gpuActive->setTextInteractionFlags(Qt::TextSelectableByMouse);
        gpuLayout->addWidget(m_gpuActive);
        auto* gpuRow = new QHBoxLayout;
        m_gpuDevice = new QComboBox(m_gpuBox);
        m_gpuDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_gpuDevice->setMinimumContentsLength(18);
        m_gpuDevice->setAccessibleName(QStringLiteral("Render GPU for the next niri login"));
        m_gpuSave = new QPushButton(QStringLiteral("Save for next login"), m_gpuBox);
        gpuRow->addWidget(m_gpuDevice, 1);
        gpuRow->addWidget(m_gpuSave);
        gpuLayout->addLayout(gpuRow);
        m_gpuMessage = new QLabel(m_gpuBox);
        m_gpuMessage->setWordWrap(true);
        m_gpuMessage->setTextFormat(Qt::PlainText);
        m_gpuMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
        gpuLayout->addWidget(m_gpuMessage);
        m_gpuBox->setVisible(false);
        layout->addWidget(m_gpuBox);
        m_summary = new QLabel(QStringLiteral("Connecting to %1…").arg(compositorName), this);
        m_summary->setWordWrap(true);
        m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_summary->setTextFormat(Qt::PlainText);
        layout->addWidget(m_summary);
        m_error = new QLabel(this);
        m_error->setWordWrap(true);
        m_error->setTextFormat(Qt::PlainText);
        m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(m_error);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        m_parameters = new QWidget(scroll);
        m_form = new QFormLayout(m_parameters);
        m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        scroll->setWidget(m_parameters);
        layout->addWidget(scroll, 1);
        auto* note = new QLabel(QStringLiteral("Shader geometry changes pixels, not mouse hit regions. "
                                               "Curvature and distortion can make pointing inaccurate."),
            this);
        note->setWordWrap(true);
        layout->addWidget(note);
        setAvailable(false);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose shader preset"),
                m_preset->text(), QStringLiteral("RetroArch slang presets (*.slangp)"));
            if (!path.isEmpty()) {
                m_preset->setText(path);
                m_pathEdited = true;
            }
        });
        connect(m_preset, &QLineEdit::textEdited, this, [this] { m_pathEdited = true; });
        connect(m_load, &QPushButton::clicked, this, [this] { load(); });
        connect(m_preset, &QLineEdit::returnPressed, this, [this] { if (m_load->isEnabled()) load(); });
        connect(reconnect, &QPushButton::clicked, this, [this] { reconnectToEffect(); });
        connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
            m_enablePending = true;
            m_enabled->setEnabled(false);
            m_operationError.clear();
            m_client.setEnabled(enabled, [this](const ControllerError& error) {
                m_enablePending = false;
                finishMutation(error);
            });
        });
        connect(m_output, &QComboBox::currentIndexChanged, this, [this](int index) {
            selectOutput(m_output->itemData(index).toString());
            updateOutputControls();
        });
        connect(m_outputShader, &QCheckBox::toggled, this, [this](bool enabled) {
            changeOutput(&ControllerClient::setOutputShaderEnabled, enabled);
        });
        connect(m_outputColor, &QCheckBox::toggled, this, [this](bool enabled) {
            changeOutput(&ControllerClient::setOutputColorEnabled, enabled);
        });
        connect(m_gamma, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            changeOutput(&ControllerClient::setOutputGamma, value);
        });
        connect(m_saturation, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            changeOutput(&ControllerClient::setOutputSaturation, value);
        });
        connect(m_gpuDevice, &QComboBox::currentIndexChanged, this, [this] {
            m_gpuFeedback.clear();
            updateGpu();
        });
        connect(m_gpuSave, &QPushButton::clicked, this, [this] {
            m_gpuPending = true;
            m_gpuFeedback.clear();
            m_operationError.clear();
            updateGpu();
            showErrors();
            m_client.setRenderDevice(m_gpuDevice->currentData().toString(), [this](const ControllerError& error) {
                m_gpuPending = false;
                if (!error)
                    m_gpuFeedback = QStringLiteral("Saved. niri will use it from the next login.");
                updateGpu();
                finishMutation(error);
            });
        });
        for (auto* spin : { m_gamma, m_saturation }) {
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this] {
                // Refresh only after focus moves; this callback never submits an edit.
                QTimer::singleShot(0, this, [this] { updateOutputControls(); });
            });
        }
        connect(&m_client, &ControllerClient::statusChanged, this,
            [this](const ControllerStatus& status) { updateStatus(status); });
        connect(&m_client, &ControllerClient::transportError, this, [this](const ControllerError& error) {
            setAvailable(false);
            m_operationError = error.message;
            showErrors();
            m_summary->setText(QStringLiteral("Disconnected — displayed controls may be stale. Use Reconnect / refresh."));
        });
        QTimer::singleShot(0, this, [this] { reconnectToEffect(); });
    }

private:
    struct Editor {
        ControllerParameter metadata;
        ParameterSpinBox* spin;
        QTimer* timer;
        bool dirty = false;
        quint64 revision = 0;
    };

    void setAvailable(bool available)
    {
        m_available = available;
        m_load->setEnabled(available && !m_loading && !m_loadPending);
        m_enabled->setEnabled(available && !m_enablePending);
        m_parameters->setEnabled(available && !m_loading);
        updateOutputAvailability();
        updateGpu(); // setAvailable(true) runs on every status update.
    }

    static QString deviceLabel(const ControllerGpuDevice& device)
    {
        return device.name.isEmpty() ? device.node : device.name;
    }

    QString outputRoute(const ControllerOutput& output) const
    {
        const auto& gpu = m_client.status().gpu;
        if (!gpu && output.transfer.isEmpty())
            return { }; // Older compositors do not report the render path.
        if (output.transfer.isEmpty())
            return QStringLiteral("Not rendered yet");
        QString target = gpu ? gpu->outputs.value(output.name) : QString();
        if (gpu) {
            for (const auto& device : gpu->devices) {
                if (!target.isEmpty() && device.node == target) {
                    target = deviceLabel(device);
                    break;
                }
            }
        }
        if (target.isEmpty())
            target = QStringLiteral("another GPU");
        const QString route = output.transfer == QStringLiteral("same-gpu") ? QStringLiteral("same GPU")
            : output.transfer == QStringLiteral("gpu-copy") ? QStringLiteral("copied to %1 by the GPU").arg(target)
            : output.transfer == QStringLiteral("cpu-copy") ? QStringLiteral("CPU copy to %1 — effects bypassed").arg(target)
                                                            : output.transfer;
        return gpu && gpu->active ? QStringLiteral("Rendered on %1; %2").arg(deviceLabel(*gpu->active), route) : route;
    }

    void updateGpu()
    {
        const auto& gpu = m_client.status().gpu;
        m_gpuBox->setVisible(gpu.has_value());
        if (!gpu)
            return;
        m_gpuActive->setText(QStringLiteral("In use: ")
            + (gpu->active ? QStringLiteral("%1 (%2)").arg(gpu->active->name, gpu->active->node) : QStringLiteral("unknown"))
            + (gpu->fallback ? QStringLiteral(" — the configured GPU was unavailable at login; automatic selection is in use")
                             : QString()));
        m_gpuActive->setToolTip(QStringLiteral("Requested at login: ") + gpu->startup.value_or(QStringLiteral("Automatic")));
        // configured may name a device by path or node, or something not selectable at all.
        QString configured = gpu->configured.value_or(QString());
        bool known = !gpu->configured;
        for (const auto& device : gpu->devices) {
            if (!known && (device.path == configured || device.node == configured)) {
                configured = device.path;
                known = true;
            }
        }
        QVector<std::pair<QString, QString>> items { { QStringLiteral("Automatic"), QString() } };
        for (const auto& device : gpu->devices)
            items.push_back({ deviceLabel(device) + QStringLiteral(" — ") + device.path, device.path });
        if (!known)
            items.push_back({ QStringLiteral("Configured: ") + configured, configured });
        bool changed = m_gpuDevice->count() != items.size();
        for (int index = 0; !changed && index < m_gpuDevice->count(); ++index) {
            changed = m_gpuDevice->itemText(index) != items[index].first
                || m_gpuDevice->itemData(index).toString() != items[index].second;
        }
        // Keep an unsaved choice; otherwise follow the configured value.
        const QString selected = m_gpuDevice->currentData().toString();
        const bool edited = m_gpuDevice->currentIndex() >= 0 && selected != m_gpuConfigured;
        const QSignalBlocker blocked(m_gpuDevice);
        if (changed) {
            m_gpuDevice->clear();
            for (const auto& [text, data] : items)
                m_gpuDevice->addItem(text, data);
            if (!known)
                static_cast<QStandardItemModel*>(m_gpuDevice->model())->item(m_gpuDevice->count() - 1)->setEnabled(false);
        }
        const int index = edited ? m_gpuDevice->findData(selected) : -1;
        m_gpuDevice->setCurrentIndex(index >= 0 ? index : m_gpuDevice->findData(configured));
        m_gpuConfigured = configured;
        const bool ready = gpu->preference.state == QStringLiteral("ready");
        m_gpuDevice->setEnabled(m_available && !m_gpuPending);
        m_gpuSave->setEnabled(m_available && !m_gpuPending && ready
            && m_gpuDevice->currentIndex() >= 0 && m_gpuDevice->currentData().toString() != configured);
        QStringList messages;
        if (!m_gpuFeedback.isEmpty())
            messages.push_back(m_gpuFeedback);
        else if (gpu->pending)
            messages.push_back(QStringLiteral("Takes effect at the next niri login."));
        if (!ready) {
            messages.push_back(gpu->preference.detail);
            if (gpu->preference.state == QStringLiteral("not-adopted"))
                messages.push_back(gpu->preference.include);
        }
        m_gpuMessage->setText(messages.join(QLatin1Char('\n')));
        m_gpuMessage->setVisible(!messages.isEmpty());
    }

    const ControllerOutput* selectedOutput() const
    {
        for (const auto& output : m_client.status().outputs) {
            if (output.id == m_selectedOutputId)
                return &output;
        }
        return nullptr;
    }

    void updateOutputAvailability()
    {
        m_output->setEnabled(m_available && !m_outputPending && m_output->count() > 0);
        const bool editable = m_available && !m_outputPending && selectedOutput();
        m_outputShader->setEnabled(editable);
        m_outputColor->setEnabled(editable);
        m_gamma->setEnabled(editable);
        m_saturation->setEnabled(editable);
    }

    void selectOutput(const QString& id)
    {
        if (id == m_selectedOutputId)
            return;
        // Focus loss can commit spin-box text. Discard any remaining edit before
        // rebinding the controls, especially when hotplug removes the old monitor.
        m_updatingOutputs = true;
        const QSignalBlocker gammaBlocked(m_gamma);
        const QSignalBlocker saturationBlocked(m_saturation);
        m_gamma->clearFocus();
        m_saturation->clearFocus();
        m_selectedOutputId = id;
        const auto* output = selectedOutput();
        m_gamma->setValue(output ? output->settings.gamma : 1.0);
        m_saturation->setValue(output ? output->settings.saturation : 1.0);
        m_output->setToolTip(id);
        m_updatingOutputs = false;
    }

    void updateOutputControls()
    {
        const auto* output = selectedOutput();
        const QSignalBlocker shaderBlocked(m_outputShader);
        const QSignalBlocker colorBlocked(m_outputColor);
        const QSignalBlocker gammaBlocked(m_gamma);
        const QSignalBlocker saturationBlocked(m_saturation);
        const bool pending = m_outputPending && m_pendingOutputId == m_selectedOutputId;
        if (!pending) {
            const OutputSettings settings = output ? output->settings : OutputSettings { };
            m_outputShader->setChecked(output && settings.shaderEnabled);
            m_outputColor->setChecked(output && settings.colorEnabled);
            if (!m_gamma->hasFocus())
                m_gamma->setValue(settings.gamma);
            if (!m_saturation->hasFocus())
                m_saturation->setValue(settings.saturation);
        }
        m_outputState->setText(!output                        ? QString()
                : pending                                     ? QStringLiteral("Saving…")
                : !m_client.status().enabled                  ? QStringLiteral("Bypassed")
                : !output->bypass.isEmpty()                   ? QStringLiteral("Unfiltered: ") + output->bypass
                : output->shaderActive && output->colorActive ? QStringLiteral("Preset + color active")
                : output->shaderActive                        ? QStringLiteral("Preset active")
                : output->colorActive                         ? QStringLiteral("Color active")
                                                              : QStringLiteral("No processing active"));
        m_outputState->setToolTip(output ? outputRoute(*output) : QString());
        updateOutputAvailability();
    }

    void updateOutputs(const ControllerStatus& status)
    {
        bool changed = m_output->count() != status.outputs.size();
        for (int index = 0; !changed && index < m_output->count(); ++index) {
            const auto& output = status.outputs[index];
            changed = m_output->itemData(index).toString() != output.id
                || m_output->itemText(index) != output.name;
        }
        const QSignalBlocker blocked(m_output);
        if (changed) {
            m_output->clear();
            for (const auto& output : status.outputs) {
                m_output->addItem(output.name, output.id);
                m_output->setItemData(m_output->count() - 1, output.id, Qt::ToolTipRole);
            }
        }
        int index = m_output->findData(m_selectedOutputId);
        if (index < 0 && m_output->count() > 0)
            index = 0;
        m_output->setCurrentIndex(index);
        selectOutput(m_output->itemData(index).toString());
        updateOutputControls();
    }

    template <typename Value>
    void changeOutput(void (ControllerClient::*method)(const QString&, Value, ControllerClient::Reply), Value value)
    {
        if (m_updatingOutputs || !m_available || m_outputPending || !selectedOutput())
            return;
        const QString id = m_selectedOutputId;
        m_outputPending = true;
        m_pendingOutputId = id;
        m_operationError.clear();
        updateOutputControls();
        showErrors();
        // Capture the stable id at commit time, not in a delayed callback. Keep
        // edits disabled until the post-mutation status has reconciled the values.
        (m_client.*method)(id, value, [this](const ControllerError& error) {
            finishMutation(error, true);
        });
    }

    void showErrors()
    {
        QStringList errors;
        if (!m_operationError.isEmpty())
            errors.push_back(m_operationError);
        if (m_client.hasStatus() && !m_client.status().error.isEmpty()) {
            errors.push_back(QStringLiteral("Effect error: ") + m_client.status().error);
        }
        m_error->setText(errors.join(QLatin1Char('\n')));
        m_error->setVisible(!errors.isEmpty());
    }

    void reconnectToEffect()
    {
        setAvailable(false);
        m_operationError.clear();
        m_client.ensureAvailable([this](const ControllerError& error) {
            if (error) {
                m_operationError = error.message;
                m_summary->setText(QStringLiteral("WaylandShader is unavailable."));
                setAvailable(false);
            } else {
                setAvailable(true);
            }
            showErrors();
        });
    }

    void finishMutation(const ControllerError& error, bool outputMutation = false)
    {
        if (error) {
            m_operationError = error.message;
            if (error.kind != ControllerError::Rejected)
                setAvailable(false);
        }
        showErrors();
        m_client.requestStatus([this, outputMutation](const ControllerError& statusError) {
            if (outputMutation) {
                m_outputPending = false;
                m_pendingOutputId.clear();
            }
            if (statusError) {
                m_operationError = statusError.message;
                setAvailable(false);
                showErrors();
            }
            if (outputMutation)
                updateOutputControls();
        });
    }

    void load()
    {
        const QFileInfo file(m_preset->text());
        if (!file.isFile() || file.suffix().compare(QStringLiteral("slangp"), Qt::CaseInsensitive) != 0) {
            m_operationError = QStringLiteral("Choose an existing .slangp preset file.");
            showErrors();
            return;
        }
        m_operationError.clear();
        m_loadPending = true;
        setAvailable(m_available);
        m_client.loadPreset(file.canonicalFilePath(), [this](const ControllerError& error) {
            m_loadPending = false;
            finishMutation(error);
        });
    }

    bool sameParameters(const ControllerStatus& status) const
    {
        if (m_form->rowCount() == 0)
            return false;
        if (m_editors.size() != status.parameters.size() || m_parameterPreset != status.preset)
            return false;
        for (const auto& parameter : status.parameters) {
            const auto found = m_editors.constFind(parameter.name);
            if (found == m_editors.cend())
                return false;
            const auto& old = found->metadata;
            if (old.description != parameter.description || old.minimum != parameter.minimum
                || old.maximum != parameter.maximum || old.step != parameter.step
                || old.defaultValue != parameter.defaultValue)
                return false;
        }
        return true;
    }

    void buildParameters(const ControllerStatus& status)
    {
        m_editors.clear();
        while (m_form->rowCount())
            m_form->removeRow(0);
        m_parameterPreset = status.preset;
        for (const auto& parameter : status.parameters) {
            auto* spin = new ParameterSpinBox(m_parameters);
            spin->setRange(parameter.minimum, parameter.maximum);
            const double span = parameter.maximum - parameter.minimum;
            spin->setSingleStep(parameter.step > 0 ? parameter.step
                                                   : (span > 0 && std::isfinite(span) ? span / 100.0 : 1.0));
            spin->setValue(parameter.value);
            spin->setKeyboardTracking(false);
            spin->setToolTip(QStringLiteral("%1\nDefault: %2; range: %3 to %4")
                    .arg(parameter.name)
                    .arg(parameter.defaultValue, 0, 'g', 9)
                    .arg(parameter.minimum, 0, 'g', 9)
                    .arg(parameter.maximum, 0, 'g', 9));
            auto* timer = new QTimer(spin);
            timer->setSingleShot(true);
            timer->setInterval(100);
            m_editors.insert(parameter.name, { parameter, spin, timer });
            auto* label = new QLabel(parameter.description.isEmpty() ? parameter.name : parameter.description,
                m_parameters);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            label->setBuddy(spin);
            m_form->addRow(label, spin);
            const QString name = parameter.name;
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, name] {
                auto& editor = m_editors[name];
                editor.dirty = true;
                ++editor.revision;
                editor.timer->start();
            });
            connect(timer, &QTimer::timeout, this, [this, name, guard = QPointer<ParameterSpinBox>(spin)] {
                if (!guard)
                    return;
                auto& editor = m_editors[name];
                const auto revision = editor.revision;
                m_operationError.clear();
                m_client.setParameter(name, editor.spin->value(),
                    [this, name, guard, revision](const ControllerError& error) {
                        if (!guard)
                            return;
                        auto found = m_editors.find(name);
                        if (found != m_editors.end() && found->spin == guard.data()
                            && found->revision == revision)
                            found->dirty = false;
                        finishMutation(error);
                    });
            });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this] {
                // Run after focus moves so clean controls can reflect the latest server value.
                QTimer::singleShot(0, this, [this] {
                    if (m_client.hasStatus())
                        updateStatus(m_client.status());
                });
            });
        }
        if (status.parameters.isEmpty()) {
            m_form->addRow(new QLabel(status.preset.isEmpty()
                    ? QStringLiteral("Load a preset to see its declared parameters.")
                    : QStringLiteral("This preset declares no adjustable parameters."),
                m_parameters));
        }
    }

    void updateRecentPresets(const QStringList& presets)
    {
        if (m_recentPresets == presets)
            return;
        m_recentPresets = presets;
        auto* menu = m_recent->menu();
        menu->clear();
        for (const auto& path : presets) {
            auto label = path;
            auto* action = menu->addAction(label.replace(QLatin1Char('&'), QStringLiteral("&&")));
            connect(action, &QAction::triggered, this, [this, path] {
                m_preset->setText(path);
                m_pathEdited = true;
                m_preset->setFocus();
            });
        }
        m_recent->setEnabled(!presets.isEmpty());
    }

    void updateStatus(const ControllerStatus& status)
    {
        m_loading = status.loading;
        setAvailable(true);
        if (!m_enablePending) {
            const QSignalBlocker blocked(m_enabled);
            m_enabled->setChecked(status.enabled);
        }
        updateRecentPresets(status.recentPresets);
        if (!m_pathEdited && !m_preset->hasFocus()) {
            m_preset->setText(status.loading ? status.requestedPreset : status.preset);
        }
        updateOutputs(status);
        QString summary = status.active ? QStringLiteral("Desktop effects active")
            : !status.enabled           ? QStringLiteral("Desktop effects disabled")
                                        : QStringLiteral("Desktop effects enabled, but not active");
        if (status.loading)
            summary += QStringLiteral("\nCompiling: %1\nThe previous shader is retained until loading succeeds.")
                           .arg(status.requestedPreset);
        if (!status.preset.isEmpty())
            summary += QStringLiteral("\nLoaded preset: ") + status.preset;
        QStringList outputNames;
        for (const auto& output : status.outputs)
            outputNames.push_back(output.name);
        summary += QStringLiteral("\nOutputs: ")
            + (outputNames.isEmpty() ? QStringLiteral("No connected monitors") : outputNames.join(QStringLiteral(", ")));
        m_summary->setText(summary);
        if (!sameParameters(status))
            buildParameters(status);
        for (const auto& parameter : status.parameters) {
            auto& editor = m_editors[parameter.name];
            if (!editor.dirty && !editor.spin->hasFocus()) {
                const QSignalBlocker blocked(editor.spin);
                editor.spin->setValue(parameter.value);
            }
        }
        showErrors();
    }

    ControllerClient m_client;
    QLineEdit* m_preset;
    QPushButton* m_recent;
    QPushButton* m_load;
    QCheckBox* m_enabled;
    QComboBox* m_output;
    QCheckBox* m_outputShader;
    QCheckBox* m_outputColor;
    QDoubleSpinBox* m_gamma;
    QDoubleSpinBox* m_saturation;
    QLabel* m_outputState;
    QGroupBox* m_gpuBox;
    QLabel* m_gpuActive;
    QComboBox* m_gpuDevice;
    QPushButton* m_gpuSave;
    QLabel* m_gpuMessage;
    QString m_gpuConfigured;
    QString m_gpuFeedback;
    QLabel* m_summary;
    QLabel* m_error;
    QWidget* m_parameters;
    QFormLayout* m_form;
    QMap<QString, Editor> m_editors;
    QString m_parameterPreset;
    QStringList m_recentPresets;
    QString m_operationError;
    QString m_selectedOutputId;
    QString m_pendingOutputId;
    bool m_available = false;
    bool m_loading = false;
    bool m_loadPending = false;
    bool m_enablePending = false;
    bool m_pathEdited = false;
    bool m_updatingOutputs = false;
    bool m_outputPending = false;
    bool m_gpuPending = false;
};
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("waylandshader-niri-controller"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.waylandshader.NiriController"));
    QCoreApplication::setOrganizationName(QStringLiteral("WaylandShader"));
    SettingsWindow window;
    window.show();
    return application.exec();
}
