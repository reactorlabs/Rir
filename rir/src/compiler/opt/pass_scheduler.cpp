#include "pass_scheduler.h"
#include "pass_definitions.h"

namespace rir {
namespace pir {

static std::regex getPassBlacklist() {
    auto filter = getenv("PIR_PASS_BLACKLIST");
    if (filter)
        return std::regex(filter);
    return std::regex("");
}

static const std::regex PIR_PASS_BLACKLIST = getPassBlacklist();

void PassScheduler::add(std::unique_ptr<const Pass>&& t) {
    auto name = t->getName();
    if (std::regex_match(name.begin(), name.end(), PIR_PASS_BLACKLIST))
        return;
    currentPhase->passes.push_back(std::move(t));
}

PassScheduler::PassScheduler() {
    // Keep the number in sync with the passes below!
    static size_t nDefaultOpts = 26;
    auto addDefaultOpt = [&]() {
        add<DotDotDots>();
        add<MatchCallArgs>();
        add<EagerCalls>();
        add<TypeInference>();

        add<Inline>();
        add<OptimizeContexts>();

        add<ForceDominance>();
        add<ScopeResolution>();
        add<LoadElision>();
        add<GVN>();
        add<Constantfold>();
        add<DeadStoreRemoval>();

        add<Inline>();
        add<OptimizeContexts>();

        add<OptimizeVisibility>();
        add<OptimizeAssumptions>();
        add<Constantfold>();
        add<Cleanup>();

        add<ElideEnv>();
        add<DelayEnv>();
        add<DelayInstr>();
        add<Cleanup>();

        add<OptimizeVisibility>();
        add<OptimizeAssumptions>();
        add<Cleanup>();
    };
    auto addDefaultPostPhaseOpt = [&]() {
        add<HoistInstruction>();
        add<LoopInvariant>();
        add<Overflow>();
        // Since some builtin implementations in the native backend depend on it
        add<TypeInference>();
    };

    nextPhase("Initial", 2 * nDefaultOpts);
    addDefaultOpt();
    nextPhase("Initial post");
    addDefaultPostPhaseOpt();

    // ==== Phase 2) Speculate away environments
    //
    // This pass is scheduled second, since we want to first try to do this
    // statically in Phase 1
    nextPhase("Speculation", 3 * nDefaultOpts);
    add<ElideEnvSpec>();
    addDefaultOpt();
    add<TypeSpeculation>();

    nextPhase("Speculation post");
    addDefaultPostPhaseOpt();

    // ==== Phase 3) Remove checkpoints we did not use
    //
    // This pass removes unused checkpoints.
    // We schedule this pass here, since it might unblock optimizations.
    // Since for example even unused checkpoints keep variables live.
    //
    // After this phase it is no longer possible to add assumptions at any point
    nextPhase("Remove CP");
    add<CleanupCheckpoints>();
    addDefaultPostPhaseOpt();

    nextPhase("Intermediate 2", 2 * nDefaultOpts);
    addDefaultOpt();
    nextPhase("Intermediate 2 post");
    addDefaultPostPhaseOpt();

    // ==== Phase 3.1) Remove Framestates we did not use
    //
    // Framestates can be used by call instructions. This pass removes this
    // dependency and the framestates will subsequently be cleaned.
    //
    // After this pass it is no longer possible to inline callees with deopts
    nextPhase("Cleanup FS");
    add<CleanupFramestate>();
    add<CleanupCheckpoints>();

    nextPhase("Final", 3 * nDefaultOpts);
    // ==== Phase 4) Final round of default opts
    addDefaultOpt();
    add<ElideEnvSpec>();
    add<CleanupCheckpoints>();

    nextPhase("Final post");
    addDefaultPostPhaseOpt();
    add<Cleanup>();
    add<CleanupCheckpoints>();

    nextPhase("done");
}

void PassScheduler::nextPhase(const std::string& name, unsigned budget) {
    schedule_.phases.push_back(Phase(name, budget));
    currentPhase = schedule_.phases.end() - 1;
    currentPhase->passes.push_back(
        std::unique_ptr<const Pass>(new PhaseMarker(name)));
}
}
}
