// Loads the built plugin DLL into a fake SCS host and talks to it over the real
// named pipe. This checks the SDK-facing contract (device registration, callback
// protocol, value range) and the pipe transport. It is NOT evidence that the
// game accepts or applies the input; that must be verified in ATS/ETS2.
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "amtrucks/scssdk_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "input_protocol.hpp"
#include "scssdk_input.h"

using namespace roadpilot::input;

static const char* const kDefaultPipeName = "\\\\.\\pipe\\roadpilot-input-v1";
typedef SCSAPI_RESULT_FPTR(init_fn_t)(const scs_u32_t, const scs_input_init_params_t* const);
typedef SCSAPI_VOID_FPTR(shutdown_fn_t)(void);

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static scs_input_device_t g_device;
static scs_input_device_input_t g_inputs[8];
static int g_register_calls = 0;

static SCSAPI_RESULT fake_register(const scs_input_device_t* const info) {
  ++g_register_calls;
  g_device = *info;
  CHECK(info->input_count <= 8);
  std::memcpy(g_inputs, info->inputs, sizeof(scs_input_device_input_t) * info->input_count);
  g_device.inputs = g_inputs;
  return SCS_RESULT_ok;
}
static SCSAPI_VOID fake_log(const scs_log_type_t, const scs_string_t) {}

struct Frame {
  float v[2];
  int events;
};

// Runs one game frame: calls the event callback until not_found.
static Frame run_frame(bool first_after_activation = false) {
  Frame f{{-1.0f, -1.0f}, 0};
  scs_u32_t flags = SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame |
                    (first_after_activation ? SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation : 0);
  for (int guard = 0; guard < 10; ++guard) {
    scs_input_event_t ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.input_index = 99;
    const scs_result_t r = g_device.input_event_callback(&ev, flags, g_device.callback_context);
    flags = 0;
    if (r == SCS_RESULT_not_found) return f;
    CHECK(r == SCS_RESULT_ok);
    CHECK(ev.input_index < 2);
    if (ev.input_index < 2) f.v[ev.input_index] = ev.value_float.value;
    ++f.events;
  }
  CHECK(false);  // callback never returned not_found
  return f;
}

static bool in_range(const Frame& f) {
  for (float v : f.v) {
    if (!std::isfinite(v) || v < 0.0f || v > 1.0f) return false;
  }
  return true;
}

