cmake_minimum_required(VERSION 3.20)
project(BonDriver_PlutoSDR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# TVTest / EDCB に配る DLL なので CRT は静的リンク（再頒布可能パッケージ不要）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(MSVC)
  add_compile_options(/W4 /utf-8 /EHsc)
  # スタンドアロンモードの復調コアは速度が要るので rt/ と同じく Release は AVX2
  # （Haswell 以降前提。古い CPU 向けには -DISDBT_NO_AVX2=ON で外す）
  option(ISDBT_NO_AVX2 "AVX2 を使わない" OFF)
  if(NOT ISDBT_NO_AVX2)
    add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
  endif()
endif()

# 復調コア（rt/CMakeLists.txt と同じソース。スタンドアロンモードで DLL に静的リンクする）
add_library(isdbt_core STATIC
  ${RT_DIR}/isdbt/fft.cpp
  ${RT_DIR}/isdbt/npy.cpp
  ${RT_DIR}/isdbt/io.cpp
  ${RT_DIR}/isdbt/fec.cpp
  ${RT_DIR}/isdbt/pilots.cpp
  ${RT_DIR}/isdbt/tmcc.cpp
  ${RT_DIR}/isdbt/deinterleave.cpp
  ${RT_DIR}/isdbt/equalize.cpp
  ${RT_DIR}/isdbt/sync.cpp
  ${RT_DIR}/isdbt/demod.cpp
)
target_include_directories(isdbt_core PUBLIC ${RT_DIR}/isdbt)

add_library(BonDriver_PlutoSDR SHARED
  BonDriver_PlutoSDR.cpp
  BonDriver_PlutoSDR.def
  IBonDriver.h
  IBonDriver2.h
  ${RT_DIR}/demod/live_demod.h
  ${RT_DIR}/demod/live_demod.cpp
  ${RT_DIR}/demod/sdr_source.h
  ${RT_DIR}/demod/sdr_source.cpp
)
target_link_libraries(BonDriver_PlutoSDR PRIVATE isdbt_core ws2_32)
set_target_properties(BonDriver_PlutoSDR PROPERTIES PREFIX "" OUTPUT_NAME "BonDriver_PlutoSDR")

# SDR スタンドアロンモード: rt/third_party/libiio があれば有効化（x64 のみ。配布 .lib が x64 のため）。
# 無ければ [SDR] Enable=1 が実行時エラーになるだけで、ビルドとデーモンモードは通る。
# libiio1.dll は /DELAYLOAD にして、libiio が無い環境でも DLL 自体は読み込めるようにする
# （デーモンモードは libiio 無しで動く。実際のロードは OpenTuner の PreloadLibiio）。
set(ISDBT_LIBIIO_DIR "${RT_DIR}/third_party/libiio")
if(CMAKE_SIZEOF_VOID_P EQUAL 8
   AND EXISTS "${ISDBT_LIBIIO_DIR}/include/iio/iio.h"
   AND EXISTS "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
  message(STATUS "libiio found: ${ISDBT_LIBIIO_DIR} -> [SDR] standalone enabled")
  target_compile_definitions(BonDriver_PlutoSDR PRIVATE ISDBT_WITH_LIBIIO)
  target_include_directories(BonDriver_PlutoSDR PRIVATE "${ISDBT_LIBIIO_DIR}/include")
  target_link_libraries(BonDriver_PlutoSDR PRIVATE "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib" delayimp)
  target_link_options(BonDriver_PlutoSDR PRIVATE "/DELAYLOAD:libiio1.dll")
  # ADI のインストーラを入れない環境向けに、TVTest フォルダへコピーできる DLL 一式を隣に置く
  file(GLOB ISDBT_LIBIIO_DLLS "${ISDBT_LIBIIO_DIR}/bin/*.dll")
  foreach(dll ${ISDBT_LIBIIO_DLLS})
    add_custom_command(TARGET BonDriver_PlutoSDR POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:BonDriver_PlutoSDR>")
  endforeach()
else()
  message(STATUS "libiio not found (rt/third_party/libiio, x64) -> [SDR] standalone はスタブ")
endif()

# DLL を TVTest 無しで叩く確認用コンソール
add_executable(bontest bontest.cpp IBonDriver.h IBonDriver2.h)

# サンプル ini を DLL の隣に置く（既にあれば上書きしない）
add_custom_command(TARGET BonDriver_PlutoSDR POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/BonDriver_PlutoSDR.ini.sample
          $<TARGET_FILE_DIR:BonDriver_PlutoSDR>/BonDriver_PlutoSDR.ini.sample
)
