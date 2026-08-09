//-----------------------------------------------------------------------------
// FSR depth beacon.
//
// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Copies BeamNG's device depth into a render target the native bridge can
// identify by its exact description. See
// mods/shared/lua/ge/extensions/fsr_core/beacon.lua for why identification
// works this way, and why depth and motion vectors are two targets rather than
// one packed one.
//
// This shader deliberately does no filtering, remapping or clamping. FSR needs
// the engine's values as they are; converting them is the bridge's job, where
// the depth convention is configurable and can be verified against AMD's debug
// view. Transforming them here would bake in an assumption the user could not
// correct.
//-----------------------------------------------------------------------------

#include "shaders/common/postFx/postFx.hlsl"

TORQUE_UNIFORM_SAMPLER2D(prepassDepth, 0);

//-----------------------------------------------------------------------------
// Output layout (R32G32B32A32_FLOAT)
//
//   .x = device depth, exactly as the prepass wrote it. Reversed-Z vs standard
//        Z is not resolved here; the bridge is told which convention to assume
//        and exposes an override, because BeamNG does not document it.
//   .yzw = unused, written for determinism rather than left undefined.
//
// FSR reads depth from channel 0, which is why it is in .x and not elsewhere.
//-----------------------------------------------------------------------------

float4 main(PFXVertToPix IN) : TORQUE_TARGET0
{
    const float depth = TORQUE_TEX2D(prepassDepth, IN.uv0).r;
    return float4(depth, 0.0, 0.0, 1.0);
}
