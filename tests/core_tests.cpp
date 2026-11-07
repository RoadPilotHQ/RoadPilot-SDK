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

static bool is_safe(const Output& o) {
  return !o.enabled && o.throttle == 0.0f && o.brake == 0.0f && o.steering == 0.0f &&
         !o.steering_active;
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static ParseStatus send(InputState& s, const Command& c, std::uint64_t now) {
  const auto m = encode_message(c);
  return s.handle_message(m.data(), m.size(), now);
}

static ParseStatus send2(InputState& s, const Command& c, std::uint64_t now) {
  const auto m = encode_message_v2(c);
  return s.handle_message(m.data(), m.size(), now);
}

static Command steer_cmd(float steering, bool steering_enabled = true, bool enabled = true) {
  Command c;
  c.enabled = enabled;
  c.steering = steering;
  c.steering_enabled = steering_enabled;
  return c;
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
  m[4] = 3;
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadVersion);
  m = good;
  m[4] = 2;  // v2 needs the 20-byte layout
  CHECK(parse_message(m.data(), m.size(), c) == ParseStatus::BadSize);
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

static void test_v2_wire_format_and_validation() {
  // throttle 0.5, brake 1.0, steering -0.5 (0xBF000000), flags = steering enabled.
  const std::uint8_t m[20] = {'R', 'P', 'I', 'N', 2, 0, 1, 1, 0, 0, 0, 0x3F, 0, 0, 0x80, 0x3F,
                              0, 0, 0, 0xBF};
  Command c;
  CHECK(parse_message(m, sizeof(m), c) == ParseStatus::Ok);
  CHECK(c.enabled && c.steering_enabled && c.throttle == 0.5f && c.brake == 1.0f &&
        c.steering == -0.5f);
  const auto enc = encode_message_v2(c);
  CHECK(std::memcmp(enc.data(), m, 20) == 0);

  // v1 messages decode with steering off and 0, and encode_message ignores steering.
  Command v1;
  v1.enabled = true;
  v1.steering = 0.9f;
  v1.steering_enabled = true;
  const auto e1 = encode_message(v1);
  CHECK(parse_message(e1.data(), e1.size(), c) == ParseStatus::Ok);
  CHECK(!c.steering_enabled && c.steering == 0.0f);

  auto bad = enc;
  bad[7] = 0x03;  // unknown flag bit
  CHECK(parse_message(bad.data(), bad.size(), c) == ParseStatus::BadReserved);
  CHECK(!c.enabled && !c.steering_enabled && c.steering == 0.0f);
  bad = enc;
  bad[4] = 1;  // v1 version with 20 bytes
  CHECK(parse_message(bad.data(), bad.size(), c) == ParseStatus::BadSize);
  CHECK(parse_message(enc.data(), 19, c) == ParseStatus::BadSize);
  CHECK(parse_message(enc.data(), 16, c) == ParseStatus::BadSize);  // v2 version, v1 size
  auto v1_flag = e1;
  v1_flag[7] = 1;  // flags do not exist in v1
  CHECK(parse_message(v1_flag.data(), v1_flag.size(), c) == ParseStatus::BadReserved);
  for (float badv : {kNaN, kInf, -kInf}) {
    const auto n = encode_message_v2(steer_cmd(badv));
    CHECK(parse_message(n.data(), n.size(), c) == ParseStatus::NonFinite);
    InputState s;
    send2(s, steer_cmd(0.5f), 0);
    CHECK(send2(s, steer_cmd(badv), 10) == ParseStatus::NonFinite);
    CHECK(is_safe(s.poll(10)));
    CHECK(!s.apply(steer_cmd(badv), 20));
  }
}

static void test_steering_gates_and_defaults() {
  InputState s;
  CHECK(is_safe(s.poll(0)));  // disarmed by default
  // Both flags are required.
  send2(s, steer_cmd(0.5f, true, false), 0);
  CHECK(is_safe(s.poll(1)));
  send2(s, steer_cmd(0.5f, false, true), 10);
  Output o = s.poll(11);
  CHECK(o.enabled && !o.steering_active && o.steering == 0.0f);
  // v1 clients never activate steering.
  send(s, {true, 0.3f, 0.0f}, 20);
  o = s.poll(21);
  CHECK(o.enabled && o.throttle == 0.3f && !o.steering_active && o.steering == 0.0f);
  // Downgrading from v2 to v1 releases steering at once.
  send2(s, steer_cmd(1.0f), 30);
  s.poll(30);
  send2(s, steer_cmd(1.0f), 80);
  CHECK(s.poll(80).steering_active);
  send(s, {true, 0.0f, 0.0f}, 90);
  o = s.poll(91);
  CHECK(!o.steering_active && o.steering == 0.0f);
  // Clearing the steering flag releases steering and a later arm restarts from neutral.
  send2(s, steer_cmd(1.0f), 100);
  s.poll(100);
  send2(s, steer_cmd(1.0f), 150);
  CHECK(s.poll(150).steering > 0.0f);
  send2(s, steer_cmd(1.0f, false), 160);
  CHECK(!s.poll(161).steering_active);
  send2(s, steer_cmd(1.0f), 170);
  CHECK(s.poll(171).steering == 0.0f);
}

static void test_steering_clamp_and_rate_limit() {
  InputState s;  // 2.0 units/s, 150 ms timeout
  std::uint64_t t = 0;
  float prev = 0.0f;
  float out = 0.0f;
  // Target far outside the range is clamped; each 50 ms step adds at most 0.1.
  for (int i = 0; i < 14; ++i, t += 50) {
    send2(s, steer_cmd(5.0f), t);
    const Output o = s.poll(t);
    CHECK(o.steering_active);
    CHECK(o.steering - prev <= 0.1f + 1e-5f);
    CHECK(o.steering <= 1.0f);
    prev = out = o.steering;
  }
  CHECK(near(out, 1.0f));  // neutral first poll, then 0.1 per step, held at the clamp
  // Negative side is symmetric.
  InputState n;
  n.apply(steer_cmd(-5.0f), 0);
  n.poll(0);
  n.apply(steer_cmd(-5.0f), 50);
  CHECK(near(n.poll(50).steering, -0.1f));
  // Reductions are immediate.
  InputState r;
  for (std::uint64_t k = 0; k <= 500; k += 50) {
    r.apply(steer_cmd(1.0f), k);
    r.poll(k);
  }
  r.apply(steer_cmd(0.25f), 550);
  CHECK(near(r.poll(550).steering, 0.25f));
  r.apply(steer_cmd(0.0f), 600);
  const Output zero = r.poll(600);
  CHECK(near(zero.steering, 0.0f) && zero.steering_active);
}

static void test_steering_reversal_goes_through_zero() {
  InputState s;
  std::uint64_t t = 0;
  for (; t <= 200; t += 50) {
    s.apply(steer_cmd(1.0f), t);
    s.poll(t);
  }
  s.apply(steer_cmd(1.0f), 250);
  CHECK(s.poll(250).steering > 0.3f);
  s.apply(steer_cmd(-1.0f), 300);
  CHECK(s.poll(300).steering == 0.0f);  // returns to zero first
  s.apply(steer_cmd(-1.0f), 350);
  CHECK(near(s.poll(350).steering, -0.1f));  // then ramps in the new direction
}

static void test_steering_large_poll_gaps_restart_from_neutral() {
  InputState s(10000, 10000);  // long timeouts so only the ramp logic is under test
  s.apply(steer_cmd(1.0f), 0);
  s.poll(0);
  s.apply(steer_cmd(1.0f), 50);
  CHECK(near(s.poll(50).steering, 0.1f));
  // 400 ms without a poll (stalled game): no big step, restart from neutral.
  s.apply(steer_cmd(1.0f), 450);
  CHECK(s.poll(450).steering == 0.0f);
  s.apply(steer_cmd(1.0f), 500);
  CHECK(near(s.poll(500).steering, 0.1f));
  // Polls at the same millisecond do not advance the ramp.
  CHECK(near(s.poll(500).steering, 0.1f));
  // A clock that goes backwards restarts from neutral.
  CHECK(s.poll(400).steering == 0.0f);
}

static void test_steering_timeout_is_shorter_than_throttle_timeout() {
  InputState s;
  Command c;
  c.enabled = true;
  c.throttle = 0.6f;
  c.steering = 1.0f;
  c.steering_enabled = true;
  send2(s, c, 1000);
  s.poll(1000);
  send2(s, c, 1050);
  s.poll(1050);
  Output o = s.poll(1100);
  CHECK(o.steering_active && o.steering > 0.0f && o.throttle == 0.6f);
  o = s.poll(1200);  // exactly 150 ms after the last message (1050): still valid
  CHECK(o.steering_active);
  o = s.poll(1201);  // 151 ms after the last message: steering stale, throttle still valid
  CHECK(!o.steering_active && o.steering == 0.0f && o.throttle == 0.6f && o.enabled);
  CHECK(is_safe(s.poll(1301)));  // 251 ms: everything stale
  // A fresh message revives steering from neutral, never from the old value.
  send2(s, c, 2000);
  CHECK(s.poll(2000).steering == 0.0f);
}

static void test_steering_safe_state_on_disarm_disconnect_invalid() {
  InputState s;
  const auto arm = [&s](std::uint64_t from) {
    for (std::uint64_t t = from; t <= from + 200; t += 50) {
      send2(s, steer_cmd(1.0f), t);
      s.poll(t);
    }
  };
  arm(0);
  CHECK(s.poll(200).steering > 0.0f);
  s.disarm();
  CHECK(is_safe(s.poll(201)));
  arm(1000);
  s.on_disconnect();
  CHECK(is_safe(s.poll(1201)));
  arm(2000);
  const std::uint8_t junk[3] = {1, 2, 3};
  CHECK(s.handle_message(junk, 3, 2201) == ParseStatus::BadSize);
  CHECK(is_safe(s.poll(2202)));
  arm(3000);
  send2(s, steer_cmd(1.0f, true, false), 3201);  // explicit disarm via enabled = 0
  CHECK(is_safe(s.poll(3202)));
  // After any of these, steering has to be armed again and starts from neutral.
  send2(s, steer_cmd(1.0f), 4000);
  CHECK(s.poll(4000).steering == 0.0f);
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
  test_v2_wire_format_and_validation();
  test_steering_gates_and_defaults();
  test_steering_clamp_and_rate_limit();
  test_steering_reversal_goes_through_zero();
  test_steering_large_poll_gaps_restart_from_neutral();
  test_steering_timeout_is_shorter_than_throttle_timeout();
  test_steering_safe_state_on_disarm_disconnect_invalid();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("core tests passed\n");
  return 0;
}
