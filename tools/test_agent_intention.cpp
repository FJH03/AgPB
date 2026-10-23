// Optional agent intentions must preserve SyPB local tasks and token identity.
// This standalone regression runs the production task scheduler without SDK.
#include "../src/tasks.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static AgPBTask Task(AgPBTaskId id, float priority, int waypoint, float deadline = 0) {
    AgPBTask task;
    task.id = id; task.desire = priority; task.waypoint = waypoint;
    task.deadline = deadline; task.canResume = true;
    return task;
}
int main() {
    AgPBTaskStack tasks;
    const auto normal = tasks.Current().token;
    const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
    const auto agent = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 22, 5));
    Require(tasks.Current().token == agent && tasks.State(local) == AgPBTaskState::Suspended,
            "agent goal suspends the exact local task");
    Require(tasks.RenewDeadline(agent, 10) && tasks.Current().token == agent && tasks.Count() == 3,
            "same-goal renewal preserves identity and task count");
    Require(tasks.Find(AgPBTaskId::MoveToPosition)->waypoint == 11 && !tasks.Expire(5),
            "renewal preserves local goal and prevents old deadline expiry");
    Require(tasks.Expire(10) && tasks.Current().token == local && tasks.State(normal) == AgPBTaskState::Suspended,
            "TTL expiry restores local task automatically");
    const auto first = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 33, 20));
    const auto replacement = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 44, 20));
    Require(first != replacement && !tasks.RenewDeadline(first, 25) && !tasks.RemoveToken(first),
            "stale renewal or clear cannot affect replacement intention");
    Require(tasks.Current().token == replacement && tasks.Current().waypoint == 44,
            "changed goal replaces only the agent task");
    const auto objective = tasks.Push(Task(AgPBTaskId::DefuseBomb, AgPBTaskPriority::Objective, 55, 40));
    Require(tasks.Current().token == objective && tasks.State(replacement) == AgPBTaskState::Suspended,
            "local objective preempts an agent goal");
    Require(tasks.RenewDeadline(replacement, 21) && tasks.Current().token == objective,
            "suspended renewal never replaces active objective");
    Require(tasks.Expire(21) && tasks.Current().token == objective && tasks.State(replacement) == AgPBTaskState::Missing,
            "suspended lease expires without canceling objective");
    Require(tasks.Complete(objective) && tasks.Current().token == local,
            "objective completion returns to original local task after lease expiry");
    const auto clearable = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 66, 30));
    Require(tasks.RemoveToken(clearable) && tasks.Current().token == local,
            "explicit clear restores local task");
    Require(!tasks.RenewDeadline(normal, 50) && !tasks.RenewDeadline(local, 0) &&
            !tasks.RenewDeadline(local, std::numeric_limits<float>::quiet_NaN()) &&
            !tasks.RenewDeadline(local, std::numeric_limits<float>::infinity()),
            "deadline renewal rejects normal, unbounded and nonfinite requests");
    std::puts("PASS agent renewal, priority, stale identity, expiry and local fallback");
    return 0;
}
