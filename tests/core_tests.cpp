// Platform independent tests: protocol validation and InputState behaviour.
// No game, no SCS SDK, no pipe needed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "input_protocol.hpp"
#include "input_state.hpp"

using namespace roadpilot::input;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

static bool is_safe(const Output& o) { return !o.enabled && o.throttle == 0.0f && o.brake == 0.0f; }

static ParseStatus send(InputState& s, const Command& c, std::uint64_t now) {
  const auto m = encode_message(c);
  return s.handle_message(m.data(), m.size(), now);
}

static const float kNaN = std::numeric_limits<float>::quiet_NaN();
static const float kInf = std::numeric_limits<float>::infinity();

static void test_defaults() {
  InputState s;
  CHECK(is_safe(s.poll(0)));
  CHECK(is_safe(s.poll(1000000)));
}

static void test_valid_message() {
  InputState s;
  CHECK(send(s, {true, 0.5f, 0.0f}, 100) == ParseStatus::Ok);
  const Output o = s.poll(150);
  CHECK(o.enabled && o.throttle == 0.5f && o.brake == 0.0f);
}

static void test_wire_format() {
  // Fixed bytes of the documented format: throttle 0.5 (0x3F000000), brake 1.0 (0x3F800000).
  const std::uint8_t m[16] = {'R', 'P', 'I', 'N', 1, 0, 1, 0, 0, 0, 0, 0x3F, 0, 0, 0x80, 0x3F};
  Command c;
  CHECK(parse_message(m, sizeof(m), c) == ParseStatus::Ok);
  CHECK(c.enabled && c.throttle == 0.5f && c.brake == 1.0f);
  const auto enc = encode_message(c);
  CHECK(std::memcmp(enc.data(), m, 16) == 0);
}

static void test_message_validation() {
  const auto good = encode_message({true, 0.2f, 0.0f});
  Command c;
  CHECK(parse_message(good.data(), good.size(), c) == ParseStatus::Ok);
  CHECK(parse_message(good.data(), 15, c) == ParseStatus::BadSize);
  CHECK(parse_message(good.data(), 0, c) == ParseStatus::BadSize);
  CHECK(parse_message(nullptr, 16, c) == ParseStatus::BadSize);
  std::uint8_t big[17] = {};
  std::memcpy(big, good.data(), 16);
  CHECK(parse_message(big, 17, c) == ParseStatus::BadSize);

  auto m = good;
  m[0] = 'X';
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadMagic);
  m = good;
  m[4] = 2;
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadVersion);
  m = good;
  m[6] = 2;
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadEnabled);
  m = good;
  m[7] = 1;
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadReserved);

  // Failed parse leaves the safe default in `out`.
  CHECK(!c.enabled && c.throttle == 0.0f && c.brake == 0.0f);
}

static void test_non_finite_rejected() {
  for (float bad : {kNaN, kInf, -kInf}) {
    Command c;
    auto m = encode_message({true, bad, 0.0f});
    CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::NonFinite);
    m = encode_message({true, 0.0f, bad});
    CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::NonFinite);

    // And through the state: previously armed input must be dropped.
    InputState s;
    CHECK(send(s, {true, 0.7f, 0.0f}, 0) == ParseStatus::Ok);
    CHECK(send(s, {true, bad, 0.0f}, 10) == ParseStatus::NonFinite);
    CHECK(is_safe(s.poll(10)));
    // apply() with a decoded command also refuses non-finite values.
    CHECK(send(s, {true, 0.7f, 0.0f}, 20) == ParseStatus::Ok);
    CHECK(!s.apply({true, 0.1f, bad}, 30));
    CHECK(is_safe(s.poll(30)));
  }
}

static void test_clamping() {
  InputState s;
  send(s, {true, 5.0f, 0.0f}, 0);
  CHECK(s.poll(1).throttle == 1.0f);
  send(s, {true, -3.0f, 0.0f}, 2);
  CHECK(s.poll(3).throttle == 0.0f);
  send(s, {true, 0.0f, 99.0f}, 4);
  CHECK(s.poll(5).brake == 1.0f);
  send(s, {true, 0.0f, -0.5f}, 6);
  CHECK(s.poll(7).brake == 0.0f && s.poll(7).enabled);
  send(s, {true, 1.0f, 0.0f}, 8);
  CHECK(s.poll(9).throttle == 1.0f);
}

static void test_throttle_and_brake_together() {
  InputState s;
  send(s, {true, 0.8f, 0.3f}, 0);
  Output o = s.poll(1);
  CHECK(o.enabled && o.throttle == 0.0f && o.brake == 0.3f);
  // Out-of-range values are clamped first, brake still wins.
  send(s, {true, 7.0f, 7.0f}, 2);
  o = s.poll(3);
  CHECK(o.throttle == 0.0f && o.brake == 1.0f);
  // A negative brake clamps to 0, so it does not suppress throttle.
  send(s, {true, 0.4f, -1.0f}, 4);
  o = s.poll(5);
  CHECK(o.throttle == 0.4f && o.brake == 0.0f);
}

static void test_timeout() {
  InputState s(250);
  send(s, {true, 0.6f, 0.0f}, 1000);
  CHECK(s.poll(1250).throttle == 0.6f);  // exactly at the limit still valid
  CHECK(is_safe(s.poll(1251)));          // stale
  CHECK(is_safe(s.poll(5000)));
  // A fresh message revives it.
  send(s, {true, 0.2f, 0.0f}, 6000);
  CHECK(s.poll(6100).throttle == 0.2f);
  // Clock going backwards is not trusted.
  CHECK(is_safe(s.poll(5000)));
}

static void test_disarm() {
  InputState s;
  send(s, {true, 0.9f, 0.0f}, 0);
  s.disarm();
  CHECK(is_safe(s.poll(1)));
  // Stays disarmed without new messages even though the timeout has not elapsed.
  CHECK(is_safe(s.poll(2)));
  // Explicit disarm via message with enabled=false, values ignored.
  send(s, {true, 0.9f, 0.0f}, 10);
  CHECK(s.poll(11).enabled);
  send(s, {false, 0.9f, 0.9f}, 12);
  CHECK(is_safe(s.poll(13)));
  // Re-arming needs a new enabled message.
  send(s, {true, 0.1f, 0.0f}, 14);
  CHECK(s.poll(15).throttle == 0.1f);
}

static void test_disconnect() {
  InputState s;
  send(s, {true, 1.0f, 0.0f}, 0);
  s.on_disconnect();
  CHECK(is_safe(s.poll(1)));
}

static void test_invalid_message_disarms() {
  InputState s;
  send(s, {true, 1.0f, 0.0f}, 0);
  const std::uint8_t junk[3] = {1, 2, 3};
  CHECK(s.handle_message(junk, 3, 1) == ParseStatus::BadSize);
  CHECK(is_safe(s.poll(2)));
  auto m = encode_message({true, 1.0f, 0.0f});
  m[1] = 'Z';
  send(s, {true, 1.0f, 0.0f}, 3);
  CHECK(s.handle_message(m.data(), m.size(), 4) == ParseStatus::BadMagic);
  CHECK(is_safe(s.poll(5)));
}

int main() {
  test_defaults();
  test_valid_message();
  test_wire_format();
  test_message_validation();
  test_non_finite_rejected();
  test_clamping();
  test_throttle_and_brake_together();
  test_timeout();
  test_disarm();
  test_disconnect();
  test_invalid_message_disarms();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("core tests passed\n");
  return 0;
}
