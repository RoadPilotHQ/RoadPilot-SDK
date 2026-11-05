# RoadPilot Input pipe protocol, version 1

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
it does not know.
