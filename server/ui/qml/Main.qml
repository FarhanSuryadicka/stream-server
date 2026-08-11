import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    visible: true
    width: 1500
    height: 950
    title: "UCV Monitor — transport lab (desktop)"
    color: "#f2f4ec"

    readonly property color ink: "#17211b"
    readonly property color muted: "#66736b"
    readonly property color panel: "#fffdf7"
    readonly property color line: "#d9ddd1"
    readonly property color green: "#174f3b"
    readonly property color amber: "#d7913d"
    readonly property color red: "#b44b4b"

    function fmt(v, d) { return Number(v || 0).toFixed(d === undefined ? 2 : d) }

    Connections {
        target: controller
        function onErrorRaised(message) {
            errorDialog.text = message
            errorDialog.open()
        }
    }

    Dialog {
        id: errorDialog
        property alias text: errorLabel.text
        title: "Tidak bisa memulai"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok
        Label { id: errorLabel; wrapMode: Text.Wrap; width: 420 }
    }

    header: Rectangle {
        height: 74
        color: window.green
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 22
            ColumnLayout {
                spacing: 2
                Label {
                    text: "UCV Monitor"
                    color: "white"
                    font.pixelSize: 26
                    font.bold: true
                }
                Label {
                    text: "Desktop front end — berbagi NDJSON dan receiver dengan dashboard web"
                    color: "#cfe3d8"
                    font.pixelSize: 12
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: controller.resultsDir
                color: "#cfe3d8"
                font.family: "Consolas"
                font.pixelSize: 11
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 14
            anchors.margins: 16

            // ---------------------------------------------------------- start
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 16
                spacing: 14

                Frame {
                    Layout.preferredWidth: 430
                    Layout.fillHeight: true
                    background: Rectangle {
                        color: window.panel; radius: 14
                        border.color: window.line
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8
                        Label {
                            text: "START MEASUREMENT"
                            color: window.green
                            font.pixelSize: 13
                            font.letterSpacing: 1
                            font.bold: true
                        }
                        GridLayout {
                            columns: 2
                            columnSpacing: 10
                            rowSpacing: 8
                            Layout.fillWidth: true

                            Label { text: "Phone IP"; color: window.muted; font.pixelSize: 12 }
                            Label { text: "Protocol"; color: window.muted; font.pixelSize: 12 }
                            TextField {
                                id: phoneField
                                text: "192.168.137.139"
                                Layout.fillWidth: true
                            }
                            ComboBox {
                                id: protocolBox
                                model: controller.protocols
                                Layout.fillWidth: true
                            }

                            Label { text: "Mode (WxH@fps)"; color: window.muted; font.pixelSize: 12 }
                            Label { text: "Run ID (opsional)"; color: window.muted; font.pixelSize: 12 }
                            TextField {
                                id: modeField
                                placeholderText: "1280x720@30"
                                Layout.fillWidth: true
                            }
                            TextField {
                                id: runIdField
                                placeholderText: "otomatis bila kosong"
                                Layout.fillWidth: true
                            }

                            Label { text: "Duration (s)"; color: window.muted; font.pixelSize: 12 }
                            Label { text: "Warmup (s)"; color: window.muted; font.pixelSize: 12 }
                            SpinBox {
                                id: durationBox
                                from: 10; to: 3600; value: 60
                                Layout.fillWidth: true
                            }
                            SpinBox {
                                id: warmupBox
                                from: 0; to: 120; value: 5
                                Layout.fillWidth: true
                            }
                        }

                        Label {
                            text: "MJPEG resolution"
                            color: window.muted
                            font.pixelSize: 12
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ComboBox {
                                id: resolutionBox
                                Layout.fillWidth: true
                                model: controller.resolutionLabels.length > 0
                                       ? controller.resolutionLabels
                                       : ["Refresh camera modes dulu"]
                                enabled: controller.resolutionLabels.length > 0
                                onActivated: controller.selectResolution(currentIndex)
                            }
                            Button {
                                text: controller.modesBusy ? "Membaca..." : "Refresh"
                                enabled: !controller.modesBusy
                                onClicked: controller.refreshCameraModes(phoneField.text)
                            }
                        }

                        Label {
                            text: controller.modesStatus
                            visible: controller.modesStatus.length > 0
                            color: window.muted
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }

                        CheckBox {
                            id: manualPhone
                            text: "Manual phone (jangan remote START)"
                            font.pixelSize: 12
                        }

                        RowLayout {
                            spacing: 8
                            Button {
                                text: "Start measurement"
                                enabled: !controller.running && controller.receiverPresent
                                onClicked: controller.startRun(
                                    phoneField.text, protocolBox.currentText,
                                    runIdField.text,
                                    // Typed mode wins; otherwise use the one the
                                    // camera actually advertised.
                                    modeField.text.trim().length > 0
                                        ? modeField.text
                                        : controller.selectedMode,
                                    durationBox.value, warmupBox.value,
                                    manualPhone.checked)
                            }
                            Button {
                                text: "Stop"
                                enabled: controller.running
                                onClicked: controller.stopRun()
                            }
                            Button {
                                text: "Refresh"
                                onClicked: controller.refreshRuns()
                            }
                        }

                        Label {
                            visible: !controller.receiverPresent
                            text: "ucv-receiver.exe belum ada — jalankan experiment/build-receiver.ps1"
                            color: window.red
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        Item { Layout.fillHeight: true }
                    }
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    background: Rectangle {
                        color: window.panel; radius: 14
                        border.color: window.line
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "LIVE CONSOLE"
                                color: window.green
                                font.pixelSize: 13
                                font.bold: true
                                font.letterSpacing: 1
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: controller.sessionState
                                color: controller.sessionState === "complete" ? window.green
                                     : controller.sessionState === "failed" ? window.red
                                     : window.amber
                                font.bold: true
                            }
                        }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 250
                            TextArea {
                                id: consoleArea
                                text: controller.consoleText
                                readOnly: true
                                wrapMode: TextArea.Wrap
                                font.family: "Consolas"
                                font.pixelSize: 12
                                color: "#c7eed5"
                                background: Rectangle { color: "#111a15"; radius: 10 }
                                onTextChanged: cursorPosition = length
                            }
                        }
                    }
                }
            }

            // -------------------------------------------------- live preview
            Frame {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                background: Rectangle {
                    color: window.panel; radius: 14
                    border.color: window.line
                }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: "LIVE CAMERA PREVIEW"
                            color: window.green
                            font.pixelSize: 13
                            font.bold: true
                            font.letterSpacing: 1
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: controller.previewState
                            color: controller.previewState === "live" ? window.green
                                 : controller.previewState === "error" ? window.red
                                 : window.amber
                            font.bold: true
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 300
                        color: "#101713"
                        radius: 12
                        clip: true

                        Image {
                            id: previewImage
                            anchors.fill: parent
                            anchors.margins: 2
                            fillMode: Image.PreserveAspectFit
                            cache: false
                            asynchronous: true
                            // The frame counter is part of the URL so Qt treats
                            // each frame as a new image; without it the cache
                            // would show the first frame forever.
                            source: controller.previewFrame > 0
                                    ? "image://preview/f" + controller.previewFrame
                                    : ""
                            visible: controller.previewFrame > 0
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: controller.previewFrame === 0
                            text: controller.previewNote
                            color: "#d8e9df"
                            font.pixelSize: 13
                        }

                        Rectangle {
                            visible: controller.previewFrame > 0
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 12
                            radius: 8
                            color: "#07100dcc"
                            width: noteLabel.implicitWidth + 18
                            height: noteLabel.implicitHeight + 12
                            Label {
                                id: noteLabel
                                anchors.centerIn: parent
                                text: controller.previewNote
                                color: "#d8e9df"
                                font.pixelSize: 12
                            }
                        }
                    }

                    Label {
                        text: "Preview didekode oleh FFmpeg di PC (MJPEG langsung tanpa decode). " +
                              "Jalur ini hanya salinan lokal setelah frame diterima, sehingga tidak " +
                              "mengambil port video dari pengukur."
                        color: window.muted
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            // -------------------------------------------------- selected run
            Frame {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                background: Rectangle {
                    color: window.panel; radius: 14
                    border.color: window.line
                }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: {
                                var r = controller.selectedRun
                                if (!r || !r.run_id) return "Belum ada run dipilih"
                                if (r.pending) return r.run_id + " · menunggu frame…"
                                return r.run_id + " · " + r.protocol + " · " + r.mode
                            }
                            color: window.green
                            font.pixelSize: 15
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: {
                                var r = controller.selectedRun
                                if (!r || !r.clock_status) return ""
                                return r.valid ? "VALID RUN" : r.clock_status
                            }
                            color: (controller.selectedRun && controller.selectedRun.valid)
                                   ? window.green : window.amber
                            font.bold: true
                        }
                    }

                    // KPI tiles
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Repeater {
                            model: [
                                { label: "Transport p50", key: "p50",     unit: "ms", d: 2 },
                                { label: "Transport p95", key: "p95",     unit: "ms", d: 2 },
                                { label: "Transport p99", key: "p99",     unit: "ms", d: 2 },
                                { label: "Jitter",        key: "jitter",  unit: "ms", d: 2 },
                                { label: "Loss",          key: "loss",    unit: "%",  d: 3 },
                                { label: "Goodput",       key: "goodput", unit: "Mbps", d: 2 }
                            ]
                            delegate: Frame {
                                Layout.fillWidth: true
                                background: Rectangle {
                                    color: "white"; radius: 10
                                    border.color: window.line
                                }
                                ColumnLayout {
                                    spacing: 2
                                    Label {
                                        text: modelData.key === "loss" && controller.selectedRun
                                              && controller.selectedRun.loss_label
                                              ? controller.selectedRun.loss_label
                                              : modelData.label
                                        color: window.muted
                                        font.pixelSize: 10
                                        font.capitalization: Font.AllUppercase
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        spacing: 4
                                        Label {
                                            text: {
                                                var r = controller.selectedRun
                                                if (!r || r.pending || r[modelData.key] === undefined)
                                                    return "—"
                                                return window.fmt(r[modelData.key], modelData.d)
                                            }
                                            font.pixelSize: 21
                                            font.bold: true
                                            color: window.ink
                                        }
                                        Label {
                                            text: modelData.unit
                                            color: window.muted
                                            font.pixelSize: 11
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        MetricChart {
                            Layout.fillWidth: true
                            points: controller.series
                            valueKey: "latency"
                            title: "Network latency (ms)"
                            lineColor: "#397f91"
                            floorMax: 5
                            decimals: 0
                        }
                        MetricChart {
                            Layout.fillWidth: true
                            points: controller.series
                            valueKey: "jitter"
                            title: "Running jitter (ms)"
                            lineColor: "#d7913d"
                            floorMax: 1
                            decimals: 1
                        }
                        MetricChart {
                            Layout.fillWidth: true
                            points: controller.series
                            valueKey: "loss"
                            title: (controller.selectedRun && controller.selectedRun.loss_label)
                                   ? controller.selectedRun.loss_label + " (%)"
                                   : "Observed loss (%)"
                            lineColor: "#b44b4b"
                            floorMax: 0.1
                            decimals: 2
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: "Comparison group"
                            color: window.muted
                            font.pixelSize: 12
                        }
                        TextField {
                            id: groupField
                            Layout.preferredWidth: 380
                            placeholderText: "tulis nama grup, mis. 720p uji ulang"
                            // Only synced when the shown run changes, so typing
                            // is never overwritten by the 1 Hz refresh.
                            property string shownFor: ""
                            Connections {
                                target: controller
                                function onSelectionChanged() {
                                    var r = controller.selectedRun
                                    var f = controller.selectedFile
                                    if (f !== groupField.shownFor && !groupField.activeFocus) {
                                        groupField.shownFor = f
                                        groupField.text = (r && r.group) ? r.group : ""
                                    }
                                }
                            }
                        }
                        Button {
                            text: "Save label"
                            onClicked: controller.saveGroup(groupField.text)
                        }
                        Label {
                            text: {
                                var r = controller.selectedRun
                                if (!r) return ""
                                if (r.overhead === undefined)
                                    return "overhead: n/a (tidak terukur di atas API protokol ini)"
                                return "overhead: " + window.fmt(r.overhead, 2) + " %"
                            }
                            color: window.muted
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ------------------------------------------- history + comparison
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.bottomMargin: 16
                spacing: 14

                Frame {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 340
                    background: Rectangle {
                        color: window.panel; radius: 14
                        border.color: window.line
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        Label {
                            text: "RUN HISTORY"
                            color: window.green
                            font.pixelSize: 13
                            font.bold: true
                            font.letterSpacing: 1
                        }
                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: runsModel
                            ScrollBar.vertical: ScrollBar {}
                            header: Rectangle {
                                width: ListView.view.width
                                height: 26
                                color: window.panel
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 6
                                    Label { text: "Run";      Layout.preferredWidth: 150; color: window.muted; font.pixelSize: 11 }
                                    Label { text: "Protocol"; Layout.preferredWidth: 70;  color: window.muted; font.pixelSize: 11 }
                                    Label { text: "Mode";     Layout.preferredWidth: 100; color: window.muted; font.pixelSize: 11 }
                                    Label { text: "State";    Layout.preferredWidth: 70;  color: window.muted; font.pixelSize: 11 }
                                    Label { text: "Frames";   Layout.preferredWidth: 60;  color: window.muted; font.pixelSize: 11 }
                                    Label { text: "p50";      Layout.preferredWidth: 60;  color: window.muted; font.pixelSize: 11 }
                                    Label { text: "Loss";     Layout.fillWidth: true;     color: window.muted; font.pixelSize: 11 }
                                }
                            }
                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 28
                                color: file === controller.selectedFile ? "#e8f2e9" : "transparent"
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: controller.selectRun(file)
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 6
                                    Label { text: runId;   Layout.preferredWidth: 150; font.pixelSize: 11; elide: Text.ElideRight }
                                    Label { text: protocol;Layout.preferredWidth: 70;  font.pixelSize: 11 }
                                    Label { text: mode;    Layout.preferredWidth: 100; font.pixelSize: 11 }
                                    Label {
                                        text: clockStatus
                                        Layout.preferredWidth: 70
                                        font.pixelSize: 11
                                        font.bold: true
                                        color: valid ? window.green
                                             : clockStatus === "RUNNING" ? window.amber
                                             : window.red
                                    }
                                    Label { text: frames;  Layout.preferredWidth: 60; font.pixelSize: 11 }
                                    Label { text: window.fmt(p50, 2); Layout.preferredWidth: 60; font.pixelSize: 11 }
                                    Label {
                                        text: window.fmt(loss, 3) + " % (" + lossBasis + ")"
                                        Layout.fillWidth: true
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                Frame {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 340
                    background: Rectangle {
                        color: window.panel; radius: 14
                        border.color: window.line
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        Label {
                            text: "COMPARISON"
                            color: window.green
                            font.pixelSize: 13
                            font.bold: true
                            font.letterSpacing: 1
                        }
                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: comparisonModel
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Rectangle {
                                width: ListView.view.width
                                height: contentCol.implicitHeight + 12
                                color: "transparent"
                                ColumnLayout {
                                    id: contentCol
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            text: group
                                            font.pixelSize: 12
                                            font.bold: true
                                            color: window.ink
                                        }
                                        Label {
                                            visible: pending > 0
                                            text: "+" + pending + " berjalan"
                                            color: window.amber
                                            font.pixelSize: 11
                                        }
                                        Item { Layout.fillWidth: true }
                                        Label {
                                            text: runCount + " run · " + frames + " frames"
                                            color: window.muted
                                            font.pixelSize: 11
                                        }
                                    }
                                    Label {
                                        text: runCount === 0
                                              ? "menunggu run selesai…"
                                              : "p50 " + window.fmt(p50, 2) + " · p95 " + window.fmt(p95, 2)
                                                + " · p99 " + window.fmt(p99, 2)
                                                + " ms · jitter " + window.fmt(jitter, 2)
                                                + " ms · " + window.fmt(goodput, 2) + " Mbps"
                                        color: window.muted
                                        font.pixelSize: 11
                                    }
                                    Label {
                                        // Averaging different loss definitions would invent a
                                        // figure describing nothing, so a mixed group says so.
                                        text: mixedBasis
                                              ? "basis loss campur — tidak dirata-ratakan"
                                              : (runCount > 0 ? "loss " + window.fmt(loss, 3) + " % (" + lossBasis + ")" : "")
                                        color: mixedBasis ? window.amber : window.muted
                                        font.pixelSize: 11
                                    }
                                    Label {
                                        text: runIds.join("  ")
                                        color: window.muted
                                        font.pixelSize: 10
                                        font.family: "Consolas"
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
