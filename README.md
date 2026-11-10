# mod_audiosocket

A FreeSWITCH module that sends a call's audio to a TCP server and plays the server's audio back to the caller, at the same time. It uses the simple AudioSocket framing protocol (described in section 4), so a bot or any other TCP service can listen to the caller and talk back.

The module runs inside the call's own FreeSWITCH thread and does all its socket work without blocking, so it adds no threads of its own.

What it does:

- Adds a dialplan app: `audiosocket <host:port> <uuid> [rate]`
- Supports all the AudioSocket sample rates (8 kHz up to 192 kHz) and resamples in both directions
- Sends the caller's key presses to the server as DTMF frames
- Plays out whatever audio the server already sent when it hangs up, instead of cutting it off
- Gives up on a server that is connected but has stopped reading
- Optional config file, per-call channel variables, and an optional metadata frame
- `audiosocket_stats` in `fs_cli` for a quick look at what is running

Licence: MPL-2.0. Copyright (c) 2026 Uday Kumar.

## How well is it tested?

I built and tested it on FreeSWITCH 1.10.12 (compiled from source, Debian 13). I used loopback calls and real softphone calls (PCMU at 8 kHz and Opus at 48 kHz), and ran it under AddressSanitizer and UBSan against a deliberately badly behaved server. On a small 2-vCPU test machine it used about 1.8 ms of CPU and 0.5 MB of memory per stream.

What I have not tried: other FreeSWITCH versions or Linux distros, Docker, third-party AudioSocket clients and servers (I only tested against my own test servers), and thousands of real RTP calls. Please test on your own setup before putting live traffic on it.

## 1. Install

You need a C compiler, `make`, `pkg-config`, and the FreeSWITCH development files for the same FreeSWITCH version you run (the headers and `freeswitch.pc`). If you built FreeSWITCH from source, you already have them. Nothing else is needed.

```bash
git clone https://github.com/udy786/mod_audiosocket.git
cd mod_audiosocket

# only if pkg-config can't find freeswitch.pc:
export PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig

make
sudo make install
```

`make install` copies the module into the directory `pkg-config` reports (`/usr/local/freeswitch/lib/freeswitch/mod` for a source build, usually `/usr/lib/freeswitch/mod` for packages). Use `FS_MODDIR=/path` to choose another. It also puts a default `audiosocket.conf.xml` into FreeSWITCH's `autoload_configs`, but never overwrites one you already have. `FORCE_CONFIG=1` replaces it and keeps the old file as `.bak`. `FS_CONFDIR=/path` and `DESTDIR=/stage` also work.

To load it every time FreeSWITCH starts, add this line to `autoload_configs/modules.conf.xml`:

```xml
<load module="mod_audiosocket"/>
```

To load it right now: `fs_cli -x "load mod_audiosocket"`. Then `fs_cli -x "module_exists mod_audiosocket"` should print `true`.

For Docker, build the module against the same FreeSWITCH version as your runtime image and copy the `.so` into its module directory. I haven't tried this. Raise the `nofile` limit, because every stream is one TCP socket.

## 2. Dialplan

Dialplan files go in FreeSWITCH's conf directory, for example `/usr/local/freeswitch/etc/freeswitch/dialplan/default/` (or `/etc/freeswitch/dialplan/default/` for packages). Save this as `bot.xml` there and run `fs_cli -x "reloadxml"`:

```xml
<include>
  <extension name="bot">
    <condition field="destination_number" expression="^bot$">
      <action application="answer"/>
      <action application="audiosocket" data="10.0.0.20:9000 ${uuid}"/>
      <action application="hangup"/>
    </condition>
  </extension>
</include>
```

A few things to know:

- Run `audiosocket` after `answer`. If the call isn't answered yet, attaching to its audio answers it for you.
- The app blocks until the call ends or the server hangs up or disconnects. Anything that must happen first (writing to Redis, starting a recording) goes before it, and anything for afterwards goes after it.
- The second argument is the UUID sent to the server. Use `${uuid}` so your bot can refer to the call by it later. An optional third argument sets the audio rate, for example `10.0.0.20:9000 ${uuid} 16000`.
- Keep the `host:port` fixed in the dialplan (see Security).

A fuller example with recording, metadata for the bot, and a fallback when the bot can't be reached:

