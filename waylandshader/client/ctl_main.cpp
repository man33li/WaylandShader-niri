#include "controller_client.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace WaylandShader;

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("waylandshader-nirictl"));
    QCoreApplication::setOrganizationName(QStringLiteral("WaylandShader"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Control native WaylandShader desktop effects over the session D-Bus.\n"
        "Commands:\n"
        "  status                         Print complete JSON status (read-only)\n"
        "  parameters                     Print declared parameters as JSON (read-only)\n"
        "  outputs                        Print connected monitors as JSON (read-only)\n"
        "  load FILE                      Load a .slangp preset and wait for compilation\n"
        "  param NAME VALUE               Set a declared shader parameter\n"
        "  output ID_OR_NAME shader on|off Enable/bypass the preset on one monitor\n"
        "  output ID_OR_NAME color on|off  Enable/bypass color adjustments on one monitor\n"
        "  output ID_OR_NAME gamma NUMBER  Set monitor gamma (%1–%2; 1 is neutral)\n"
        "  output ID_OR_NAME saturation NUMBER\n"
        "                                 Set monitor saturation (%3–%4; 1 is neutral)\n"
        "  enable | disable               Set master desktop-effect enablement\n"
        "  toggle                         Invert master desktop-effect enablement\n\n"
        "Monitor selectors match an exact id first, otherwise a unique exact name.\n"
        "Master disable bypasses both presets and color adjustments on all monitors.\n"
        "Monitor controls preserve other settings and never enable the master switch.\n"
        "Color adjustments work without a preset; shader and color switches are independent.\n"
        "Requires niri-waylandshader; stock niri is never replaced.\n"
        "Exit codes: 0 success, 1 rejected/shader failure, 2 usage error,\n"
        "3 unavailable/transport/protocol error, 4 timeout (operation may still finish).")
            .arg(OutputSettings::MinimumGamma)
            .arg(OutputSettings::MaximumGamma)
            .arg(OutputSettings::MinimumSaturation)
            .arg(OutputSettings::MaximumSaturation));
    parser.addHelpOption();
    const QCommandLineOption timeoutOption(QStringLiteral("timeout"),
        QStringLiteral("Overall deadline in seconds, including asynchronous compilation (1–3600)."),
        QStringLiteral("seconds"), QStringLiteral("120"));
    parser.addOption(timeoutOption);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("One of the commands above."));
    parser.addPositionalArgument(QStringLiteral("arguments"), QStringLiteral("Command-specific arguments."),
        QStringLiteral("[arguments…]"));
    if (!parser.parse(application.arguments())) {
        QTextStream(stderr) << parser.errorText() << '\n';
        return 2;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    const auto usageError = [](const QString& message) {
        QTextStream(stderr) << message << "\nUse " << QCoreApplication::applicationName() << " --help for usage.\n";
        return 2;
    };
    const auto arguments = parser.positionalArguments();
    if (arguments.isEmpty())
        return usageError(QStringLiteral("A command is required."));
    const QString command = arguments.front();
    const bool readOnly = command == QStringLiteral("status") || command == QStringLiteral("parameters")
        || command == QStringLiteral("outputs");
    const bool isLoad = command == QStringLiteral("load");
    const bool isParameter = command == QStringLiteral("param");
    const bool isOutput = command == QStringLiteral("output");
    const bool isEnable = command == QStringLiteral("enable");
    const bool isDisable = command == QStringLiteral("disable");
    const bool isToggle = command == QStringLiteral("toggle");
    if (!readOnly && !isLoad && !isParameter && !isOutput && !isEnable && !isDisable && !isToggle) {
        return usageError(QStringLiteral("Unknown command: ") + command);
    }
    const int required = isLoad ? 2 : isParameter ? 3
        : isOutput                                ? 4
                                                  : 1;
    if (arguments.size() != required)
        return usageError(QStringLiteral("Incorrect number of arguments for ") + command);
    bool timeoutOk = false;
    const int timeoutSeconds = parser.value(timeoutOption).toInt(&timeoutOk);
    if (!timeoutOk || timeoutSeconds < 1 || timeoutSeconds > 3600) {
        return usageError(QStringLiteral("--timeout must be an integer from 1 to 3600."));
    }
    QString preset;
    if (isLoad) {
        const QFileInfo file(arguments.at(1));
        if (!file.isFile() || file.suffix().compare(QStringLiteral("slangp"), Qt::CaseInsensitive) != 0) {
            return usageError(QStringLiteral("load requires an existing .slangp preset file."));
        }
        preset = file.canonicalFilePath();
        if (preset.isEmpty())
            return usageError(QStringLiteral("Could not resolve the preset's absolute path."));
    }
    double parameterValue = 0;
    if (isParameter) {
        bool ok = false;
        parameterValue = arguments.at(2).toDouble(&ok);
        if (arguments.at(1).isEmpty() || !ok || !std::isfinite(parameterValue)) {
            return usageError(QStringLiteral("param requires a nonempty parameter name and a finite numeric value."));
        }
    }
    QString outputAction;
    bool outputEnabled = false;
    double outputValue = 0;
    if (isOutput) {
        if (arguments.at(1).isEmpty())
            return usageError(QStringLiteral("output requires a nonempty monitor id or name."));
        outputAction = arguments.at(2);
        const QString value = arguments.at(3);
        if (outputAction == QStringLiteral("shader") || outputAction == QStringLiteral("color")) {
            if (value != QStringLiteral("on") && value != QStringLiteral("off"))
                return usageError(QStringLiteral("output ") + outputAction + QStringLiteral(" requires on or off."));
            outputEnabled = value == QStringLiteral("on");
        } else if (outputAction == QStringLiteral("gamma") || outputAction == QStringLiteral("saturation")) {
            bool ok = false;
            outputValue = value.toDouble(&ok);
            const bool gamma = outputAction == QStringLiteral("gamma");
            const double minimum = gamma ? OutputSettings::MinimumGamma : OutputSettings::MinimumSaturation;
            const double maximum = gamma ? OutputSettings::MaximumGamma : OutputSettings::MaximumSaturation;
            if (!ok || !std::isfinite(outputValue) || outputValue < minimum || outputValue > maximum) {
                return usageError(QStringLiteral("output %1 requires a finite number from %2 to %3.")
                        .arg(outputAction)
                        .arg(minimum)
                        .arg(maximum));
            }
        } else {
            return usageError(QStringLiteral("Unknown output control: ") + outputAction);
        }
    }

    ControllerClient client;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool finished = false;
    bool loadAccepted = false;
    bool loadInProgress = false;
    const auto finish = [&](int code, const QString& error = { }) {
        if (finished)
            return;
        finished = true;
        deadline.stop();
        if (!error.isEmpty())
            QTextStream(stderr) << error << '\n';
        application.exit(code);
    };
    const auto fail = [&](const ControllerError& error) {
        finish(error.kind == ControllerError::Rejected ? 1 : 3, error.message);
    };
    const auto printStatus = [&] {
        QTextStream(stdout) << QJsonDocument(client.status().json).toJson(QJsonDocument::Indented);
    };
    const auto evaluateLoad = [&](const ControllerStatus& status) {
        if (finished || !loadAccepted)
            return;
        if (status.loading) {
            if (status.requestedPreset != preset) {
                finish(1, QStringLiteral("Another preset load replaced this request: ") + status.requestedPreset);
            }
            return;
        }
        if (!status.error.isEmpty()) {
            finish(1, QStringLiteral("Shader load failed: ") + status.error + QStringLiteral("\nPreviously loaded preset: ") + status.preset);
        } else if (status.preset != preset) {
            finish(1, QStringLiteral("Loading ended without the requested preset becoming current. Current preset: ") + status.preset);
        } else {
            printStatus();
            finish(0);
        }
    };
    QObject::connect(&client, &ControllerClient::statusChanged, &application,
        [&](const ControllerStatus& status) { evaluateLoad(status); });
    QObject::connect(&client, &ControllerClient::transportError, &application,
        [&](const ControllerError& error) { fail(error); });
    QObject::connect(&deadline, &QTimer::timeout, &application, [&] {
        finish(4, loadInProgress ? QStringLiteral("Timed out waiting for shader loading. The request was not cancelled and may still finish; use status to check.") : QStringLiteral("Timed out waiting for the compositor. A mutation may still finish; use status to check."));
    });
    QTimer::singleShot(0, &application, [&] {
        deadline.start(timeoutSeconds * 1000);
        client.ensureAvailable([&](const ControllerError& error) {
            if (finished)
                return;
            if (error) {
                fail(error);
                return;
            }
            if (readOnly) {
                if (command == QStringLiteral("parameters") || command == QStringLiteral("outputs")) {
                    QTextStream(stdout) << QJsonDocument(client.status().json.value(command).toArray())
                                               .toJson(QJsonDocument::Indented);
                } else {
                    printStatus();
                }
                finish(0);
                return;
            }
            if (isLoad) {
                loadInProgress = true;
                client.loadPreset(preset, [&](const ControllerError& loadError) {
                    if (finished)
                        return;
                    if (loadError) {
                        fail(loadError);
                        return;
                    }
                    // A status signal may arrive before the acceptance reply.
                    // Query again rather than interpreting pre-request state as completion.
                    loadAccepted = true;
                    client.requestStatus([&](const ControllerError& statusError) {
                        if (finished)
                            return;
                        if (statusError)
                            fail(statusError);
                    });
                });
                return;
            }
            ControllerClient::Reply mutationComplete = [&](const ControllerError& mutationError) {
                if (finished)
                    return;
                if (mutationError) {
                    fail(mutationError);
                    return;
                }
                client.requestStatus([&](const ControllerError& statusError) {
                    if (finished)
                        return;
                    if (statusError) {
                        fail(statusError);
                        return;
                    }
                    printStatus();
                    finish(0);
                });
            };
            if (isParameter) {
                client.setParameter(arguments.at(1), parameterValue, mutationComplete);
            } else if (isOutput) {
                const QString selector = arguments.at(1);
                QString id;
                for (const auto& output : client.status().outputs) {
                    if (output.id == selector) {
                        id = output.id;
                        break;
                    }
                }
                if (id.isEmpty()) {
                    for (const auto& output : client.status().outputs) {
                        if (output.name != selector)
                            continue;
                        if (!id.isEmpty()) {
                            finish(usageError(QStringLiteral("Ambiguous monitor name: ") + selector
                                + QStringLiteral(". Use a monitor id from outputs instead.")));
                            return;
                        }
                        id = output.id;
                    }
                }
                if (id.isEmpty()) {
                    finish(usageError(QStringLiteral("Unknown monitor id or name: ") + selector));
                    return;
                }
                if (outputAction == QStringLiteral("shader")) {
                    client.setOutputShaderEnabled(id, outputEnabled, mutationComplete);
                } else if (outputAction == QStringLiteral("color")) {
                    client.setOutputColorEnabled(id, outputEnabled, mutationComplete);
                } else if (outputAction == QStringLiteral("gamma")) {
                    client.setOutputGamma(id, outputValue, mutationComplete);
                } else {
                    client.setOutputSaturation(id, outputValue, mutationComplete);
                }
            } else {
                client.setEnabled(isEnable || (isToggle && !client.status().enabled), mutationComplete);
            }
        });
    });
    return application.exec();
}
