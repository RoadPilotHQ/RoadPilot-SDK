# RoadPilot Input pipe protocol, versions 1 and 2

Transport: Windows named pipe `\\.\pipe\roadpilot-input-v1`, created by the plugin
(inbound only, message mode, one client at a time, remote clients rejected). The
client opens it for writing and sends **one message per write**. The plugin never
writes back.

## Message (16 bytes, little endian)

| Offset | Size | Field      | Meaning                                         |
|-------:|-----:|------------|-------------------------------------------------|
| 0      | 4    | magic      | bytes `R` `P` `I` `N`                           |
| 4      | 2    | version    | `1`                                             |
| 6      | 1    | `enabled`  | `0` = disarm, `1` = armed                       |
| 7      | 1    | reserved   | must be `0`                                     |
| 8      | 4    | `throttle` | IEEE-754 float32                                |
| 12     | 4    | `brake`    | IEEE-754 float32                                |

## Rules

- Valid and `enabled = 1`: throttle and brake are clamped to `0.0..1.0` and applied.
- Throttle and brake both `> 0` after clamping: throttle becomes `0`, brake is kept.
- `enabled = 0`: explicit stop/disarm; the values are ignored and the output is
  throttle 0 / brake 0 / disabled.
- Invalid message (wrong size, magic, version, `enabled` not 0/1, reserved not 0,
  NaN or infinity): rejected, output falls back to the safe state and stays disarmed.
  The next valid `enabled = 1` message re-arms.
- No valid message for more than 250 ms (stale input): output is safe. The client
  must resend at least every ~100 ms while it wants a value held.
- Client disconnect or pipe error: safe state, stays disarmed until a new client
  sends a valid enabled message.
- Game device deactivation and plugin shutdown also disarm.

Safe state = `enabled=false, throttle=0, brake=0`. This is also the state at plugin
start. The plugin reports both axes to the game every frame (zeros included).

Changes that break this layout must increase `version`; the plugin rejects versions
it does not know. Version 2 (below) is an additive extension.

## Version 2: steering (additive)

Version 2 extends version 1 with a steering command. **Version 1 stays valid and unchanged**:
the plugin accepts both, selects the layout by `version`, and a version 1 message can never
activate steering. A message whose size does not match its version (for example version 1
with 20 bytes) is invalid.

### Message (20 bytes, little endian)

| Offset | Size | Field      | Meaning                                               |
|-------:|-----:|------------|-------------------------------------------------------|
| 0      | 4    | magic      | bytes `R` `P` `I` `N`                                 |
| 4      | 2    | version    | `2`                                                   |
| 6      | 1    | `enabled`  | `0` = disarm everything, `1` = armed                  |
| 7      | 1    | `flags`    | bit 0 = `steering_enabled`; all other bits must be 0  |
| 8      | 4    | `throttle` | IEEE-754 float32                                      |
| 12     | 4    | `brake`    | IEEE-754 float32                                      |
| 16     | 4    | `steering` | IEEE-754 float32                                      |

The first 16 bytes keep the version 1 meaning (byte 7 was reserved and is now `flags`).

### Steering rules

- **Both flags are required.** Steering is applied only when `enabled = 1` **and**
  `steering_enabled = 1`. With either one off, steering is 0 and not active. Throttle and
  brake behave as in version 1.
- **Disarmed by default.** The plugin starts with steering off, never arms itself, and a
  reconnect, disarm, invalid message, stale input or device deactivation always requires a
  new message with both flags to arm again.
- **Range:** `steering` is clamped to `-1.0..1.0`. NaN or infinity makes the whole message
  invalid (see below). Neutral is `0`. The meaning of the sign is the game's binding
  convention and is **not verified in game yet**.
- **Rate limit:** the magnitude of the steering value handed to the game can increase by at
  most **2.0 units per second** (full lock in 0.5 s), measured between consecutive game
  frames. Reductions toward 0 are immediate. A direction reversal first returns to 0 and then
  ramps up in the new direction. The first output after arming, and the first output after a
  gap of more than **100 ms** between game frames (stalled game, loading screen) or after the
  clock went backwards, is 0, so a long polling interval never turns into one large step.
- **Steering timeout: 150 ms.** If no valid message arrived for more than 150 ms, steering is
  released to 0 at once (no ramp) and must be armed again by a new message. Throttle and brake
  keep their separate 250 ms timeout.
- **Safe state** (invalid message, disconnect, explicit disarm, device deactivation, plugin
  shutdown): `enabled = false`, throttle 0, brake 0, steering 0, steering not active.
- **Neutral values are always reported.** The plugin reports all three axes every frame,
  including 0, because the Input SDK needs a reported value to release an axis. The plugin
  never stays silent on an axis to "hold" it.

Invalid in version 2 in addition to the version 1 rules: unknown flag bits, a size that does
not match the version, non-finite `steering`. In version 1, byte 7 must still be 0.

### Client requirements (heartbeat)

The plugin cannot detect a stalled client except by the timeout, so the client must:

- **Resend the latest valid command at least every 50 ms** (recommended 20-33 ms) as long as it
  wants a value held. That leaves room for three lost or late messages inside the 150 ms
  steering timeout. A message may only repeat a command the client itself considers valid and
  armed; after the user disarms, or on any internal error, the client sends `enabled = 0` once
  and then stops.
- **Run the heartbeat independently of other processing.** The application's telemetry engine
  samples at 10 Hz (100 ms) and the UI follows it; that rate must **not** become the heartbeat
  rate, and a 10 Hz sender would trip the steering timeout. A sender needs its own timer, for
  example a dedicated thread or task with a monotonic interval that resends the newest command
  and never queues stale ones, and with timer resolution suitable for 20-33 ms intervals on
  Windows. It must not be blocked by rendering, disk or network work.
- **Never arm automatically.** `steering_enabled = 1` must follow an explicit user action.
- **Disarm on its own failures:** loss of telemetry, game paused, controller error or exit. The
  plugin only knows about the pipe, the game's device activation and the timeouts; it does not
  know the game is paused.

At the time of writing no RoadPilot component implements this sender; this document is the
requirement for it.

### Manual control and emergency override

The plugin cannot read the player's own inputs and cannot tell whether the player is steering.
Manual control is **not guaranteed** while the RoadPilot steering axis is bound: how the game
combines several devices bound to the same function (sum, maximum, last writer) is
**unverified**, and a bound axis that reports 0 may still compete with manual steering. Ways to
take control that do not depend on that interaction: disarm in the client (kill switch),
unbind the RoadPilot axis, close the pipe client, remove the plugin. Treat any in-game
behaviour as unverified until it has been tested.
