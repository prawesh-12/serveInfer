#include "testHarness.h"

#include "../../supervisor/workerLiveness.h"

#include <string>

namespace {

LivenessLimits limits() {
  LivenessLimits out;
  out.startupGraceMs = 120000;
  out.heartbeatTimeoutMs = 15000;
  out.stuckRequestMs = 180000;
  return out;
}

constexpr long long kSpawn = 1'000'000;

WorkerHealth beating(long long lastHeartbeatMs, long long busyMs = 0) {
  return WorkerHealth{kSpawn, lastHeartbeatMs, busyMs};
}

}  // namespace

EDGE_TEST(a_worker_loading_the_model_is_starting_rather_than_silent,
          "nothing beats until the model is loaded, so silence inside the grace must not be read "
          "as a hang") {
  const WorkerHealth loading{kSpawn, 0, 0};
  CHECK(classifyWorker(kSpawn + 60000, loading, limits()) == WorkerLiveness::kStarting);
  CHECK(classifyWorker(kSpawn + 120000, loading, limits()) == WorkerLiveness::kStarting);
}

EDGE_TEST(a_worker_that_never_beats_is_declared_dead_once_the_grace_runs_out,
          "a process that came up but never reached its heartbeat loop cannot be left running "
          "forever") {
  const WorkerHealth loading{kSpawn, 0, 0};
  CHECK(classifyWorker(kSpawn + 120001, loading, limits()) == WorkerLiveness::kNoHeartbeat);
}

EDGE_TEST(a_worker_that_stops_beating_is_no_heartbeat,
          "the ordinary hang: the process is alive to waitpid but nothing is answering") {
  CHECK(classifyWorker(kSpawn + 20000, beating(kSpawn + 10000), limits()) == WorkerLiveness::kHealthy);
  CHECK(classifyWorker(kSpawn + 25001, beating(kSpawn + 10000), limits()) == WorkerLiveness::kNoHeartbeat);
}

EDGE_TEST(a_wedged_decode_is_caught_by_elapsed_request_time_not_by_silence,
          "the heartbeat runs on its own thread, so a worker stuck in the decode loop keeps "
          "beating; only the age of the request in flight gives it away") {
  const WorkerHealth wedged = beating(kSpawn + 300000, 180001);
  CHECK(classifyWorker(kSpawn + 300000, wedged, limits()) == WorkerLiveness::kStuckRequest);

  const WorkerHealth slowButMoving = beating(kSpawn + 300000, 179999);
  CHECK(classifyWorker(kSpawn + 300000, slowButMoving, limits()) == WorkerLiveness::kHealthy);
}

EDGE_TEST(an_idle_worker_is_never_stuck,
          "busyMs is 0 between requests, so an idle worker cannot trip the stuck ceiling however "
          "long it sits there") {
  CHECK(classifyWorker(kSpawn + 5'000'000, beating(kSpawn + 5'000'000, 0), limits()) ==
        WorkerLiveness::kHealthy);
}

EDGE_TEST(a_zero_limit_switches_that_check_off,
          "an operator debugging a worker under a breakpoint needs a way to stop the supervisor "
          "killing it") {
  LivenessLimits off = limits();
  off.heartbeatTimeoutMs = 0;
  off.stuckRequestMs = 0;
  CHECK(classifyWorker(kSpawn + 9'000'000, beating(kSpawn, 9'000'000), off) ==
        WorkerLiveness::kHealthy);

  LivenessLimits noGrace = limits();
  noGrace.startupGraceMs = 0;
  const WorkerHealth loading{kSpawn, 0, 0};
  CHECK(classifyWorker(kSpawn + 1, loading, noGrace) == WorkerLiveness::kStarting);
}

EDGE_TEST(the_heartbeat_frame_the_worker_sends_parses_back,
          "the supervisor has no JSON library, so the one frame it now reads has to survive a "
          "hand-rolled extractor") {
  int workerId = -1;
  long long busyMs = -1;

  CHECK(parseHeartbeat(R"({"type":"heartbeat","workerId":3,"status":"busy","device":"cuda","busyMs":4210})",
                       workerId, busyMs));
  CHECK_EQ(workerId, 3);
  CHECK_EQ(busyMs, 4210LL);

  CHECK(parseHeartbeat(R"({"type":"heartbeat","workerId":0,"status":"ready","device":"cpu","busyMs":0})",
                       workerId, busyMs));
  CHECK_EQ(workerId, 0);
  CHECK_EQ(busyMs, 0LL);
}

EDGE_TEST(a_frame_without_busy_ms_still_counts_as_a_heartbeat,
          "a worker built before this change reports liveness but no request age, and must not be "
          "killed for the missing field") {
  int workerId = -1;
  long long busyMs = -1;
  CHECK(parseHeartbeat(R"({"type":"heartbeat","workerId":2,"status":"ready","device":"cpu"})",
                       workerId, busyMs));
  CHECK_EQ(workerId, 2);
  CHECK_EQ(busyMs, 0LL);
}

EDGE_TEST(anything_that_is_not_a_heartbeat_is_ignored,
          "the supervisor socket is shared, so a malformed or unrelated line must not be read as a "
          "worker checking in") {
  int workerId = -1;
  long long busyMs = -1;
  CHECK(!parseHeartbeat("", workerId, busyMs));
  CHECK(!parseHeartbeat("not json at all", workerId, busyMs));
  CHECK(!parseHeartbeat(R"({"type":"worker_ready","workerId":1})", workerId, busyMs));
  CHECK(!parseHeartbeat(R"({"type":"heartbeat","status":"ready"})", workerId, busyMs));
  CHECK(!parseHeartbeat(R"({"type":"heartbeat","workerId":-4})", workerId, busyMs));
}

EDGE_TEST(every_verdict_has_its_own_name,
          "the kill line names the reason, and \"silent\" and \"wedged\" are different operator "
          "problems") {
  const std::string healthy = workerLivenessName(WorkerLiveness::kHealthy);
  const std::string starting = workerLivenessName(WorkerLiveness::kStarting);
  const std::string silent = workerLivenessName(WorkerLiveness::kNoHeartbeat);
  const std::string stuck = workerLivenessName(WorkerLiveness::kStuckRequest);
  CHECK(healthy != starting && silent != stuck && starting != silent);
}
