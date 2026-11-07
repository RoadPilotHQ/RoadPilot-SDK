// Thread-safe controller state: the single place that decides which throttle/brake/steering
// values are handed to the game. Platform independent; time is injected so tests need no
// clock. Safe state = disabled, throttle 0, brake 0, steering 0 and not active.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "input_protocol.hpp"

namespace roadpilot::input {

struct Output {
  bool enabled = false;
  float throttle = 0.0f;
  float brake = 0.0f;
  float steering = 0.0f;         // -1..1 after clamping and rate limiting; 0 = neutral
  bool steering_active = false;  // steering is armed, fresh and being applied
};

class InputState {
 public:
  static constexpr std::uint64_t kDefaultTimeoutMs = 250;          // throttle / brake
  static constexpr std::uint64_t kDefaultSteeringTimeoutMs = 150;  // steering
  static constexpr float kDefaultSteeringRatePerSecond = 2.0f;     // magnitude increase limit
  // Longest time between two polls over which the rate limiter integrates. A longer gap
  // (stalled game, loading screen) restarts steering from neutral instead of allowing one
  // large step.
  static constexpr std::uint64_t kMaxRampStepMs = 100;

  explicit InputState(std::uint64_t timeout_ms = kDefaultTimeoutMs,
                      std::uint64_t steering_timeout_ms = kDefaultSteeringTimeoutMs,
                      float steering_rate_per_second = kDefaultSteeringRatePerSecond);

  // Parses and applies a raw pipe message. Any invalid message drops to the
  // safe state (disarmed); a later valid message re-arms.
  ParseStatus handle_message(const std::uint8_t* data, std::size_t size, std::uint64_t now_ms);

  // Applies an already decoded command. Returns false (and goes to the safe
  // state) if a value is NaN/infinite.
  bool apply(const Command& command, std::uint64_t now_ms);

  // Explicit stop: safe state until the next valid enabled message.
  void disarm();

  // Pipe client went away: identical to disarm().
  void on_disconnect() { disarm(); }

  // Values to give to the game right now. Stale input yields the safe state. Call once per
  // game frame: steering is rate limited between consecutive calls.
  Output poll(std::uint64_t now_ms);

 private:
  void set_safe_locked();
  void reset_steering_locked();
  float limit_steering_locked(float target, std::uint64_t now_ms);

  const std::uint64_t timeout_ms_;
  const std::uint64_t steering_timeout_ms_;
  const float steering_rate_;
  std::mutex mutex_;
  bool has_command_ = false;
  Output current_{};                 // enabled, throttle, brake as commanded (clamped)
  bool steering_requested_ = false;  // steering_enabled flag of the last valid command
  float steering_target_ = 0.0f;     // clamped target of the last valid command
  float steering_out_ = 0.0f;        // last value handed out (rate limiter state)
  bool steering_ramping_ = false;    // steering_out_ / last_poll_ms_ are valid
  std::uint64_t last_poll_ms_ = 0;
  std::uint64_t last_update_ms_ = 0;
};

}  // namespace roadpilot::input
