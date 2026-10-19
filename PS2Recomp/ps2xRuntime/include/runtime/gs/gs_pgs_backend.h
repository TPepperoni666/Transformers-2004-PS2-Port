#pragma once
// TEMP(transformers): GS backend selection. PS2X_GS_BACKEND=PGS -> parallel-gs (M1, needs -DPS2X_WITH_PGS=ON,
// src/lib/gs/gs_pgs_backend.cpp); anything else -> GSCpuBackend (default).
#include "runtime/gs/gs_backend.h"
#include <memory>
std::unique_ptr<GSRasterBackend> CreateSelectedGSBackend();
