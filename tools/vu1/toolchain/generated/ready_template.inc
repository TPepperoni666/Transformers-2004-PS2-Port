template<const AotVU::DecodedInstructionPair& D>
inline uint64_t AotVU::readyStatic() const {
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &D.upperUsage,
        &D.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        // Ported from KIexster/god-of-war-recomp patches/ps2recomp-vu1-perf.patch (GPL-3.0)
        // Visit only the local research sources the instruction reads (lane bit 3 = x ... bit 0 = w,
        // i.e. component = 3 - bit). std::max is order-independent, so this is bit-exact.
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            const auto &vfReady = m_vfReady[access.reg];
            for (uint32_t lanes = access.lanes & 0xFu; lanes != 0u; lanes &= lanes - 1u)
                ready = std::max(ready, vfReady[3u - vuCtz32(lanes)]);
        }
        for (uint32_t mask = usage->viRead & 0xFFFEu; mask; mask &= mask - 1u)
            ready = std::max(ready, m_viReady[vuCtz32(mask)]);
        for (uint32_t lanes = usage->accRead & 0xFu; lanes != 0u; lanes &= lanes - 1u)
            ready = std::max(ready, m_accReady[3u - vuCtz32(lanes)]);
    }

    if (D.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (D.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (D.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (D.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (D.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}
