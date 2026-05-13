# Droidspaces audio bridge

`--audio` wires the container's PulseAudio clients into the Android app's
foreground `AudioBridgeService`, so apps inside the container can play sound
out the phone's speakers and capture from the mic — without Termux, without a
network bridge, without any setup inside the container.

Android-only. On Linux desktop the flag is a no-op (with a warning).

## Quick start

```sh
# Start a container with audio enabled
droidspaces --rootfs=/path/to/rootfs --name=mycontainer --audio start

# Inside the container - apps that link libpulse just work:
paplay /usr/share/sounds/alsa/Front_Center.wav
parec --format=s16le --rate=48000 --channels=1 > /tmp/mic.raw
```

The `PULSE_SERVER` env var is set automatically to `unix:/run/pulse/native`,
so anything that reads it (most desktop audio apps) finds the gateway with
no extra config.

## How it works

```
┌─────────────────────── Android app process ─────────────────────────┐
│  AudioBridgeService (foreground)                                    │
│    AudioTrack ◀── PCM ◀── unix-socket protocol ◀── /files/audio.sock│
│    AudioRecord ──▶ PCM ──▶ unix-socket protocol ──▶ /files/audio.sock│
└───────────────────────────────────┬─────────────────────────────────┘
                                    │ bind-mounted by droidspaces
                                    ▼
┌──────────────── Container namespace ────────────────────────────────┐
│  /run/droidspaces/audio.host.sock                                   │
│       ▲                                                             │
│       │ custom binary protocol (audio_proto.h)                      │
│       │                                                             │
│  droidspaces __pulse-gateway  (forked at boot)                      │
│       │                                                             │
│       │ PulseAudio native protocol                                  │
│       ▼                                                             │
│  /run/pulse/native  ◀── libpulse clients (paplay, parec, ffmpeg…)   │
└─────────────────────────────────────────────────────────────────────┘
```

Two binaries, one wire on each side:

- **AudioBridgeService** (Kotlin foreground service): owns the AudioTrack
  and AudioRecord, holds `RECORD_AUDIO`, hosts the host-side unix socket
  at `<app-files-dir>/audio.sock`.

- **`droidspaces __pulse-gateway`** (in-container daemon): listens on
  `/run/pulse/native` where libpulse expects to find a Pulse server.
  Translates the subset of the Pulse native protocol that
  `libpulse-simple` clients need into the small framed PCM protocol the
  service speaks.

`droidspaces` bind-mounts the host socket into the container at
`/run/droidspaces/audio.host.sock` and spawns the gateway as a child
process during boot.

## Microphone permission

Capture (`parec`, etc.) needs the `RECORD_AUDIO` permission granted to the
Droidspaces app. Output works without it.

To grant it:

1. Open the Droidspaces app.
2. Tap the audio settings entry (or any control that calls
   `AudioBridgeController.registerRecordPermissionLauncher(…).launch(
   Manifest.permission.RECORD_AUDIO)`).
3. Accept the system permission dialog.

If the permission isn't granted, `parec` and friends will see "stream
open failed" — the protocol returns `OPEN_FAIL` with `EACCES`. Playback
continues to work either way.

## Limitations and known sharp edges

- The Pulse-protocol gateway implements only what `libpulse-simple` needs
  to open a stream: `AUTH`, `SET_CLIENT_NAME`, `CREATE_PLAYBACK_STREAM`,
  `CREATE_RECORD_STREAM`, `DRAIN`, `DELETE_*`, and inline PCM data. Apps
  that introspect sinks/sources, subscribe to events, or use the
  extension protocol will see `ERROR_NOT_SUPPORTED` and may log errors —
  but the audio hot path still works.
- The `pavucontrol` / `pactl info` style introspection tools will *not*
  see useful output.
- Sample-rate conversion happens in `AudioTrack`/`AudioRecord` — pick a
  rate the device supports natively (usually 48 kHz) to avoid SRC
  artefacts.
- Each playback stream gets its own `AudioTrack`; AudioFlinger mixes
  them. There is no software mixer in the gateway itself.
- One service instance per device. Multiple containers with `--audio`
  share the same AudioBridgeService — all their playback streams mix
  together at the system level.
- Latency: roughly one buffer (≈ 20 ms) of unix-socket + one
  `AudioTrack` buffer of OS-side latency. Fine for media playback and
  voice; not real-time-audio tight.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `[AUDIO] host socket … missing` on container start | The Droidspaces app isn't running, or `AudioBridgeService` hasn't been started (call `AudioBridgeController.start(context)` from the app). |
| Playback silent, no errors | `AudioTrack` may have been built for an unsupported rate; try `paplay --rate=48000`. |
| `parec` fails with permission error | Grant `RECORD_AUDIO` to the Droidspaces app. |
| `pactl info` returns "Connection refused" | Gateway didn't start — check `dmesg` inside the container for `pulse_gateway` failures, and confirm `/run/droidspaces/audio.host.sock` is a live socket. |
| Service killed on Android 14+ | `foregroundServiceType="microphone"` and the matching `FOREGROUND_SERVICE_MICROPHONE` permission must both be present in the manifest; rebuild the app if you changed either. |

## Config persistence

`--audio` is stored in the container's config file as `audio=1`. It will
re-enable automatically on the next `start` / `restart`. To turn it off
permanently, pass `--reset` (which clears all flags) or hand-edit the
config.

