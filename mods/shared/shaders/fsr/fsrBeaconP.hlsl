//-----------------------------------------------------------------------------
// FSR depth / motion-vector beacon.
//
// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Packs BeamNG's device depth and screen-space velocity into a single
// R32G32_FLOAT render target that the native bridge can identify by its exact
// description. See mod/lua/ge/extensions/fsr/beacon.lua for why identification
// works this way.
//
// This shader deliberately does no filtering, remapping or clamping. FSR needs
// the engine's values as they are; converting them is the bridge's job, where
// the depth convention and motion-vector encoding are configurable and can be
// verified against AMD's debug view. Transforming them here would bake in an
// assumption the user could not correct.
//-----------------------------------------------------------------------------

#include "shaders/common/postFx/postFx.hlsl"

TORQUE_UNIFORM_SAMPLER2D(prepassDepth, 0);
TORQUE_UNIFORM_SAMPLER2D(velocityBuffer, 1);

uniform float2 targetSize;

//-----------------------------------------------------------------------------
// Output layout (R32G32B32A32_FLOAT)
//
//   .x = device depth, exactly as the prepass wrote it. Reversed-Z vs standard
//        Z is not resolved here; the bridge is told which convention to assume
//        and exposes an override, because BeamNG does not document it.
//   .y = screen-space velocity X
//   .z = screen-space velocity Y
//   .w = 1.0, reserved
//
// Both velocity components are carried: dropping either loses the direction
// FSR reprojects with, and a packed magnitude cannot reconstruct it.
//-----------------------------------------------------------------------------

float4 main(PFXVertToPix IN) : TORQUE_TARGET0
{
    const float2 uv = IN.uv0;

    // Depth: single channel, full precision preserved.
    const float depth = TORQUE_TEX2D(prepassDepth, uv).r;

    // Velocity: BeamNG's velocity buffer is two-channel screen-space motion.
    // Carried verbatim; the bridge applies the scale, Y-flip and UV-vs-pixel
    // conversion, all of which are user-adjustable because BeamNG's encoding is
    // undocumented (docs/FEASIBILITY.md §5.2).
    const float2 velocity = TORQUE_TEX2D(velocityBuffer, uv).rg;

    return float4(depth, velocity.x, velocity.y, 1.0);
}
