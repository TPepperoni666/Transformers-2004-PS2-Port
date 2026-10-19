namespace
{
    // normalizeOperand on four lanes (bit-identical to vuNormalizeOperand). Ported from
    // Scotho/socom-unzipped ps2_vu1_ops.h normalize4 (GPL-3.0).
    __forceinline __m128 vuNormalize4(__m128 value)
    {
        const __m128i bits = _mm_castps_si128(value);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i zeroExp = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i maxExp = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        __m128i out = _mm_andnot_si128(_mm_or_si128(zeroExp, maxExp), bits);
        out = _mm_or_si128(out, _mm_and_si128(sign, zeroExp));
        out = _mm_or_si128(out, _mm_and_si128(_mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), maxExp));
        return _mm_castsi128_ps(out);
    }

    // Component bit c (bit 0 = x) -> per-lane all-ones mask (SSE lane 0 = x); after Scotho/socom-unzipped
    // ps2_vu1_ops.h kLaneMaskTable (GPL-3.0).
    alignas(16) constexpr int32_t kVuLaneMask[16][4] = {
        {0, 0, 0, 0}, {-1, 0, 0, 0}, {0, -1, 0, 0}, {-1, -1, 0, 0},
        {0, 0, -1, 0}, {-1, 0, -1, 0}, {0, -1, -1, 0}, {-1, -1, -1, 0},
        {0, 0, 0, -1}, {-1, 0, 0, -1}, {0, -1, 0, -1}, {-1, -1, 0, -1},
        {0, 0, -1, -1}, {-1, 0, -1, -1}, {0, -1, -1, -1}, {-1, -1, -1, -1}};
    // Reverses 4 bits: component bits (bit 0 = x) <-> VU lane bits (bit 3 = x, the dest / MAC order).
    constexpr uint8_t kVuRev4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
    __forceinline __m128i vuLaneMask(uint32_t componentBits)
    {
        return _mm_load_si128(reinterpret_cast<const __m128i *>(kVuLaneMask[componentBits & 0xFu]));
    }

    // Z/S/U/O classes of four double lanes (lo = components 0,1, hi = 2,3), as bit c = component c:
    // the classification of normalizeFmacExactResult (|e| == 0, |e| > FLT_MAX, |e| < FLT_MIN, signbit).
    struct VuLaneClass
    {
        uint32_t zero, over, under, sign;
    };
    __forceinline VuLaneClass vuClassify(__m128d lo, __m128d hi)
    {
        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
        const __m128d fltMax = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));
        const __m128d fltMin = _mm_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
        const __m128d zero = _mm_setzero_pd();
        const __m128d absLo = _mm_and_pd(lo, absMask);
        const __m128d absHi = _mm_and_pd(hi, absMask);
        VuLaneClass c;
        c.zero = static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpeq_pd(absLo, zero))) |
                 (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpeq_pd(absHi, zero))) << 2);
        c.over = static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpgt_pd(absLo, fltMax))) |
                 (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmpgt_pd(absHi, fltMax))) << 2);
        c.under = (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmplt_pd(absLo, fltMin))) |
                   (static_cast<uint32_t>(_mm_movemask_pd(_mm_cmplt_pd(absHi, fltMin))) << 2)) &
                  ~c.zero;
        c.sign = static_cast<uint32_t>(_mm_movemask_pd(lo)) | (static_cast<uint32_t>(_mm_movemask_pd(hi)) << 2);
        return c;
    }

    enum VuFmacKind : uint8_t { VuAdd, VuSub, VuMul, VuMadd, VuMsub };
    enum VuFmacSrc : uint8_t { VuSrcBc, VuSrcQ, VuSrcI, VuSrcVt, VuSrcCross };
    // The arithmetic FMAC ops of execUpper (the ones that go through applyFmacDest/applyFmacDestAcc).
    __forceinline bool vuDecodeFmac(uint32_t upper, VuFmacKind &kind, VuFmacSrc &src, uint8_t &sel, bool &toAcc)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        toAcc = op >= 0x3Cu;
        if (!toAcc && op > 0x2Fu)
            return false;
        sel = toAcc ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        if (sel <= 0x0Fu)
        {
            static constexpr VuFmacKind kBcKinds[4] = {VuAdd, VuSub, VuMadd, VuMsub};
            kind = kBcKinds[sel >> 2];
            src = VuSrcBc;
            return true;
        }
        switch (sel)
        {
        case 0x18: case 0x19: case 0x1A: case 0x1B: kind = VuMul; src = VuSrcBc; return true;
        case 0x1C: kind = VuMul; src = VuSrcQ; return true;
        case 0x1E: kind = VuMul; src = VuSrcI; return true;
        case 0x20: kind = VuAdd; src = VuSrcQ; return true;
        case 0x21: kind = VuMadd; src = VuSrcQ; return true;
        case 0x22: kind = VuAdd; src = VuSrcI; return true;
        case 0x23: kind = VuMadd; src = VuSrcI; return true;
        case 0x24: kind = VuSub; src = VuSrcQ; return true;
        case 0x25: kind = VuMsub; src = VuSrcQ; return true;
        case 0x26: kind = VuSub; src = VuSrcI; return true;
        case 0x27: kind = VuMsub; src = VuSrcI; return true;
        case 0x28: kind = VuAdd; src = VuSrcVt; return true;
        case 0x29: kind = VuMadd; src = VuSrcVt; return true;
        case 0x2A: kind = VuMul; src = VuSrcVt; return true;
        case 0x2C: kind = VuSub; src = VuSrcVt; return true;
        case 0x2D: kind = VuMsub; src = VuSrcVt; return true;
        case 0x2E: kind = toAcc ? VuMul : VuMsub; src = VuSrcCross; return true; // OPMULA / OPMSUB
        default: return false;
        }
    }
}

