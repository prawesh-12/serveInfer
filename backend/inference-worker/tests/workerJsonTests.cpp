#include "testHarness.h"

// Includes worker.cpp whole for its anonymous-namespace parsers, so worker.cpp must NOT
// also be listed as a source of this target.
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

const std::vector<std::string> kShippedLadder{"cuda", "npu", "ane", "cpu", "remote"};

EDGE_TEST(a_gpu_worker_keeps_the_whole_priority_order,
          "a worker given a gpu slot tries cuda, then npu, then ane, then cpu, then remote") {
  const std::vector<std::string> order = ladderFrom(kShippedLadder, false, false);
  CHECK_EQ(order.size(), static_cast<std::size_t>(5));
  CHECK_EQ(order[0], std::string("cuda"));
  CHECK_EQ(order[1], std::string("npu"));
  CHECK_EQ(order[2], std::string("ane"));
  CHECK_EQ(order[3], std::string("cpu"));
  CHECK_EQ(order[4], std::string("remote"));
}

EDGE_TEST(a_worker_with_no_gpu_slot_keeps_the_npu_and_the_ane,
          "the supervisor rations only cuda, so the other accelerators stay reachable") {
  const std::vector<std::string> order = ladderFrom(kShippedLadder, true, false);
  CHECK_EQ(order.size(), static_cast<std::size_t>(4));
  CHECK_EQ(order[0], std::string("npu"));
  CHECK_EQ(order[1], std::string("ane"));
  CHECK_EQ(order[2], std::string("cpu"));
  CHECK_EQ(order[3], std::string("remote"));
}

EDGE_TEST(force_cpu_drops_every_local_accelerator,
          "EDGE_FORCE_CPU leaves only the cpu and the cloud rung below it") {
  const std::vector<std::string> order = ladderFrom(kShippedLadder, false, true);
  CHECK_EQ(order.size(), static_cast<std::size_t>(2));
  CHECK_EQ(order[0], std::string("cpu"));
  CHECK_EQ(order[1], std::string("remote"));
}

EDGE_TEST(remote_is_always_the_last_rung,
          "the cloud fallback never sits above a local tier, whatever was dropped") {
  for (const bool noGpuSlot : {false, true}) {
    for (const bool forceCpu : {false, true}) {
      const std::vector<std::string> order = ladderFrom(kShippedLadder, noGpuSlot, forceCpu);
      CHECK_EQ(order.back(), std::string("remote"));
    }
  }
}

EDGE_TEST(a_ladder_naming_no_cpu_still_gets_one,
          "narrowing a ladder that never named the cpu still leaves a usable floor") {
  const std::vector<std::string> order = ladderFrom({"cuda", "npu"}, true, false);
  CHECK_EQ(order.size(), static_cast<std::size_t>(2));
  CHECK_EQ(order[0], std::string("npu"));
  CHECK_EQ(order[1], std::string("cpu"));
}
