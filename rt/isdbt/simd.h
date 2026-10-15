// SIMD の有無。x86-64 は __AVX2__（CMake の Release が -mavx2 / /arch:AVX2 を付ける）、
// arm64（Apple Silicon / Raspberry Pi 4 以降の 64bit OS）は AArch64 で常にある NEON（Advanced SIMD）。
// 比較やデバッグ用に -DISDBT_NO_NEON（CMake の ISDBT_NO_NEON=ON）でスカラー版に戻せる。
// MSVC の arm64（_M_ARM64）は未確認なので対象にしていない
#pragma once

#if !defined(ISDBT_NO_NEON) && defined(__aarch64__) && defined(__ARM_NEON)
#define ISDBT_NEON 1
#include <arm_neon.h>
#endif
