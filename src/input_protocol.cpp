#include "input_protocol.hpp"

#include <cmath>
#include <cstring>

namespace roadpilot::input {
namespace {

std::uint32_t read_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

float read_f32(const std::uint8_t* p) {
  const std::uint32_t bits = read_u32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void write_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

void write_f32(std::uint8_t* p, float v) {
  std::uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  write_u32(p, bits);
}

}  // namespace

const char* to_string(ParseStatus status) {
  switch (status) {
    case ParseStatus::Ok: return "ok";
    case ParseStatus::BadSize: return "bad size";
    case ParseStatus::BadMagic: return "bad magic";
    case ParseStatus::BadVersion: return "unsupported version";
    case ParseStatus::BadReserved: return "reserved byte not zero";
    case ParseStatus::BadEnabled: return "enabled byte not 0 or 1";
    case ParseStatus::NonFinite: return "non-finite value";
  }
  return "unknown";
}

ParseStatus parse_message(const std::uint8_t* data, std::size_t size, Command& out) {
  out = Command{};
  if (data == nullptr || (size != kMessageSize && size != kMessageSizeV2)) return ParseStatus::BadSize;
  if (read_u32(data) != kMagic) return ParseStatus::BadMagic;
  const std::uint16_t version = read_u16(data + 4);
  if (version != kProtocolVersion && version != kProtocolVersionV2) return ParseStatus::BadVersion;
  // The version decides the size; a mismatch is a malformed message.
  if ((version == kProtocolVersion) != (size == kMessageSize)) return ParseStatus::BadSize;
  if (data[6] > 1) return ParseStatus::BadEnabled;
  const bool v2 = version == kProtocolVersionV2;
  if (v2 ? (data[7] & ~kFlagSteeringEnabled) != 0 : data[7] != 0) return ParseStatus::BadReserved;
  const float throttle = read_f32(data + 8);
  const float brake = read_f32(data + 12);
  const float steering = v2 ? read_f32(data + 16) : 0.0f;
  if (!std::isfinite(throttle) || !std::isfinite(brake) || !std::isfinite(steering)) {
    return ParseStatus::NonFinite;
  }
  out.enabled = data[6] == 1;
  out.throttle = throttle;
  out.brake = brake;
  out.steering = steering;
  out.steering_enabled = v2 && (data[7] & kFlagSteeringEnabled) != 0;
  return ParseStatus::Ok;
}

std::array<std::uint8_t, kMessageSize> encode_message(const Command& command) {
  std::array<std::uint8_t, kMessageSize> m{};
  write_u32(m.data(), kMagic);
  m[4] = static_cast<std::uint8_t>(kProtocolVersion & 0xFF);
  m[5] = static_cast<std::uint8_t>(kProtocolVersion >> 8);
  m[6] = command.enabled ? 1 : 0;
  m[7] = 0;
  write_f32(m.data() + 8, command.throttle);
  write_f32(m.data() + 12, command.brake);
  return m;
}

std::array<std::uint8_t, kMessageSizeV2> encode_message_v2(const Command& command) {
  std::array<std::uint8_t, kMessageSizeV2> m{};
  write_u32(m.data(), kMagic);
  m[4] = static_cast<std::uint8_t>(kProtocolVersionV2 & 0xFF);
  m[5] = static_cast<std::uint8_t>(kProtocolVersionV2 >> 8);
  m[6] = command.enabled ? 1 : 0;
  m[7] = command.steering_enabled ? kFlagSteeringEnabled : 0;
  write_f32(m.data() + 8, command.throttle);
  write_f32(m.data() + 12, command.brake);
  write_f32(m.data() + 16, command.steering);
  return m;
}

}  // namespace roadpilot::input
