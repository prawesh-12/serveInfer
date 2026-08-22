#pragma once

#include <functional>
#include <iosfwd>
#include <vector>

#include "../hardware/capacityPlan.h"

enum class ReassignmentOutcome {
  kNotApplicable,
  kRestarted,
  kStartFailed,
};

const char* reassignmentOutcomeName(ReassignmentOutcome outcome);

struct ReassignmentHooks {
  std::function<bool(int workerId)> startWorker;
  std::function<void(int workerId)> notifyCrash;
  std::function<void(int workerId)> notifyRestarted;
};

ReassignmentOutcome applyWorkerReassignment(std::vector<WorkerAssignment>& assignments,
                                            int workerId,
                                            bool isReassignExit,
                                            bool supervisorRunning,
                                            const ReassignmentHooks& hooks,
                                            std::ostream& log);
