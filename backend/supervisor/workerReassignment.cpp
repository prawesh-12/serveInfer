#include "workerReassignment.h"

#include <ostream>
#include <string>

const char* reassignmentOutcomeName(ReassignmentOutcome outcome) {
  switch (outcome) {
    case ReassignmentOutcome::kNotApplicable:
      return "not_applicable";
    case ReassignmentOutcome::kRestarted:
      return "restarted";
    case ReassignmentOutcome::kStartFailed:
      return "start_failed";
  }
  return "unknown";
}

ReassignmentOutcome applyWorkerReassignment(std::vector<WorkerAssignment>& assignments,
                                            int workerId,
                                            bool isReassignExit,
                                            bool supervisorRunning,
                                            const ReassignmentHooks& hooks,
                                            std::ostream& log) {
  if (workerId < 0 || !isReassignExit || !supervisorRunning) {
    return ReassignmentOutcome::kNotApplicable;
  }

  WorkerAssignment* target = nullptr;
  for (WorkerAssignment& assignment : assignments) {
    if (assignment.workerId == workerId) {
      target = &assignment;
      break;
    }
  }
  if (target == nullptr) {
    log << "[supervisor] reassignment ignored workerId=" << workerId
        << " reason=no_assignment_entry\n";
    return ReassignmentOutcome::kNotApplicable;
  }

  if (target->backend != WorkerBackend::kCuda) {
    return ReassignmentOutcome::kNotApplicable;
  }

  const std::string previous = workerBackendName(target->backend);
  *target = demoteToCpu(*target,
                        "device fault on " + previous +
                            ": released model and context, re-exec is the only way to give "
                            "back the cuda primary context");
  log << "[supervisor] fallback workerId=" << workerId << " previous=" << previous
      << " error=worker_requested_cpu_reassignment cleanup=released_model_and_context"
      << " next=cpu\n";

  if (hooks.notifyCrash) {
    hooks.notifyCrash(workerId);
  }

  const bool started = hooks.startWorker ? hooks.startWorker(workerId) : false;
  if (!started) {
    log << "[supervisor] reassignment failed workerId=" << workerId
        << " reason=replacement_worker_did_not_start next=crash_handling\n";
    return ReassignmentOutcome::kStartFailed;
  }

  if (hooks.notifyRestarted) {
    hooks.notifyRestarted(workerId);
  }
  return ReassignmentOutcome::kRestarted;
}