```xml
<extension name="bot_call">
  <condition field="destination_number" expression="^bot_call$">
    <action application="answer"/>
    <!-- record to a local WAV; upload and clean up with a separate process -->
    <action application="set" data="RECORD_STEREO=true"/>
    <action application="record_session" data="/var/lib/freeswitch/recordings/${strftime(%Y%m%d)}/${uuid}.wav"/>
    <!-- 16 kHz to the bot, plus these two variables sent once as metadata -->
    <action application="set" data="audiosocket_send_vars=customer_phone,customer_id"/>
    <action application="audiosocket" data="10.0.0.20:9000 ${uuid} 16000"/>
    <!-- if we get here because the bot was unreachable, do something else -->
    <action application="execute_extension" data="bot_failed XML default"/>
    <action application="hangup"/>
  </condition>
</extension>

<extension name="bot_failed">
  <condition field="${audiosocket_status}" expression="^(connect_failed|error|server_stalled)$">
    <action application="playback" data="ivr/ivr-call_cannot_be_completed_as_dialed.wav"/>
  </condition>
</extension>
```

Redis, recording and agent transfer are normal FreeSWITCH features and the module doesn't do them. To move a call to an agent you can run `uuid_transfer <uuid> agent_transfer XML default` over the event socket. I haven't checked how the stream behaves when a call is transferred.

### Trying it

Save the `bot` extension above, run `reloadxml`, start any AudioSocket server on the address you used (your bot, or a small test script), and place a loopback call that plays a tone into it:

```bash
fs_cli -x "originate loopback/bot/default &playback(tone_stream://%(5000,0,440))"
```

The log shows `audiosocket: connected to ...`, and at the end `audiosocket: finished, status ...` with the frame counts. If nothing is listening you get `connect_failed`. One tip: don't put two hyphens in a row inside an XML comment, because FreeSWITCH will refuse to load the file.

## 3. Configuration

For each call, a setting comes from the first of these that is set: the app's rate argument, a channel variable set before the app runs, the config file `autoload_configs/audiosocket.conf.xml`, and finally the built-in default.

| File setting | Channel variable | Allowed values | Default |
| --- | --- | --- | --- |
| `default-rate` | `audiosocket_rate` | 8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000 | 8000 |
| `connect-timeout-ms` | `audiosocket_connect_timeout_ms` | 1 to 60000 | 2000 |
| `jitter-buffer-ms` | `audiosocket_jitter_ms` | 20 to 10000 | 200 |
| `max-queue-ms` | `audiosocket_max_queue_ms` | 100 to 60000 | 10000 |
| `send-queue-ms` | (file only) | 100 to 10000 | 500 |
| `stall-timeout-ms` | `audiosocket_stall_timeout_ms` | 0 to 600000 (0 means never) | 10000 |
| `dtmf` | `audiosocket_dtmf` | true or false (also yes, no, on, off, 1, 0) | true |
| `send-vars` | `audiosocket_send_vars` | channel variable names separated by commas; `none` turns it off for one call | none |

What they mean:

- `jitter-buffer-ms`: how much of the server's audio is collected before playback starts, and again after it runs dry.
- `max-queue-ms`: the most server audio kept waiting to play. It is large on purpose, since a bot may send speech faster than real time. Only the excess is dropped, oldest first.
- `send-queue-ms`: how much audio can wait for a slow server before frames start being dropped.
- `stall-timeout-ms`: if the server stays connected but accepts no data for this long, the module gives up on it and the call moves on with status `server_stalled`.

`examples/audiosocket.conf.xml` is a commented template with every setting. A minimal config file looks like this:

```xml
<configuration name="audiosocket.conf" description="mod_audiosocket">
  <settings>
    <param name="default-rate" value="8000"/>
    <param name="jitter-buffer-ms" value="200"/>
    <param name="stall-timeout-ms" value="10000"/>
  </settings>
</configuration>
```

FreeSWITCH keeps its XML in memory, so after editing the file run `fs_cli -x "reloadxml"` and then `fs_cli -x "audiosocket_reload"`. The second command prints the settings now in force. Calls that are already running keep the settings they started with. An unknown setting, or a value outside its range, is logged as a warning and the default is kept. Channel variables are held to the same limits.

### What the dialplan can read afterwards

When the app finishes it sets these channel variables:

| Variable | Meaning |
| --- | --- |
| `audiosocket_status` | `ok` (the call ended), `server_hangup`, `server_closed`, `server_stalled`, `connect_failed`, `bad_args` or `error` |
| `audiosocket_frames_sent` | audio frames sent to the server |
| `audiosocket_frames_dropped` | frames dropped because the server was too slow |
| `audiosocket_frames_received` | audio frames received from the server |
| `audiosocket_underruns` | how many times playback ran out of audio |
| `audiosocket_dtmf_sent` | key presses sent to the server |