// Arithmetic FMAC op for the fast path: operands, float result, classification, flags, write-back.
// Returns false (nothing done) for every other upper op; the caller then runs execUpper.
// `given` (nullable) is execUpper's float result when called from applyFmacDest's fast hook; `target`
// is then the destination it passed. Semantics are execUpper + normalizeFmacResult
// (calculateFmacExactResult + normalizeFmacExactResult) + calculateFmacProductSticky + updateFmacFlags
// + applyDest, unchanged:
//   * float result: execUpper's expressions on vuNormalizeOperand'd operands (same order: product,
//     then add/sub), in SSE lanes under the run's MXCSR (FE_TOWARDZERO) - lane for lane what the scalar
//     code computes. OPMULA/OPMSUB's w lane is 0.0f.
//   * the "exact" result is the reference's long double expression evaluated in SSE2 double lanes. On
//     MSVC long double IS double, so this is the reference computation itself, vectorised. (SOCOM's
//     fmacSingle4/fmacProductSum4 instead prove when the float result decides, against an 80-bit long
//     double; that proof is not for our 64-bit reference, so it is not used.) Where long double is
//     wider than double this returns false and execUpper's reference classifier runs.
//   * value: the float result, or +/-0 (exact zero / underflow), or +/-FLT_MAX (overflow).
//   * product-sum ops OR the Z/S/U/O class of every dest lane's exact product (float*float is exact in
//     double) into the sticky bits; OPMSUB includes its w lane (vs.w*vt.w), like the reference.
//   * flag entry: updateFmacFlags (first free slot, ready at +4, calendar bit), skipped for dest == 0.
template<uint32_t U>
inline void AotVU::commitStatic(float *target,bool toAcc,uint32_t destBits,const uint32_t *valueBits,
 uint32_t zeroOrUnder,uint32_t sign,uint32_t under,uint32_t over,uint32_t extraSticky)
{
    // Transformers port: only blocks without MAC/STATUS readers use this mode.
    // Entries crossing the boundary retain the original +4 queue semantics.
    if (aotCoalesceFlags && m_cycle+kFmacLatency<=aotFlagCutoff) {
        aotCoalescedValid=true;
        aotCoalescedMac=kVuRev4[zeroOrUnder] | (kVuRev4[sign]<<4) | (kVuRev4[under]<<8) | (kVuRev4[over]<<12);
        aotCoalescedCurrent=(zeroOrUnder?1u:0u)|(sign?2u:0u)|(under?4u:0u)|(over?8u:0u);
        aotCoalescedSticky|=(aotCoalescedCurrent|extraSticky)<<6;
    } else
    {
    // Transformers port: no MAC/STATUS readers or FSSET anywhere in this image.
    // Keep final MAC/current bits and all sticky bits, without +4 queue history.
    if (aotFlagsImmediate) {
        m_state.mac=kVuRev4[zeroOrUnder] | (kVuRev4[sign]<<4) | (kVuRev4[under]<<8) | (kVuRev4[over]<<12);
        const uint32_t current=(zeroOrUnder?1u:0u)|(sign?2u:0u)|(under?4u:0u)|(over?8u:0u);
        m_state.status=(m_state.status&0xFF0u)|current|((current|extraSticky)<<6);
        m_fastMaxWriteReady=std::max(m_fastMaxWriteReady,m_cycle+kFmacLatency);
    } else {
    // updateFmacFlags: MAC = Z | S << 4 | U << 8 | O << 12 in lane order (bit 3 = x); Z includes underflow.
    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry)
    {
        const uint32_t mac = kVuRev4[zeroOrUnder] | (kVuRev4[sign] << 4) | (kVuRev4[under] << 8) | (kVuRev4[over] << 12);
        const uint64_t ready = m_cycle + kFmacLatency;
        entry->readyCycle = ready;
        entry->issueCycle = m_cycle;
        entry->mac = mac;
        entry->status = (zeroOrUnder ? 0x1u : 0u) | (sign ? 0x2u : 0u) | (under ? 0x4u : 0u) | (over ? 0x8u : 0u);
        entry->extraSticky = extraSticky;
        entry->clip = 0u;
        entry->valid = true;
        entry->writesMac = true;
        entry->writesStatus = true;
        entry->writesSticky = false;
        entry->writesClip = false;
        // scheduleCommit(ready, slot), inline.
        m_commitCalendar[ready & 63u] |= uint64_t{1} << static_cast<uint32_t>(entry - m_flagPipeline.data());
        m_commitCycles |= uint64_t{1} << (ready & 63u);
    }
    else
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
    }

    }
    }
    // applyDest: VF0 is immutable; masked 16-byte store of the dest lanes.
    float *dst = target ? target : (toAcc ? m_state.acc : m_state.vf[FD(U)]);
    if (dst != m_state.vf[0])
    {
        const __m128i mask = vuLaneMask(destBits);
        const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst));
        const __m128i value = _mm_load_si128(reinterpret_cast<const __m128i *>(valueBits));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_or_si128(_mm_andnot_si128(mask, old), _mm_and_si128(mask, value)));
    }
}

