// A small time-series chart drawn on a Canvas.
//
// Canvas rather than QtCharts: QtCharts is a separate module that is often not
// installed with a default Qt build, and requiring it would turn "no chart"
// into "application will not start". This draws the same thing the web
// dashboard draws, with the same axis rules.

import QtQuick

Item {
    id: root

    // Array of {t, latency, jitter, loss}
    property var points: []
    property string valueKey: "latency"
    property string title: ""
    property color lineColor: "#397f91"
    property real floorMax: 5
    property int decimals: 0

    implicitHeight: 190

    onPointsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()

    Text {
        id: heading
        text: root.title
        color: "#66736b"
        font.pixelSize: 12
    }

    Canvas {
        id: canvas
        anchors.top: heading.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // Round tick step (1/2/5 x 10^n) so labels land on values an operator
        // can reason about ("at 20 s") instead of arbitrary ones.
        function timeStep(span) {
            var raw = span / 5;
            var mag = Math.pow(10, Math.floor(Math.log(raw <= 0 ? 1 : raw) / Math.LN10));
            var n = raw / mag;
            return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * mag;
        }

        onPaint: {
            var ctx = getContext("2d");
            var w = width, h = height;
            ctx.clearRect(0, 0, w, h);
            ctx.fillStyle = "#ffffff";
            ctx.fillRect(0, 0, w, h);
            ctx.strokeStyle = "#d9ddd1";
            ctx.lineWidth = 1;
            ctx.strokeRect(0.5, 0.5, w - 1, h - 1);

            var pts = root.points;
            if (!pts || pts.length === 0) {
                ctx.fillStyle = "#66736b";
                ctx.font = "12px sans-serif";
                ctx.fillText("Menunggu frame...", 14, 24);
                return;
            }

            var padL = 46, padR = 12, padT = 10, padB = 26;
            var plotW = w - padL - padR, plotH = h - padT - padB;

            var maxV = root.floorMax;
            for (var i = 0; i < pts.length; i++) {
                var v = pts[i][root.valueKey] || 0;
                if (v > maxV) maxV = v;
            }
            maxV *= 1.08;

            var tMin = pts[0].t || 0;
            var tMax = pts[pts.length - 1].t || 0;
            var span = (tMax - tMin) || 1;

            // Horizontal gridlines and value labels.
            ctx.font = "10px sans-serif";
            ctx.textAlign = "left";
            for (var g = 0; g <= 4; g++) {
                var y = padT + plotH * g / 4;
                ctx.strokeStyle = "#e0e4dc";
                ctx.beginPath();
                ctx.moveTo(padL, y);
                ctx.lineTo(w - padR, y);
                ctx.stroke();
                ctx.fillStyle = "#66736b";
                ctx.fillText((maxV * (1 - g / 4)).toFixed(root.decimals), 4, y + 3);
            }

            // Time axis. The "s" suffix says what the axis is, so no caption is
            // drawn — it used to collide with the right-most tick.
            var step = timeStep(span);
            ctx.textAlign = "center";
            for (var t = Math.ceil(tMin / step) * step; t <= tMax + 1e-9; t += step) {
                var gx = padL + plotW * (t - tMin) / span;
                ctx.strokeStyle = "#eef1ea";
                ctx.beginPath();
                ctx.moveTo(gx, padT);
                ctx.lineTo(gx, padT + plotH);
                ctx.stroke();
                ctx.fillStyle = "#66736b";
                ctx.fillText((step < 1 ? t.toFixed(1) : Math.round(t)) + "s",
                             gx, h - 8);
            }

            // The series itself, plotted against TIME rather than sample index:
            // a stall then shows up as a visible horizontal gap instead of
            // being hidden by even spacing.
            ctx.strokeStyle = root.lineColor;
            ctx.lineWidth = 1.7;
            ctx.beginPath();
            for (var k = 0; k < pts.length; k++) {
                var px = padL + plotW * ((pts[k].t || 0) - tMin) / span;
                var py = padT + plotH * (1 - (pts[k][root.valueKey] || 0) / maxV);
                if (k === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
            }
            ctx.stroke();
        }
    }
}
