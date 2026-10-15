cmake_minimum_required(VERSION 3.20)
project(isdbt_rt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 単一構成ジェネレータ（Linux / macOS の Makefile, Ninja）は既定を Release にする
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# デーモンも配布しうるので CRT は静的リンク（bondriver と同じ方針）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
# 復調コアは速度が要るので Release は常に最適化 + AVX2（x86-64 は Haswell 以降前提。
# 古い CPU 向けには -DISDBT_NO_AVX2=ON で外す）。arm64（Apple Silicon / Raspberry Pi）では
# AArch64 に常にある NEON 版（isdbt/simd.h）が使われ、-DISDBT_NO_NEON=ON でスカラー版に戻せる
option(ISDBT_NO_AVX2 "AVX2 を使わない" OFF)
option(ISDBT_NO_NEON "NEON を使わない（arm64。速度比較・切り分け用）" OFF)
if(ISDBT_NO_NEON)
  add_compile_definitions(ISDBT_NO_NEON)
endif()
if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc)
  if(NOT ISDBT_NO_AVX2)
    add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
  endif()
else()
  add_compile_options(-Wall -Wextra)
  # クロスビルド（macOS の CMAKE_OSX_ARCHITECTURES=x86_64 など）は CMAKE_SYSTEM_PROCESSOR がホストのままなので別に見る
  if(NOT ISDBT_NO_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"))
    add_compile_options($<$<CONFIG:Release>:-mavx2> $<$<CONFIG:Release>:-mfma>)
  endif()
endif()

find_package(Threads REQUIRED)

# 復調コア（静的ライブラリ）
add_library(isdbt_core STATIC
  isdbt/params.h
  isdbt/isdbt_tables.h
  isdbt/mat.h
  isdbt/fft.h isdbt/fft.cpp
  isdbt/npy.h isdbt/npy.cpp
  isdbt/io.h isdbt/io.cpp
  isdbt/fec.h isdbt/fec.cpp
  isdbt/pilots.h isdbt/pilots.cpp
  isdbt/tmcc.h isdbt/tmcc.cpp
  isdbt/deinterleave.h isdbt/deinterleave.cpp
  isdbt/equalize.h isdbt/equalize.cpp
  isdbt/sync.h isdbt/sync.cpp
  isdbt/demod.h isdbt/demod.cpp
  # ISDB-C（ケーブル TV トラモジ）: J.83 Annex C + TSMF
  isdbc/params.h
  isdbc/mapping.h isdbc/mapping.cpp
  isdbc/framing.h isdbc/framing.cpp
  isdbc/tsmf.h isdbc/tsmf.cpp
  isdbc/demod.h isdbc/demod.cpp
)
target_include_directories(isdbt_core PUBLIC isdbt isdbc)
target_link_libraries(isdbt_core PUBLIC Threads::Threads)

# ref/ とのビット一致テスト
add_executable(rt_test tests/rt_test.cpp)
target_link_libraries(rt_test PRIVATE isdbt_core)

# 実時間復調デーモン
add_executable(isdbtd demod/isdbtd.cpp demod/sdr_source.h demod/sdr_source.cpp
  demod/live_demod.h demod/live_demod.cpp demod/live_isdbc.h demod/live_isdbc.cpp
  demod/live_switch.h demod/live_switch.cpp demod/rate_monitor.h)
target_link_libraries(isdbtd PRIVATE isdbt_core)
if(WIN32)
  target_link_libraries(isdbtd PRIVATE ws2_32)
endif()

# Mirakurun 向けチューナーコマンド（recpt1 互換の引数。rt/mirakurun/recpluto.cpp）。isdbtd と同じライブパイプライン
add_executable(recpluto mirakurun/recpluto.cpp demod/sdr_source.h demod/sdr_source.cpp
  demod/live_demod.h demod/live_demod.cpp demod/live_isdbc.h demod/live_isdbc.cpp
  demod/live_switch.h demod/live_switch.cpp demod/rate_monitor.h)
target_link_libraries(recpluto PRIVATE isdbt_core)
if(NOT WIN32)
  install(TARGETS recpluto isdbtd RUNTIME DESTINATION bin)
endif()

# SDR ライブ受信（--sdr）: libiio 1.x があれば有効化する
# （無ければ --sdr が実行時エラーになるだけで、ビルドは通る）
#   Windows: rt/third_party/libiio（取得は rt/third_party/fetch_libiio.ps1）
#   Linux / macOS: pkg-config の libiio（1.0 以降）、無ければ iio/iio.h と libiio を探す
set(ISDBT_LIBIIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libiio")
if(WIN32)
  if(EXISTS "${ISDBT_LIBIIO_DIR}/include/iio/iio.h" AND EXISTS "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
    message(STATUS "libiio found: ${ISDBT_LIBIIO_DIR} -> --sdr enabled")
    foreach(t isdbtd recpluto)
      target_compile_definitions(${t} PRIVATE ISDBT_WITH_LIBIIO)
      target_include_directories(${t} PRIVATE "${ISDBT_LIBIIO_DIR}/include")
      target_link_libraries(${t} PRIVATE "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
    endforeach()
    file(GLOB ISDBT_LIBIIO_DLLS "${ISDBT_LIBIIO_DIR}/bin/*.dll")
    foreach(dll ${ISDBT_LIBIIO_DLLS})
      add_custom_command(TARGET isdbtd POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:isdbtd>")
    endforeach()
  else()
    message(STATUS "libiio not found (rt/third_party/libiio) -> --sdr はスタブ")
  endif()
else()
  set(ISDBT_LIBIIO_FOUND OFF)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBIIO QUIET IMPORTED_TARGET libiio>=1.0)
    if(LIBIIO_FOUND)
      target_link_libraries(isdbtd PRIVATE PkgConfig::LIBIIO)
      target_link_libraries(recpluto PRIVATE PkgConfig::LIBIIO)
      set(ISDBT_LIBIIO_FOUND ON)
      message(STATUS "libiio ${LIBIIO_VERSION} (pkg-config) -> --sdr enabled")
    endif()
  endif()
  if(NOT ISDBT_LIBIIO_FOUND)
    # libiio 1.x のヘッダは <iio/iio.h>（0.x は <iio.h> で API が違うので対象外）
    find_path(ISDBT_LIBIIO_INCLUDE iio/iio.h HINTS /usr/local/include /opt/homebrew/include)
    find_library(ISDBT_LIBIIO_LIB NAMES iio HINTS /usr/local/lib /opt/homebrew/lib)
    if(ISDBT_LIBIIO_INCLUDE AND ISDBT_LIBIIO_LIB)
      foreach(t isdbtd recpluto)
        target_include_directories(${t} PRIVATE "${ISDBT_LIBIIO_INCLUDE}")
        target_link_libraries(${t} PRIVATE "${ISDBT_LIBIIO_LIB}")
      endforeach()
      set(ISDBT_LIBIIO_FOUND ON)
      message(STATUS "libiio found: ${ISDBT_LIBIIO_LIB} -> --sdr enabled")
    endif()
  endif()
  if(ISDBT_LIBIIO_FOUND)
    target_compile_definitions(isdbtd PRIVATE ISDBT_WITH_LIBIIO)
    target_compile_definitions(recpluto PRIVATE ISDBT_WITH_LIBIIO)
  else()
    message(STATUS "libiio 1.x not found -> --sdr はスタブ（libiio v1 をソースからビルド。手順は docs/mirakurun.md）")
  endif()
endif()
