#pragma once

#include <cstdint>

namespace rex::graphics::diag {

// REX_LOG_FROM_SUB=<n> keeps the per-draw diagnostics (REX_LOG_DRAWS,
// REX_LOG_READBACK, REX_LOG_OWNERSHIP) silent until submission n.
bool LogGateOpen();
void SetCurrentSubmission(uint64_t submission);

// REX_LOG_TEXTURE_BASE=<hex guest address> logs one texture's creation,
// invalidation, uploads, watch activity, reloads and destruction.
uint32_t LoggedTextureBase();
bool RangeCoversLoggedTexture(uint32_t start, uint32_t length);

// REX_VERIFY_TEXTURES=1 checks at every draw that the shared memory pages and
// bound textures the draw reads still match guest memory.
bool VerifyTextures();

// Per-submission draw sequence shared by DRAWLOG and the verification lines,
// so a stale resource can be matched to the draw that read it.
uint64_t CurrentSubmission();
void BeginDraw();
uint32_t CurrentDrawIndex();

}  // namespace rex::graphics::diag
