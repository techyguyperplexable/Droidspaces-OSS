/*
 * Droidspaces - Audio bridge foreground service.
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hosts a unix socket at <filesDir>/audio.sock which the in-container
 * Pulse gateway connects to.  Speaks the binary protocol defined in
 * src/audio_proto.h: 8-byte little-endian frame header followed by an
 * opaque payload.  Playback frames feed AudioTrack instances; capture
 * frames come from AudioRecord and are pushed back the same way.
 *
 * Filesystem (not abstract) socket is required so droidspaces can
 * bind-mount the socket into the container's /run/droidspaces dir.
 * LocalSocket.bind() + Os.listen() gets us that on API 26+ without
 * needing UnixSocketAddress (API 31+).
 */
package com.droidspaces.app.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.Build
import android.os.IBinder
import android.system.Os
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

class AudioBridgeService : Service() {

    /* Constants - keep in lockstep with src/audio_proto.h. */
    private object Proto {
        const val MAGIC: Int = 0x55415344.toInt()   // "DSAU" little-endian
        const val VERSION: Short = 1

        const val MSG_HELLO: Byte = 0x01
        const val MSG_HELLO_ACK: Byte = 0x02
        const val MSG_OPEN_PLAY: Byte = 0x10
        const val MSG_OPEN_REC: Byte = 0x11
        const val MSG_OPEN_OK: Byte = 0x12
        const val MSG_OPEN_FAIL: Byte = 0x13
        const val MSG_CLOSE: Byte = 0x20
        const val MSG_CLOSE_OK: Byte = 0x21
        const val MSG_PCM: Byte = 0x30
        const val MSG_XRUN: Byte = 0x40

        const val FMT_S16LE: Byte = 0
        const val FMT_S32LE: Byte = 1
        const val FMT_F32LE: Byte = 2
        const val FMT_U8: Byte = 3

        const val CAP_PLAYBACK: Short = 0x0001
        const val CAP_CAPTURE: Short = 0x0002
        const val CAP_MIXING: Short = 0x0004

        const val FRAME_HDR_SIZE: Int = 8
    }

    companion object {
        private const val TAG = "DSAudio"
        private const val SOCKET_NAME = "audio.sock"
        private const val NOTIF_CHANNEL = "droidspaces_audio"
        private const val NOTIF_ID = 0xa0d10
    }

