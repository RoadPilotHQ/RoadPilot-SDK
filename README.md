# RoadPilot-SDK

Prototype of a native SCS **Input API** plugin for American Truck Simulator and
Euro Truck Simulator 2 (Windows x64). It registers one generic input device,
"RoadPilot Input", with two float axes, **Throttle** and **Brake** (service brake),
and fills them from a local named pipe so a later RoadPilot process can drive them.

Scope of this prototype: throttle and service brake only. Nothing is written to
`controls.sii`, profiles, saves or game files.

## Layout

- `src/input_protocol.*`: wire format parser/encoder (platform independent)
- `src/input_state.*`: arm/disarm, clamping, brake priority, stale timeout (platform independent)
- `src/pipe_server.*`: Windows named pipe transport (no SDK, no meaning of messages)
- `src/plugin.cpp`: SCS SDK callbacks only
- `docs/PROTOCOL.md`: message format and fail-safe rules
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

Exact menu labels differ between game versions; verify them in game.

## Safety behaviour

At start, on invalid messages, on disconnect, on stale input (>250 ms), and on an explicit
disable message the output is throttle 0 / brake 0 / disabled. If throttle and brake are
requested together, throttle is set to 0 and brake kept. See `docs/PROTOCOL.md`.

## Test status

Automated tests cover message validation, clamping, timeout, disarm, NaN/infinity,
simultaneous throttle/brake, and a fake-host run of the real DLL over the real pipe.
These do **not** prove in-game control.

**Open, not yet verified in ATS/ETS2:**
- plugin loads and is activated by the game (ATS and ETS2, 64-bit)
- device "RoadPilot Input" appears in the binding UI and both axes can be assigned
- throttle/brake values from the pipe move the truck's pedals as expected, and release to 0
- fallback to 0 on pipe disconnect and on client stalls, observed in game
- behaviour across game pause, menu, and `sdk reinit`
