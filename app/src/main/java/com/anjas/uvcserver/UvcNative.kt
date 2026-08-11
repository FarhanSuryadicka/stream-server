package com.anjas.uvcserver

object UvcNative {
    init {
        System.loadLibrary("uvcserver")
    }

    external fun openDevice(fd: Int): Boolean
    external fun closeDevice()
    external fun startServer(port: Int, capW: Int, capH: Int, capFps: Int): Boolean
    external fun stopServer()
    external fun getLogText(): String

    // --- Protocol experiment harness (experiment/docs/) ---
    //
    // The control channel carries PC -> phone commands AND answers the PING
    // the PC uses to estimate the clock offset. The receiver refuses to run
    // without it, since every one-way latency figure depends on that offset.
    external fun startControl(port: Int): Boolean
    external fun stopControl()
    external fun getControlState(): String

    // Stamped into every frame so the receiver can discard frames from a
    // stale run instead of folding them into the current run's statistics.
    external fun setRunId(runId: String)

    /**
     * Starts a measurement run: UVC capture -> JPEG decode -> H.264 encode
     * -> the selected transport.
     *
     * [protocol] is a UCV_PROTO_* id from ucv_wire.h. Protocols without an
     * implementation fail here rather than falling back to another
     * transport, so a run can never be mislabelled.
     *
     * [peerCfg] is transport specific — "ip:port" for Raw UDP/RTP, a port
     * for MJPEG.
     *
     * Fails if a hardware H.264 encoder cannot be confirmed: a software
     * encoder changes the encode cost inside every latency number.
     */
    external fun startExperiment(
        protocol: Int,
        peerCfg: String,
        capW: Int,
        capH: Int,
        capFps: Int
    ): Boolean

    external fun stopExperiment()
    external fun isExperimentRunning(): Boolean
    external fun getExperimentState(): String

    // Protocol ids — must match ucv_wire.h.
    const val PROTO_WEBRTC = 1
    const val PROTO_SRT = 2
    const val PROTO_RTSP = 3
    const val PROTO_RAWUDP = 4
    const val PROTO_MJPEG = 5
    const val PROTO_RTMPS = 6
    const val PROTO_HLS = 7

    // RTSP with real signalling (OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN). It puts
    // the same RTP packets on the wire as PROTO_RTSP, which is the bare RTP data
    // plane started out-of-band — the two are separate ids so a run records
    // which one actually carried it.
    const val PROTO_RTSP_SIGNALLED = 8
}
