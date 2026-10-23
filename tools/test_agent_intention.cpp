// High-level agent authority and local SyPB fallback regression.
// Runs the production task scheduler without the game SDK.
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
static AgPBTask Child(std::uint64_t parent, float priority = AgPBTaskPriority::DestroyBreakable, float deadline = 0) {
    AgPBTask task = Task(AgPBTaskId::DestroyBreakable, priority, -1, deadline);
    task.parentToken = parent; task.canResume = false;
    task.entityHandle = 123; task.hasEntity = true;
    return task;
}
static void TestLocalDesires() {
    AgPBTaskStack tasks;
    const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
    const auto objective = tasks.Push(Task(AgPBTaskId::DefuseBomb, AgPBTaskPriority::Objective, 12));
    Require(tasks.Current().token == objective && tasks.State(local) == AgPBTaskState::Suspended,
            "without an agent, local objectives retain SyPB desire arbitration");
    const auto clearing = tasks.Push(Child(local));
    Require(tasks.Current().token == clearing && tasks.State(local) == AgPBTaskState::Suspended,
            "local breakable execution retains the existing SyPB desire level");
    Require(tasks.Complete(clearing) && tasks.Current().token == objective,
            "local child completion restores the most desired local task");
    Require(tasks.Complete(objective) && tasks.Current().token == local,
            "local objective completion restores its suspended navigation task");
    AgPBTask pause = Task(AgPBTaskId::Pause, AgPBTaskPriority::Pause, -1);
    pause.canResume = false;
    Require(tasks.Push(pause) == 0 && !tasks.Contains(AgPBTaskId::Pause),
            "local nonresumable losers still follow SyPB discard behavior");
}
static void TestAuthorityAndRenewal() {
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
    const auto escape = tasks.Push(Task(AgPBTaskId::EscapeBomb, AgPBTaskPriority::EscapeBomb, 56, 35));
    Require(tasks.Current().token == replacement && tasks.State(objective) == AgPBTaskState::Suspended &&
            tasks.State(escape) == AgPBTaskState::Suspended,
            "agent authority outranks independent objective and bomb escape goals");
    const auto excessive = tasks.Push(Task(AgPBTaskId::PlantBomb, 1000000, 57, 30));
    Require(tasks.Current().token == replacement && tasks.State(excessive) == AgPBTaskState::Suspended,
            "authority is separate from numeric local desire");
    Require(tasks.RenewDeadline(replacement, 21) && tasks.Current().token == replacement,
            "renewal preserves the top-level owner despite pending local goals");
    Require(tasks.Expire(21) && tasks.Current().token == excessive && tasks.State(replacement) == AgPBTaskState::Missing,
            "lease expiry immediately restores ordinary local desire arbitration");
    Require(tasks.Complete(excessive) && tasks.Current().token == escape &&
            tasks.Complete(escape) && tasks.Current().token == objective &&
            tasks.Complete(objective) && tasks.Current().token == local,
            "retained local goals and their exact tokens survive an intervention");
    const auto clearable = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 66, 30));
    Require(tasks.RemoveToken(clearable) && tasks.Current().token == local,
            "explicit clear restores local task");
    Require(!tasks.RenewDeadline(normal, 50) && !tasks.RenewDeadline(local, 0) &&
            !tasks.RenewDeadline(local, std::numeric_limits<float>::quiet_NaN()) &&
            !tasks.RenewDeadline(local, std::numeric_limits<float>::infinity()),
            "deadline renewal rejects normal, unbounded and nonfinite requests");
}
static void TestNonresumableOwner() {
    for (int operation = 0; operation < 3; ++operation) {
        AgPBTaskStack tasks;
        const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
        AgPBTask investigation = Task(AgPBTaskId::Investigate, AgPBTaskPriority::Investigate, 22, 5);
        investigation.canResume = false;
        const auto owner = tasks.Push(investigation);
        const auto child = tasks.Push(Child(owner));
        Require(tasks.Current().token == child && tasks.State(owner) == AgPBTaskState::Suspended,
                "a nonresumable investigation remains live while its own child executes");
        if (operation == 0) {
            Require(tasks.Complete(child) && tasks.Current().token == owner && tasks.Complete(owner),
                    "child completion resumes the exact nonresumable investigation owner");
        } else if (operation == 1) {
            Require(tasks.Expire(5), "investigation owner deadline expires during child execution");
        } else {
            const auto agent = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 33, 10));
            Require(tasks.Current().token == agent && tasks.State(owner) == AgPBTaskState::Missing &&
                    tasks.State(child) == AgPBTaskState::Missing && tasks.RemoveToken(agent),
                    "independent agent authority discards nonresumable investigation and child");
        }
        Require(tasks.State(owner) == AgPBTaskState::Missing && tasks.State(child) == AgPBTaskState::Missing &&
                tasks.Current().token == local && tasks.Push(Child(owner)) == 0,
                "nonresumable owner cleanup removes its child before exact local fallback");
    }
}
static void TestExecutionChildren() {
    AgPBTaskStack tasks;
    const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
    const auto agent = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 22, 5));
    Require(tasks.Push(Child(local)) == 0 && !tasks.Contains(AgPBTaskId::DestroyBreakable),
            "a different local goal cannot delegate execution through agent authority");
    Require(tasks.Push(Child(0)) == 0 && !tasks.Contains(AgPBTaskId::DestroyBreakable),
            "an independent local breakable task cannot replace the agent's goal");
    // Deliberately below the agent desire: the parent relation authorizes
    // execution, while local desire numbers do not grant goal authority.
    const auto child = tasks.Push(Child(agent, 1));
    Require(child != 0 && tasks.Current().token == child && tasks.Current().parentToken == agent &&
            tasks.State(agent) == AgPBTaskState::Suspended && tasks.Find(AgPBTaskId::AgentIntention)->waypoint == 22,
            "agent child clears an obstacle while preserving the selected goal");
    Require(tasks.RenewDeadline(agent, 10) && tasks.Current().token == child && !tasks.Expire(5),
            "owner heartbeat preserves its exact execution child and renewed lease");
    const auto objective = tasks.Push(Task(AgPBTaskId::DefuseBomb, AgPBTaskPriority::Objective, 33));
    const auto escape = tasks.Push(Task(AgPBTaskId::EscapeBomb, AgPBTaskPriority::EscapeBomb, 44));
    Require(tasks.Current().token == child && tasks.State(objective) == AgPBTaskState::Suspended &&
            tasks.State(escape) == AgPBTaskState::Suspended && !tasks.Complete(agent),
            "independent goals cannot replace agent child execution or complete its suspended owner");
    Require(tasks.Complete(child) && tasks.Current().token == agent && !tasks.Complete(child),
            "child completion resumes its exact owner and rejects stale completion");
    const auto expiringChild = tasks.Push(Child(agent, 1, 8));
    Require(tasks.Expire(8) && tasks.State(expiringChild) == AgPBTaskState::Missing && tasks.Current().token == agent,
            "child timeout preserves the unexpired owner");
    const auto finalChild = tasks.Push(Child(agent, 1));
    Require(tasks.Expire(10) && tasks.State(agent) == AgPBTaskState::Missing &&
            tasks.State(finalChild) == AgPBTaskState::Missing && tasks.Current().token == escape,
            "owner expiry atomically removes its child and falls back to local escape");
    Require(tasks.Push(Child(agent)) == 0 && !tasks.Complete(finalChild) && tasks.Current().token == escape,
            "expired owner and child tokens cannot affect local fallback");
}
static void TestOwnerCleanup() {
    for (int operation = 0; operation < 4; ++operation) {
        AgPBTaskStack tasks;
        const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
        const auto agent = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 22, 5));
        const auto child = tasks.Push(Child(agent));
        if (operation == 0) Require(tasks.RemoveToken(agent), "owner can be removed while its child executes");
        else if (operation == 1) Require(tasks.Remove(AgPBTaskId::AgentIntention), "owner removal by ID succeeds");
        else if (operation == 2) Require(tasks.Expire(5), "owner expires while its child executes");
        else tasks.Reset();
        Require(tasks.State(agent) == AgPBTaskState::Missing && tasks.State(child) == AgPBTaskState::Missing &&
                !tasks.Contains(AgPBTaskId::DestroyBreakable),
                "owner removal, expiry and reset remove execution children atomically");
        Require(tasks.Current().token == local || (operation == 3 && tasks.Current().id == AgPBTaskId::Normal),
                "owner cleanup resumes the retained local task or reset normal");
        Require(tasks.Push(Child(agent)) == 0 && !tasks.RemoveToken(child) && !tasks.Complete(child),
                "stale child work cannot survive owner cleanup");
    }
    AgPBTaskStack tasks;
    const auto local = tasks.Push(Task(AgPBTaskId::MoveToPosition, AgPBTaskPriority::MoveToPosition, 11));
    const auto first = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 22, 5));
    const auto child = tasks.Push(Child(first));
    const auto replacement = tasks.Push(Task(AgPBTaskId::AgentIntention, AgPBTaskPriority::AgentIntention, 33, 10));
    Require(tasks.State(first) == AgPBTaskState::Missing && tasks.State(child) == AgPBTaskState::Missing &&
            tasks.Current().token == replacement && tasks.Current().waypoint == 33,
            "replacing a goal atomically removes its old execution child");
    const auto replacementChild = tasks.Push(Child(replacement));
    Require(tasks.Push(Child(first)) == 0 && tasks.Current().token == replacementChild &&
            !tasks.RemoveToken(first) && !tasks.RenewDeadline(first, 20) && !tasks.Complete(child),
            "stale parent requests cannot replace a live owner's child");
    AgPBTask invalid = Task(AgPBTaskId::EscapeBomb, AgPBTaskPriority::EscapeBomb, 44);
    invalid.parentToken = replacement;
    Require(tasks.Push(invalid) == 0, "independent objectives cannot claim execution-child status");
    Require(tasks.Complete(replacementChild) && tasks.Complete(replacement) && tasks.Current().token == local,
            "finishing child and owner returns to the original autonomous task");
    Require(tasks.Push(Child(replacement)) == 0, "completed owner tokens cannot accept children");
}
int main() {
    TestLocalDesires();
    TestAuthorityAndRenewal();
    TestNonresumableOwner();
    TestExecutionChildren();
    TestOwnerCleanup();
    std::puts("PASS agent goal authority, child execution, lease renewal, atomic cleanup and SyPB local fallback");
    return 0;
}
