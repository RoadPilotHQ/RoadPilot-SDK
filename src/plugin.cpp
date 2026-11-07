// SCS Input API glue (SDK 1.16, input API 1.00). Only translates between the SDK
// callbacks and InputState; all decisions live in InputState.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <memory>

#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_input_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_input_eut2.h"
#include "input_state.hpp"
#include "pipe_server.hpp"
#include "scssdk_input.h"

namespace {

using namespace roadpilot::input;

constexpr scs_u32_t kThrottleIndex = 0;
constexpr scs_u32_t kBrakeIndex = 1;
constexpr scs_u32_t kSteeringIndex = 2;
constexpr scs_u32_t kInputCount = 3;

std::uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct Plugin final : MessageSink {
  InputState state;
  scs_log_t log = nullptr;
  std::unique_ptr<PipeServer> pipe;
  Output frame{};  // snapshot taken once per game frame
  scs_u32_t next_input = kInputCount;

  void on_message(const std::uint8_t* data, std::size_t size) override {
    const ParseStatus status = state.handle_message(data, size, now_ms());
    if (status != ParseStatus::Ok && log) {
      log(SCS_LOG_TYPE_warning, "[roadpilot-input] invalid message, input disarmed");
    }
  }
  void on_disconnect() override { state.on_disconnect(); }
};

std::unique_ptr<Plugin> g_plugin;

SCSAPI_VOID input_active_callback(const scs_u8_t active, const scs_context_t context) {
  if (!active) static_cast<Plugin*>(context)->state.disarm();
}

SCSAPI_RESULT input_event_callback(scs_input_event_t* const event_info, const scs_u32_t flags,
                                   const scs_context_t context) {
  Plugin& p = *static_cast<Plugin*>(context);
  if (flags & (SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame |
               SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation)) {
    p.frame = p.state.poll(now_ms());
    p.next_input = 0;
  }
  if (p.next_input >= kInputCount) return SCS_RESULT_not_found;

  // All axes are reported every frame, including neutral zeros, so releasing works.
  event_info->input_index = p.next_input;
  float value = 0.0f;
  switch (p.next_input) {
    case kThrottleIndex: value = p.frame.throttle; break;
    case kBrakeIndex: value = p.frame.brake; break;
    case kSteeringIndex: value = p.frame.steering; break;
  }
  event_info->value_float.value = value;
  ++p.next_input;
  return SCS_RESULT_ok;
}

}  // namespace

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t* const params) {
  if (version != SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
  const auto* p = static_cast<const scs_input_init_params_v100_t*>(params);

  const char* game = p->common.game_id;
  if (std::strcmp(game, SCS_GAME_ID_EUT2) != 0 && std::strcmp(game, SCS_GAME_ID_ATS) != 0) {
    p->common.log(SCS_LOG_TYPE_error, "[roadpilot-input] unsupported game");
    return SCS_RESULT_unsupported;
  }

  g_plugin = std::make_unique<Plugin>();
  g_plugin->log = p->common.log;

  static const scs_input_device_input_t inputs[kInputCount] = {
      {"throttle", "Throttle", SCS_VALUE_TYPE_float},
      {"brake", "Brake", SCS_VALUE_TYPE_float},
      {"steering", "Steering", SCS_VALUE_TYPE_float},
  };
  scs_input_device_t device;
  std::memset(&device, 0, sizeof(device));
  device.name = "roadpilot_input";
  device.display_name = "RoadPilot Input";
  device.type = SCS_INPUT_DEVICE_TYPE_generic;
  device.input_count = kInputCount;
  device.inputs = inputs;
  device.callback_context = g_plugin.get();
  device.input_active_callback = input_active_callback;
  device.input_event_callback = input_event_callback;

  if (p->register_device(&device) != SCS_RESULT_ok) {
    p->common.log(SCS_LOG_TYPE_error, "[roadpilot-input] unable to register device");
    g_plugin.reset();
    return SCS_RESULT_generic_error;
  }

  g_plugin->pipe = std::make_unique<PipeServer>(kDefaultPipeName, *g_plugin);
  if (!g_plugin->pipe->start()) {
    p->common.log(SCS_LOG_TYPE_error, "[roadpilot-input] unable to start pipe server");
    g_plugin.reset();
    return SCS_RESULT_generic_error;
  }
  p->common.log(SCS_LOG_TYPE_message, "[roadpilot-input] ready, waiting for pipe client");
  return SCS_RESULT_ok;
}

SCSAPI_VOID scs_input_shutdown(void) {
  if (g_plugin) {
    g_plugin->pipe.reset();  // joins the pipe thread before state goes away
    g_plugin->state.disarm();
    g_plugin.reset();
  }
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
