# RoadPilot-SDK

Prototype of a native SCS **Input API** plugin for American Truck Simulator and
Euro Truck Simulator 2 (Windows x64). It registers one generic input device,
"RoadPilot Input", with three float axes, **Throttle**, **Brake** (service brake) and **Steering**,
and fills them from a local named pipe so a later RoadPilot process can drive them. It is the Input SDK counterpart of [RoadPilot-Telemetry](https://github.com/RoadPilotHQ/RoadPilot-Telemetry), which is read-only and a separate plugin; the two share no code or pipe.

Scope of this prototype: throttle, service brake and steering (protocol v2; v1 clients keep
working with throttle and brake only). There is no driving logic in this repository. Nothing is written to
`controls.sii`, profiles, saves or game files.

## Status

Phase 4.3 (steering input support) is completed in this repository: protocol v2, the steering axis (index 2) and tests. **There is currently no RoadPilot input sender and no steering activation UI**, and no driving logic. The 50 ms command refresh interval is a requirement for that future sender, not an existing, verified capability. Nothing here has been tested in ATS or ETS2 (see [Test status](#test-status)); the fake-host tests do not prove in-game control.

| Item | Value |
|---|---|
| Protocol | v1 (16 bytes, throttle and brake) and v2 (20 bytes, adds steering); v1 clients stay supported and cannot steer |
| Steering activation | needs `enabled` and `steering_enabled`; disarmed by default, never armed automatically |
| Steering range, rate limit | -1.0 to 1.0, magnitude increase at most 2.0 units per second, direction change goes through zero |
| Timeouts | steering 150 ms, throttle and brake 250 ms; stale input releases to neutral |
| Client refresh | at least every 50 ms (future sender requirement) |

All values and rules are specified in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Layout

- `src/input_protocol.*`: wire format parser/encoder (platform independent)
- `src/input_state.*`: arm/disarm, clamping, brake priority, steering gates, rate limit and stale timeouts (platform independent)
- `src/pipe_server.*`: Windows named pipe transport (no SDK, no meaning of messages)
- `src/plugin.cpp`: SCS SDK callbacks only
- `docs/PROTOCOL.md`: message formats (v1 and v2), steering rules, timeouts, client heartbeat requirements and fail-safe rules (the authoritative specification)
- `tests/`: `core_tests` (no game, no SDK) and `plugin_host_tests` (loads the DLL into a fake host)

## Build

Requirements: CMake 3.24+, Visual Studio (MSVC, x64), and the official
**SCS SDK 1.16** extracted somewhere. The SDK headers are not part of this repository.

```
cmake -S . -B build -A x64 -DSCS_SDK_ROOT=C:\path\to\scs_sdk_1_16
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`SCS_SDK_ROOT` may also be an environment variable. It must contain `include/scssdk_input.h`
and the ETS2/ATS input headers; otherwise configuration stops with a list of the missing
files. To build and test only the SDK-independent logic, add `-DROADPILOT_BUILD_PLUGIN=OFF`.

Output: `build\Release\roadpilot-input.dll`.

## Install and activate

1. Copy `roadpilot-input.dll` to `<game install>\bin\win_x64\plugins\` (create `plugins`
   if it does not exist) for ATS and/or ETS2. Alternatively register the DLL path as a
   string value under `HKLM\SOFTWARE\SCS Software\<Euro Truck Simulator 2|American Truck Simulator>\Plugins`.
2. Start the game. On first start with an SDK plugin the game asks whether to activate it
   (a warning dialog about third-party plugins); confirm. The game log should contain
   `[roadpilot-input] ready, waiting for pipe client`.
3. Remove the DLL (and registry value) to uninstall.

## Manual axis assignment in game

The device is a *generic* input device, so the game does not bind it automatically:

1. Options → Controls, choose a controller type that allows custom mapping (keyboard + mouse
   or a custom/joystick profile).
2. Select the **Acceleration** (throttle) entry, start binding, and pick
   *RoadPilot Input → Throttle*. Do the same for **Brake** with *RoadPilot Input → Brake*.
   The binding needs the axis to move, so a client must send a non-zero value while binding.
3. Make sure no other device/pedal is bound to the same function, or both will contribute.
4. For steering, select the **Steering** entry of the controls and bind
   *RoadPilot Input → Steering*. The binding needs the axis to move, so a client must send a
   non-zero steering value (protocol v2, `steering_enabled` set) while binding. Check the
   direction in game: the sign convention of the axis is not verified. Unbind the axis again
   when you do not want RoadPilot to be able to steer.

**Manual driving.** While the RoadPilot axes are bound the plugin reports neutral values
(0) every frame when disarmed, as the Input SDK requires. How the game combines these with your
own keyboard, wheel or pedals bound to the same functions is **unverified**; do not assume that
manual steering keeps working. Keep a way to take over: disarm in the client, unbind the axis
or remove the plugin.

Exact menu labels differ between game versions; verify them in game.

## Safety behaviour

At start, on invalid messages, on disconnect, on stale input (>250 ms), and on an explicit
disable message the output is throttle 0 / brake 0 / steering 0 / disabled. If throttle and
brake are requested together, throttle is set to 0 and brake kept.

Steering is stricter: it needs both the `enabled` flag and the separate `steering_enabled`
flag (never set by v1 clients), is clamped to -1..1, can grow by at most 2.0 units per second,
goes through zero on a direction change, and is released immediately when no valid message
arrived for 150 ms. A client must therefore refresh it at least every 50 ms, independently of
any slower processing loop. Steering is never armed automatically. See `docs/PROTOCOL.md`.

## Test status

Automated tests cover message validation (v1 and v2), clamping, timeouts, disarm, NaN/infinity,
simultaneous throttle/brake, steering gates, rate limiting, direction reversal, large polling
gaps, and a fake-host run of the real DLL over the real pipe.
These do **not** prove in-game control.

**Open, not yet verified in ATS/ETS2:**
- plugin loads and is activated by the game (ATS and ETS2, 64-bit)
- the Steering axis can be bound, its range and sign convention, and how the game combines it
  with manual steering input
- device "RoadPilot Input" appears in the binding UI and both axes can be assigned
- throttle/brake values from the pipe move the truck's pedals as expected, and release to 0
- fallback to 0 on pipe disconnect and on client stalls, observed in game
- behaviour across game pause, menu, and `sdk reinit`
