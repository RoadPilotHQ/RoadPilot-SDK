// Thread-safe controller state: the single place that decides which throttle/brake
// values are handed to the game. Platform independent; time is injected so tests
// need no clock. Safe state = disabled, throttle 0, brake 0.
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
};

class InputState {
 public:
  static constexpr std::uint64_t kDefaultTimeoutMs = 250;

  explicit InputState(std::uint64_t timeout_ms = kDefaultTimeoutMs) : timeout_ms_(timeout_ms) {}

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

  // Values to give to the game right now. Stale input yields the safe state.
  Output poll(std::uint64_t now_ms) const;

 private:
  void set_safe_locked();

  const std::uint64_t timeout_ms_;
  mutable std::mutex mutex_;
  bool has_command_ = false;
  Output current_{};
  std::uint64_t last_update_ms_ = 0;
};

}  // namespace roadpilot::input
