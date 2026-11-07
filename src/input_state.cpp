#include "input_state.hpp"

#include <algorithm>
#include <cmath>

namespace roadpilot::input {
namespace {
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float clamp_signed(float v) { return std::min(1.0f, std::max(-1.0f, v)); }
}  // namespace

InputState::InputState(std::uint64_t timeout_ms, std::uint64_t steering_timeout_ms,
                       float steering_rate_per_second)
    : timeout_ms_(timeout_ms),
      steering_timeout_ms_(steering_timeout_ms),
      steering_rate_(std::isfinite(steering_rate_per_second) && steering_rate_per_second > 0.0f
                         ? steering_rate_per_second
                         : kDefaultSteeringRatePerSecond) {}

void InputState::reset_steering_locked() {
  steering_requested_ = false;
  steering_target_ = 0.0f;
  steering_out_ = 0.0f;
  steering_ramping_ = false;
}

void InputState::set_safe_locked() {
  has_command_ = false;
  current_ = Output{};
  reset_steering_locked();
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
  if (!std::isfinite(command.throttle) || !std::isfinite(command.brake) ||
      !std::isfinite(command.steering)) {
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
  current_ = Output{true, throttle, brake, 0.0f, false};
  has_command_ = true;
  last_update_ms_ = now_ms;
  // Steering needs the general enabled flag (checked above) and its own flag.
  if (command.steering_enabled) {
    steering_requested_ = true;
    steering_target_ = clamp_signed(command.steering);
  } else {
    reset_steering_locked();
  }
  return true;
}

void InputState::disarm() {
  std::lock_guard<std::mutex> lock(mutex_);
  set_safe_locked();
}

// Rate limiting rules (all in magnitude, steering_rate_ units per second):
//  - the first active poll, and any poll after a gap over kMaxRampStepMs, outputs 0;
//  - a direction reversal outputs 0 first and ramps in the new direction on later polls;
//  - reducing the magnitude (including to 0) is immediate;
//  - increasing the magnitude moves by at most rate * elapsed time toward the target;
//  - a clock that went backwards restarts from 0.
float InputState::limit_steering_locked(float target, std::uint64_t now_ms) {
  const float current = steering_out_;
  float next = 0.0f;
  const bool continuous = steering_ramping_ && now_ms >= last_poll_ms_ &&
                          now_ms - last_poll_ms_ <= kMaxRampStepMs;
  if (continuous) {
    const bool reversal = current != 0.0f && target != 0.0f && (current > 0.0f) != (target > 0.0f);
    if (reversal) {
      next = 0.0f;
    } else if (std::fabs(target) <= std::fabs(current)) {
      next = target;
    } else {
      const float step = steering_rate_ * static_cast<float>(now_ms - last_poll_ms_) / 1000.0f;
      const float remaining = std::fabs(target) - std::fabs(current);
      next = current + (target > 0.0f ? 1.0f : -1.0f) * std::min(step, remaining);
    }
  }
  steering_out_ = clamp_signed(next);
  steering_ramping_ = true;
  last_poll_ms_ = now_ms;
  return steering_out_;
}

Output InputState::poll(std::uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!has_command_ || !current_.enabled) {
    reset_steering_locked();
    return Output{};
  }
  // A clock that went backwards is treated as stale rather than trusted.
  if (now_ms < last_update_ms_ || now_ms - last_update_ms_ > timeout_ms_) {
    reset_steering_locked();
    return Output{};
  }
  Output out = current_;
  if (steering_requested_ && now_ms - last_update_ms_ <= steering_timeout_ms_) {
    out.steering = limit_steering_locked(steering_target_, now_ms);
    out.steering_active = true;
  } else {
    reset_steering_locked();  // stale steering is released immediately
  }
  return out;
}

}  // namespace roadpilot::input
