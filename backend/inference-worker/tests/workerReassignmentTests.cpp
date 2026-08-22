// Drives applyWorkerReassignment, the same function the supervisor calls; only the fork is faked.

#include "testHarness.h"

#include "../../supervisor/workerReassignment.h"

#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr bool kReassignExit = true;
constexpr bool kRunning = true;

std::vector<WorkerAssignment> oneCudaWorker() {
  std::vector<WorkerAssignment> assignments;
  WorkerAssignment cuda;
  cuda.workerId = 0;
  cuda.backend = WorkerBackend::kCuda;
  cuda.reason = "gpu slot 0";
  assignments.push_back(cuda);
  WorkerAssignment cpu;
  cpu.workerId = 1;
  cpu.backend = WorkerBackend::kCpu;
  cpu.reason = "no gpu budget left";
  assignments.push_back(cpu);
  return assignments;
}

struct Recorder {
  bool startResult = true;
  int startCalls = 0;
  int crashNotices = 0;
  int restartNotices = 0;

  ReassignmentHooks hooks() {
    ReassignmentHooks h;
    h.startWorker = [this](int) {
      startCalls += 1;
      return startResult;
    };
    h.notifyCrash = [this](int) { crashNotices += 1; };
    h.notifyRestarted = [this](int) { restartNotices += 1; };
    return h;
  }
};

}  // namespace

EDGE_TEST(reassignment_succeeds_when_the_replacement_worker_starts,
          "startWorker succeeds -> reassignment succeeds, the worker is demoted to cpu and the "
          "api-server is told it came back") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  recorder.startResult = true;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 0, kReassignExit, kRunning, recorder.hooks(), log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("restarted"));
  CHECK_EQ(recorder.startCalls, 1);
  CHECK_EQ(recorder.crashNotices, 1);
  CHECK_EQ(recorder.restartNotices, 1);
  CHECK(assignments[0].backend == WorkerBackend::kCpu);
  CHECK(assignments[0].reason.find("device fault on cuda") != std::string::npos);
  CHECK_EQ(assignments[1].workerId, 1);
  CHECK(assignments[1].backend == WorkerBackend::kCpu);
  CHECK_EQ(assignments[1].reason, std::string("no gpu budget left"));
}

EDGE_TEST(reassignment_fails_when_the_replacement_worker_does_not_start,
          "startWorker fails -> reassignment fails, so the caller falls through to crash handling "
          "instead of losing the worker silently") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  recorder.startResult = false;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 0, kReassignExit, kRunning, recorder.hooks(), log);

  // The regression this part exists for: this used to be reported as success.
  CHECK(outcome != ReassignmentOutcome::kRestarted);
  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("start_failed"));
  CHECK_EQ(recorder.startCalls, 1);
  CHECK_EQ(recorder.restartNotices, 0);
  CHECK(log.str().find("replacement_worker_did_not_start") != std::string::npos);
  CHECK(assignments[0].backend == WorkerBackend::kCpu);
}

EDGE_TEST(a_failed_reassignment_is_not_handled_so_the_crash_path_takes_over,
          "kStartFailed is not kRestarted, which is what makes handleCrash write the crash log and "
          "tick the circuit breaker") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  recorder.startResult = false;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 0, kReassignExit, kRunning, recorder.hooks(), log);

  // handleCrash continues to the crash log and circuit breaker whenever this is false.
  const bool handled = outcome == ReassignmentOutcome::kRestarted;
  CHECK(!handled);
}

EDGE_TEST(a_worker_with_no_assignment_entry_is_not_treated_as_a_fallback,
          "an exit 70 from a worker the plan has no entry for falls through to crash handling "
          "rather than being honoured on an empty loop") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 7, kReassignExit, kRunning, recorder.hooks(), log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("not_applicable"));
  CHECK_EQ(recorder.startCalls, 0);
  CHECK_EQ(recorder.crashNotices, 0);
  CHECK(log.str().find("no_assignment_entry") != std::string::npos);
}

EDGE_TEST(a_cpu_worker_asking_to_be_reassigned_goes_down_the_crash_path,
          "a worker that is already on cpu has nowhere to fall, so it is a crash and not a "
          "fallback") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 1, kReassignExit, kRunning, recorder.hooks(), log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("not_applicable"));
  CHECK_EQ(recorder.startCalls, 0);
}

EDGE_TEST(an_exit_that_is_not_the_reassignment_code_is_an_ordinary_crash,
          "only EdgeExit::kReassignCpu means a planned fallback; every other exit keeps its crash "
          "log and its circuit-breaker tick") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 0, /*isReassignExit=*/false, kRunning,
                              recorder.hooks(), log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("not_applicable"));
  CHECK_EQ(recorder.startCalls, 0);
  CHECK(assignments[0].backend == WorkerBackend::kCuda);
}

EDGE_TEST(a_shutting_down_supervisor_does_not_restart_a_reassigned_worker,
          "during shutdown the exit is left alone rather than respawning a worker that is about to "
          "be killed anyway") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  Recorder recorder;
  std::ostringstream log;

  const ReassignmentOutcome outcome = applyWorkerReassignment(
      assignments, 0, kReassignExit, /*supervisorRunning=*/false, recorder.hooks(), log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("not_applicable"));
  CHECK_EQ(recorder.startCalls, 0);
}

EDGE_TEST(a_missing_start_hook_is_a_failed_start_not_a_silent_success,
          "nothing to start with is still a failure, so the outcome can never be restarted") {
  std::vector<WorkerAssignment> assignments = oneCudaWorker();
  ReassignmentHooks hooks;  // every hook empty
  std::ostringstream log;

  const ReassignmentOutcome outcome =
      applyWorkerReassignment(assignments, 0, kReassignExit, kRunning, hooks, log);

  CHECK_EQ(reassignmentOutcomeName(outcome), std::string("start_failed"));
}
