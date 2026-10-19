// Host tests of the placeholders of an HTTP step (lib/http_template): pio test -e native
#include <unity.h>

#include <cstring>
#include <string>

#include "http_template.h"

using HttpTemplate::Field;
using HttpTemplate::Ref;

namespace {

bool valid(const char* text, bool url = false, bool hasWhen = true) {
  std::string err;
  return HttpTemplate::validate(text, strlen(text), url, hasWhen, err);
}

std::string error(const char* text, bool url = false, bool hasWhen = true) {
  std::string err;
  TEST_ASSERT_FALSE(HttpTemplate::validate(text, strlen(text), url, hasWhen, err));
  return err;
}

// Names every placeholder by what it refers to, so a test sees what was asked.
std::string names(const Ref& r) {
  static const char* kFields[] = {"power", "current", "voltage", "energy", "relay"};
  switch (r.kind) {
    case Ref::Kind::Device: return "<" + r.device + ":" + kFields[static_cast<int>(r.field)] + ">";
    case Ref::Kind::When: return std::string("<when:") + kFields[static_cast<int>(r.field)] + ">";
    case Ref::Kind::Time: return "<time>";
    case Ref::Kind::Scenario: return "<scenario>";
    case Ref::Kind::Trigger: return "<trigger>";
  }
  return "?";
}

std::string expanded(const char* text, size_t max = 1000) {
  std::string out;
  TEST_ASSERT_TRUE(HttpTemplate::expand(text, strlen(text), names, max, out));
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_every_placeholder_is_recognised() {
  TEST_ASSERT_EQUAL_STRING("<heater1:power> <tw-2_x:current> <a:voltage> <a:energy> <a:relay>",
                           expanded("{{heater1.power}} {{tw-2_x.current}} {{a.voltage}} {{a.energy}} {{a.relay}}").c_str());
  TEST_ASSERT_EQUAL_STRING("<when:power>/<time>/<scenario>/<trigger>", expanded("{{when.power}}/{{time}}/{{scenario}}/{{trigger}}").c_str());
}

void test_json_braces_are_left_alone() {
  const char* body = "{\"chat_id\":1,\"text\":\"boiler {{when.power}} W\",\"o\":{\"a\":{\"b\":1}}}";
  TEST_ASSERT_TRUE(valid(body));
  TEST_ASSERT_EQUAL_STRING("{\"chat_id\":1,\"text\":\"boiler <when:power> W\",\"o\":{\"a\":{\"b\":1}}}", expanded(body).c_str());
  TEST_ASSERT_TRUE(valid("no placeholders at all, } { }} {"));
}

void test_unknown_or_broken_placeholders_are_errors() {
  TEST_ASSERT_TRUE(error("x {{heater1.watts}} y").find("unknown placeholder at byte 2") != std::string::npos);
  error("{{heater1}}");
  error("{{Heater1.power}}");
  error("{{.power}}");
  error("{{ heater1.power }}");
  error("{{time");
  error("{{date}}");
  error("{{abcdefghijklmnopqrstuvwxyz0123456.power}}");  // a name of 33 characters
  TEST_ASSERT_TRUE(valid("{{abcdefghijklmnopqrstuvwxyz012345.power}}"));  // 32
}

void test_when_needs_a_trigger_device() {
  TEST_ASSERT_TRUE(valid("{{when.relay}}", false, true));
  TEST_ASSERT_TRUE(error("{{when.relay}}", false, false).find("`when` device") != std::string::npos);
  TEST_ASSERT_TRUE(valid("{{heater1.relay}} {{time}}", false, false));
}

void test_no_placeholder_in_the_host_of_a_url() {
  TEST_ASSERT_TRUE(valid("https://hc-ping.com/uuid?w={{heater1.power}}&t={{time}}", true));
  TEST_ASSERT_TRUE(valid("https://example.com/{{scenario}}/x", true));
  TEST_ASSERT_TRUE(error("https://{{scenario}}.example.com/x", true).find("in the host") != std::string::npos);
  error("http://example.com:{{heater1.power}}/x", true);
  TEST_ASSERT_TRUE(valid("https://{{scenario}}.example.com/x", false));  // a body has no host
}

void test_units() {
  TEST_ASSERT_EQUAL_STRING("106.7", HttpTemplate::format(Field::Power, 1067).c_str());
  TEST_ASSERT_EQUAL_STRING("0.0", HttpTemplate::format(Field::Power, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("229.5", HttpTemplate::format(Field::Voltage, 2295).c_str());
  TEST_ASSERT_EQUAL_STRING("0.452", HttpTemplate::format(Field::Current, 452).c_str());
  TEST_ASSERT_EQUAL_STRING("12.345", HttpTemplate::format(Field::Energy, 12345).c_str());
  TEST_ASSERT_EQUAL_STRING("0.007", HttpTemplate::format(Field::Energy, 7).c_str());
  TEST_ASSERT_EQUAL_STRING("-0.5", HttpTemplate::format(Field::Power, -5).c_str());
  TEST_ASSERT_EQUAL_STRING("-", HttpTemplate::format(Field::Power, HttpTemplate::kNone).c_str());
  TEST_ASSERT_EQUAL_STRING("on", HttpTemplate::format(Field::Relay, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("off", HttpTemplate::format(Field::Relay, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("-", HttpTemplate::format(Field::Relay, -1).c_str());
}

void test_the_result_is_limited() {
  std::string out;
  const char* text = "abc{{time}}";
  TEST_ASSERT_TRUE(HttpTemplate::expand(text, strlen(text), names, 9, out));  // "abc<time>"
  TEST_ASSERT_FALSE(HttpTemplate::expand(text, strlen(text), names, 8, out));
  TEST_ASSERT_FALSE(HttpTemplate::expand("abcdefgh", 8, names, 7, out));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_placeholder_is_recognised);
  RUN_TEST(test_json_braces_are_left_alone);
  RUN_TEST(test_unknown_or_broken_placeholders_are_errors);
  RUN_TEST(test_when_needs_a_trigger_device);
  RUN_TEST(test_no_placeholder_in_the_host_of_a_url);
  RUN_TEST(test_units);
  RUN_TEST(test_the_result_is_limited);
  return UNITY_END();
}
