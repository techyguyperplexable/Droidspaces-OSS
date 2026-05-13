/*
 * Droidspaces - Audio bridge controller helper.
 * Copyright (C) 2026 Josh Law <joshlaw48@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin wrapper around AudioBridgeService start/stop and the RECORD_AUDIO
 * permission grant flow.  The Compose UI layer doesn't need to know about
 * Service or Manifest plumbing - it just calls into here.
 */
package com.droidspaces.app.util

import android.Manifest
import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.droidspaces.app.service.AudioBridgeService
import java.io.File

object AudioBridgeController {

    /** Filesystem path where the service hosts the unix socket. Matches the
     *  bind-mount source path baked into the native droidspaces binary. */
    fun socketPath(ctx: Context): File = File(ctx.filesDir, "audio.sock")

    fun start(ctx: Context) {
        val intent = Intent(ctx, AudioBridgeService::class.java)
        ContextCompat.startForegroundService(ctx, intent)
    }

    fun stop(ctx: Context) {
        ctx.stopService(Intent(ctx, AudioBridgeService::class.java))
    }

    fun isRunning(ctx: Context): Boolean {
        /* ActivityManager.getRunningServices is deprecated for cross-app
         * lookups but still works for our own services.  Good enough for a
         * "service alive?" toggle indicator. */
        @Suppress("DEPRECATION")
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        @Suppress("DEPRECATION")
        return am.getRunningServices(Int.MAX_VALUE).any {
            it.service.className == AudioBridgeService::class.java.name
        }
    }

    fun hasRecordPermission(ctx: Context): Boolean =
        ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    /** Register a launcher in a ComponentActivity's onCreate that handles the
     *  RECORD_AUDIO grant flow.  Call .launch() from a button handler when
     *  the user opts into capture. */
    fun registerRecordPermissionLauncher(
        activity: ComponentActivity,
        onResult: (Boolean) -> Unit
    ): ActivityResultLauncher<String> =
        activity.registerForActivityResult(
            ActivityResultContracts.RequestPermission(), onResult
        )
}
