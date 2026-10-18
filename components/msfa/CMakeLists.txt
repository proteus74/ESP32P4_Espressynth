# msfa: the DX7 FM core of Google's music-synthesizer-for-android (Apache-2.0), as used by Dexed and
# MicroDexed. See NOTICE.md. Only msfa_bridge.h is meant to be included by the application.
idf_component_register(
    SRCS
        msfa_bridge.cpp
        dx7note.cpp
        env.cpp
        exp2.cpp
        fm_core.cpp
        fm_op_kernel.cpp
        freqlut.cpp
        lfo.cpp
        pitchenv.cpp
        sin.cpp
    INCLUDE_DIRS "."
)

# Third-party code (written for -Wall-less builds): keep warnings visible but do not fail the build.
# -O3 -funroll-loops: the 64-sample operator loops (fm_op_kernel.cpp) are ~22 instructions per
# sample with 4 of them loop/index overhead; unrolling removes most of that. Nothing here is
# size-critical (the kernels live in IRAM, ~1 KB).
# -fno-math-errno: the few float exp/log calls per block (dx7note.cpp) become plain calls.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-type-limits -Wno-unused-variable -Wno-unused-function
    -O3 -fno-math-errno -funroll-loops)
