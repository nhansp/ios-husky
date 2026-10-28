# Third-Party Notices: `libair2valhall`

`libair2valhall` is MIT licensed (see `LICENSE`). It incorporates and links against code from Mesa, the `air2ir3` front end, and LLVM, each under their respective open-source licenses as detailed below.

---

## 1. Summary of Third-Party Components

| Component / Files | Upstream Source & Revision | License | Primary Copyright Holders | Description & Usage |
|---|---|---|---|---|
| **Mesa Panfrost & NIR**<br>`src/panfrost/compiler/`<br>`src/compiler/nir/`<br>`src/panfrost/genxml/`<br>`src/util/` | Mesa @ commit `1f9035d` | MIT | Copyright © Collabora Ltd.<br>Copyright © Alyssa Rosenzweig<br>Copyright © Arm Ltd.<br>Copyright © Intel Corporation<br>Copyright © Google LLC<br>and Mesa contributors | Valhall arch 11 backend code generator (`bifrost/valhall`), NIR optimization and lowering passes, genxml (`v11.xml`) descriptor definitions, and utility data structures. |
| **air2ir3 Front End**<br>`src/air2ir3.h`<br>`src/vh_frontend.cpp`<br>`src/vh_sampler.c` | `refs/air2ir3` (MIT) | MIT | Copyright © air2ir3 contributors<br>Copyright © 2016 Rob Clark<br>Copyright © 2018 Google, Inc. | AIR LLVM bitcode parsing and initial translation into NIR. Sampler decode logic in `vh_sampler.c` is adapted from `air2ir3/src/a8xx_sampler.c` (derived from freedreno `fd6_texture.cc`). |
| **LLVM Libraries** | LLVM 18/19/20/21/22 | Apache-2.0 with LLVM Exception | LLVM Project contributors | LLVM bitcode reader and core IR libraries used at build time to ingest AIR bitcode. Linked as dynamic/static system libraries. |

---

## 2. License Texts

### 2.1 Mesa (MIT License)

```text
Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### 2.2 air2ir3 (MIT License)

```text
Copyright (c) 2024 air2ir3 contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### 2.3 LLVM (Apache License v2.0 with LLVM Exceptions)

```text
==============================================================================
The LLVM with Runtime Exception is available under Apache License v2.0
with the LLVM Exceptions.
==============================================================================
See https://llvm.org/LICENSE.txt for full license and exception terms.
```
