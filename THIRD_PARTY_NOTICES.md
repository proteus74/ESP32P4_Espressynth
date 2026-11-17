# Third-party notices

Espressynth itself is licensed under the Apache License 2.0 (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).
It includes or links the third-party components below. Each keeps its own license.

| Component | Where | License | Copyright |
|-----------|-------|---------|-----------|
| msfa (DX7 FM core) | `components/msfa/` (vendored) | Apache-2.0 | Google Inc.; Pascal Gauthier |
| TinySoundFont | `components/tiny_soundfont/` (vendored) | MIT | Bernhard Schelling; Steve Folta (SFZero) |
| LVGL 9.5.0 | `managed_components/lvgl__lvgl/` (IDF component manager) | MIT | LVGL Kft |
| littlefs for ESP-IDF 1.22.3 (`joltwallet/littlefs`) | `managed_components/joltwallet__littlefs/` (IDF component manager) | MIT | Brian Pugh |
| littlefs (bundled in the component above) | `managed_components/joltwallet__littlefs/src/littlefs/` | BSD-3-Clause | The littlefs authors; Arm Limited |
| WM8960 driver 0.0.3 (`jonlidgard/wm8960`) | `managed_components/jonlidgard__wm8960/` (IDF component manager) | MIT | Jon Lidgard; SparkFun Electronics |
| DX7 voice banks (SysEx data) | `littlefs_data/FM8/` | not stated by the source; rights stay with the original authors | Yamaha; the bank authors named below |

Other dependencies (ESP-IDF, TinyUSB, ESP-Hosted, the remaining Espressif and Waveshare components)
ship with their own license files in `managed_components/` and in the ESP-IDF tree.
LVGL additionally bundles third-party code, listed in its `COPYRIGHTS.md`.

---

## msfa (DX7 FM synthesis core) - Apache License 2.0

Files in `components/msfa/` (everything except `msfa_bridge.*` and `NOTICE.md`) are the DX7 synthesis
core of music-synthesizer-for-android, with the changes made by the Dexed project and the
MicroDexed/ESP32 ports.

- Copyright 2012-2013 Google Inc.
- Copyright 2016-2017 Pascal Gauthier

Licensed under the Apache License, Version 2.0; the full text is in [LICENSE](LICENSE) and at
<http://www.apache.org/licenses/LICENSE-2.0>. Each file carries its license header.

Obtained from <https://github.com/nyh-workshop/esp32-microdexed> (`main/`). Only the Apache-2.0
files were taken; the GPLv3 parts of that repository are not included.

Changes in this project: `fm_op_kernel.cpp` no longer includes MicroDexed's `config.h`; compiler
warnings are not treated as errors for this component. `msfa_bridge.*` is original code of this project.

## TinySoundFont - MIT License

Used in `components/tiny_soundfont/`.

```
Copyright (C) 2017-2023 Bernhard Schelling (Based on SFZero, Copyright (C) 2012 Steve Folta, https://github.com/stevefolta/SFZero)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

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

## LVGL - MIT License

Version 9.5.0, <https://lvgl.io>.

```
MIT licence
Copyright (c) 2025 LVGL Kft

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## littlefs for ESP-IDF (`joltwallet/littlefs`) - MIT License

Version 1.22.3, <https://github.com/joltwallet/esp_littlefs>.

```
Copyright 2020 Brian Pugh

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## littlefs (bundled in `joltwallet/littlefs`) - BSD-3-Clause

<https://github.com/littlefs-project/littlefs>

```
Copyright (c) 2022, The littlefs authors.
Copyright (c) 2017, Arm Limited. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

-  Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
-  Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.
-  Neither the name of ARM nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior
   written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## WM8960 driver (`jonlidgard/wm8960`) - MIT License

Version 0.0.3, based on the SparkFun WM8960 Arduino library.

```
The MIT License (MIT)

Copyright (c)   2025 Jon Lidgard
                2016 SparkFun Electronics

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## DX7 voice banks (`littlefs_data/FM8/`)

The SysEx banks seeded into the FM8 widget (`factory/`, `vrc/`, `greymatter/`) are DX7 voice data and were
obtained from the Yamaha Black Boxes DX7 patch collection:
<https://yamahablackboxes.com/collection/yamaha-dx7-synthesizer/patches/>

| Folder | Content (as named by the source) |
|--------|----------------------------------|
| `factory/` | Yamaha DX7 factory ROM banks ROM1A to ROM4B |
| `vrc/` | Yamaha VRC voice ROM cartridges VRC101 to VRC112 (VRC101-106: Yamaha sets; VRC107-112: selections by David Bristow, Gary Leuenberger, Studio 64, Bo Tomlyn (two sets) and Akira Inoue) |
| `greymatter/` | GreyMatter E! card soundbank disks #2, #5 and #7 |

The source page states no license or terms of use. The banks are not covered by this project's Apache-2.0
license: all rights remain with Yamaha and the respective authors, and the banks are included without
any claim of ownership. Rights holders who object to their inclusion can contact the project maintainer
and the banks will be removed.
