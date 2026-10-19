bool ps2Vu1AotRun(int image,void* state,bool continuing,uint8_t*code,uint8_t*data,
 GS&gs,PS2Memory*memory,uint32_t entry,uint32_t top,uint32_t itop,uint32_t budget,void* transfer){
 static thread_local AotVU v;
 std::memcpy(&v.state(),state,sizeof(VU1State));v.resetScheduler();v.m_cycle=v.state().cycles;
 v.aotImage=image;v.aotEnabled=true;
 const uint64_t fallback0=v.fallbackPairs;
 if(continuing)v.resume(code,16384,data,16384,gs,memory,top,itop,budget);
 else v.execute(code,16384,data,16384,gs,memory,entry,top,itop,budget);
 if(v.fallbackPairs!=fallback0)g_aotFallbackCalls.fetch_add(1,std::memory_order_relaxed);
 std::memcpy(state,&v.state(),sizeof(VU1State));
 if(v.pipelinesPending() || v.m_programPending || v.m_stopRequested) {
     auto* bytes=static_cast<uint8_t*>(transfer);size_t offset=0;
     static_assert(sizeof(v.m_flagPipeline)<80000);std::memcpy(bytes+offset,&v.m_flagPipeline,sizeof(v.m_flagPipeline));offset+=sizeof(v.m_flagPipeline);
static_assert(sizeof(v.m_fdiv)<80000);std::memcpy(bytes+offset,&v.m_fdiv,sizeof(v.m_fdiv));offset+=sizeof(v.m_fdiv);
static_assert(sizeof(v.m_efu)<80000);std::memcpy(bytes+offset,&v.m_efu,sizeof(v.m_efu));offset+=sizeof(v.m_efu);
static_assert(sizeof(v.m_storePipeline)<80000);std::memcpy(bytes+offset,&v.m_storePipeline,sizeof(v.m_storePipeline));offset+=sizeof(v.m_storePipeline);
static_assert(sizeof(v.m_vfWritePipeline)<80000);std::memcpy(bytes+offset,&v.m_vfWritePipeline,sizeof(v.m_vfWritePipeline));offset+=sizeof(v.m_vfWritePipeline);
static_assert(sizeof(v.m_viWritePipeline)<80000);std::memcpy(bytes+offset,&v.m_viWritePipeline,sizeof(v.m_viWritePipeline));offset+=sizeof(v.m_viWritePipeline);
static_assert(sizeof(v.m_accWritePipeline)<80000);std::memcpy(bytes+offset,&v.m_accWritePipeline,sizeof(v.m_accWritePipeline));offset+=sizeof(v.m_accWritePipeline);
static_assert(sizeof(v.m_commitCalendar)<80000);std::memcpy(bytes+offset,&v.m_commitCalendar,sizeof(v.m_commitCalendar));offset+=sizeof(v.m_commitCalendar);
static_assert(sizeof(v.m_commitCycles)<80000);std::memcpy(bytes+offset,&v.m_commitCycles,sizeof(v.m_commitCycles));offset+=sizeof(v.m_commitCycles);
static_assert(sizeof(v.m_xgkick)<80000);std::memcpy(bytes+offset,&v.m_xgkick,sizeof(v.m_xgkick));offset+=sizeof(v.m_xgkick);
static_assert(sizeof(v.m_vfReady)<80000);std::memcpy(bytes+offset,&v.m_vfReady,sizeof(v.m_vfReady));offset+=sizeof(v.m_vfReady);
static_assert(sizeof(v.m_viReady)<80000);std::memcpy(bytes+offset,&v.m_viReady,sizeof(v.m_viReady));offset+=sizeof(v.m_viReady);
static_assert(sizeof(v.m_accReady)<80000);std::memcpy(bytes+offset,&v.m_accReady,sizeof(v.m_accReady));offset+=sizeof(v.m_accReady);
static_assert(sizeof(v.m_vfLatestWrite)<80000);std::memcpy(bytes+offset,&v.m_vfLatestWrite,sizeof(v.m_vfLatestWrite));offset+=sizeof(v.m_vfLatestWrite);
static_assert(sizeof(v.m_viLatestWrite)<80000);std::memcpy(bytes+offset,&v.m_viLatestWrite,sizeof(v.m_viLatestWrite));offset+=sizeof(v.m_viLatestWrite);
static_assert(sizeof(v.m_accLatestWrite)<80000);std::memcpy(bytes+offset,&v.m_accLatestWrite,sizeof(v.m_accLatestWrite));offset+=sizeof(v.m_accLatestWrite);
static_assert(sizeof(v.m_cycle)<80000);std::memcpy(bytes+offset,&v.m_cycle,sizeof(v.m_cycle));offset+=sizeof(v.m_cycle);
static_assert(sizeof(v.m_nextWriteSequence)<80000);std::memcpy(bytes+offset,&v.m_nextWriteSequence,sizeof(v.m_nextWriteSequence));offset+=sizeof(v.m_nextWriteSequence);
static_assert(sizeof(v.m_efuResourceReady)<80000);std::memcpy(bytes+offset,&v.m_efuResourceReady,sizeof(v.m_efuResourceReady));offset+=sizeof(v.m_efuResourceReady);
static_assert(sizeof(v.m_workingClip)<80000);std::memcpy(bytes+offset,&v.m_workingClip,sizeof(v.m_workingClip));offset+=sizeof(v.m_workingClip);
static_assert(sizeof(v.m_currentUpperInstruction)<80000);std::memcpy(bytes+offset,&v.m_currentUpperInstruction,sizeof(v.m_currentUpperInstruction));offset+=sizeof(v.m_currentUpperInstruction);
static_assert(sizeof(v.m_viBranchBackupValue)<80000);std::memcpy(bytes+offset,&v.m_viBranchBackupValue,sizeof(v.m_viBranchBackupValue));offset+=sizeof(v.m_viBranchBackupValue);
static_assert(sizeof(v.m_viBranchBackupReg)<80000);std::memcpy(bytes+offset,&v.m_viBranchBackupReg,sizeof(v.m_viBranchBackupReg));offset+=sizeof(v.m_viBranchBackupReg);
static_assert(sizeof(v.m_viBranchBackupValid)<80000);std::memcpy(bytes+offset,&v.m_viBranchBackupValid,sizeof(v.m_viBranchBackupValid));offset+=sizeof(v.m_viBranchBackupValid);
static_assert(sizeof(v.m_stopRequested)<80000);std::memcpy(bytes+offset,&v.m_stopRequested,sizeof(v.m_stopRequested));offset+=sizeof(v.m_stopRequested);
static_assert(sizeof(v.m_pendingHaltD)<80000);std::memcpy(bytes+offset,&v.m_pendingHaltD,sizeof(v.m_pendingHaltD));offset+=sizeof(v.m_pendingHaltD);
static_assert(sizeof(v.m_pendingHaltT)<80000);std::memcpy(bytes+offset,&v.m_pendingHaltT,sizeof(v.m_pendingHaltT));offset+=sizeof(v.m_pendingHaltT);
static_assert(sizeof(v.m_fast)<80000);std::memcpy(bytes+offset,&v.m_fast,sizeof(v.m_fast));offset+=sizeof(v.m_fast);
static_assert(sizeof(v.m_fastMaxWriteReady)<80000);std::memcpy(bytes+offset,&v.m_fastMaxWriteReady,sizeof(v.m_fastMaxWriteReady));offset+=sizeof(v.m_fastMaxWriteReady);
static_assert(sizeof(v.m_programPending)<80000);std::memcpy(bytes+offset,&v.m_programPending,sizeof(v.m_programPending));offset+=sizeof(v.m_programPending);
static_assert(sizeof(v.m_quietBudget)<80000);std::memcpy(bytes+offset,&v.m_quietBudget,sizeof(v.m_quietBudget));offset+=sizeof(v.m_quietBudget);
static_assert(sizeof(v.m_fastVfLog)<80000);std::memcpy(bytes+offset,&v.m_fastVfLog,sizeof(v.m_fastVfLog));offset+=sizeof(v.m_fastVfLog);
static_assert(sizeof(v.m_fastVfLogCount)<80000);std::memcpy(bytes+offset,&v.m_fastVfLogCount,sizeof(v.m_fastVfLogCount));offset+=sizeof(v.m_fastVfLogCount);
static_assert(sizeof(v.m_fastViLog)<80000);std::memcpy(bytes+offset,&v.m_fastViLog,sizeof(v.m_fastViLog));offset+=sizeof(v.m_fastViLog);
static_assert(sizeof(v.m_fastViLogCount)<80000);std::memcpy(bytes+offset,&v.m_fastViLogCount,sizeof(v.m_fastViLogCount));offset+=sizeof(v.m_fastViLogCount);
static_assert(sizeof(v.m_fastVisVf)<80000);std::memcpy(bytes+offset,&v.m_fastVisVf,sizeof(v.m_fastVisVf));offset+=sizeof(v.m_fastVisVf);
static_assert(sizeof(v.m_fastIssueVf)<80000);std::memcpy(bytes+offset,&v.m_fastIssueVf,sizeof(v.m_fastIssueVf));offset+=sizeof(v.m_fastIssueVf);
static_assert(sizeof(v.m_fastHasVf)<80000);std::memcpy(bytes+offset,&v.m_fastHasVf,sizeof(v.m_fastHasVf));offset+=sizeof(v.m_fastHasVf);
static_assert(sizeof(v.m_fastVisVi)<80000);std::memcpy(bytes+offset,&v.m_fastVisVi,sizeof(v.m_fastVisVi));offset+=sizeof(v.m_fastVisVi);
static_assert(sizeof(v.m_fastReadyVi)<80000);std::memcpy(bytes+offset,&v.m_fastReadyVi,sizeof(v.m_fastReadyVi));offset+=sizeof(v.m_fastReadyVi);
static_assert(sizeof(v.m_fastHasVi)<80000);std::memcpy(bytes+offset,&v.m_fastHasVi,sizeof(v.m_fastHasVi));offset+=sizeof(v.m_fastHasVi);
     return true;
 }
 return false;
}
