#include "testHarness.h"

#include "../deviceLadder.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// cpu is the only tier that passes its probe on every machine. So a test that
// needs two usable tiers just names cpu twice. The ladder does not remove
// duplicates. This keeps the escalation tests working with or without a GPU.
std::vector<std::string> twoUsableTiers() {
  return {"cpu", "cpu"};
}

std::string faultName(DeviceFault fault) {
  return deviceFaultName(fault);
}

void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A small JSON reader, just enough to prove toJson emits valid output.
// Returns the index after the value it read, or npos if the text is not JSON.
std::size_t skipValue(const std::string& text, std::size_t i);

std::size_t skipWhitespace(const std::string& text, std::size_t i) {
  while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')) {
    ++i;
  }
  return i;
}

std::size_t skipString(const std::string& text, std::size_t i) {
  if (i >= text.size() || text[i] != '"') {
    return std::string::npos;
  }
  ++i;
  while (i < text.size()) {
    if (text[i] == '\\') {
      i += 2;
      continue;
    }
    if (text[i] == '"') {
      return i + 1;
    }
    ++i;
  }
  return std::string::npos;
}

std::size_t skipNumber(const std::string& text, std::size_t i) {
  const std::size_t start = i;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    ++i;
  }
  while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
    ++i;
  }
  return i == start ? std::string::npos : i;
}

std::size_t skipLiteral(const std::string& text, std::size_t i, const char* literal) {
  const std::string word(literal);
  if (text.compare(i, word.size(), word) != 0) {
    return std::string::npos;
  }
  return i + word.size();
}

std::size_t skipContainer(const std::string& text, std::size_t i, char open, char close,
                          bool keyed) {
  if (i >= text.size() || text[i] != open) {
    return std::string::npos;
  }
  i = skipWhitespace(text, i + 1);
  if (i < text.size() && text[i] == close) {
    return i + 1;
  }
  for (;;) {
    i = skipWhitespace(text, i);
    if (keyed) {
      i = skipString(text, i);
      if (i == std::string::npos) {
        return std::string::npos;
      }
      i = skipWhitespace(text, i);
      if (i >= text.size() || text[i] != ':') {
        return std::string::npos;
      }
      i = skipWhitespace(text, i + 1);
    }
    i = skipValue(text, i);
    if (i == std::string::npos) {
      return std::string::npos;
    }
    i = skipWhitespace(text, i);
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    if (i < text.size() && text[i] == close) {
      return i + 1;
    }
    return std::string::npos;
  }
}

std::size_t skipValue(const std::string& text, std::size_t i) {
  i = skipWhitespace(text, i);
  if (i >= text.size()) {
    return std::string::npos;
  }
  switch (text[i]) {
    case '{':
      return skipContainer(text, i, '{', '}', true);
    case '[':
      return skipContainer(text, i, '[', ']', false);
    case '"':
      return skipString(text, i);
    case 't':
      return skipLiteral(text, i, "true");
    case 'f':
      return skipLiteral(text, i, "false");
    case 'n':
      return skipLiteral(text, i, "null");
    default:
      return skipNumber(text, i);
  }
}

bool isValidJson(const std::string& text) {
  const std::size_t end = skipValue(text, 0);
  return end != std::string::npos && skipWhitespace(text, end) == text.size();
}

bool hasKey(const std::string& json, const std::string& key) {
  return json.find("\"" + key + "\":") != std::string::npos;
}

}  // namespace

EDGE_TEST(parse_ladder_trims_and_drops_empties,
          "parseDeviceLadder trims whitespace and drops empty entries") {
  const std::vector<std::string> spaced = parseDeviceLadder("  cuda ,\tcpu  ");
  CHECK_EQ(spaced.size(), std::size_t{2});
  CHECK_EQ(spaced[0], std::string("cuda"));
  CHECK_EQ(spaced[1], std::string("cpu"));

  const std::vector<std::string> gappy = parseDeviceLadder("npu,,cuda,,,cpu,");
  CHECK_EQ(gappy.size(), std::size_t{3});
  CHECK_EQ(gappy[0], std::string("npu"));
  CHECK_EQ(gappy[2], std::string("cpu"));
}

EDGE_TEST(parse_ladder_defaults_to_cpu,
          "parseDeviceLadder falls back to cpu when the setting is empty") {
  CHECK_EQ(parseDeviceLadder("").size(), std::size_t{1});
  CHECK_EQ(parseDeviceLadder("")[0], std::string("cpu"));
  CHECK_EQ(parseDeviceLadder("  ,  , ")[0], std::string("cpu"));
}

EDGE_TEST(select_skips_tiers_whose_probe_fails,
          "select skips a tier that has no backend in this build") {
  DeviceLadder ladder({"npu", "ane", "cpu"}, 1000, 100);
  CHECK(ladder.select());
  CHECK_EQ(ladder.active(), std::string("cpu"));
  CHECK_EQ(ladder.activeIndex(), std::size_t{2});
}

EDGE_TEST(select_fails_when_no_tier_is_usable,
          "select reports failure and clears the active tier when nothing passes its probe") {
  DeviceLadder ladder({"npu", "ane", "metal"}, 1000, 100);
  CHECK(!ladder.select());
  CHECK(ladder.active().empty());
}

EDGE_TEST(baseline_is_not_degraded,
          "a machine running at its startup baseline is not degraded, even below tier 0") {
  DeviceLadder ladder({"npu", "cpu"}, 1000, 100);
  CHECK(ladder.select());
  CHECK_EQ(ladder.baselineIndex(), std::size_t{1});
  CHECK(!ladder.degraded());
  CHECK(ladder.degradedReason().empty());

  // Selecting again must not move the baseline underneath us.
  CHECK(ladder.select());
  CHECK(!ladder.degraded());
}

