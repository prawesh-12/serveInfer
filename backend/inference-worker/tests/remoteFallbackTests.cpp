#include "testHarness.h"

#include "../backendRouter.h"
#include "../deviceBackends.h"
#include "../deviceLadder.h"
#include "../inferenceBackend.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The remote tier over an injected fake transport, and recovery driven through route() only.

namespace {

class EnvGuard {
 public:
  explicit EnvGuard(const char* name) : name_(name) {
    const char* current = std::getenv(name);
    had_ = current != nullptr;
    if (had_) {
      previous_ = current;
    }
  }

  ~EnvGuard() {
    if (had_) {
      setenv(name_, previous_.c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  void set(const char* value) const {
    setenv(name_, value, 1);
  }

  void clear() const {
    unsetenv(name_);
  }

 private:
  const char* name_;
  bool had_ = false;
  std::string previous_;
};

struct RemotePolicy {
  EnvGuard endpoint{"EDGE_REMOTE_ENDPOINT"};
  EnvGuard allowed{"EDGE_REMOTE_FALLBACK_ALLOWED"};

  void allow() const {
    endpoint.set("https://example.invalid/v1/infer");
    allowed.set("1");
  }
  void endpointOnly() const {
    endpoint.set("https://example.invalid/v1/infer");
    allowed.clear();
  }
  void none() const {
    endpoint.clear();
    allowed.clear();
  }
};

// The injected fake transport; it never leaves the process.
struct FakeTransport {
  int calls = 0;
  std::string lastEndpoint;
  std::string lastPrompt;
  RemoteResponse reply;
  bool shouldThrow = false;

  RemoteTransport asTransport() {
    return [this](const RemoteRequest& request) {
      ++calls;
      lastEndpoint = request.endpoint;
      lastPrompt = request.prompt;
      if (shouldThrow) {
        throw std::runtime_error("socket exploded");
      }
      return reply;
    };
  }
};

class FakeLocalBackend : public InferenceBackend {
 public:
  explicit FakeLocalBackend(std::string name) : name_(std::move(name)) {}

  const std::string& name() const override {
    return name_;
  }
  ProbeResult available() override {
    ++probeCalls;
    return probeResult;
  }
  BackendExecution execute(const std::string& prompt, const TokenSink& onToken) override {
    ++executeCalls;
    BackendExecution result;
    if (nextFault != DeviceFault::kNone) {
      result.fault = nextFault;
      result.detail = name_ + " faulted";
      return result;
    }
    result.text = reply.empty() ? name_ + " ran: " + prompt : reply;
    if (onToken) {
      onToken(result.text);
    }
    return result;
  }
  bool healthCheck() override {
    return probeResult == ProbeResult::kAvailable;
  }
  bool executable() const override {
    return true;
  }

  ProbeResult probeResult = ProbeResult::kAvailable;
  DeviceFault nextFault = DeviceFault::kNone;
  std::string reply;
  int probeCalls = 0;
  int executeCalls = 0;

 private:
  std::string name_;
};

struct Rig {
  std::unique_ptr<BackendRouter> router;
  std::vector<std::shared_ptr<FakeLocalBackend>> locals;

  FakeLocalBackend& operator[](const std::string& name) const {
    for (const auto& backend : locals) {
      if (backend->name() == name) {
        return *backend;
      }
    }
    throw std::runtime_error("no fake backend named " + name);
  }
};

// Every tier except "remote" gets a fake; "remote" gets the real adapter with a fake transport.
Rig makeRig(const std::vector<std::string>& order, int quarantineMs, int probeIntervalMs,
            RemoteTransport transport) {
  Rig rig;
  rig.router = std::make_unique<BackendRouter>(order, quarantineMs, probeIntervalMs);
  for (const std::string& name : order) {
    if (name == "remote") {
      rig.router->registerBackend(std::make_shared<RemoteInferenceBackend>(transport));
      continue;
    }
    auto backend = std::make_shared<FakeLocalBackend>(name);
    rig.locals.push_back(backend);
    rig.router->registerBackend(backend);
  }
  return rig;
}

}  // namespace

EDGE_TEST(remote_unconfigured_is_an_explicit_unavailable_result,
          "an unconfigured remote tier probes runtime_missing and refuses by name, it does "
          "not fail as an unregistered tier (fake transport, no network)") {
  const RemotePolicy policy;
  policy.none();

  FakeTransport transport;
  transport.reply.status = 200;
  transport.reply.text = "this must never be reached";
  RemoteInferenceBackend remote(transport.asTransport());

  CHECK_EQ(std::string(probeResultName(remote.available())), std::string("runtime_missing"));

  const BackendExecution result = remote.execute("secret prompt", nullptr);
  CHECK(!result.ok());
  CHECK(result.fault == DeviceFault::kUnavailable);
  CHECK(result.detail.find("EDGE_REMOTE_ENDPOINT") != std::string::npos);
  CHECK(result.text.empty());
  CHECK_EQ(transport.calls, 0);
}

EDGE_TEST(remote_endpoint_without_the_opt_in_is_policy_disabled,
          "an endpoint alone is not consent: the tier reads policy_disabled and the prompt "
          "stays on the device (fake transport, no network)") {
  const RemotePolicy policy;
  policy.endpointOnly();

  FakeTransport transport;
  transport.reply.status = 200;
  transport.reply.text = "leaked";
  RemoteInferenceBackend remote(transport.asTransport());

  CHECK_EQ(std::string(probeResultName(remote.available())), std::string("policy_disabled"));

  const BackendExecution result = remote.execute("secret prompt", nullptr);
  CHECK(!result.ok());
  CHECK(result.fault == DeviceFault::kUnavailable);
  CHECK(result.detail.find("EDGE_REMOTE_FALLBACK_ALLOWED") != std::string::npos);
  CHECK_EQ(transport.calls, 0);
}

EDGE_TEST(remote_serves_when_it_is_configured_and_has_a_transport,
          "with both gates open and a transport installed, remote runs the prompt and "
          "returns the endpoint's text (fake transport, no network)") {
  const RemotePolicy policy;
  policy.allow();

  FakeTransport transport;
  transport.reply.status = 200;
  transport.reply.text = "cloud answer";
  RemoteInferenceBackend remote(transport.asTransport());

  CHECK_EQ(std::string(probeResultName(remote.available())), std::string("available"));
  CHECK(remote.executable());

  std::string streamed;
  const BackendExecution result = remote.execute("summarise this", [&](const std::string& token) {
    streamed += token;
  });
  CHECK(result.ok());
  CHECK_EQ(result.text, std::string("cloud answer"));
  CHECK_EQ(streamed, std::string("cloud answer"));
  CHECK_EQ(transport.calls, 1);
  CHECK_EQ(transport.lastPrompt, std::string("summarise this"));
  // The adapter passes EDGE_REMOTE_ENDPOINT through verbatim, never parsing or rewriting it.
  CHECK_EQ(transport.lastEndpoint, std::string("https://example.invalid/v1/infer"));
}

EDGE_TEST(remote_transport_failures_are_reported_honestly_by_class,
          "a failed remote call is classified, not swallowed: unreachable and 5xx are "
          "runtime errors, 4xx is unsupported or unavailable (fake transport, no network)") {
  const RemotePolicy policy;
  policy.allow();

  FakeTransport transport;
  RemoteInferenceBackend remote(transport.asTransport());

  transport.reply = RemoteResponse{};
  transport.reply.error = "connection refused";
  BackendExecution result = remote.execute("prompt", nullptr);
  CHECK(!result.ok());
  CHECK(result.fault == DeviceFault::kRuntimeError);
  CHECK_EQ(result.detail, std::string("connection refused"));

  transport.reply = RemoteResponse{};
  transport.reply.status = 503;
  result = remote.execute("prompt", nullptr);
  CHECK(result.fault == DeviceFault::kRuntimeError);
  CHECK_EQ(result.vendorStatus, 503L);
  CHECK(result.detail.find("503") != std::string::npos);

  transport.reply = RemoteResponse{};
  transport.reply.status = 422;
  result = remote.execute("prompt", nullptr);
  CHECK(result.fault == DeviceFault::kUnsupportedOp);

  transport.reply = RemoteResponse{};
  transport.reply.status = 401;
  result = remote.execute("prompt", nullptr);
  CHECK(result.fault == DeviceFault::kUnavailable);

  transport.shouldThrow = true;
  result = remote.execute("prompt", nullptr);
  CHECK(result.fault == DeviceFault::kRuntimeError);
  CHECK(result.detail.find("socket exploded") != std::string::npos);
}

EDGE_TEST(remote_status_mapping_never_produces_a_session_fatal_removal,
          "no HTTP status means the hardware vanished, so faultFromRemoteStatus never "
          "returns device_removed") {
  CHECK(faultFromRemoteStatus(200) == DeviceFault::kNone);
  CHECK(faultFromRemoteStatus(204) == DeviceFault::kNone);
  for (long status : {0L, 400L, 401L, 403L, 404L, 408L, 413L, 415L, 422L, 429L, 500L, 502L, 503L,
                      504L, 599L, -1L}) {
    CHECK(faultFromRemoteStatus(status) != DeviceFault::kRemoved);
    CHECK(faultFromRemoteStatus(status) != DeviceFault::kNone);
  }
}

EDGE_TEST(remote_is_only_reached_after_every_local_tier_has_failed,
          "remote is the last rung: it is untouched while any local tier works, and serves "
          "only once they have all faulted (fake transport, no network)") {
  const RemotePolicy policy;
  policy.allow();

  FakeTransport transport;
  transport.reply.status = 200;
  transport.reply.text = "cloud answer";

  Rig rig = makeRig({"ane", "metal", "cpu", "remote"}, 60000, 0, transport.asTransport());
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("ane"));

  CHECK(rig.router->route("local work", nullptr).ok());
  CHECK_EQ(transport.calls, 0);

  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["metal"].nextFault = DeviceFault::kRuntimeError;
  rig["cpu"].nextFault = DeviceFault::kRuntimeError;

  const RouteResult routed = rig.router->route("everything local is gone", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("remote"));
  CHECK_EQ(routed.text, std::string("cloud answer"));
  CHECK(routed.degraded);
  CHECK_EQ(transport.calls, 1);
  CHECK_EQ(transport.lastPrompt, std::string("everything local is gone"));
}

EDGE_TEST(a_configured_remote_tier_with_no_transport_falls_through_instead_of_lying,
          "the tier this build actually ships is configured-but-not-servable, so the router "
          "routes past it rather than crediting it with an answer") {
  const RemotePolicy policy;
  policy.allow();

  // makeRemoteTransport() is what Worker::selectDevice registers; it is empty in this build.
  RemoteInferenceBackend shipped(makeRemoteTransport());
  CHECK(!shipped.executable());
  CHECK_EQ(std::string(probeResultName(shipped.available())), std::string("runtime_missing"));
  const BackendExecution refused = shipped.execute("prompt", nullptr);
  CHECK(!refused.ok());
  CHECK(refused.fault == DeviceFault::kUnavailable);
  CHECK(refused.detail.find("no remote transport") != std::string::npos);
  CHECK(refused.text.empty());
}

EDGE_TEST(remote_registers_an_adapter_so_a_configured_tier_is_never_unrouted,
          "the tier has an adapter even when it cannot serve, so the failure names the "
          "reason instead of reading 'no backend adapter registered'") {
  const RemotePolicy policy;
  policy.allow();

  BackendRouter router({"remote"}, 60000, 0);
  router.registerBackend(std::make_shared<RemoteInferenceBackend>(makeRemoteTransport()));

  CHECK(!router.select());

  const RouteResult routed = router.route("prompt", nullptr);
  CHECK(!routed.ok());
  CHECK_EQ(routed.detail, std::string("no usable tier in the ladder"));
  CHECK(routed.detail.find("no backend adapter registered") == std::string::npos);
}

EDGE_TEST(remote_availability_uses_the_existing_state_vocabulary,
          "remote reports REMOTE_AVAILABLE, REMOTE_UNAVAILABLE and REMOTE_UNHEALTHY through "
          "the same backendAvailabilityState projection every other tier uses") {
  CHECK_EQ(backendAvailabilityState("remote", ProbeResult::kAvailable, DeviceFault::kNone, false,
                                    false),
           std::string("REMOTE_AVAILABLE"));
  CHECK_EQ(backendAvailabilityState("remote", ProbeResult::kRuntimeMissing, DeviceFault::kNone,
                                    false, false),
           std::string("REMOTE_UNAVAILABLE"));
  CHECK_EQ(backendAvailabilityState("remote", ProbeResult::kPolicyDisabled, DeviceFault::kNone,
                                    false, false),
           std::string("REMOTE_UNAVAILABLE"));
  CHECK_EQ(backendAvailabilityState("remote", ProbeResult::kAvailable,
                                    faultFromRemoteStatus(503), true, false),
           std::string("REMOTE_UNHEALTHY"));
  CHECK_EQ(backendAvailabilityState("remote", ProbeResult::kAvailable,
                                    faultFromRemoteStatus(422), true, false),
           std::string("REMOTE_UNSUPPORTED"));
}

EDGE_TEST(remote_state_reaches_the_ladder_json_for_health,
          "a live ladder reports the remote tier's state, so /health shows why it was "
          "skipped (fake transport, no network)") {
  const RemotePolicy policy;
  policy.endpointOnly();

  FakeTransport transport;
  transport.reply.status = 200;
  Rig rig = makeRig({"cpu", "remote"}, 60000, 0, transport.asTransport());
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cpu"));

  const std::string json = rig.router->ladder().toJson();
  CHECK(json.find("\"name\":\"remote\"") != std::string::npos);
  CHECK(json.find("\"probe\":\"policy_disabled\"") != std::string::npos);
  CHECK(json.find("REMOTE_UNAVAILABLE") != std::string::npos);
}

EDGE_TEST(the_remote_adapter_releases_its_session_on_every_path_out,
          "the remote adapter owns a transport session and gives it back after success, "
          "after failure and after a throw (fake transport, no network)") {
  const RemotePolicy policy;
  policy.allow();

  FakeTransport transport;
  transport.reply.status = 200;
  transport.reply.text = "ok";
  RemoteInferenceBackend remote(transport.asTransport());

  CHECK(!remote.deviceResourcesResident());
  CHECK(remote.execute("prompt", nullptr).ok());
  CHECK(!remote.deviceResourcesResident());

  transport.reply = RemoteResponse{};
  transport.reply.status = 500;
  CHECK(!remote.execute("prompt", nullptr).ok());
  CHECK(!remote.deviceResourcesResident());

  transport.shouldThrow = true;
  CHECK(!remote.execute("prompt", nullptr).ok());
  CHECK(!remote.deviceResourcesResident());

  // Unlike CUDA there is nothing here the process cannot free, so a move off remote needs no re-exec.
  CHECK(remote.releaseDeviceResources());
  CHECK(!remote.deviceResourcesResident());
  CHECK(!requiresProcessRestartForCpuOnly(remote, "remote", "cpu"));
}

// Everything below drives route() only: no case calls beginSession or puts a tier back by hand.

EDGE_TEST(a_quarantined_tier_comes_back_through_the_production_route_path,
          "a worker that fell to the cpu climbs back to cuda on a later request, once the "
          "quarantine has closed and the health check passes (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 40, 0, {});
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cuda"));

  rig["cuda"].nextFault = DeviceFault::kRuntimeError;
  rig["cpu"].reply = "cpu answer";
  RouteResult routed = rig.router->route("first", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK(routed.degraded);

  rig["cuda"].nextFault = DeviceFault::kNone;
  rig["cuda"].reply = "gpu answer";
  routed = rig.router->route("while quarantined", nullptr);
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK(routed.degraded);

  std::this_thread::sleep_for(std::chrono::milliseconds(90));

  routed = rig.router->route("after the window", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cuda"));
  CHECK_EQ(routed.text, std::string("gpu answer"));
  CHECK(!routed.degraded);
  CHECK_EQ(routed.degradedReason, std::string(""));
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("GPU_AVAILABLE"));
}

EDGE_TEST(a_tier_whose_health_check_still_fails_is_not_brought_back,
          "the closing quarantine is not the gate, the health check is: a tier that still "
          "probes missing stays out however many requests arrive (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 20, 0, {});
  CHECK(rig.router->select());
  rig["cuda"].nextFault = DeviceFault::kRuntimeError;
  CHECK_EQ(rig.router->route("first", nullptr).device, std::string("cpu"));

  rig["cuda"].nextFault = DeviceFault::kNone;
  rig["cuda"].probeResult = ProbeResult::kDeviceMissing;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  const int executesAtFault = rig["cuda"].executeCalls;
  for (int i = 0; i < 5; ++i) {
    const RouteResult routed = rig.router->route("later", nullptr);
    CHECK(routed.ok());
    CHECK_EQ(routed.device, std::string("cpu"));
    CHECK(routed.degraded);
  }
  CHECK_EQ(rig["cuda"].executeCalls, executesAtFault);

  rig["cuda"].probeResult = ProbeResult::kAvailable;
  const RouteResult recovered = rig.router->route("finally", nullptr);
  CHECK_EQ(recovered.device, std::string("cuda"));
  CHECK(!recovered.degraded);
}

EDGE_TEST(a_removed_tier_never_returns_within_the_worker_process,
          "ERROR_DEVICE_REMOVED stays unrecoverable for the session: no number of "
          "requests brings it back, only a new process does (fake, no hardware)") {
  // Quarantine 0 makes the window irrelevant, so only sessionFatal keeps the tier out.
  Rig rig = makeRig({"npu", "cpu"}, 0, 0, {});
  CHECK(rig.router->select());
  rig["npu"].nextFault = DeviceFault::kRemoved;
  CHECK(rig.router->route("first", nullptr).ok());

  rig["npu"].nextFault = DeviceFault::kNone;
  rig["npu"].probeResult = ProbeResult::kAvailable;
  const int executesAtRemoval = rig["npu"].executeCalls;
  for (int i = 0; i < 10; ++i) {
    const RouteResult routed = rig.router->route("later", nullptr);
    CHECK(routed.ok());
    CHECK_EQ(routed.device, std::string("cpu"));
  }
  CHECK_EQ(rig["npu"].executeCalls, executesAtRemoval);
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("NPU_UNHEALTHY"));

  // Only the session boundary clears it, and only a new worker process is one.
  rig.router->beginSession();
  CHECK_EQ(rig.router->route("new session", nullptr).device, std::string("npu"));
}

EDGE_TEST(recovery_rebuilds_the_engine_through_the_same_fallback_hook,
          "an upward move fires the one FallbackHook a downward move fires, so the worker "
          "reloads its engine on the restored tier (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 20, 0, {});
  std::vector<std::string> moves;
  rig.router->setFallbackHook([&](const std::string& previous, const std::string& next) {
    moves.push_back(previous + "->" + next);
    return true;
  });
  CHECK(rig.router->select());

  rig["cuda"].nextFault = DeviceFault::kRuntimeError;
  CHECK_EQ(rig.router->route("first", nullptr).device, std::string("cpu"));
  CHECK_EQ(moves.size(), std::size_t{1});
  CHECK_EQ(moves[0], std::string("cuda->cpu"));

  rig["cuda"].nextFault = DeviceFault::kNone;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK_EQ(rig.router->route("after the window", nullptr).device, std::string("cuda"));

  CHECK_EQ(moves.size(), std::size_t{2});
  CHECK_EQ(moves[1], std::string("cpu->cuda"));
}

EDGE_TEST(a_recovered_tier_that_will_not_come_up_is_benched_again_and_the_request_still_serves,
          "a health check can pass while the engine still fails to load; the tier goes back "
          "into quarantine through reportFault and the request is answered (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 20, 0, {});
  bool refuseUpward = false;
  std::vector<std::string> moves;
  rig.router->setFallbackHook([&](const std::string& previous, const std::string& next) {
    moves.push_back(previous + "->" + next);
    if (refuseUpward && next == "cuda") {
      return false;
    }
    return true;
  });
  CHECK(rig.router->select());

  rig["cuda"].nextFault = DeviceFault::kRuntimeError;
  CHECK_EQ(rig.router->route("first", nullptr).device, std::string("cpu"));

  rig["cuda"].nextFault = DeviceFault::kNone;
  refuseUpward = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  const RouteResult routed = rig.router->route("second", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));
  CHECK(routed.degraded);
  CHECK_EQ(rig.router->ladder().tierState(0), std::string("GPU_UNHEALTHY"));
  const RouteResult next = rig.router->route("third", nullptr);
  CHECK_EQ(next.device, std::string("cpu"));
  CHECK_EQ(rig["cuda"].executeCalls, 1);
}

EDGE_TEST(recovery_does_not_probe_a_quarantined_tier_once_per_request,
          "a benched tier costs a clock read per request and no probe at all while its "
          "quarantine is open (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 60000, 0, {});
  CHECK(rig.router->select());
  rig["cuda"].nextFault = DeviceFault::kRuntimeError;
  CHECK_EQ(rig.router->route("first", nullptr).device, std::string("cpu"));

  const int probesAtFault = rig["cuda"].probeCalls;
  for (int i = 0; i < 20; ++i) {
    CHECK(rig.router->route("prompt", nullptr).ok());
  }
  CHECK_EQ(rig["cuda"].probeCalls, probesAtFault);
}

EDGE_TEST(recovery_never_probes_a_tier_this_machine_simply_does_not_have,
          "a tier that never faulted is not a recovery candidate, so an absent gpu is "
          "not re-probed once per request either (fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 60000, 0, {});
  rig["cuda"].probeResult = ProbeResult::kDeviceMissing;
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cpu"));
  CHECK(!rig.router->ladder().degraded());

  const int probesAtStart = rig["cuda"].probeCalls;
  for (int i = 0; i < 20; ++i) {
    CHECK(rig.router->route("prompt", nullptr).ok());
  }
  CHECK_EQ(rig["cuda"].probeCalls, probesAtStart);
}

EDGE_TEST(recovery_costs_nothing_on_the_healthy_path,
          "a worker sitting on the best tier it has does no probing between requests "
          "(fake, no hardware)") {
  Rig rig = makeRig({"cuda", "cpu"}, 60000, 0, {});
  CHECK(rig.router->select());
  CHECK_EQ(rig.router->active(), std::string("cuda"));

  const int cudaProbes = rig["cuda"].probeCalls;
  const int cpuProbes = rig["cpu"].probeCalls;
  for (int i = 0; i < 20; ++i) {
    CHECK(rig.router->route("prompt", nullptr).ok());
  }
  CHECK_EQ(rig["cuda"].probeCalls, cudaProbes);
  CHECK_EQ(rig["cpu"].probeCalls, cpuProbes);
}

EDGE_TEST(recovery_walks_back_up_one_rung_at_a_time_and_stops_at_the_best_healthy_tier,
          "with two tiers down, the request path restores the best one whose quarantine has "
          "closed, not merely the next one up (fake, no hardware)") {
  Rig rig = makeRig({"ane", "metal", "cpu"}, 25, 0, {});
  CHECK(rig.router->select());
  rig["ane"].nextFault = faultFromCoreMediaStatus(kCMErrorUnsupportedOperation);
  rig["metal"].nextFault = DeviceFault::kRuntimeError;
  CHECK_EQ(rig.router->route("first", nullptr).device, std::string("cpu"));

  rig["metal"].nextFault = DeviceFault::kNone;
  std::this_thread::sleep_for(std::chrono::milliseconds(70));

  const RouteResult routed = rig.router->route("second", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("metal"));
  CHECK_EQ(rig["ane"].executeCalls, 2);

  rig["ane"].nextFault = DeviceFault::kNone;
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  const RouteResult best = rig.router->route("third", nullptr);
  CHECK_EQ(best.device, std::string("ane"));
  CHECK(!best.degraded);
}

EDGE_TEST(a_recovered_remote_tier_is_reached_through_the_same_request_path,
          "remote is quarantined by a 503 and served again once the window closes and "
          "its policy gate still passes (fake transport, no network)") {
  const RemotePolicy policy;
  policy.allow();

  FakeTransport transport;
  transport.reply.status = 503;
  Rig rig = makeRig({"cpu", "remote"}, 25, 0, transport.asTransport());
  CHECK(rig.router->select());

  rig["cpu"].nextFault = DeviceFault::kRuntimeError;
  RouteResult routed = rig.router->route("first", nullptr);
  CHECK(!routed.ok());
  CHECK_EQ(rig.router->ladder().tierState(1), std::string("REMOTE_UNHEALTHY"));

  rig["cpu"].nextFault = DeviceFault::kNone;
  rig["cpu"].reply = "local again";
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  routed = rig.router->route("second", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("cpu"));

  transport.reply = RemoteResponse{};
  transport.reply.status = 200;
  transport.reply.text = "cloud again";
  rig["cpu"].nextFault = DeviceFault::kRuntimeError;
  routed = rig.router->route("third", nullptr);
  CHECK(routed.ok());
  CHECK_EQ(routed.device, std::string("remote"));
  CHECK_EQ(routed.text, std::string("cloud again"));
}

EDGE_TEST(a_tier_change_can_be_told_apart_from_a_fallback_by_ladder_position,
          "indexOf is what lets Worker::onTierChanged distinguish a recovery from a "
          "fallback, so an upward move is never charged the re-exec a device-class drop is") {
  // The bug this guards: remote->cpu recovery looks like a cuda->cpu fallback on tier names alone.
  DeviceLadder ladder({"cuda", "cpu", "remote"}, 60000, 0);
  CHECK_EQ(ladder.indexOf("cuda"), std::size_t{0});
  CHECK_EQ(ladder.indexOf("cpu"), std::size_t{1});
  CHECK_EQ(ladder.indexOf("remote"), std::size_t{2});
  CHECK_EQ(ladder.indexOf("not-a-tier"), ladder.tierCount());

  CHECK(ladder.indexOf("cpu") > ladder.indexOf("cuda"));
  CHECK(ladder.indexOf("cpu") < ladder.indexOf("remote"));
}
