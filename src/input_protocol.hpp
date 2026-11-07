// RoadPilot input pipe protocol v1 and v2 - platform independent, no SCS SDK dependency.
// See docs/PROTOCOL.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace roadpilot::input {

constexpr std::uint32_t kMagic = 0x4E495052u;  // bytes 'R' 'P' 'I' 'N' on the wire
constexpr std::uint16_t kProtocolVersion = 1;    // throttle + brake (16 bytes)
constexpr std::uint16_t kProtocolVersionV2 = 2;  // adds steering (20 bytes)
constexpr std::size_t kMessageSize = 16;         // v1 size
constexpr std::size_t kMessageSizeV2 = 20;
constexpr std::uint8_t kFlagSteeringEnabled = 0x01;  // v2 flags byte (offset 7), bit 0

struct Command {
  bool enabled = false;
  float throttle = 0.0f;  // as received, may be outside 0..1 (clamped by InputState)
  float brake = 0.0f;
  // v2 only. v1 messages decode to steering_enabled = false, steering = 0, so a v1 client
  // can never activate steering.
  float steering = 0.0f;  // as received, may be outside -1..1 (clamped by InputState)
  bool steering_enabled = false;
};

enum class ParseStatus {
  Ok,
  BadSize,
  BadMagic,
  BadVersion,
  BadReserved,  // v1: byte 7 not zero; v2: unknown flag bits set
  BadEnabled,
  NonFinite,
};

const char* to_string(ParseStatus status);

// Validates and decodes one complete message (v1: 16 bytes, v2: 20 bytes). The version
// field must match the size. On anything but Ok, `out` is left in the safe default
// (disabled, all values 0, steering disabled).
ParseStatus parse_message(const std::uint8_t* data, std::size_t size, Command& out);

// v1 encoding; steering fields of `command` are ignored.
std::array<std::uint8_t, kMessageSize> encode_message(const Command& command);

// v2 encoding including steering and the steering_enabled flag.
std::array<std::uint8_t, kMessageSizeV2> encode_message_v2(const Command& command);

}  // namespace roadpilot::input
