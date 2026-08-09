//-----------------------------------------------------------------------------
// FSR motion-vector beacon.
//
// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Copies BeamNG's screen-space velocity into a render target the native bridge
// can identify by its exact description, separately from the depth beacon. FSR
// reads a motion vector from channels 0 and 1 of its motion-vector resource,
// so those channels have to carry velocity and nothing else.
//
// As with the depth beacon: no filtering, no remapping, no clamping. The
// bridge applies the scale, the Y flip and the UV-versus-pixel conversion, all
// of which are user-adjustable because BeamNG's encoding is undocumented
// (docs/FEASIBILITY.md §5.2).
//-----------------------------------------------------------------------------

#include "shaders/common/postFx/postFx.hlsl"

TORQUE_UNIFORM_SAMPLER2D(velocityBuffer, 0);

//-----------------------------------------------------------------------------
// Output layout (R32G32B32A32_FLOAT)
//
//   .x = screen-space velocity X
//   .y = screen-space velocity Y
//   .zw = unused, written for determinism rather than left undefined.
//
// Both components are carried verbatim: dropping either loses the direction
// FSR reprojects with, and a packed magnitude cannot reconstruct it.
//-----------------------------------------------------------------------------

float4 main(PFXVertToPix IN) : TORQUE_TARGET0
{
    const float2 velocity = TORQUE_TEX2D(velocityBuffer, IN.uv0).rg;
    return float4(velocity.x, velocity.y, 0.0, 1.0);
}
