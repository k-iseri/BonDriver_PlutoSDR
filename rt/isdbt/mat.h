// 行優先の素朴な 2 次元配列（復調パイプラインの中間データ用）。
#pragma once
#include <cstddef>
#include <vector>

namespace isdbt {

template <typename T>
struct Mat {
    size_t rows = 0, cols = 0;
    std::vector<T> v;

    Mat() = default;
    Mat(size_t r, size_t c) : rows(r), cols(c), v(r * c) {}
    T* operator[](size_t r) { return v.data() + r * cols; }
    const T* operator[](size_t r) const { return v.data() + r * cols; }
    T* data() { return v.data(); }
    const T* data() const { return v.data(); }
    size_t size() const { return v.size(); }
};

}  // namespace isdbt
