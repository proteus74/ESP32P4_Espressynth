# msfa (DX7 sound engine)

The files in this directory (everything except `msfa_bridge.*` and this file) are the DX7 synthesis
core of **music-synthesizer-for-android** (msfa), (c) Google Inc., with the changes made by the
Dexed project (Pascal Gauthier) and the MicroDexed/ESP32 ports. They are licensed under the
**Apache License, Version 2.0** - see the header of each file and
<https://www.apache.org/licenses/LICENSE-2.0>.

Obtained from <https://github.com/nyh-workshop/esp32-microdexed> (`main/`). Only the
Apache-2.0 msfa files were taken; the GPLv3 parts of that repository (dexed.cpp, EngineMkI/Opl,
MicroDexed config) are intentionally NOT included.

Local changes: `fm_op_kernel.cpp` no longer includes MicroDexed's `config.h`; compiler warnings are not errors for this component.
`msfa_bridge.*` is original code of this project.
