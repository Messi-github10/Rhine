#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>

#include "PlayerController.h"
#include "videodisplay.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    PlayerController controller;

    // Load QML
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("player", &controller);

    engine.load("qrc:/Rhine/UI/Main.qml");
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Failed to load QML";
        return 1;
    }

    QObject *root = engine.rootObjects().first();
    VideoDisplay *videoDisplay = root->findChild<VideoDisplay *>("videoDisplay");
    if (!videoDisplay) {
        qCritical() << "VideoDisplay not found in QML";
        return 1;
    }

    controller.setVideoDisplay(videoDisplay);

    // Cleanup on exit
    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&controller]() {
        controller.shutdown();
    });

    return app.exec();
}