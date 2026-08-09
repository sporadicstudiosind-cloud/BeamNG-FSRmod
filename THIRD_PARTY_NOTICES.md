# Third-party notices

This project redistributes and depends on software owned by others. The notices
below are reproduced as required by their licences.

---

## AMD FidelityFX / AMD FSR SDK

**Redistributed:** yes, in binary form only.

The release package includes these prebuilt, AMD-signed DLLs, taken unmodified
from `Kits/FidelityFX/signedbin` of the AMD FSR SDK:

| File | Purpose |
| --- | --- |
| `amd_fidelityfx_loader_dx12.dll` | The loader. Required; routes to the effect DLLs below. |
| `amd_fidelityfx_upscaler_dx12.dll` | FSR Upscaling 4.x (ML), FSR 3.1.5 and FSR 2.3.4 (analytical). |
| `amd_fidelityfx_framegeneration_dx12.dll` | FSR Frame Generation 4.x (ML) and 3.1.x (analytical), plus the frame-generation swapchain. |

No AMD binary is modified, repackaged, decompiled, disassembled or reverse
engineered. They are copied byte-for-byte. `tools/package.py` prints the SHA-256
of each file it copies so a recipient can verify this.

**Not redistributed:** AMD's headers and source. This repository contains no AMD
source file. The build fetches the SDK with `tools/fetch_fsr_sdk.py`. See
`docs/LEGAL.md` for why.

### AMD licence text

The following is reproduced verbatim from `docs/license.md` of the AMD FSR SDK:

```
The following license applies to all files except as noted below. 

Copyright (C) Advanced Micro Devices, Inc.
 
REDISTRIBUTION: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to install, reproduce, copy and distribute copies of the Software, in binary form only, and to permit persons to whom the Software is provided to do the same, provided that the following conditions are met: 
 
No reverse engineering, decompilation, or disassembly of this Software is permitted. 
 
Redistributions must reproduce the above copyright notice, this permission notice, and the following disclaimers and notices in the Software documentation and/or other materials provided with the Software. 
 
DISCLAIMER: THE USE OF THE SOFTWARE IS AT YOUR SOLE RISK.  THE SOFTWARE 
IS PROVIDED "AS IS" AND WITHOUT WARRANTY OF ANY KIND AND COPYRIGHT 
HOLDER AND ITS LICENSORS EXPRESSLY DISCLAIM ALL WARRANTIES, EXPRESS AND 
IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. 
COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT THAT THE SOFTWARE WILL 
MEET YOUR REQUIREMENTS, OR THAT THE OPERATION OF THE SOFTWARE WILL BE 
UNINTERRUPTED OR ERROR-FREE.  THE ENTIRE RISK ASSOCIATED WITH THE USE OF 
THE SOFTWARE IS ASSUMED BY YOU.  FURTHERMORE, COPYRIGHT HOLDER AND ITS 
LICENSORS DO NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE 
OR THE RESULTS OF THE USE OF THE SOFTWARE IN TERMS OF ITS CORRECTNESS, 
ACCURACY, RELIABILITY, CURRENTNESS, OR OTHERWISE. 
 
DISCLAIMER: UNDER NO CIRCUMSTANCES INCLUDING NEGLIGENCE, SHALL COPYRIGHT HOLDER AND ITS LICENSORS OR ITS DIRECTORS, OFFICERS, EMPLOYEES OR AGENTS ("AUTHORIZED REPRESENTATIVES") BE LIABLE FOR ANY INCIDENTAL, INDIRECT, 
SPECIAL OR CONSEQUENTIAL DAMAGES (INCLUDING DAMAGES FOR LOSS OF BUSINESS 
PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS INFORMATION, AND THE 
LIKE) ARISING OUT OF THE USE, MISUSE OR INABILITY TO USE THE SOFTWARE, 
BREACH OR DEFAULT, INCLUDING THOSE ARISING FROM INFRINGEMENT OR ALLEGED 
INFRINGEMENT OF ANY PATENT, TRADEMARK, COPYRIGHT OR OTHER INTELLECTUAL 
PROPERTY RIGHT EVEN IF COPYRIGHT HOLDER AND ITS AUTHORIZED 
REPRESENTATIVES HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.  IN 
NO EVENT SHALL COPYRIGHT HOLDER OR ITS AUTHORIZED REPRESENTATIVES TOTAL 
LIABILITY FOR ALL DAMAGES, LOSSES, AND CAUSES OF ACTION (WHETHER IN 
CONTRACT, TORT (INCLUDING NEGLIGENCE) OR OTHERWISE) EXCEED THE AMOUNT OF 
US$10.
```

The AMD FSR SDK also carries an MIT grant covering its samples, documentation
and tools, and the public API headers each carry an inline MIT notice. Neither
is relied upon for redistribution here — see `docs/LEGAL.md`.

AMD, the AMD Arrow logo, Radeon, FidelityFX, RDNA and combinations thereof are
trademarks of Advanced Micro Devices, Inc.

---

## BeamNG.drive

**Redistributed:** nothing.

This project contains no BeamNG code, assets or binaries, and modifies no
BeamNG file. The mod half uses only BeamNG's documented Lua and PostFX modding
APIs. The native half hooks only Microsoft-provided APIs (DXGI and Direct3D 12)
and does not reference, scan, patch or depend on any BeamNG symbol, offset or
binary.

BeamNG.drive is a product of BeamNG GmbH and is governed by its own EULA. This
project is not affiliated with, endorsed by, or supported by BeamNG GmbH.

---

## Microsoft DirectX

The proxy DLLs implement the public, documented DXGI and Direct3D 12 entry
points and forward every call to the genuine system libraries. No Microsoft
binary is redistributed or modified.

DirectX, Direct3D and Windows are trademarks of Microsoft Corporation.
