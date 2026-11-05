// RoadPilot input pipe protocol v1 - platform independent, no SCS SDK dependency.
// See docs/PROTOCOL.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace roadpilot::input {

constexpr std::uint32_t kMagic = 0x4E495052u;  // bytes 'R' 'P' 'I' 'N' on the wire
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kMessageSize = 16;

struct Command {
  bool enabled = false;
  float throttle = 0.0f;  // as received, may be outside 0..1 (clamped by InputState)
  float brake = 0.0f;
};

enum class ParseStatus {
  Ok,
  BadSize,
  BadMagic,
  BadVersion,
  BadReserved,
  BadEnabled,
  NonFinite,
};

const char* to_string(ParseStatus status);

// Validates and decodes one complete message. On anything but Ok, `out` is left
// in the safe default (disabled, 0, 0).
ParseStatus parse_message(const std::uint8_t* data, std::size_t size, Command& out);

std::array<std::uint8_t, kMessageSize> encode_message(const Command& command);

}  // namespace roadpilot::input
