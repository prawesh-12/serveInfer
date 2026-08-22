#include "testHarness.h"

#include "../../ipc/modelReady.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using EdgeIPC::evaluateModelHeader;
using EdgeIPC::generateRunNonce;
using EdgeIPC::ModelReadyState;

constexpr std::uint64_t kPreviousRun = 0x1111'2222'3333'4444ULL;
constexpr std::uint64_t kThisRun = 0xAAAA'BBBB'CCCC'DDDDULL;

// What ModelCache::claimHeader writes the moment it owns the metadata segment.
SharedModelHeader claimed(std::uint64_t nonce) {
  SharedModelHeader header{};
  std::memcpy(header.magic, "EDGE", 4);
  header.version = kSharedModelHeaderVersion;
  header.runNonce = nonce;
  header.ready = 0;
  return header;
}

// What the same header looks like once the bytes are copied and validated.
SharedModelHeader published(std::uint64_t nonce, std::uint64_t modelSize = 2393231072ULL) {
  SharedModelHeader header = claimed(nonce);
  header.modelSize = modelSize;
  header.checksum = 0x33227261'21541147ULL;
  header.loadedAt = 1787415134;
  header.ready = 1;
  return header;
}

ModelReadyState evaluate(const SharedModelHeader& header, std::uint64_t expectedNonce) {
  return evaluateModelHeader(&header, sizeof(header), expectedNonce);
}

}  // namespace

EDGE_TEST(a_ready_flag_left_by_the_previous_run_is_never_accepted,
          "the shm objects outlive the run that made them, so a leftover ready=1 must read as "
          "another run's business and never start workers on it") {
  const SharedModelHeader stale = published(kPreviousRun);

  CHECK(evaluate(stale, kThisRun) == ModelReadyState::kForeignRun);
  CHECK(evaluate(stale, kPreviousRun) == ModelReadyState::kReady);
}

EDGE_TEST(a_fresh_nonce_does_not_inherit_the_old_ready_byte,
          "every start draws a new nonce, so the header the last cache published is foreign to "
          "the next supervisor no matter how ready it claims to be") {
  const SharedModelHeader stale = published(kPreviousRun);

  for (int start = 0; start < 3; ++start) {
    const std::uint64_t nonce = generateRunNonce();
    CHECK(nonce != kPreviousRun);
    CHECK(evaluate(stale, nonce) == ModelReadyState::kForeignRun);
  }
}

EDGE_TEST(this_runs_header_is_not_ready_until_the_bytes_are_in,
          "the cache stamps its nonce before it copies anything, so a matching header with "
          "ready=0 means keep waiting rather than give up") {
  CHECK(evaluate(claimed(kThisRun), kThisRun) == ModelReadyState::kNotReady);
}

EDGE_TEST(this_runs_header_is_ready_once_the_flag_is_set,
          "nonce match plus ready=1 is the only combination that releases the workers") {
  CHECK(evaluate(published(kThisRun), kThisRun) == ModelReadyState::kReady);
}

EDGE_TEST(a_missing_or_truncated_header_is_malformed_rather_than_ready,
          "a metadata segment that is absent or shorter than the struct must keep the supervisor "
          "polling, not crash it and not release the workers") {
  const SharedModelHeader header = published(kThisRun);

  CHECK(evaluateModelHeader(nullptr, sizeof(header), kThisRun) == ModelReadyState::kMalformed);
  CHECK(evaluateModelHeader(&header, 0, kThisRun) == ModelReadyState::kMalformed);
  CHECK(evaluateModelHeader(&header, sizeof(header) - 1, kThisRun) == ModelReadyState::kMalformed);
}

EDGE_TEST(a_header_without_the_edge_magic_is_malformed,
          "a zeroed or reused segment that happens to carry the right nonce bytes is still not a "
          "header this stack wrote") {
  SharedModelHeader corrupt = published(kThisRun);
  std::memcpy(corrupt.magic, "XXXX", 4);
  CHECK(evaluate(corrupt, kThisRun) == ModelReadyState::kMalformed);

  const SharedModelHeader zeroed{};
  CHECK(evaluate(zeroed, kThisRun) == ModelReadyState::kMalformed);
}

EDGE_TEST(a_zero_nonce_matches_nothing_including_a_zeroed_header,
          "a truncated or never-stamped header reads as nonce 0, so 0 must never be a value the "
          "supervisor can be waiting on") {
  SharedModelHeader unstamped = published(0);
  CHECK(evaluate(unstamped, 0) == ModelReadyState::kMalformed);
  CHECK(evaluate(published(kThisRun), 0) == ModelReadyState::kMalformed);
}

EDGE_TEST(every_generated_nonce_is_unique_and_never_zero,
          "the nonce is the whole handshake, so a repeat or a 0 would hand back the bug it exists "
          "to close") {
  std::set<std::uint64_t> seen;
  for (int i = 0; i < 256; ++i) {
    const std::uint64_t nonce = generateRunNonce();
    CHECK(nonce != 0);
    CHECK(seen.insert(nonce).second);
  }
}

EDGE_TEST(the_startup_sequence_reaches_ready_exactly_once_and_only_for_this_run,
          "walked in the order model-cache writes it: a stale segment, then claimed, then "
          "published, judged by both the old supervisor's nonce and this one's") {
  std::vector<ModelReadyState> thisRun;
  std::vector<ModelReadyState> previousRun;

  for (const SharedModelHeader& step :
       {published(kPreviousRun), claimed(kThisRun), published(kThisRun)}) {
    thisRun.push_back(evaluate(step, kThisRun));
    previousRun.push_back(evaluate(step, kPreviousRun));
  }

  CHECK(thisRun[0] == ModelReadyState::kForeignRun);
  CHECK(thisRun[1] == ModelReadyState::kNotReady);
  CHECK(thisRun[2] == ModelReadyState::kReady);

  CHECK(previousRun[0] == ModelReadyState::kReady);
  CHECK(previousRun[1] == ModelReadyState::kForeignRun);
  CHECK(previousRun[2] == ModelReadyState::kForeignRun);
}

EDGE_TEST(the_nonce_came_out_of_reserved_space_and_moved_nothing,
          "workers and the supervisor read this struct from shared memory, so every field that "
          "existed before the nonce has to sit at the offset it always did") {
  CHECK_EQ(sizeof(SharedModelHeader), static_cast<std::size_t>(256));
  CHECK_EQ(offsetof(SharedModelHeader, magic), static_cast<std::size_t>(0));
  CHECK_EQ(offsetof(SharedModelHeader, modelSize), static_cast<std::size_t>(8));
  CHECK_EQ(offsetof(SharedModelHeader, checksum), static_cast<std::size_t>(16));
  CHECK_EQ(offsetof(SharedModelHeader, loadedAt), static_cast<std::size_t>(24));
  CHECK_EQ(offsetof(SharedModelHeader, version), static_cast<std::size_t>(32));
  CHECK_EQ(offsetof(SharedModelHeader, ready), static_cast<std::size_t>(36));
  CHECK_EQ(offsetof(SharedModelHeader, runNonce), static_cast<std::size_t>(40));
}

EDGE_TEST(the_state_names_are_distinct_so_a_log_line_says_which_gate_failed,
          "\"waiting\" and \"someone else's flag\" are different operator problems") {
  std::set<std::string> names;
  for (const ModelReadyState state : {ModelReadyState::kMalformed, ModelReadyState::kForeignRun,
                                      ModelReadyState::kNotReady, ModelReadyState::kReady}) {
    names.insert(EdgeIPC::modelReadyStateName(state));
  }
  CHECK_EQ(names.size(), static_cast<std::size_t>(4));
}
