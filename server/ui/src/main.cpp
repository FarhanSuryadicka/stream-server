// ucv-monitor â€” desktop front end for the UVC transport lab.
//
// A native alternative to the Python/HTML dashboard, not a replacement: both
// read the same NDJSON and drive the same ucv-receiver, and neither knows or
// cares whether the other is running.

#include "run_list_model.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QQmlError>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  app.setApplicationName("UCV Monitor");
  app.setOrganizationName("UCV Transport Lab");

  // The native Windows style refuses background customisation, which this UI
  // relies on for its panels and cards — it renders but floods the log with
  // "current style does not support customization". Basic is the neutral
  // non-native style and looks the same on every platform, which also keeps
  // the desktop app visually consistent with the web dashboard.
  QQuickStyle::setStyle("Basic");

  MonitorController controller;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("controller", &controller);
  engine.rootContext()->setContextProperty("runsModel", controller.runs());
  engine.rootContext()->setContextProperty("comparisonModel",
                                           controller.comparison());

  // Report why QML failed rather than exiting silently. A GUI build has no
  // console, so a bare "exit 1" is indistinguishable from a crash; warnings are
  // routed to a message box so the reason is visible either way.
  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                   [](const QList<QQmlError>& warnings) {
                     for (const QQmlError& e : warnings)
                       qCritical().noquote() << "QML:" << e.toString();
                   });

  // loadFromModule resolves through the QML module registered by
  // qt_add_qml_module, so the URL cannot drift from the qrc layout CMake
  // generates. Hard-coding "qrc:/..." looked equivalent and was not: the files
  // live in a qml/ subdirectory, so their alias became qml/Main.qml under the
  // /ucv/qml prefix, and the guessed path silently did not exist.
  const QString module = "ucv.qml";
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      []() {
        qCritical() << "QML: root object could not be created";
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);
  engine.loadFromModule(module, "Main");
  if (engine.rootObjects().isEmpty()) {
    qCritical() << "QML: no root objects loaded from module" << module;
    return 1;
  }

  return app.exec();
}
