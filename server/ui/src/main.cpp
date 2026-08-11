// ucv-monitor — desktop front end for the UVC transport lab.
//
// A native alternative to the Python/HTML dashboard, not a replacement: both
// read the same NDJSON and drive the same ucv-receiver, and neither knows or
// cares whether the other is running.

#include "run_list_model.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  app.setApplicationName("UCV Monitor");
  app.setOrganizationName("UCV Transport Lab");

  MonitorController controller;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("controller", &controller);
  engine.rootContext()->setContextProperty("runsModel", controller.runs());
  engine.rootContext()->setContextProperty("comparisonModel",
                                           controller.comparison());

  const QUrl url("qrc:/ucv/qml/Main.qml");
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
  engine.load(url);
  if (engine.rootObjects().isEmpty()) return 1;

  return app.exec();
}
