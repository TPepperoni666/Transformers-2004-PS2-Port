// Third-party adaptation notice: GPL-3.0; XGKICK reset / fast-path design adapted from Scotho/socom-unzipped.
// See CREDITS.md and NOTICE/THIRD_PARTY.md; original inline provenance retained.
#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "ps2_vu1_worker.h"

class GS;
class PS2Memory;

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void setDependencyFence(std::function<void()> fn) { m_dependencyFence=std::move(fn); }
    ~VU1Interpreter();
    void synchronize(); // EE-owner fence; also publishes all preceding PATH1 packets.
    void synchronizeIfOwner() { if (m_ownerToken.load(std::memory_order_relaxed) == Vu1Worker::currentThreadToken()) synchronize(); }
    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { synchronize(); return m_state; }
    const VU1State &state() const { const_cast<VU1Interpreter *>(this)->synchronize(); return m_state; }

    // True when the last run stopped on its cycle budget before the program's E bit (old budget
    // path, PS2X_VU1_RUN_TO_END=0). With run-to-end (default) execute()/resume() never return with
    // a program pending unless the runaway guard dropped it.
    bool programPending() const { const_cast<VU1Interpreter *>(this)->synchronize(); return m_programPending; }

private:
    std::function<void()> m_dependencyFence;
    std::unique_ptr<Vu1Worker> m_worker;
    std::atomic<uint64_t> m_ownerToken{0};
    bool m_hostPending = false;
    bool m_capturePath1 = false; // accessed only by worker between submit and finish
    GS *m_publishGs = nullptr;
    PS2Memory *m_publishMemory = nullptr;
    std::vector<std::vector<uint8_t>> m_path1Packets;
    size_t m_path1Bytes = 0;
    void emitPath1(const uint8_t *, uint32_t);
    void executeInline(uint8_t *, uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t, uint32_t, uint32_t, uint32_t);
    void resumeInline(uint8_t *, uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t, uint32_t, uint32_t);
    // Transformers port: local-only VU1 AOT dispatch (src/lib/vu/ps2_vu1_aot_bridge.cpp, knob PS2X_VU1_AOT).
    // Declaration only, no data members: the class layout (embedded by value in PS2Runtime and seen by
    // TUs built without PS2X_WITH_VU1_AOT) must not depend on the AOT build option.
    bool tryAot(uint8_t *, uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t, uint32_t, uint32_t, uint32_t, bool);
    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
        // Ported from Scotho/socom-unzipped third_party/ps2recomp ps2_vu1.h (GPL-3.0): reset the
        // bookkeeping without touching the 64 KB packet buffer. Every packet byte that is read (GIFtag
        // parse, PATH1 submit of [0, totalBytes)) was copied from VU memory during the same kick,
        // because copiedBytes restarts at 0 and grows contiguously.
        void clear()
        {
            sourceAddress = 0;
            totalBytes = 0;
            copiedBytes = 0;
            currentTagEnd = 0;
            cycleCredit = 0;
            issueCycle = 0;
            active = false;
            currentTagEop = false;
        }
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    // All modeled pipeline latencies are in [1,54]. A 64-cycle calendar
    // contains one bit per slot, ordered like the original commit loops.
    std::array<uint64_t, 64> m_commitCalendar{};
    uint64_t m_commitCycles = 0;
    void scheduleCommit(uint64_t cycle, uint32_t bit);
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    // VU1 fast path (PS2X_VU1_FAST; idea and structure from Scotho/socom-unzipped runFast, GPL-3.0,
    // re-done on this interpreter's semantics). True only while runFast() executes pairs: VF/VI/ACC
    // writes and stores land at once, FMAC flags use the vector classifier, everything else is the
    // interpreter's own scheduler (scoreboard, flag/Q/P calendar, cycle-stepped XGKICK).
    bool m_fast = false;
    // Latest ready cycle of any VF/VI/ACC write the fast path landed early: flushPipelines() at a
    // program end keeps advancing until then, exactly as long as the interpreter's write queues
    // would have stayed pending.
    uint64_t m_fastMaxWriteReady = 0;
    // Run-to-end (pending-program) semantics, PS2X_VU1_RUN_TO_END (ported from Scotho/socom-unzipped
    // executeProgram/continueProgram, GPL-3.0).
    bool m_programPending = false;
    bool m_quietBudget = false;
    void finishPendingProgram(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                              GS &gs, PS2Memory *memory, uint32_t slice, uint64_t guardEnd);
    void runFast(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory, uint64_t fastEnd, bool &programEnded);
    uint64_t fastReadyCycle(const DecodedInstructionPair &decoded, uint64_t maxReady) const;
    bool fastFmac(uint32_t instr, float *target, const float *given);
    // Fast path VF write log: pre-write lane values of the last kFastVfLogSize early-landed VF writes.
    struct FastVfLogEntry
    {
        uint64_t issue = 0;
        float old[4]{};
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };
    static constexpr uint32_t kFastVfLogSize = 32u;
    std::array<FastVfLogEntry, kFastVfLogSize> m_fastVfLog{};
    uint32_t m_fastVfLogCount = 0;
    // Exact per-lane view of the fast path's early-landed VF writes (replaces walking the bounded write log in
    // fastVfVisible/fastMaterializeVf): for each lane the issue cycle of its newest fast write and the value the
    // interpreter would show while that write is still pending (= the value of the newest write that committed).
    float m_fastVisVf[32][4]{};
    uint64_t m_fastIssueVf[32][4]{};
    bool m_fastHasVf[32][4]{};
    // Same for VI: value the interpreter shows while the newest early-landed write to the register is pending, and
    // that write's issue / due cycles.
    int32_t m_fastVisVi[16]{};
    uint64_t m_fastReadyVi[16]{};
    bool m_fastHasVi[16]{};
    float fastVfVisible(uint8_t reg, uint32_t component, bool &exact) const;
    uint32_t fastPatchCrossOperands(uint32_t upper, float saved[2][4]);
    void fastUnpatchCrossOperands(uint32_t upper, uint32_t patched, const float saved[2][4]);
    void fastMaterializeVf();
    struct FastViLogEntry
    {
        uint64_t issue = 0;
        uint64_t ready = 0;
        int32_t old = 0;
        uint8_t reg = 0;
    };
    static constexpr uint32_t kFastViLogSize = 32u;
    std::array<FastViLogEntry, kFastViLogSize> m_fastViLog{};
    uint32_t m_fastViLogCount = 0;
    int32_t fastViVisible(uint8_t reg) const;
    void fastMaterializeVi();
    void fastFmacCommit(uint32_t instr, float *target, bool toAcc, uint32_t destBits, const uint32_t *valueBits,
                        uint32_t zeroOrUnder, uint32_t sign, uint32_t under, uint32_t over, uint32_t extraSticky);

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    DecodedInstructionPair getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    float normalizeOperand(float value) const;
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
