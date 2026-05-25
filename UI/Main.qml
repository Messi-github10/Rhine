import QtQuick
import QtQuick.Window
import Rhine

Window {
    id: mainWindow
    visible: true
    title: "Rhine"
    color: "black"
    flags: Qt.FramelessWindowHint | Qt.Window

    // --- 80% centered window ---
    property int screenWidth: Screen.desktopAvailableWidth
    property int screenHeight: Screen.desktopAvailableHeight

    width: Math.round(screenWidth * 0.8)
    height: Math.round(screenHeight * 0.8)
    x: Math.round((screenWidth - width) / 2)
    y: Math.round((screenHeight - height) / 2)

    // --- Video fills entire window ---
    VideoDisplay {
        id: videoDisplay
        objectName: "videoDisplay"
        anchors.fill: parent
    }

    // ============================================================
    // Drop area + welcome screen
    // ============================================================
    DropArea {
        id: dropArea
        anchors.fill: parent

        onEntered: function(drag) {
            dropOverlay.visible = true
        }
        onExited: {
            dropOverlay.visible = false
        }
        onDropped: function(drop) {
            dropOverlay.visible = false
            if (drop.urls.length > 0) {
                player.loadFile(drop.urls[0])  // QML auto-converts QUrl → QString
            }
        }
    }

    // Drop highlight overlay
    Rectangle {
        id: dropOverlay
        anchors.fill: parent
        color: "#44ffffff"
        border { width: 3; color: "#aa80c0ff" }
        visible: false
        z: 10

        Text {
            anchors.centerIn: parent
            text: "Drop to play"
            color: "white"
            font.pixelSize: 28
        }
    }

    // Welcome screen (shown when no video loaded)
    Rectangle {
        id: welcomeScreen
        anchors.fill: parent
        color: "#1a1a1a"
        visible: player ? !player.hasVideo : true
        z: 5

        Text {
            anchors.centerIn: parent
            text: "Drop files to play here"
            color: "#66ffffff"
            font.pixelSize: 24
        }
    }

    // ============================================================
    // Top bar
    // ============================================================
    Rectangle {
        id: topBar
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: 36
        color: "#cc000000"

        // Draggable area (whole top bar except buttons)
        MouseArea {
            anchors.fill: parent
            onPressed: mainWindow.startSystemMove()
        }

        // File name — top left (only when video loaded)
        Text {
            visible: player ? player.hasVideo : false
            anchors {
                left: parent.left
                leftMargin: 12
                verticalCenter: parent.verticalCenter
            }
            text: player ? player.fileName : ""
            color: "white"
            font.pixelSize: 13
            elide: Text.ElideRight
            maximumLineCount: 1
        }

        // --- Window buttons — top right ---
        Row {
            anchors {
                right: parent.right
                verticalCenter: parent.verticalCenter
            }
            spacing: 0

            // Minimize
            Rectangle {
                width: 44; height: 36
                color: btnMinMouse.containsPress ? "#44ffffff" : "transparent"

                Rectangle {
                    anchors.centerIn: parent
                    width: 14; height: 2
                    color: "white"
                }

                MouseArea {
                    id: btnMinMouse
                    anchors.fill: parent
                    onClicked: mainWindow.showMinimized()
                    hoverEnabled: true
                }
            }

            // Maximize / Restore
            Rectangle {
                id: btnMax
                width: 44; height: 36
                color: btnMaxMouse.containsPress ? "#44ffffff" : "transparent"

                // Maximize icon (single box)
                Rectangle {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    color: "transparent"
                    border { width: 2; color: "white" }
                    visible: mainWindow.visibility !== Window.Maximized
                }
                // Restore icon (two overlapping boxes)
                Item {
                    anchors.centerIn: parent
                    width: 14; height: 14
                    visible: mainWindow.visibility === Window.Maximized
                    Rectangle {
                        x: 0; y: 2
                        width: 10; height: 10
                        color: "#cc000000"
                        border { width: 2; color: "white" }
                    }
                    Rectangle {
                        x: 4; y: 0
                        width: 10; height: 10
                        color: "#cc000000"
                        border { width: 2; color: "white" }
                    }
                }

                MouseArea {
                    id: btnMaxMouse
                    anchors.fill: parent
                    onClicked: {
                        if (mainWindow.visibility === Window.Maximized)
                            mainWindow.showNormal()
                        else
                            mainWindow.showMaximized()
                    }
                    hoverEnabled: true
                }
            }

            // Close
            Rectangle {
                width: 44; height: 36
                color: btnCloseMouse.containsPress ? "#e81123" : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "\u2715"  // ✕
                    color: "white"
                    font.pixelSize: 16
                }

                MouseArea {
                    id: btnCloseMouse
                    anchors.fill: parent
                    onClicked: {
                        mainWindow.close()
                        Qt.quit()
                    }
                    hoverEnabled: true
                }
            }
        }
    }

    // ============================================================
    // Bottom overlay controls (visible only when video loaded)
    // ============================================================
    Rectangle {
        id: controls
        visible: player ? player.hasVideo : false
        anchors {
            bottom: parent.bottom
            left: parent.left
            right: parent.right
        }
        height: 56
        color: "#cc000000"

        // << Skip backward
        Rectangle {
            id: skipBackBtn
            anchors {
                left: parent.left
                leftMargin: 12
                verticalCenter: parent.verticalCenter
            }
            width: 32; height: 32
            radius: 16
            color: skipBackMouse.containsPress ? "#44ffffff" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u23EE"  // ⏮
                color: "white"
                font.pixelSize: 16
            }

            MouseArea {
                id: skipBackMouse
                anchors.fill: parent
                onClicked: player.skipBackward()
                hoverEnabled: true
            }
        }

        // Play / Pause button
        Rectangle {
            id: playBtn
            anchors {
                left: skipBackBtn.right
                leftMargin: 4
                verticalCenter: parent.verticalCenter
            }
            width: 36; height: 36
            radius: 18
            color: playBtnMouse.containsPress ? "#44ffffff" : "transparent"

            Text {
                anchors.centerIn: parent
                text: player && player.isPlaying ? "\u23F8" : "\u25B6"   // ⏸ / ▶
                color: "white"
                font.pixelSize: 22
            }

            MouseArea {
                id: playBtnMouse
                anchors.fill: parent
                onClicked: player.togglePlayPause()
                hoverEnabled: true
            }
        }

        // >> Skip forward
        Rectangle {
            id: skipFwdBtn
            anchors {
                left: playBtn.right
                leftMargin: 4
                verticalCenter: parent.verticalCenter
            }
            width: 32; height: 32
            radius: 16
            color: skipFwdMouse.containsPress ? "#44ffffff" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u23ED"  // ⏭
                color: "white"
                font.pixelSize: 16
            }

            MouseArea {
                id: skipFwdMouse
                anchors.fill: parent
                onClicked: player.skipForward()
                hoverEnabled: true
            }
        }

        // Current time
        Text {
            id: currentTimeText
            anchors {
                left: skipFwdBtn.right
                leftMargin: 8
                verticalCenter: parent.verticalCenter
            }
            text: player ? formatTime(player.currentTime) : "00:00"
            color: "white"
            font.pixelSize: 13
            font.family: "monospace"
        }

        Text {
            anchors {
                left: currentTimeText.right
                leftMargin: 4
                verticalCenter: parent.verticalCenter
            }
            text: "/"
            color: "#99ffffff"
            font.pixelSize: 13
        }

        // Total duration
        Text {
            id: durationText
            anchors {
                left: currentTimeText.right
                leftMargin: 16
                verticalCenter: parent.verticalCenter
            }
            text: player ? formatTime(player.duration) : "00:00"
            color: "#99ffffff"
            font.pixelSize: 13
            font.family: "monospace"
        }

        // Progress bar
        Rectangle {
            id: progressBar
            anchors {
                left: durationText.right
                leftMargin: 12
                right: parent.right
                rightMargin: 12
                verticalCenter: parent.verticalCenter
            }
            height: 4
            radius: 2
            color: "#44ffffff"

            // Filled portion
            Rectangle {
                id: progressFill
                anchors {
                    left: parent.left
                    top: parent.top
                    bottom: parent.bottom
                }
                radius: 2
                color: "#ffffff"
                width: {
                    if (!player || player.duration <= 0) return 0
                    var frac = player.currentTime / player.duration
                    if (frac < 0) frac = 0
                    if (frac > 1) frac = 1
                    return frac * parent.width
                }
            }

            // Click / drag to seek
            MouseArea {
                anchors.fill: parent
                anchors.margins: -8  // larger hit target
                onClicked: function(mouse) {
                    if (player && player.duration > 0) {
                        var frac = mouse.x / parent.width
                        player.seek(frac * player.duration)
                    }
                }
            }
        }
    }

    // --- Time formatter ---
    function formatTime(secs) {
        if (secs < 0) return "00:00"
        var m = Math.floor(secs / 60)
        var s = Math.floor(secs % 60)
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
}
