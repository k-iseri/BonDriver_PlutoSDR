cmake_minimum_required(VERSION 3.20)
project(isdbt_rt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# デーモンも配布しうるので CRT は静的リンク（bondriver と同じ方針）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc)
  # 復調コアは速度が要るので Release は常に最適化 + AVX2（Haswell 以降前提。
  # 古い CPU 向けには -DISDBT_NO_AVX2=ON で外す）
  option(ISDBT_NO_AVX2 "AVX2 を使わない" OFF)
  if(NOT ISDBT_NO_AVX2)
    add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
  endif()
endif()

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
)
target_include_directories(isdbt_core PUBLIC isdbt)

# ref/ とのビット一致テスト
add_executable(rt_test tests/rt_test.cpp)
target_link_libraries(rt_test PRIVATE isdbt_core)

# 実時間復調デーモン
add_executable(isdbtd demod/isdbtd.cpp demod/sdr_source.h demod/sdr_source.cpp
  demod/live_demod.h demod/live_demod.cpp)
target_link_libraries(isdbtd PRIVATE isdbt_core ws2_32)

# SDR ライブ受信（--sdr）: rt/third_party/libiio があれば有効化する
# （無ければ --sdr が実行時エラーになるだけで、ビルドは通る。
#   取得は rt/third_party/fetch_libiio.ps1）
set(ISDBT_LIBIIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libiio")
if(EXISTS "${ISDBT_LIBIIO_DIR}/include/iio/iio.h" AND EXISTS "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
  message(STATUS "libiio found: ${ISDBT_LIBIIO_DIR} -> --sdr enabled")
  target_compile_definitions(isdbtd PRIVATE ISDBT_WITH_LIBIIO)
  target_include_directories(isdbtd PRIVATE "${ISDBT_LIBIIO_DIR}/include")
  target_link_libraries(isdbtd PRIVATE "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
  file(GLOB ISDBT_LIBIIO_DLLS "${ISDBT_LIBIIO_DIR}/bin/*.dll")
  foreach(dll ${ISDBT_LIBIIO_DLLS})
    add_custom_command(TARGET isdbtd POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:isdbtd>")
  endforeach()
else()
  message(STATUS "libiio not found (rt/third_party/libiio) -> --sdr はスタブ")
endif()
