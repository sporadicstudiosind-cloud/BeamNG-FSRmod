# Legal analysis

This project touches two licences that pull in different directions, and one of
them contradicts itself. Both are laid out here so you can check the reasoning
rather than take it on faith.

Nothing here is legal advice.

---

## 1. BeamNG.drive's EULA

The [BeamNG EULA](https://beamng.com/game/support/policies/eula/) prohibits, in
relevant part, that you "in whole or in part, reverse engineer, merge,
translate, disassemble or decompile the Software", while carving out that
**"Modding of the Software's content files is not reverse engineering"**. It
also forbids exploiting the Software for commercial purposes.

### What this project does and does not do

| Action | Taken? | Assessment |
| --- | --- | --- |
| Decompile or disassemble BeamNG binaries | **No — forbidden by project rule** | Would violate the EULA |
| Signature-scan or pattern-match BeamNG code | **No — forbidden by project rule** | Reverse engineering in substance |
| Patch BeamNG memory, or hook a BeamNG function | **No — forbidden by project rule** | Reverse engineering in substance |
| Modify, replace or overwrite a BeamNG file | **No** | The installer writes only to its own subdirectory |
| Hook Microsoft's DXGI / Direct3D 12 | **Yes** | Implementing a Microsoft-specified public interface; nothing of BeamNG's is examined |
| Content modding via the documented Lua and PostFX APIs | **Yes** | Explicitly carved out as permitted |
| Commercial exploitation | **No** | Free, non-commercial |

### The hard project rule

> The native component may hook **only** Microsoft-provided APIs. No BeamNG
> symbol, offset, signature, vtable or binary may be referenced, scanned,
> patched or depended upon.

This is enforced in review and is why `device_proxy.cpp` and `factory_proxy.cpp`
are pass-through COM wrappers rather than vtable patches or hook trampolines. It
also has a practical payoff: there is nothing version-specific to break, so
BeamNG updates do not break the bridge.

### Residual risk, stated plainly

Loading *any* DLL into BeamNG's process is not something BeamNG has explicitly
permitted. This project's position is that the EULA's prohibition targets
reverse engineering and modification of the Software, neither of which happens
here — but that is an interpretation, not a permission slip.

The precedent is that ReShade, which works the same way, is widely used with
BeamNG and BeamNG ships no anti-tamper that blocks it. That is evidence about
practice, not about permission.

If BeamNG objects, the right response is to stop distributing this, not to
argue. The feature is opt-in and the installer surfaces this paragraph.

---

## 2. The AMD FSR SDK — and a genuine conflict in AMD's own terms

There are **two conflicting licences** inside the AMD FSR SDK 2.3.0 checkout.

**The blanket licence** (`docs/license.md`) opens with *"The following license
applies to all files except as noted below"* and grants permission "to install,
reproduce, copy and distribute copies of the Software, **in binary form only**",
requires reproducing the notices, and forbids reverse engineering,
decompilation and disassembly. The "noted below" exception list covers samples,
documentation and tools — **it does not list the public API headers**.

**The per-file notices** on those same headers say something different. Each of
`api/include/ffx_api.h`, `ffx_api_types.h`, `ffx_api_loader.h`,
`dx12/ffx_api_dx12.h`, `upscalers/include/ffx_upscale.h`,
`framegeneration/include/ffx_framegeneration.h`,
`framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` and
`OpenSource/amd/antilag2/ffx_antilag2_dx12.h` carries an inline **MIT** grant —
"to deal in the Software without restriction… copy, modify, merge, publish,
distribute".

Under MIT the headers could be vendored freely. Under the blanket licence they
could not be redistributed in source form at all.

### Resolution adopted: the conservative reading

Treat the blanket binary-only licence as governing redistribution, and rely on
the per-file MIT notices only for *use*, not for *redistribution*. Concretely:

- ✅ **The signed AMD DLLs are redistributed** with the release package. This is
  explicitly permitted ("binary form"), and `docs/license.md` is reproduced in
  `THIRD_PARTY_NOTICES.md` and shipped alongside the binaries as
  `AMD_FSR_SDK_LICENSE.md`.
- ❌ **No AMD header or source file is vendored into this repository.** The build
  fetches the SDK via `tools/fetch_fsr_sdk.py`. This costs one build step and
  removes the ambiguity entirely.
- ❌ **No reverse engineering, decompilation or disassembly** of the AMD DLLs —
  and none is needed, because the API is public.
- ✅ The DLLs are copied byte-for-byte, never repackaged or modified.
  `tools/package.py` prints each file's SHA-256 so a recipient can verify that.

If you conclude the MIT notices govern and vendoring is fine, the only thing
that changes is that you can skip the fetch step. Nothing else in the project
depends on which reading is correct — which is the point of choosing the
conservative one.

---

## 3. AMD's driver-side FSR 4 upgrade

AMD requires, for the Adrenalin driver-level FSR upgrade, a *native, in-game*
FSR 3.1 (upscaling) or 3.1.4 (frame generation) integration on DirectX 12 using
**the signed FSR 3.1 DLL**, integrated per AMD's developer guidelines. AMD
explicitly states it is **not** compatible with Vulkan titles or with
"non-standard methods of integration of FSR 3.1 into their games, such as using
**third-party plug-ins**".

BeamNG.drive has no FSR integration, and this project *is* a third-party layer.
**BeamNG cannot qualify for the driver upgrade path**, and this project does not
claim it can. Native integration through the FidelityFX API — what this project
does — is the only route to FSR 4 in BeamNG.

---

## 4. Trademarks and affiliation

AMD, Radeon, FidelityFX, FSR and RDNA are trademarks of Advanced Micro Devices,
Inc. BeamNG.drive is a product of BeamNG GmbH. DirectX, Direct3D and Windows are
trademarks of Microsoft Corporation.

This project is not affiliated with, endorsed by, or supported by any of them.
