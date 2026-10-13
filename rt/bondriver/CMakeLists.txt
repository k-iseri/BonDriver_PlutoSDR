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
  # ISDB-C（ケーブル TV トラモジ、J.83 Annex C + TSMF）
  ${RT_DIR}/isdbc/mapping.cpp
  ${RT_DIR}/isdbc/framing.cpp
  ${RT_DIR}/isdbc/tsmf.cpp
  ${RT_DIR}/isdbc/demod.cpp
)
target_include_directories(isdbt_core PUBLIC ${RT_DIR}/isdbt ${RT_DIR}/isdbc)

# SDR スタンドアロンモード: rt/third_party/libiio があれば有効化（x64 のみ。配布 .lib が x64 のため）。
# 無ければ [SDR] Enable=1 が実行時エラーになるだけで、ビルドとデーモンモードは通る。
# libiio1.dll は /DELAYLOAD にして、libiio が無い環境でも DLL 自体は読み込めるようにする
# （デーモンモードは libiio 無しで動く。実際のロードは OpenTuner の PreloadLibiio）。
set(ISDBT_LIBIIO_DIR "${RT_DIR}/third_party/libiio")
set(ISDBT_HAVE_LIBIIO OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8
   AND EXISTS "${ISDBT_LIBIIO_DIR}/include/iio/iio.h"
   AND EXISTS "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib")
  set(ISDBT_HAVE_LIBIIO ON)
  message(STATUS "libiio found: ${ISDBT_LIBIIO_DIR} -> [SDR] standalone enabled")
else()
  message(STATUS "libiio not found (rt/third_party/libiio, x64) -> [SDR] standalone はスタブ")
endif()

set(BON_SOURCES
  BonDriver_PlutoSDR.cpp
  BonDriver_PlutoSDR.def
  IBonDriver.h
  IBonDriver2.h
  ${RT_DIR}/demod/live_demod.h
  ${RT_DIR}/demod/live_demod.cpp
  ${RT_DIR}/demod/live_isdbc.h
  ${RT_DIR}/demod/live_isdbc.cpp
  ${RT_DIR}/demod/live_switch.h
  ${RT_DIR}/demod/live_switch.cpp
  ${RT_DIR}/demod/sdr_source.h
  ${RT_DIR}/demod/sdr_source.cpp
  ${RT_DIR}/demod/rate_monitor.h
)

# 同じソースから 2 つの DLL を作る。ini / log は DLL 名から実行時に導出される
# （BonDriver_PlutoSDR_T.ini / BonDriver_PlutoSDR_TC.ini。BonDriver_PlutoSDR.cpp の LoadConfig 参照）。
#   T  … 地デジ（ISDB-T）のみ
#   TC … 地デジ + ケーブル TV の BS/CS トランスモジュレーション（ISDB-C）。BONDRIVER_CATV 定義で空間 1 = CATV
foreach(variant T TC)
  set(tgt BonDriver_PlutoSDR_${variant})
  add_library(${tgt} SHARED ${BON_SOURCES})
  target_link_libraries(${tgt} PRIVATE isdbt_core ws2_32)
  set_target_properties(${tgt} PROPERTIES PREFIX "" OUTPUT_NAME "BonDriver_PlutoSDR_${variant}")
  if(variant STREQUAL "TC")
    target_compile_definitions(${tgt} PRIVATE BONDRIVER_CATV)
  endif()
  if(ISDBT_HAVE_LIBIIO)
    target_compile_definitions(${tgt} PRIVATE ISDBT_WITH_LIBIIO)
    target_include_directories(${tgt} PRIVATE "${ISDBT_LIBIIO_DIR}/include")
    target_link_libraries(${tgt} PRIVATE "${ISDBT_LIBIIO_DIR}/lib/libiio1.lib" delayimp)
    target_link_options(${tgt} PRIVATE "/DELAYLOAD:libiio1.dll")
    # ADI のインストーラを入れない環境向けに、TVTest フォルダへコピーできる DLL 一式を隣に置く
    file(GLOB ISDBT_LIBIIO_DLLS "${ISDBT_LIBIIO_DIR}/bin/*.dll")
    foreach(dll ${ISDBT_LIBIIO_DLLS})
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${tgt}>")
    endforeach()
  endif()
  # サンプル ini を DLL の隣に置く（DLL 名に合わせたファイル名で）
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/BonDriver_PlutoSDR.ini.sample
            $<TARGET_FILE_DIR:${tgt}>/BonDriver_PlutoSDR_${variant}.ini.sample
  )
endforeach()

# DLL を TVTest 無しで叩く確認用コンソール
add_executable(bontest bontest.cpp IBonDriver.h IBonDriver2.h)
