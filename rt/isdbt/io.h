// SigMF I/Q と TS ファイルの読み書き（ref/isdbt/io.py の C++ 版）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "params.h"

namespace isdbt {

struct SigmfMeta {
    std::string datatype;       // "cf32_le" / "ci16_le" / "ci8"
    double sample_rate = 0.0;
    double frequency = 0.0;     // captures[0] の core:frequency（無ければ 0）
};

// `.sigmf-meta` のパスまたは拡張子なしのベースパスを受け取る。
// ci16 は /2048、ci8 は /128 で正規化する（ref/isdbt/io.py と同じ規約）。
std::vector<cfloat> ReadSigmf(const std::string& path, SigmfMeta* meta);

std::vector<uint8_t> ReadFileBytes(const std::string& path);
void WriteFileBytes(const std::string& path, const uint8_t* data, size_t bytes);

}  // namespace isdbt