// Polls frames until `pred` holds or one second passes.
template <class P>
static bool wait_for(P pred) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < end) {
    const Frame f = run_frame();
    CHECK(in_range(f));
    if (pred(f)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

static bool write_raw(HANDLE pipe, const void* data, DWORD size) {
  DWORD written = 0;
  return WriteFile(pipe, data, size, &written, nullptr) && written == size;
}
static bool send_cmd(HANDLE pipe, const Command& c) {
  const auto m = encode_message(c);
  return write_raw(pipe, m.data(), static_cast<DWORD>(m.size()));
}

static HANDLE connect_client() {
  for (int i = 0; i < 100; ++i) {
    HANDLE h = CreateFileA(kDefaultPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_MESSAGE;
      SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
      return h;
    }
    if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(kDefaultPipeName, 100);
    else std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return INVALID_HANDLE_VALUE;
}

static auto is_zero = [](const Frame& f) { return f.v[0] == 0.0f && f.v[1] == 0.0f; };

static void run_session(init_fn_t init, shutdown_fn_t shutdown, const char* game_id) {
  scs_input_init_params_v100_t params;
  std::memset(&params, 0, sizeof(params));
  params.common.game_name = "Fake";
  params.common.game_id = game_id;
  params.common.game_version = SCS_MAKE_VERSION(1, 0);
  params.common.log = fake_log;
  params.register_device = fake_register;

  g_register_calls = 0;
  CHECK(init(SCS_INPUT_VERSION_1_00, &params) == SCS_RESULT_ok);
  CHECK(g_register_calls == 1);
  CHECK(g_device.type == SCS_INPUT_DEVICE_TYPE_generic);
  CHECK(g_device.input_count == 2);
  CHECK(g_device.input_event_callback != nullptr);
  CHECK(g_inputs[0].value_type == SCS_VALUE_TYPE_float);
  CHECK(g_inputs[1].value_type == SCS_VALUE_TYPE_float);
  CHECK(std::strcmp(g_inputs[0].name, "throttle") == 0);
  CHECK(std::strcmp(g_inputs[1].name, "brake") == 0);

  // Startup state, no client yet: safe zeros, both axes reported.
  Frame f = run_frame(true);
  CHECK(f.events == 2 && is_zero(f));

  HANDLE pipe = connect_client();
  CHECK(pipe != INVALID_HANDLE_VALUE);
  if (pipe == INVALID_HANDLE_VALUE) {
    shutdown();
    return;
  }

  // Valid input reaches the callback.
  CHECK(send_cmd(pipe, {true, 0.5f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.5f && x.v[1] == 0.0f; }));

  // Out-of-range values are clamped.
  CHECK(send_cmd(pipe, {true, 4.0f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 1.0f; }));

  // Throttle + brake: brake wins.
  CHECK(send_cmd(pipe, {true, 0.9f, 0.4f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.0f && x.v[1] == 0.4f; }));

  // Invalid message (wrong size) -> safe.
  CHECK(send_cmd(pipe, {true, 0.5f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.5f; }));
  const std::uint8_t junk[5] = {1, 2, 3, 4, 5};
  CHECK(write_raw(pipe, junk, sizeof(junk)));
  CHECK(wait_for(is_zero));

  // Explicit disarm.
  CHECK(send_cmd(pipe, {true, 0.5f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.5f; }));
  CHECK(send_cmd(pipe, {false, 0.5f, 0.0f}));
  CHECK(wait_for(is_zero));

  // Stale input: no update for longer than the timeout (250 ms).
  CHECK(send_cmd(pipe, {true, 0.7f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.7f; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  f = run_frame();
  CHECK(is_zero(f));

  // Connection loss while armed -> safe, and stays safe.
  CHECK(send_cmd(pipe, {true, 0.8f, 0.0f}));
  CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.8f; }));
  CloseHandle(pipe);
  CHECK(wait_for(is_zero));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(is_zero(run_frame()));

  // A new client can reconnect afterwards.
  pipe = connect_client();
  CHECK(pipe != INVALID_HANDLE_VALUE);
  if (pipe != INVALID_HANDLE_VALUE) {
    CHECK(send_cmd(pipe, {true, 0.3f, 0.0f}));
    CHECK(wait_for([](const Frame& x) { return x.v[0] == 0.3f; }));
    // Device deactivation disarms.
    g_device.input_active_callback(0, g_device.callback_context);
    CHECK(is_zero(run_frame()));
    CloseHandle(pipe);
  }

  shutdown();
}

int main() {
  HMODULE dll = LoadLibraryA(ROADPILOT_PLUGIN_PATH);
  CHECK(dll != nullptr);
  if (!dll) return 1;
  auto init = reinterpret_cast<init_fn_t>(GetProcAddress(dll, "scs_input_init"));
  auto shutdown = reinterpret_cast<shutdown_fn_t>(GetProcAddress(dll, "scs_input_shutdown"));
  CHECK(init && shutdown);
  if (!init || !shutdown) return 1;

  // Unsupported API version / game are refused.
  {
    scs_input_init_params_v100_t p;
    std::memset(&p, 0, sizeof(p));
    p.common.game_id = SCS_GAME_ID_ATS;
    p.common.log = fake_log;
    p.register_device = fake_register;
    CHECK(init(SCS_MAKE_VERSION(2, 0), &p) == SCS_RESULT_unsupported);
    p.common.game_id = "other";
    CHECK(init(SCS_INPUT_VERSION_1_00, &p) == SCS_RESULT_unsupported);
  }

  // init/shutdown may cycle several times per load; both games must work.
  run_session(init, shutdown, SCS_GAME_ID_ATS);
  run_session(init, shutdown, SCS_GAME_ID_EUT2);

  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("plugin host tests passed\n");
  return 0;
}
