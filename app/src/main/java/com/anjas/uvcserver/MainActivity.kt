package com.anjas.uvcserver

import android.Manifest
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.ConnectivityManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.delay
import java.net.Inet4Address
import java.net.NetworkInterface

private const val ACTION_USB_PERMISSION = "com.anjas.uvcserver.USB_PERMISSION"
private const val SERVER_PORT = 8181

// Experiment ports — must match ucv_wire.h.
private const val CONTROL_PORT = 8200
private const val RAWUDP_PORT = 8201
private const val DEFAULT_CAP_W = 320
private const val DEFAULT_CAP_H = 240
private const val DEFAULT_CAP_FPS = 20

// Auto-picked "highest score" mode is unreliable over isochronous USB (see
// UvcNative.startServer); pin a modest known-good mode instead of always
// getting auto-best.

class MainActivity : ComponentActivity() {
    private lateinit var usbManager: UsbManager
    private var statusText = mutableStateOf("Idle")
    private var serverRunning = mutableStateOf(false)
    private var controlRunning = mutableStateOf(false)
    private var experimentRunning = mutableStateOf(false)
    private var runId = mutableStateOf("")
    private var cameraChoices = mutableStateOf(listOf<UsbDevice>())

    // UVC access is raw USB host I/O, not the Camera2 API, but some OEM
    // ROMs (MIUI/HyperOS observed) gate any USB device that declares a
    // Video-class interface behind the same CAMERA/RECORD_AUDIO runtime
    // permissions Camera2 needs — without them, opening/streaming the
    // device fails (denial or crash) even though AOSP itself doesn't
    // require them for plain USB host access.
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        if (results.values.all { it }) {
            requestCameraAccess()
        } else {
            statusText.value = "Camera/microphone permission denied"
        }
    }

    private fun hasCameraPermissions(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) ==
            PackageManager.PERMISSION_GRANTED &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private fun requestCameraAccessWithPermissions() {
        if (hasCameraPermissions()) {
            requestCameraAccess()
        } else {
            permissionLauncher.launch(
                arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
            )
        }
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (granted && device != null) {
                openDevice(device)
            } else {
                statusText.value = "Permission denied"
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        ContextCompat.registerReceiver(
            this, usbReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )

        enableEdgeToEdge()
        setContent {
            com.anjas.uvcserver.ui.theme.UvcserverTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    val status by statusText
                    val running by serverRunning
                    val choices by cameraChoices
                    var logText by remember { mutableStateOf("") }
                    var controlState by remember { mutableStateOf("") }
                    var experimentState by remember { mutableStateOf("") }
                    var pcIp by remember { mutableStateOf("192.168.1.") }
                    var capWText by remember { mutableStateOf(DEFAULT_CAP_W.toString()) }
                    var capHText by remember { mutableStateOf(DEFAULT_CAP_H.toString()) }
                    var capFpsText by remember { mutableStateOf(DEFAULT_CAP_FPS.toString()) }
                    var manualMode by remember { mutableStateOf(false) }
                    val capW = capWText.toIntOrNull() ?: 0
                    val capH = capHText.toIntOrNull() ?: 0
                    val capFps = capFpsText.toIntOrNull() ?: 0
                    val captureModeValid = capW > 0 && capH > 0 && capFps > 0

                    LaunchedEffect(Unit) {
                        while (true) {
                            logText = UvcNative.getLogText()
                            experimentRunning.value = UvcNative.isExperimentRunning()
                            if (controlRunning.value) {
                                controlState = UvcNative.getControlState()
                            }
                            if (experimentRunning.value) {
                                experimentState = UvcNative.getExperimentState()
                            }
                            delay(500)
                        }
                    }

                    Column(
                        modifier = Modifier.fillMaxSize().padding(innerPadding).padding(16.dp),
                        verticalArrangement = Arrangement.Top,
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Button(
                                enabled = manualMode,
                                onClick = { manualMode = false },
                                modifier = Modifier.weight(1f)
                            ) { Text("Website / Auto") }
                            Button(
                                enabled = !manualMode,
                                onClick = { manualMode = true },
                                modifier = Modifier.weight(1f)
                            ) { Text("Manual / Debug") }
                        }
                        Spacer(Modifier.height(8.dp))
                        Text(text = status)
                        if (choices.isNotEmpty()) {
                            Text("Multiple UVC cameras found — pick one:")
                            choices.forEach { dev ->
                                Button(onClick = {
                                    cameraChoices.value = emptyList()
                                    requestPermissionAndOpen(dev)
                                }) {
                                    Text(
                                        dev.productName
                                            ?: "VID:%04x PID:%04x".format(dev.vendorId, dev.productId)
                                    )
                                }
                            }
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(onClick = { requestCameraAccessWithPermissions() }) {
                                Text("Open USB Camera")
                            }
                            Button(onClick = {
                                UvcNative.stopExperiment()
                                experimentRunning.value = false
                                UvcNative.stopControl()
                                controlRunning.value = false
                                UvcNative.stopServer()
                                UvcNative.closeDevice()
                                statusText.value = "Closed"
                            }) {
                                Text("Close")
                            }
                        }

                        if (manualMode) {
                        Spacer(Modifier.height(12.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(
                                enabled = captureModeValid,
                                onClick = {
                                    UvcNative.stopServer()
                                    val ok = UvcNative.startServer(
                                        SERVER_PORT, capW, capH, capFps
                                    )
                                    serverRunning.value = ok
                                    statusText.value = if (ok)
                                        "Serving on ${localIpAddress()}:$SERVER_PORT"
                                    else "startServer failed - see log below"
                                }
                            ) {
                                Text(if (running) "Server running" else "Start Server")
                            }
                            Button(onClick = {
                                UvcNative.stopServer()
                                serverRunning.value = false
                                statusText.value = "Server stopped"
                            }) {
                                Text("Stop Server")
                            }
                        }

                        // --- Protocol experiment harness ---
                        // The control channel must be up BEFORE the PC
                        // receiver starts, otherwise its clock-sync probe
                        // fails and it aborts the run rather than reporting
                        // latency that is really clock offset.
                        Spacer(Modifier.height(12.dp))
                        Text("Manual camera controls", fontSize = 12.sp)

                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            OutlinedTextField(
                                value = capWText,
                                onValueChange = { capWText = it.filter(Char::isDigit) },
                                label = { Text("Width") },
                                singleLine = true,
                                modifier = Modifier.weight(1f)
                            )
                            OutlinedTextField(
                                value = capHText,
                                onValueChange = { capHText = it.filter(Char::isDigit) },
                                label = { Text("Height") },
                                singleLine = true,
                                modifier = Modifier.weight(1f)
                            )
                            OutlinedTextField(
                                value = capFpsText,
                                onValueChange = { capFpsText = it.filter(Char::isDigit) },
                                label = { Text("FPS") },
                                singleLine = true,
                                modifier = Modifier.weight(1f)
                            )
                        }
                        Text(
                            if (captureModeValid) "Capture: ${capW}x${capH}@${capFps}fps"
                            else "Enter a valid capture mode",
                            fontSize = 10.sp
                        )

                        OutlinedTextField(
                            value = pcIp,
                            onValueChange = { pcIp = it },
                            label = { Text("PC IP (receiver)") },
                            singleLine = true,
                            textStyle = LocalTextStyle.current.copy(fontSize = 13.sp),
                            modifier = Modifier.fillMaxWidth()
                        )

                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(onClick = {
                                val id = "%d-run".format(System.currentTimeMillis() / 1000)
                                UvcNative.setRunId(id)
                                runId.value = id
                                val ok = UvcNative.startControl(CONTROL_PORT)
                                controlRunning.value = ok
                                statusText.value = if (ok)
                                    "Control on ${localIpAddress()}:$CONTROL_PORT  run_id=$id"
                                else "startControl failed - see log below"
                            }) {
                                Text(if (controlRunning.value) "Control up" else "1. Start Control")
                            }
                            Button(onClick = {
                                UvcNative.stopControl()
                                controlRunning.value = false
                                statusText.value = "Control stopped"
                            }) {
                                Text("Stop Control")
                            }
                        }

                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(
                                enabled = controlRunning.value &&
                                    !experimentRunning.value && captureModeValid,
                                onClick = {
                                    val peer = "${pcIp.trim()}:$RAWUDP_PORT"
                                    val ok = UvcNative.startExperiment(
                                        UvcNative.PROTO_RAWUDP, peer, capW, capH, capFps
                                    )
                                    experimentRunning.value = ok
                                    statusText.value = if (ok)
                                        "Streaming H.264 (raw_udp) to $peer"
                                    else "startExperiment failed - see log below"
                                }
                            ) {
                                Text("2. Start Raw UDP")
                            }
                            Button(
                                enabled = experimentRunning.value,
                                onClick = {
                                    UvcNative.stopExperiment()
                                    experimentRunning.value = false
                                    statusText.value = "Experiment stopped"
                                }
                            ) {
                                Text("Stop")
                            }
                        }

                        if (controlRunning.value || experimentRunning.value) {
                            Text(
                                text = buildString {
                                    if (runId.value.isNotEmpty()) {
                                        append("run_id=${runId.value}\n")
                                    }
                                    if (experimentRunning.value) append("$experimentState\n")
                                    if (controlRunning.value) append(controlState)
                                },
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp
                            )
                        }
                        }

                        if (!manualMode) {
                        Spacer(Modifier.height(16.dp))
                        Text("PC-controlled camera agent", fontSize = 15.sp)
                        Text(
                            if (controlRunning.value)
                                "Control: READY  ${localIpAddress()}:$CONTROL_PORT"
                            else "Control: waiting for camera",
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp
                        )
                        Text(
                            if (experimentRunning.value) "Pipeline: STREAMING"
                            else "Pipeline: waiting for website",
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp
                        )
                        if (experimentRunning.value) {
                            Text(experimentState, fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp)
                        } else if (controlRunning.value) {
                            Text(controlState, fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp)
                        }
                        Text(
                            "Select a camera mode and start measurement from the PC website.",
                            fontSize = 11.sp
                        )
                        }

                        Spacer(Modifier.height(8.dp))
                        Text(
                            text = logText,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            modifier = Modifier
                                .fillMaxWidth()
                                .weight(1f)
                                .verticalScroll(rememberScrollState())
                        )
                    }
                }
            }
        }
    }

    private fun localIpAddress(): String {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        cm.activeNetwork?.let { network ->
            val linkProperties = cm.getLinkProperties(network)
            linkProperties?.linkAddresses?.forEach { la ->
                val addr = la.address
                if (addr is Inet4Address && !addr.isLoopbackAddress) {
                    return addr.hostAddress ?: ""
                }
            }
        }
        // Fallback: scan interfaces directly (covers odd active-network states).
        for (iface in NetworkInterface.getNetworkInterfaces()) {
            for (addr in iface.inetAddresses) {
                if (addr is Inet4Address && !addr.isLoopbackAddress) {
                    return addr.hostAddress ?: "0.0.0.0"
                }
            }
        }
        return "0.0.0.0"
    }

    /** Any attached USB device exposing a Video-class (UVC) interface —
     * matches by interface class, not vid/pid, so it works with any UVC
     * camera, not just a specific unit. Composite devices (camera+mic)
     * declare Video only on one interface, not the device descriptor
     * itself, so we must check interfaces individually. */
    private fun findUvcDevices(): List<UsbDevice> =
        usbManager.deviceList.values.filter { dev ->
            (0 until dev.interfaceCount).any { i ->
                dev.getInterface(i).interfaceClass == UsbConstants.USB_CLASS_VIDEO
            }
        }

    private fun requestCameraAccess() {
        val devices = findUvcDevices()
        when {
            devices.isEmpty() -> {
                statusText.value = "No UVC camera found (plug one in)"
                cameraChoices.value = emptyList()
            }
            devices.size == 1 -> {
                cameraChoices.value = emptyList()
                requestPermissionAndOpen(devices[0])
            }
            else -> {
                statusText.value = "Multiple cameras attached"
                cameraChoices.value = devices
            }
        }
    }

    private fun requestPermissionAndOpen(device: UsbDevice) {
        if (usbManager.hasPermission(device)) {
            openDevice(device)
            return
        }
        statusText.value = "Requesting permission..."
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            PendingIntent.FLAG_MUTABLE else 0
        // Explicit package (not just the action string) keeps this from being
        // an "implicit" intent — API 34+ rejects FLAG_MUTABLE on implicit
        // intents outright (IllegalArgumentException at getBroadcast time).
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        val pi = PendingIntent.getBroadcast(this, 0, intent, flags)
        usbManager.requestPermission(device, pi)
    }

    private fun openDevice(device: UsbDevice) {
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            statusText.value = "openDevice() failed"
            return
        }
        val fd = connection.fileDescriptor
        val ok = UvcNative.openDevice(fd)
        if (ok) {
            UvcNative.stopControl()
            val controlOk = UvcNative.startControl(CONTROL_PORT)
            controlRunning.value = controlOk
            statusText.value = if (controlOk)
                "Camera agent ready on ${localIpAddress()}:$CONTROL_PORT - choose mode on PC"
            else "Camera opened, but control agent failed - see log below"
        } else {
            statusText.value = "uvc_wrap failed - see log below"
        }
    }

    override fun onDestroy() {
        unregisterReceiver(usbReceiver)
        UvcNative.stopExperiment()
        UvcNative.stopControl()
        UvcNative.stopServer()
        UvcNative.closeDevice()
        super.onDestroy()
    }
}