template<uint32_t U,bool NS,bool NT,bool NA>
inline bool AotVU::fmacStatic(float* target,const float* given)
{
    if constexpr (sizeof(long double) != sizeof(double))
        return false;
    VuFmacKind kind;
    VuFmacSrc src;
    uint8_t sel = 0;
    bool toAcc = false;
    if (!vuDecodeFmac(U, kind, src, sel, toAcc))
    {
        // Upper NOP (special 0x2F/0x30): execUpper only records the instruction.
        if (toAcc && (sel == 0x2Fu || sel == 0x30u))
        {
            m_currentUpperInstruction = U;
            return true;
        }
        return false;
    }
    m_currentUpperInstruction = U;
    const uint8_t dest = DEST(U);
    if (dest == 0u)
        return true; // reference: result computed, no flag entry, no lane written

    const uint8_t fs = FS(U);
    const uint8_t ft = FT(U);
    __m128 s = (NS ? _mm_loadu_ps(m_state.vf[fs]) : vuNormalize4(_mm_loadu_ps(m_state.vf[fs])));
    __m128 t;
    switch (src)
    {
    case VuSrcBc:
        t = _mm_set1_ps(aotNormalizeOperand(m_state.vf[ft][sel & 3u]));
        break;
    case VuSrcQ:
        t = _mm_set1_ps(aotNormalizeOperand(m_state.q));
        break;
    case VuSrcI:
        t = _mm_set1_ps(aotNormalizeOperand(m_state.i));
        break;
    case VuSrcVt:
        t = (NT ? _mm_loadu_ps(m_state.vf[ft]) : vuNormalize4(_mm_loadu_ps(m_state.vf[ft])));
        break;
    default: // cross: x = vs.y*vt.z, y = vs.z*vt.x, z = vs.x*vt.y; w lane vs.w*vt.w (product sticky only)
    {
        const __m128 vt = (NT ? _mm_loadu_ps(m_state.vf[ft]) : vuNormalize4(_mm_loadu_ps(m_state.vf[ft])));
        s = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
        t = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 1, 0, 2));
        break;
    }
    }

    // Float result first (execUpper's expressions, lane for lane).
    const bool productSum = kind == VuMadd || kind == VuMsub;
    __m128 a = _mm_setzero_ps();
    __m128 product = _mm_setzero_ps();
    __m128 f;
    switch (kind)
    {
    case VuAdd: f = _mm_add_ps(s, t); break;
    case VuSub: f = _mm_sub_ps(s, t); break;
    case VuMul: f = _mm_mul_ps(s, t); break;
    default:
        a = (NA ? _mm_loadu_ps(m_state.acc) : vuNormalize4(_mm_loadu_ps(m_state.acc)));
        product = _mm_mul_ps(s, t);
        f = kind == VuMadd ? _mm_add_ps(a, product) : _mm_sub_ps(a, product);
        break;
    }
    if (src == VuSrcCross)
        f = _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(f), _mm_set_epi32(0, -1, -1, -1))); // w = 0.0f
    if (given)
        f = _mm_loadu_ps(given);
    const uint32_t destBits = kVuRev4[dest & 0xFu]; // component order

    // Shortcut lanes whose class provably is "normal, sign only" (proof, with x the real a op b or
    // acc +/- p, s the real acc +/- P, P the exact product, p = chop(P), r the float result, E the
    // reference's double result = chop_double(exact)):
    //   single add/sub/mul, exp(r) in [1,254], |r| != FLT_MAX: FLT_MIN <= |r| <= |E| <= |x| <
    //     next(|r|) <= FLT_MAX and sign(E) = sign(r). r == 0 or denormal is NOT taken (an exact zero vs
    //     an underflow needs the double), nor |r| == FLT_MAX (overflow needs the double).
    //   product-sum, exp(r) in [2,253], p normal, |p| != FLT_MAX, exp(p) <= exp(r) + 21:
    //     |s - x| = |P - p| < ulp(p) <= 2^(exp(r)-129) <= |r|/4, so |s| > (3/4)|r| >= (3/4)2^-125 > FLT_MIN,
    //     |s| < 2^(exp(r)-126) + 2^(exp(r)-129) <= 1.125*2^127 < FLT_MAX, and sign(s) = sign(x) = sign(r);
    //     chop_double keeps all three. The product's own class (sticky) is "sign only" because p is
    //     normal and not FLT_MAX (the single-mul case above). (Same conditions as Scotho/socom-unzipped
    //     productSumFastLanes, GPL-3.0, re-derived here against our double reference.)
    //   Any other dest lane (and OPMULA/OPMSUB's w lane, an exact zero) takes the double path below.
    {
        const __m128i rb = _mm_castps_si128(f);
        const __m128i er = _mm_and_si128(_mm_srli_epi32(rb, 23), _mm_set1_epi32(0xFF));
        const __m128i notMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(rb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                             _mm_set1_epi32(-1));
        __m128i ok;
        if (!productSum)
        {
            ok = _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi32(er, _mm_setzero_si128()), _mm_cmplt_epi32(er, _mm_set1_epi32(255))), notMax);
        }
        else
        {
            const __m128i pb = _mm_castps_si128(product);
            const __m128i ep = _mm_and_si128(_mm_srli_epi32(pb, 23), _mm_set1_epi32(0xFF));
            const __m128i pNotMax = _mm_xor_si128(_mm_cmpeq_epi32(_mm_and_si128(pb, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFF)),
                                                  _mm_set1_epi32(-1));
            ok = _mm_and_si128(_mm_cmpgt_epi32(er, _mm_set1_epi32(1)), _mm_cmplt_epi32(er, _mm_set1_epi32(254)));
            ok = _mm_and_si128(ok, _mm_and_si128(_mm_cmpgt_epi32(ep, _mm_setzero_si128()), _mm_cmplt_epi32(ep, _mm_set1_epi32(255))));
            ok = _mm_and_si128(ok, _mm_and_si128(pNotMax, _mm_cmpgt_epi32(_mm_add_epi32(er, _mm_set1_epi32(22)), ep)));
        }
        if ((static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(ok))) & destBits) == destBits)
        {
            const uint32_t sign = static_cast<uint32_t>(_mm_movemask_ps(f)) & destBits;
            const uint32_t extraSticky = productSum && (static_cast<uint32_t>(_mm_movemask_ps(product)) & destBits) ? 0x2u : 0u;
            alignas(16) uint32_t valueBits[4];
            _mm_store_ps(reinterpret_cast<float *>(valueBits), f);
            commitStatic<U>(target, toAcc, destBits, valueBits, 0u, sign, 0u, 0u, extraSticky);
            return true;
        }
    }

    // Cold operand classes reuse the shipping exact SIMD/double helper.
    return fastFmac(U,target,given);
}
