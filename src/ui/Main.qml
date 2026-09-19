import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.freedesktop.gstreamer.Qt6GLVideoItem 1.0

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    minimumWidth: 900
    minimumHeight: 620
    visible: true
    title: "quadcap"
    color: "#0b0d10"

    readonly property color panel: "#14181e"
    readonly property color border: "#272d36"
    readonly property color muted: "#8c96a5"
    readonly property color text: "#eef2f7"
    readonly property color accent: "#7c6cff"

    component LabelSmall: Text {
        color: window.muted
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    component ChannelStrip: RowLayout {
        id: strip
        property string title
        property real level: 0
        property real gainDb: 0
        property bool muted: false
        signal gainChanged(real value)
        signal muteToggled()

        spacing: 10

        Text {
            text: strip.title
            color: strip.muted ? window.muted : window.text
            font.pixelSize: 11
            font.weight: Font.DemiBold
            Layout.preferredWidth: 38
        }

        Rectangle {
            Layout.preferredWidth: 74
            Layout.preferredHeight: 8
            radius: 4
            color: "#0c0f14"
            border.color: window.border

            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 1
                height: parent.height - 2
                width: Math.max(0, (parent.width - 2) * strip.level)
                radius: 3
                color: strip.muted ? "#4a5160" : (strip.level > 0.85 ? "#f2b84b" : "#49d17d")
                Behavior on width { NumberAnimation { duration: 55 } }
            }
        }

        Slider {
            Layout.fillWidth: true
            from: -40
            to: 12
            stepSize: 1
            value: strip.gainDb
            enabled: !strip.muted
            onMoved: strip.gainChanged(value)
        }

        Text {
            text: strip.gainDb <= -40 ? "-\u221e" : (strip.gainDb > 0 ? "+" + strip.gainDb : strip.gainDb) + " dB"
            color: window.muted
            font.pixelSize: 10
            font.family: "monospace"
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: 46
        }

        Button {
            implicitWidth: 30
            implicitHeight: 22
            onClicked: strip.muteToggled()
            background: Rectangle {
                radius: 5
                color: strip.muted ? "#3a2025" : "#1c2230"
                border.color: strip.muted ? "#e65c68" : window.border
            }
            contentItem: Text {
                text: "M"
                color: strip.muted ? "#f05c67" : window.muted
                font.pixelSize: 10
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    component Panel: Rectangle {
        color: window.panel
        border.color: window.border
        border.width: 1
        radius: 10
    }

    header: Rectangle {
        height: 64
        color: "#101318"
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            spacing: 14

            Rectangle {
                width: 30
                height: 30
                radius: 8
                color: window.accent
                Text { anchors.centerIn: parent; text: "Q"; color: "white"; font.bold: true; font.pixelSize: 16 }
            }
            Text { text: "quadcap"; color: window.text; font.pixelSize: 19; font.weight: Font.DemiBold }
            Rectangle { width: 1; height: 24; color: window.border; Layout.leftMargin: 4; Layout.rightMargin: 4 }
            Rectangle { width: 8; height: 8; radius: 4; color: appController.statusColor }
            Text { text: appController.statusText; color: window.text; font.pixelSize: 13 }
            Text { text: "·  " + appController.signalText; color: window.muted; font.pixelSize: 13 }
            Item { Layout.fillWidth: true }
            Text { text: appController.diskText; color: window.muted; font.pixelSize: 13 }
            Button {
                id: refreshButton
                text: "Refresh"
                flat: true
                onClicked: appController.refreshDevice()
                contentItem: Text { text: refreshButton.text; color: window.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // Stays visible and laid out at all times. qml6glsink sizes its scene-graph node from
            // this item when the pipeline starts, so hiding it until the signal locks handed the
            // sink an item the renderer had never touched: the preview then came up blank until a
            // manual Refresh, and rendered at the wrong scale when it did appear. The no-signal
            // panel below covers it instead, which costs nothing and keeps the item real.
            GstGLQt6VideoItem {
                id: videoItem
                objectName: "previewSurface"
                anchors.fill: parent
            }

            // Tied to whether frames are actually flowing, not to device status. A transient
            // status blip must never drop a curtain over a live picture.
            Rectangle {
                anchors.fill: parent
                visible: !appController.capturing
                color: "#090b0e"

                // Nothing is wrong with the machine, we are just waiting on the console.
                Column {
                    anchors.centerIn: parent
                    spacing: 10
                    visible: !appController.setupRequired
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "NO SIGNAL"; color: window.text; font.pixelSize: 22; font.weight: Font.DemiBold; font.letterSpacing: 2 }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: appController.statusText; color: window.muted; font.pixelSize: 14 }
                }

                /*
                 * First-run guide. This card needs an out-of-tree driver that no distribution
                 * ships, and on a Secure Boot machine that driver also has to be signed and its key
                 * enrolled. Someone meeting the app for the first time cannot be expected to infer
                 * any of that from an empty window, so the exact command is on screen and copyable.
                 */
                Flickable {
                    anchors.fill: parent
                    anchors.margins: 28
                    visible: appController.setupRequired
                    contentHeight: setupColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: setupColumn
                        width: parent.width
                        spacing: 14

                        RowLayout {
                            spacing: 10
                            Rectangle { width: 9; height: 9; radius: 5; color: "#f2b84b"; Layout.alignment: Qt.AlignVCenter }
                            Text {
                                text: "SETUP REQUIRED"
                                color: "#f2b84b"
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.2
                            }
                            Item { Layout.fillWidth: true }
                            Rectangle {
                                visible: appController.secureBootEnabled
                                implicitWidth: sbLabel.width + 18
                                implicitHeight: 22
                                radius: 11
                                color: "#1b2030"
                                border.color: "#39425c"
                                Text {
                                    id: sbLabel
                                    anchors.centerIn: parent
                                    text: "SECURE BOOT ON"
                                    color: "#9fb0d0"
                                    font.pixelSize: 9
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.8
                                }
                            }
                        }

                        Text {
                            text: appController.setupHeadline
                            color: window.text
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Repeater {
                            model: appController.setupSteps
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Text {
                                    text: (index + 1) + "."
                                    color: window.accent
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    Layout.alignment: Qt.AlignTop
                                }
                                Text {
                                    text: modelData
                                    color: window.muted
                                    font.pixelSize: 13
                                    lineHeight: 1.3
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        Rectangle {
                            visible: appController.setupCommand.length > 0
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            implicitHeight: 46
                            radius: 8
                            color: "#0c0f14"
                            border.color: window.border

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                anchors.rightMargin: 8
                                spacing: 10

                                Text {
                                    text: "$ " + appController.setupCommand
                                    color: "#c9d4e4"
                                    font.pixelSize: 12
                                    font.family: "monospace"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Button {
                                    id: copyButton
                                    implicitHeight: 28
                                    implicitWidth: 64
                                    onClicked: {
                                        commandClipboard.text = appController.setupCommand
                                        commandClipboard.selectAll()
                                        commandClipboard.copy()
                                        copyButton.copied = true
                                        copyResetTimer.restart()
                                    }
                                    property bool copied: false
                                    background: Rectangle {
                                        radius: 6
                                        color: "#1c2230"
                                        border.color: copyButton.copied ? "#49d17d" : window.border
                                    }
                                    contentItem: Text {
                                        text: copyButton.copied ? "COPIED" : "COPY"
                                        color: copyButton.copied ? "#49d17d" : window.text
                                        font.pixelSize: 10
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }
                        }

                        Text {
                            text: "Run it in a terminal, then press Refresh. The installer registers the driver with DKMS, so it rebuilds itself whenever the kernel updates and loads on every boot."
                            color: "#66707f"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                        }
                    }
                }

                // Off-screen holder used only to put the command on the clipboard.
                TextEdit {
                    id: commandClipboard
                    visible: false
                    width: 0
                    height: 0
                }

                Timer {
                    id: copyResetTimer
                    interval: 1600
                    onTriggered: copyButton.copied = false
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                width: previewWarning.width + 24
                height: 30
                radius: 6
                color: "#cc11151a"
                border.color: "#3a424e"
                Text {
                    id: previewWarning
                    anchors.centerIn: parent
                    text: "PREVIEW · PLAY ON THE PASSTHROUGH DISPLAY"
                    color: "#bdc5d1"
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Panel {
                Layout.fillWidth: true
                Layout.preferredHeight: 118
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 18

                    Button {
                        id: recordButton
                        enabled: appController.capturing
                        Layout.preferredWidth: 150
                        Layout.fillHeight: true
                        onClicked: appController.toggleRecording()
                        background: Rectangle {
                            radius: 8
                            color: appController.recording ? "#3a2025" : "#222731"
                            border.color: appController.recording ? "#e65c68" : "#343b47"
                        }
                        contentItem: Row {
                            anchors.centerIn: parent
                            spacing: 10
                            Rectangle { width: 12; height: 12; radius: appController.recording ? 2 : 6; color: "#f05c67" }
                            Text { text: appController.recording ? "STOP" : "RECORD"; color: window.text; font.weight: Font.DemiBold; font.pixelSize: 13 }
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: 120
                        LabelSmall { text: "RECORDING" }
                        Text { text: appController.elapsedText; color: window.text; font.pixelSize: 22; font.family: "monospace" }
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: window.border }

                    ColumnLayout {
                        Layout.preferredWidth: 150
                        LabelSmall { text: "AUDIO" }
                        Text {
                            text: appController.audioSummary
                            // Amber when a source was dropped: a missing track is easy to not notice
                            // until you are editing, which is far too late.
                            color: appController.audioNotice.length > 0 ? "#f2b84b" : window.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Rectangle { width: 1; Layout.fillHeight: true; color: window.border }

                    ColumnLayout {
                        Layout.fillWidth: true
                        LabelSmall { text: "OUTPUT" }
                        Text { text: appController.outputDirectory; color: window.text; font.pixelSize: 13; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    }
                }
            }

            Panel {
                Layout.preferredWidth: 330
                Layout.preferredHeight: 118
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 6

                    RowLayout {
                        LabelSmall { text: "AUDIO MIX" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "isolated tracks stay raw"
                            color: "#5b6572"
                            font.pixelSize: 9
                        }
                    }

                    ChannelStrip {
                        Layout.fillWidth: true
                        title: "GAME"
                        level: appController.gameLevel
                        gainDb: appController.gameGainDb
                        muted: appController.gameMuted
                        onGainChanged: function(value) { appController.gameGainDb = value }
                        onMuteToggled: appController.gameMuted = !appController.gameMuted
                    }

                    ChannelStrip {
                        Layout.fillWidth: true
                        title: "MIC"
                        level: appController.micLevel
                        gainDb: appController.micGainDb
                        muted: appController.micMuted
                        onGainChanged: function(value) { appController.micGainDb = value }
                        onMuteToggled: appController.micMuted = !appController.micMuted
                    }
                }
            }

            Panel {
                Layout.preferredWidth: 330
                Layout.preferredHeight: 118
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8
                    RowLayout {
                        LabelSmall { text: "FLASHBACK BUFFER" }
                        Item { Layout.fillWidth: true }
                        Text { text: appController.flashbackMinutes + " min"; color: window.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                    }
                    Slider {
                        Layout.fillWidth: true
                        from: 1
                        to: 30
                        stepSize: 1
                        value: appController.flashbackMinutes
                        onMoved: appController.flashbackMinutes = value
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text {
                            text: appController.flashbackBufferedText
                            color: window.muted
                            font.pixelSize: 11
                            font.family: "monospace"
                            Layout.fillWidth: true
                        }
                        Button {
                            id: saveFlashbackButton
                            enabled: appController.flashbackReady
                            implicitHeight: 28
                            implicitWidth: 132
                            onClicked: appController.saveFlashback()
                            background: Rectangle {
                                radius: 6
                                color: saveFlashbackButton.enabled ? "#232a36" : "#191d24"
                                border.color: saveFlashbackButton.enabled ? window.accent : window.border
                            }
                            contentItem: Text {
                                text: "SAVE LAST " + appController.flashbackMinutes + "M"
                                color: saveFlashbackButton.enabled ? window.text : window.muted
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            visible: appController.audioNotice.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 42 : 0
            radius: 7
            color: "#2a2519"
            border.color: "#6b5a2a"
            Text { anchors.fill: parent; anchors.margins: 12; text: appController.audioNotice; color: "#f2d89b"; font.pixelSize: 12; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
        }

        Rectangle {
            visible: appController.lastSavedText.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 42 : 0
            radius: 7
            color: "#17251d"
            border.color: "#2f5f45"
            Text { anchors.fill: parent; anchors.margins: 12; text: appController.lastSavedText; color: "#9ee0b6"; font.pixelSize: 12; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter }
        }

        Rectangle {
            visible: appController.lastError.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 42 : 0
            radius: 7
            color: "#2b1d21"
            border.color: "#68323b"
            Text { anchors.fill: parent; anchors.margins: 12; text: appController.lastError; color: "#ffb9bf"; font.pixelSize: 12; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
        }
    }
}