    private val running = AtomicBoolean(false)
    private var listenerThread: Thread? = null
    private var serverSocket: LocalServerSocket? = null
    private var bindSocket: LocalSocket? = null
    private val clientThreads = mutableSetOf<Thread>()

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification("Audio bridge active"))
        startListener()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        running.set(false)
        try { serverSocket?.close() } catch (_: IOException) {}
        try { bindSocket?.close() } catch (_: IOException) {}
        listenerThread?.interrupt()
        synchronized(clientThreads) {
            clientThreads.forEach { it.interrupt() }
            clientThreads.clear()
        }
        super.onDestroy()
    }

    /* ----------------------------------------------------------------
     * Socket setup
     * ---------------------------------------------------------------- */

    private fun startListener() {
        val sockFile = File(filesDir, SOCKET_NAME)
        if (sockFile.exists()) sockFile.delete()

        running.set(true)
        listenerThread = thread(name = "ds-audio-listener", isDaemon = true) {
            try {
                /* Bind to filesystem path via the legacy LocalSocket API, then
                 * promote the fd to a listening server socket via Os.listen. */
                val s = LocalSocket(LocalSocket.SOCKET_STREAM)
                s.bind(LocalSocketAddress(sockFile.absolutePath,
                                          LocalSocketAddress.Namespace.FILESYSTEM))
                Os.listen(s.fileDescriptor, 8)
                /* World-RW so any UID inside the container's namespace can
                 * open it.  The bind-mount preserves the inode's mode. */
                sockFile.setReadable(true, false)
                sockFile.setWritable(true, false)
                val srv = LocalServerSocket(s.fileDescriptor)
                bindSocket = s
                serverSocket = srv
                Log.i(TAG, "listening on ${sockFile.absolutePath}")

                while (running.get()) {
                    val client = srv.accept() ?: continue
                    val t = thread(name = "ds-audio-client", isDaemon = true) {
                        try {
                            ClientSession(client).run()
                        } catch (e: Exception) {
                            Log.w(TAG, "client session ended: ${e.message}")
                        } finally {
                            try { client.close() } catch (_: IOException) {}
                            synchronized(clientThreads) {
                                clientThreads.remove(Thread.currentThread())
                            }
                        }
                    }
                    synchronized(clientThreads) { clientThreads.add(t) }
                }
            } catch (e: Exception) {
                if (running.get()) Log.e(TAG, "listener crashed", e)
            }
        }
    }

    /* ----------------------------------------------------------------
     * Per-connection session - one Pulse gateway per service typically,
     * but multiple sessions are supported.
     * ---------------------------------------------------------------- */

    private inner class ClientSession(private val sock: LocalSocket) {
        private val inp: InputStream = sock.inputStream
        private val out: OutputStream = sock.outputStream
        private val playStreams = ConcurrentHashMap<Byte, PlaybackStream>()
        private val recStreams = ConcurrentHashMap<Byte, RecordStream>()

        fun run() {
            val hdr = ByteArray(Proto.FRAME_HDR_SIZE)
            while (true) {
                if (!readFully(inp, hdr, hdr.size)) return
                val type = hdr[0]
                val streamId = hdr[1]
                val payloadLen = ByteBuffer.wrap(hdr, 4, 4)
                    .order(ByteOrder.LITTLE_ENDIAN).int
                if (payloadLen < 0 || payloadLen > 16 * 1024 * 1024) {
                    Log.w(TAG, "bad payload_len $payloadLen, dropping client")
                    return
                }
                val payload = ByteArray(payloadLen)
                if (payloadLen > 0 && !readFully(inp, payload, payloadLen)) return
                dispatch(type, streamId, payload)
            }
        }

        private fun dispatch(type: Byte, streamId: Byte, body: ByteArray) {
            when (type) {
                Proto.MSG_HELLO    -> handleHello()
                Proto.MSG_OPEN_PLAY -> handleOpenPlay(streamId, body)
                Proto.MSG_OPEN_REC  -> handleOpenRec(streamId, body)
                Proto.MSG_CLOSE    -> handleClose(streamId)
                Proto.MSG_PCM      -> handlePcm(streamId, body)
                else -> Log.d(TAG, "ignoring unknown msg type 0x${"%02x".format(type)}")
            }
        }

        private fun handleHello() {
            val ack = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            ack.putInt(Proto.MAGIC)
            ack.putShort(Proto.VERSION)
            val caps = (Proto.CAP_PLAYBACK.toInt() or Proto.CAP_CAPTURE.toInt()
                        or Proto.CAP_MIXING.toInt()).toShort()
            ack.putShort(caps)
            sendFrame(Proto.MSG_HELLO_ACK, 0, ack.array())
        }

        private fun handleOpenPlay(streamId: Byte, body: ByteArray) {
            if (body.size < 8) { sendOpenFail(streamId, 22 /* EINVAL */); return }
            val buf = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
            val rate = buf.int
            val channels = (buf.get().toInt() and 0xff)
            val format = buf.get()
            val bufferMs = (buf.short.toInt() and 0xffff)
            val stream = try {
                PlaybackStream(streamId, rate, channels, format, bufferMs)
            } catch (e: Exception) {
                Log.w(TAG, "open_play failed: ${e.message}")
                sendOpenFail(streamId, 5 /* EIO */); return
            }
            playStreams[streamId] = stream
            sendOpenOk(streamId, channels, format, rate, bufferMs)
        }

        private fun handleOpenRec(streamId: Byte, body: ByteArray) {
            if (body.size < 8) { sendOpenFail(streamId, 22); return }
            if (ContextCompat.checkSelfPermission(this@AudioBridgeService,
                    Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
                sendOpenFail(streamId, 13 /* EACCES */); return
            }
            val buf = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
            val rate = buf.int
            val channels = (buf.get().toInt() and 0xff)
            val format = buf.get()
            val bufferMs = (buf.short.toInt() and 0xffff)
            val stream = try {
                RecordStream(streamId, rate, channels, format, bufferMs) { pcm ->
                    sendFrame(Proto.MSG_PCM, streamId, pcm)
                }
            } catch (e: SecurityException) {
                sendOpenFail(streamId, 13); return
            } catch (e: Exception) {
                Log.w(TAG, "open_rec failed: ${e.message}")
                sendOpenFail(streamId, 5); return
            }
            recStreams[streamId] = stream
            stream.start()
            sendOpenOk(streamId, channels, format, rate, bufferMs)
        }

        private fun handleClose(streamId: Byte) {
            playStreams.remove(streamId)?.close()
            recStreams.remove(streamId)?.close()
            sendFrame(Proto.MSG_CLOSE_OK, streamId, ByteArray(0))
        }

        private fun handlePcm(streamId: Byte, body: ByteArray) {
            playStreams[streamId]?.write(body)
        }

        /* ------------------------------ frame I/O ------------------------------ */

        private fun sendFrame(type: Byte, streamId: Byte, body: ByteArray) {
            val hdr = ByteBuffer.allocate(Proto.FRAME_HDR_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN)
            hdr.put(type)
            hdr.put(streamId)
            hdr.putShort(0)
            hdr.putInt(body.size)
            synchronized(out) {
                out.write(hdr.array())
                if (body.isNotEmpty()) out.write(body)
                out.flush()
            }
        }

        private fun sendOpenOk(streamId: Byte, channels: Int, format: Byte,
                               rate: Int, bufferMs: Int) {
            val buf = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
            buf.put(streamId)
            buf.put(channels.toByte())
            buf.put(format)
            buf.put(0)              // reserved
            buf.putInt(rate)
            buf.putShort(bufferMs.toShort())
            buf.putShort(0)         // reserved2
            sendFrame(Proto.MSG_OPEN_OK, streamId, buf.array())
        }

        private fun sendOpenFail(streamId: Byte, errno: Int) {
            val buf = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            buf.putInt(errno)
            sendFrame(Proto.MSG_OPEN_FAIL, streamId, buf.array())
        }
    }

    /* ----------------------------------------------------------------
     * Playback - one AudioTrack per stream.  AudioFlinger handles the
     * mixing for us when multiple tracks are active.
     * ---------------------------------------------------------------- */

    private inner class PlaybackStream(
        val id: Byte, rate: Int, channels: Int, format: Byte, bufferMs: Int
    ) {
        private val track: AudioTrack
        private val frameBytes: Int

        init {
            val chMask = if (channels >= 2) AudioFormat.CHANNEL_OUT_STEREO
                         else AudioFormat.CHANNEL_OUT_MONO
            val enc = toAudioEncoding(format)
            val minBuf = AudioTrack.getMinBufferSize(rate, chMask, enc)
            val want = framesForMs(rate, channels, format, bufferMs.coerceAtLeast(20))
            val bufSize = maxOf(minBuf, want)
            frameBytes = bytesPerSample(format) * channels
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
            val fmt = AudioFormat.Builder()
                .setEncoding(enc)
                .setSampleRate(rate)
                .setChannelMask(chMask)
                .build()
            track = AudioTrack.Builder()
                .setAudioAttributes(attrs)
                .setAudioFormat(fmt)
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track.play()
        }

        fun write(pcm: ByteArray) {
            if (pcm.isEmpty()) return
            /* AudioTrack.write blocks when the device-side queue is full,
             * which provides natural backpressure to the gateway. */
            track.write(pcm, 0, pcm.size, AudioTrack.WRITE_BLOCKING)
        }

        fun close() {
            try { track.stop() } catch (_: IllegalStateException) {}
            track.release()
        }
    }

    /* ----------------------------------------------------------------
     * Capture - one AudioRecord per stream, reader thread pushes PCM
     * frames out via the supplied callback.
     * ---------------------------------------------------------------- */

    private inner class RecordStream(
        val id: Byte, rate: Int, channels: Int, format: Byte, bufferMs: Int,
        private val onPcm: (ByteArray) -> Unit
    ) {
        private val record: AudioRecord
        private val frameBytes: Int = bytesPerSample(format) * channels
        private val running = AtomicBoolean(false)
        private var reader: Thread? = null

        init {
            val chMask = if (channels >= 2) AudioFormat.CHANNEL_IN_STEREO
                         else AudioFormat.CHANNEL_IN_MONO
            val enc = toAudioEncoding(format)
            val minBuf = AudioRecord.getMinBufferSize(rate, chMask, enc)
            val want = framesForMs(rate, channels, format, bufferMs.coerceAtLeast(20))
            val bufSize = maxOf(minBuf, want)
            record = AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                                 rate, chMask, enc, bufSize)
            if (record.state != AudioRecord.STATE_INITIALIZED) {
                throw IOException("AudioRecord init failed (state=${record.state})")
            }
        }

        fun start() {
            running.set(true)
            record.startRecording()
            reader = thread(name = "ds-audio-rec-$id", isDaemon = true) {
                val chunk = ByteArray(frameBytes * 1024)
                while (running.get()) {
                    val n = record.read(chunk, 0, chunk.size)
                    if (n <= 0) {
                        if (n == AudioRecord.ERROR_INVALID_OPERATION ||
                            n == AudioRecord.ERROR_DEAD_OBJECT) break
                        continue
                    }
                    val copy = chunk.copyOf(n)
                    try { onPcm(copy) } catch (e: IOException) { break }
                }
            }
        }

        fun close() {
            running.set(false)
            try { record.stop() } catch (_: IllegalStateException) {}
            record.release()
            reader?.interrupt()
        }
    }

    /* ----------------------------------------------------------------
     * Helpers
     * ---------------------------------------------------------------- */

    private fun toAudioEncoding(format: Byte): Int = when (format) {
        Proto.FMT_U8     -> AudioFormat.ENCODING_PCM_8BIT
        Proto.FMT_S16LE  -> AudioFormat.ENCODING_PCM_16BIT
        Proto.FMT_F32LE  -> AudioFormat.ENCODING_PCM_FLOAT
        Proto.FMT_S32LE  -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                                AudioFormat.ENCODING_PCM_32BIT
                            else AudioFormat.ENCODING_PCM_16BIT
        else -> AudioFormat.ENCODING_PCM_16BIT
    }

    private fun bytesPerSample(format: Byte): Int = when (format) {
        Proto.FMT_U8    -> 1
        Proto.FMT_S16LE -> 2
        Proto.FMT_F32LE -> 4
        Proto.FMT_S32LE -> 4
        else            -> 2
    }

    private fun framesForMs(rate: Int, channels: Int, format: Byte, ms: Int): Int =
        rate * channels * bytesPerSample(format) * ms / 1000

    private fun readFully(s: InputStream, buf: ByteArray, n: Int): Boolean {
        var got = 0
        while (got < n) {
            val r = s.read(buf, got, n - got)
            if (r < 0) return false
            got += r
        }
        return true
    }

    /* ----------------------------------------------------------------
     * Notification
     * ---------------------------------------------------------------- */

    private fun createNotificationChannel() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(NOTIF_CHANNEL) != null) return
        val ch = NotificationChannel(NOTIF_CHANNEL, "Droidspaces audio bridge",
                                     NotificationManager.IMPORTANCE_LOW)
        ch.description = "Routes container audio to/from Android"
        ch.setShowBadge(false)
        nm.createNotificationChannel(ch)
    }

    private fun buildNotification(text: String): Notification {
        return NotificationCompat.Builder(this, NOTIF_CHANNEL)
            .setContentTitle("Droidspaces audio")
            .setContentText(text)
            .setSmallIcon(applicationInfo.icon)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }
}