### Watching it run

- `fs_cli -x "audiosocket_stats"` shows active streams, calls started, frames and bytes, drops, underruns, key presses, and how calls ended.
- `fs_cli -x "audiosocket_stats list"` gives one line per active stream: uuid, server, rate, seconds, frames sent, dropped and received, underruns, audio waiting to play, and bytes queued for the socket. If `out_bytes` keeps growing or `dropped` is above zero, the server isn't keeping up.

## 4. The protocol

Plain TCP. Every message is a frame: one byte for the kind, two bytes for the payload length (big endian), then the payload.

| Kind | Direction | Payload |
| --- | --- | --- |
| `0x01` UUID | module to server, always first | 16 bytes |
| `0x10` to `0x18` audio | both ways | signed 16-bit little-endian mono PCM at 8, 12, 16, 24, 32, 44.1, 48, 96 or 192 kHz |
| `0x03` DTMF | module to server | one ASCII byte (0-9, `*`, `#`, A-D) |
| `0x00` hangup | both ways | none. From the server it ends the call; from the module it says the call has ended |
| `0xFF` error | server to module | one byte; it is logged and otherwise ignored |
| `0x40` metadata | module to server, right after the UUID | a JSON object with the variables named in `send-vars` |

`0x40` is an optional extension to the base protocol. It is only sent if you turn on `send-vars`, and a server that doesn't know the kind should just ignore it.

Audio that the server sends faster than real time is queued and played in order. There is no way to cancel queued audio (no barge-in), so a bot that wants to interrupt itself should send its audio at real-time speed.

## 5. Known limits

- Playing audio back to the caller needs a mono call. On a stereo call the server's audio is ignored (the caller's audio is still sent, mixed down to mono).
- The module makes FreeSWITCH decode and re-encode the call's audio, which costs CPU for compressed codecs like Opus or G.729.
- `uuid_break` does not end the application, and error frames from the server are only logged.
- FreeSWITCH limits how fast it takes new calls. For load, raise `max-sessions` and `sessions-per-second` in `switch.conf.xml` (the defaults are 1000 and 30).
- I haven't measured capacity with real RTP calls.
- If you build FreeSWITCH 1.10.12 itself on Debian 13, you need PCRE 8.45 from source and `-Wno-error` with gcc 14. That is a FreeSWITCH issue, not this module's.

## 6. Security

- The connection to the bot is plain TCP, with no encryption or login. It carries your customers' voices, their key presses (which can include PINs and card digits) and, if you enable it, metadata. Keep it on a private network, or use a tunnel such as WireGuard, a VPN or stunnel.
- Keep `host:port` fixed in the dialplan. If it is built from something a caller or another system controls, a call could be made to connect to any address the FreeSWITCH machine can reach.
- Use an IP address for the bot. A host name goes through the system resolver, which can block that call's thread while it waits.
- Keep `audiosocket_send_vars` fixed too, and never list variables that hold secrets. Clean up anything that comes from callers before using it in dialplan commands such as Redis writes.
- Memory per call is limited by your settings: roughly the call's rate times `max-queue-ms` times 2 for playback, plus `send-queue-ms` of audio waiting to be sent.

## 7. Troubleshooting

| What you see | Likely cause |
| --- | --- |
| Module won't load | it was built against a different FreeSWITCH version or libc than the one running |
| `connect_failed` | wrong host or port, a firewall, or container networking |
| No audio from the server | the server isn't sending audio frames, or not after the UUID frame. Check `audiosocket_frames_received` |
| `underruns` keep growing | the server sends slower than real time, or the machine is overloaded |
| `dropped` frames | the server isn't reading its socket fast enough |
| Config change does nothing | run `reloadxml` first, then `audiosocket_reload`. Running calls keep their old settings |
| No metadata at the server | `audiosocket_send_vars` was set after the `audiosocket` line, or every name in it was invalid (look for warnings in the log) |
| Many calls fail at once | the open-files (`nofile`) limit is too low, or `max-sessions` / `sessions-per-second` |
| FreeSWITCH won't start after a dialplan edit | invalid XML, often `--` inside a comment |

## 8. Layout

- `src/`: the module (`mod_audiosocket.c`) and the parts that don't depend on FreeSWITCH (protocol, buffers, networking, config, JSON, utilities)
- `examples/`: the settings file template that `make install` copies into FreeSWITCH
- `Makefile`, `LICENSE`

## Licence

Mozilla Public License 2.0, see `LICENSE`. I wrote it from scratch; no code was copied from other projects.
