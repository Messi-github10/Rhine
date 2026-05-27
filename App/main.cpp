#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDebug>

#include <chrono>

#include "PlayerController.hpp"
#include "VulkanContext.hpp"
#include "videodisplay.hpp"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    // Qt scene graph uses default backend (OpenGL). libplacebo runs Vulkan
    // on an independent VkDevice for GPU-accelerated YUV→RGB conversion.
    if (VulkanContext::instance().init()) {
        qDebug() << "Vulkan renderer available";
    } else {
        qWarning() << "Vulkan init failed, falling back to CPU renderer";
    }

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
        using clock = std::chrono::steady_clock;
        auto ta = clock::now();
        controller.shutdown();
        auto tb = clock::now();
        VulkanContext::instance().destroy();
        auto tc = clock::now();
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        qDebug() << "[aboutToQuit] shutdown:" << ms(tb-ta) << "ms"
                 << "| destroy:" << ms(tc-tb) << "ms"
                 << "| total:" << ms(tc-ta) << "ms";
    });

    int ret = app.exec();

    qDebug() << "[main] app.exec() returned, process exiting";
    return ret;
}