EDGE_TEST(fallback_sets_degraded_and_names_the_reason,
          "falling to a lower tier sets degraded and names the tier and the fault") {
  DeviceLadder ladder(twoUsableTiers(), 5000, 100);
  CHECK(ladder.select());
  CHECK_EQ(ladder.activeIndex(), std::size_t{0});

  CHECK(ladder.reportFault(DeviceFault::kRuntimeError, "simulated backend failure"));
  CHECK_EQ(ladder.activeIndex(), std::size_t{1});
  CHECK(ladder.degraded());
  CHECK_EQ(ladder.degradedReason(), std::string("cpu:runtime_error"));
}

EDGE_TEST(removed_is_session_fatal,
          "a removed device stays out for the session, even after its quarantine expires") {
  DeviceLadder ladder(twoUsableTiers(), 20, 100);
  CHECK(ladder.select());
  CHECK(ladder.reportFault(DeviceFault::kRemoved, "ERROR_DEVICE_REMOVED"));
  CHECK_EQ(ladder.activeIndex(), std::size_t{1});
  CHECK_EQ(ladder.degradedReason(), std::string("cpu:device_removed"));

  sleepMs(60);
  CHECK(ladder.select());
  CHECK_EQ(ladder.activeIndex(), std::size_t{1});
  CHECK(ladder.degraded());
}

EDGE_TEST(quarantined_tier_returns_after_the_window,
          "a quarantined tier comes back once its window closes and its probe passes") {
  DeviceLadder ladder(twoUsableTiers(), 20, 100);
  CHECK(ladder.select());
  CHECK(ladder.reportFault(DeviceFault::kUnsupportedOp, "kCMErrorUnsupportedOperation"));
  CHECK_EQ(ladder.activeIndex(), std::size_t{1});
  CHECK(ladder.degraded());

  // Still inside the window: the tier is not allowed back yet.
  CHECK(ladder.select());
  CHECK_EQ(ladder.activeIndex(), std::size_t{1});

  sleepMs(60);
  CHECK(ladder.select());
  CHECK_EQ(ladder.activeIndex(), std::size_t{0});
  CHECK(!ladder.degraded());
  CHECK(ladder.degradedReason().empty());
}

EDGE_TEST(no_lower_tier_keeps_the_active_one,
          "reportFault fails without clearing the active tier when there is nowhere to fall") {
  DeviceLadder ladder({"cpu"}, 1000, 100);
  CHECK(ladder.select());
  CHECK_EQ(ladder.active(), std::string("cpu"));

  CHECK(!ladder.reportFault(DeviceFault::kRuntimeError, "nowhere to go"));
  CHECK_EQ(ladder.active(), std::string("cpu"));
  CHECK_EQ(ladder.activeIndex(), std::size_t{0});
  CHECK(!ladder.degraded());
}

EDGE_TEST(report_fault_before_select_is_a_no_op,
          "reportFault does nothing when no tier has been selected yet") {
  DeviceLadder ladder({"cpu", "cpu"}, 1000, 100);
  CHECK(!ladder.reportFault(DeviceFault::kRemoved, "before select"));
  CHECK(ladder.active().empty());
}

EDGE_TEST(fault_names_are_stable, "every fault has a stable name for the wire format") {
  CHECK_EQ(faultName(DeviceFault::kNone), std::string("none"));
  CHECK_EQ(faultName(DeviceFault::kUnavailable), std::string("device_unavailable"));
  CHECK_EQ(faultName(DeviceFault::kRemoved), std::string("device_removed"));
  CHECK_EQ(faultName(DeviceFault::kUnsupportedOp), std::string("unsupported_operation"));
  CHECK_EQ(faultName(DeviceFault::kRuntimeError), std::string("runtime_error"));
}

EDGE_TEST(to_json_is_valid_and_complete,
          "toJson emits valid JSON carrying the ladder state and every tier") {
  DeviceLadder ladder({"npu", "cpu", "cpu"}, 5000, 100);
  CHECK(ladder.select());
  CHECK(ladder.reportFault(DeviceFault::kRemoved, "simulated"));

  const std::string json = ladder.toJson();
  CHECK(isValidJson(json));
  CHECK(hasKey(json, "active"));
  CHECK(hasKey(json, "degraded"));
  CHECK(hasKey(json, "reason"));
  CHECK(hasKey(json, "baseline"));
  CHECK(hasKey(json, "tiers"));
  CHECK(hasKey(json, "name"));
  CHECK(hasKey(json, "faults"));
  CHECK(hasKey(json, "sessionFatal"));
  CHECK(hasKey(json, "quarantineMsLeft"));
  CHECK(hasKey(json, "healthy"));
  CHECK(hasKey(json, "probe"));
  CHECK(hasKey(json, "platform"));
  CHECK(json.find("\"sessionFatal\":true") != std::string::npos);
  CHECK(json.find("\"degraded\":true") != std::string::npos);
}

EDGE_TEST(to_json_survives_an_unknown_tier_name,
          "toJson stays valid for a tier name this build does not know") {
  DeviceLadder ladder({"quantum", "cpu"}, 1000, 100);
  CHECK(ladder.select());
  const std::string json = ladder.toJson();
  CHECK(isValidJson(json));
  CHECK(json.find("\"platform\":\"unknown\"") != std::string::npos);
}
