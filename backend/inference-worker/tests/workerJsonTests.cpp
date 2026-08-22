#include "testHarness.h"

// extractString and extractBool sit in an anonymous namespace inside
// worker.cpp. Nothing outside that file can see them.
//
// So this test includes worker.cpp whole. That also means worker.cpp must NOT be
// listed as a source of this build target. If it were, every symbol in it would
// be defined twice and the link would fail.
#include "../worker.cpp"

#include <string>

namespace {

std::string extracted(const std::string& json, const std::string& key) {
  std::string out;
  return extractString(json, key, out) ? out : std::string("<missing>");
}

}  // namespace

EDGE_TEST(extracts_plain_string_fields,
          "extractString reads the fields the api-server actually sends") {
  const std::string frame =
      R"({"type":"infer","requestId":"smoke-1","prompt":"What is 2+2?","mfeId":"doc-qa","stream":false})";
  CHECK_EQ(extracted(frame, "type"), std::string("infer"));
  CHECK_EQ(extracted(frame, "requestId"), std::string("smoke-1"));
  CHECK_EQ(extracted(frame, "prompt"), std::string("What is 2+2?"));
  CHECK_EQ(extracted(frame, "mfeId"), std::string("doc-qa"));
}

EDGE_TEST(missing_key_is_reported_not_guessed,
          "extractString reports a missing key instead of returning an empty value") {
  std::string out = "untouched";
  CHECK(!extractString(R"({"requestId":"a"})", "prompt", out));
  CHECK_EQ(out, std::string("untouched"));
}

EDGE_TEST(key_match_is_anchored_on_the_quotes,
          "a key is matched whole, so requestId is not read as id") {
  std::string out;
  CHECK(!extractString(R"({"requestId":"abc"})", "id", out));
}

EDGE_TEST(whitespace_around_the_colon_is_allowed,
          "extractString tolerates whitespace around the colon") {
  CHECK_EQ(extracted(R"({ "prompt"   :    "spaced out" })", "prompt"), std::string("spaced out"));
}

EDGE_TEST(escapes_are_turned_back_into_characters,
          "extractString unescapes newlines, tabs, quotes and backslashes") {
  const std::string frame = R"({"prompt":"line one\nline two\tend"})";
  CHECK_EQ(extracted(frame, "prompt"), std::string("line one\nline two\tend"));

  const std::string quoted = R"({"prompt":"he said \"hi\" once"})";
  CHECK_EQ(extracted(quoted, "prompt"), std::string("he said \"hi\" once"));

  const std::string slashed = R"({"prompt":"path C:\\temp"})";
  CHECK_EQ(extracted(slashed, "prompt"), std::string("path C:\\temp"));

  const std::string carriage = R"({"prompt":"a\rb"})";
  CHECK_EQ(extracted(carriage, "prompt"), std::string("a\rb"));
}

EDGE_TEST(an_escaped_quote_does_not_end_the_value_early,
          "a prompt whose text ends in an escaped quote is read whole") {
  const std::string frame = R"({"prompt":"ends with a quote \"","requestId":"r-9"})";
  CHECK_EQ(extracted(frame, "prompt"), std::string("ends with a quote \""));
  CHECK_EQ(extracted(frame, "requestId"), std::string("r-9"));
}

EDGE_TEST(json_punctuation_inside_a_prompt_is_kept,
          "braces, colons and commas inside a prompt survive extraction") {
  const std::string frame = R"({"prompt":"summarise {a: 1, b: 2} please","requestId":"r-1"})";
  CHECK_EQ(extracted(frame, "prompt"), std::string("summarise {a: 1, b: 2} please"));
  CHECK_EQ(extracted(frame, "requestId"), std::string("r-1"));
}

EDGE_TEST(regex_extraction_has_no_idea_about_nesting,
          "a key nested inside another object is read as if it were top level") {
  // The worker parses frames with a regex, not a JSON library, so a nested key
  // is read as if it were top level. Nothing sends nested frames today. This
  // test records the limit so a future change does not hit it by surprise.
  const std::string frame = R"({"meta":{"requestId":"inner"},"requestId":"outer"})";
  CHECK_EQ(extracted(frame, "requestId"), std::string("inner"));
}

EDGE_TEST(booleans_are_read_and_default_when_absent,
          "extractBool reads true and false, and falls back to the default when the key is absent") {
  CHECK_EQ(extractBool(R"({"stream":true})", "stream", false), true);
  CHECK_EQ(extractBool(R"({"stream":false})", "stream", true), false);
  CHECK_EQ(extractBool(R"({"stream" :  true })", "stream", false), true);
  CHECK_EQ(extractBool(R"({"requestId":"a"})", "stream", false), false);
  CHECK_EQ(extractBool(R"({"requestId":"a"})", "stream", true), true);
}

EDGE_TEST(a_quoted_boolean_is_not_a_boolean,
          "a string that says true is not accepted as a boolean") {
  CHECK_EQ(extractBool(R"({"stream":"true"})", "stream", false), false);
}
