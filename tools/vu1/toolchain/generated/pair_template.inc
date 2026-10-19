template<const AotVU::DecodedInstructionPair& D>
inline bool AotVU::pairStatic(uint32_t codeSize,uint8_t* vuData,uint32_t dataSize,GS& gs,PS2Memory* memory,uint64_t end,bool& programEnded,uint64_t planned) {
    constexpr auto decoded=D;
    uint64_t ready=planned ? planned : readyStatic<D>();
    while (ready>m_cycle) {
        if(ready>=end)return false;
        m_viBranchBackupValid=false;advanceTo(ready);
        ready=planned ? planned : readyStatic<D>();
    }
    if(m_cycle>=end || m_stopRequested)return false;
    // Transformers port: optional standalone coverage counters; absent from production timing.
    #if defined(VU1_AOT_STATS)
    ++aotPairs;
    if(planned)++staticPairs;
    #endif
        const bool takingPriorBranch = m_state.branchPending && m_state.branchDelay == 0u;
        const uint32_t priorBranchTarget = m_state.branchTarget;
        const uint32_t viWriteMask = decoded.lowerUsage.viWrite & 0xFFFEu;
        const uint8_t writtenVi = viWriteMask != 0u ? static_cast<uint8_t>(vuCtz32(viWriteMask)) : 0u;
        const int32_t oldVi = writtenVi != 0u ? m_state.vi[writtenVi] : 0;
        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const uint8_t accWrite = decoded.upperUsage.accWrite;

        // The branch-VI bypass records the value the interpreter holds in the written VI register at
        // issue (oldVi). With an earlier write to that register still pending, that is NOT what the
        // fast path's register file holds (the pending write landed early): take the interpreter's view.
        const int32_t bypassOldVi =
            (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead && m_viReady[writtenVi] > m_cycle)
                ? fastViVisible(writtenVi)
                : oldVi;
        if (writtenVi != 0u)
        {
            // Per-register tracking (see the VF block): the previous write commits iff this one is issued at or after
            // its due cycle; then its landed value is the interpreter-visible one.
            const uint64_t dueNew = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency
                                                                                 : decoded.lowerUsage.latency);
            if (!m_fastHasVi[writtenVi] || m_cycle >= m_fastReadyVi[writtenVi])
                m_fastVisVi[writtenVi] = oldVi;
            m_fastReadyVi[writtenVi] = dueNew;
            m_fastHasVi[writtenVi] = true;
        }

        // VF write log (pre-pair lane values) for fastVfVisible / fastMaterializeVf: the writes the
        // interpreter would queue (queueVfWrite), all with the 4-cycle latency.
        {
            // Per-lane tracking (before the pair executes, m_state.vf still holds the previous landed value): the
            // previous write to a lane commits iff this write is issued at or after its due cycle; then its value
            // becomes the interpreter-visible one.
            const auto track = [&](const VfAccess &w)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if ((w.lanes & laneForComponent(c)) == 0u)
                        continue;
                    if (!m_fastHasVf[w.reg][c] || m_cycle >= m_fastIssueVf[w.reg][c] + kFmacLatency)
                        m_fastVisVf[w.reg][c] = m_state.vf[w.reg][c];
                    m_fastIssueVf[w.reg][c] = m_cycle;
                    m_fastHasVf[w.reg][c] = true;
                }
            };
            if (hasUpperWrite)
                track(upperWrite);
            if (hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg))
                track(lowerWrite);
        }

        // OPMULA/OPMSUB read lanes their usage does not declare (the cross wiring), so the interpreter
        // does not stall on them and reads whatever is committed. Give the op exactly that view.
        const auto runUpper = [&]()
        {
            const uint32_t upper = decoded.upper;
            const bool cross = (upper & 0x3Fu) == 0x2Eu ||
                               ((upper & 0x3Cu) == 0x3Cu && ((upper & 3u) | ((upper >> 4) & 0x7Cu)) == 0x2Eu);
            if (!cross)
            {
                if (!fmacStatic<D.upper>(nullptr,nullptr))
                    upperStatic<D.upper>();
                return;
            }
            float saved[2][4];
            const uint32_t patched = fastPatchCrossOperands(upper, saved);
            if (!fmacStatic<D.upper>(nullptr,nullptr))
                upperStatic<D.upper>();
            if (patched)
                fastUnpatchCrossOperands(upper, patched, saved);
        };

        if (decoded.iBit)
        {
            runUpper();
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = immediate; // LOI transfers raw bits; arithmetic normalizes at use.
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            // The lower instruction reads (or writes) the upper's destination: it sees the old value,
            // and the upper's result wins over a lower write to the same register.
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf, m_state.vf[decoded.upperVfShadowReg], sizeof(oldVf));
            runUpper();
            std::memcpy(upperVf, m_state.vf[decoded.upperVfShadowReg], sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg], oldVf, sizeof(oldVf));
            lowerStatic<D.lower,D.upper>(vuData,dataSize,gs,memory);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg], upperVf, sizeof(upperVf));
        }
        else
        {
            runUpper();
            lowerStatic<D.lower,D.upper>(vuData,dataSize,gs,memory);
        }

        m_viBranchBackupValid = m_viBranchBackupValid &&
                               writtenVi == m_viBranchBackupReg &&
                               decoded.lowerUsage.delaysNextBranchRead;
        // The interpreter's VI commit narrows to 16 bits.
        if (writtenVi != 0u)
            m_state.vi[writtenVi] = static_cast<int16_t>(m_state.vi[writtenVi]);

        // Scoreboard: markPairWrites, plus the latest due cycle (maxReady for the stall scan skip,
        // m_fastMaxWriteReady for the program-end flush).
        if (hasLowerWrite)
        {
            const uint64_t ready = m_cycle + (decoded.lowerUsage.vfLatency != 0u ? decoded.lowerUsage.vfLatency
                                                                                 : decoded.lowerUsage.latency);
            auto &vfReady = m_vfReady[lowerWrite.reg];
            for (uint32_t lanes = lowerWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                vfReady[3u - vuCtz32(lanes)] = ready;
            
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (hasUpperWrite)
        {
            const uint64_t ready = m_cycle + (decoded.upperUsage.vfLatency != 0u ? decoded.upperUsage.vfLatency
                                                                                 : decoded.upperUsage.latency);
            auto &vfReady = m_vfReady[upperWrite.reg];
            for (uint32_t lanes = upperWrite.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                vfReady[3u - vuCtz32(lanes)] = ready;
            
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (viWriteMask != 0u)
        {
            const uint64_t ready = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency
                                                                                 : decoded.lowerUsage.latency);
            for (uint32_t mask = viWriteMask; mask; mask &= mask - 1u)
                m_viReady[vuCtz32(mask)] = ready;
            
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, ready);
        }
        if (accWrite != 0u)
        {
            for (uint32_t lanes = accWrite & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                m_accReady[3u - vuCtz32(lanes)] = m_cycle + kAccForwardLatency;
            m_fastMaxWriteReady = std::max(m_fastMaxWriteReady, m_cycle + kAccForwardLatency);
        }
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, bypassOldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (takingPriorBranch)
        {
            // A branch in the delay slot executes one pair at the first target.
            const bool scheduledDelayBranch = m_state.branchPending && m_state.branchDelay == 1u;
            m_state.pc = priorBranchTarget & microAddressMask();
            m_state.branchPending = scheduledDelayBranch;
            m_state.branchDelay = 0u;
        }
        else if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }


 ++m_cycle;
 if(m_commitCalendar[m_cycle&63u])commitReadyPipelines();
 if(m_xgkick.active)progressXgkick();
 return true;
}
