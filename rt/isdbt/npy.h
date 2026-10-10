// 最小限の .npy 読み書き（バージョン 1.0、C 順のみ）。
// ref/ が吐く段ごとのテストベクタ（complex64 / float32 / uint8 / int64）を読むために使う。
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace isdbt {

struct NpyArray {
    std::string dtype;              // "<c8", "<f4", "|u1", "<i8" など（numpy の descr 表記）
    std::vector<size_t> shape;
    std::vector<uint8_t> data;      // 生バイト列（リトルエンディアン前提 = x86/x64）

    size_t elements() const {
        size_t n = 1;
        for (size_t d : shape) n *= d;
        return n;
    }
    size_t rows() const { return shape.empty() ? 0 : shape[0]; }
    size_t cols() const { return shape.size() < 2 ? 1 : shape[1]; }

    template <typename T>
    const T* as() const { return reinterpret_cast<const T*>(data.data()); }
};

NpyArray NpyRead(const std::string& path);
void NpyWrite(const std::string& path, const std::string& dtype, const std::vector<size_t>& shape,
              const void* data, size_t bytes);

}  // namespace isdbt
