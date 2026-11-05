#include "input_state.hpp"

#include <algorithm>
#include <cmath>

namespace roadpilot::input {
namespace {
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
}  // namespace

void InputState::set_safe_locked() {
  has_command_ = false;
  current_ = Output{};
}

ParseStatus InputState::handle_message(const std::uint8_t* data, std::size_t size,
                                       std::uint64_t now_ms) {
  Command command;
  const ParseStatus status = parse_message(data, size, command);
  if (status != ParseStatus::Ok) {
    disarm();
    return status;
  }
  apply(command, now_ms);
  return status;
}

bool InputState::apply(const Command& command, std::uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!std::isfinite(command.throttle) || !std::isfinite(command.brake)) {
    set_safe_locked();
    return false;
  }
  if (!command.enabled) {
    set_safe_locked();  // explicit disarm from the client
    return true;
  }
  float throttle = clamp01(command.throttle);
  const float brake = clamp01(command.brake);
  if (throttle > 0.0f && brake > 0.0f) throttle = 0.0f;  // brake wins
  current_ = Output{true, throttle, brake};
  has_command_ = true;
  last_update_ms_ = now_ms;
  return true;
}

void InputState::disarm() {
  std::lock_guard<std::mutex> lock(mutex_);
  set_safe_locked();
}

Output InputState::poll(std::uint64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!has_command_ || !current_.enabled) return Output{};
  // A clock that went backwards is treated as stale rather than trusted.
  if (now_ms < last_update_ms_ || now_ms - last_update_ms_ > timeout_ms_) return Output{};
  return current_;
}

}  // namespace roadpilot::input
