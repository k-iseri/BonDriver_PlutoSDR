#include "npy.h"

#include <cstdio>
#include <cstring>

namespace isdbt {

namespace {

size_t DtypeSize(const std::string& d) {
    if (d == "<c8") return 8;
    if (d == "<c16") return 16;
    if (d == "<f8") return 8;
    if (d == "<f4") return 4;
    if (d == "<i8") return 8;
    if (d == "<i4") return 4;
    if (d == "<i2") return 2;
    if (d == "|u1" || d == "|i1") return 1;
    throw std::runtime_error("npy: unsupported dtype " + d);
}

struct FileCloser {
    FILE* f;
    ~FileCloser() { if (f) std::fclose(f); }
};

}  // namespace

NpyArray NpyRead(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("npy: cannot open " + path);
    FileCloser closer{f};
    unsigned char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        throw std::runtime_error("npy: bad magic in " + path);
    }
    size_t header_len;
    if (magic[6] == 1) {
        unsigned char b[2];
        if (std::fread(b, 1, 2, f) != 2) throw std::runtime_error("npy: truncated header");
        header_len = b[0] | (b[1] << 8);
    } else {
        unsigned char b[4];
        if (std::fread(b, 1, 4, f) != 4) throw std::runtime_error("npy: truncated header");
        header_len = static_cast<size_t>(b[0]) | (static_cast<size_t>(b[1]) << 8) |
                     (static_cast<size_t>(b[2]) << 16) | (static_cast<size_t>(b[3]) << 24);
    }
    std::string header(header_len, '\0');
    if (std::fread(header.data(), 1, header_len, f) != header_len) {
        throw std::runtime_error("npy: truncated header dict");
    }

    NpyArray a;
    // 'descr': '<c8'
    size_t p = header.find("'descr'");
    if (p == std::string::npos) throw std::runtime_error("npy: no descr");
    p = header.find('\'', header.find(':', p));
    size_t q = header.find('\'', p + 1);
    a.dtype = header.substr(p + 1, q - p - 1);
    if (header.find("'fortran_order': False") == std::string::npos) {
        throw std::runtime_error("npy: fortran order is not supported");
    }
    // 'shape': (8, 13) / (8,) / ()
    p = header.find("'shape'");
    p = header.find('(', p);
    q = header.find(')', p);
    std::string dims = header.substr(p + 1, q - p - 1);
    size_t pos = 0;
    while (pos < dims.size()) {
        while (pos < dims.size() && (dims[pos] == ' ' || dims[pos] == ',')) ++pos;
        if (pos >= dims.size()) break;
        a.shape.push_back(std::strtoull(dims.c_str() + pos, nullptr, 10));
        while (pos < dims.size() && dims[pos] != ',') ++pos;
    }

    size_t bytes = a.elements() * DtypeSize(a.dtype);
    a.data.resize(bytes);
    if (std::fread(a.data.data(), 1, bytes, f) != bytes) {
        throw std::runtime_error("npy: truncated data in " + path);
    }
    return a;
}

void NpyWrite(const std::string& path, const std::string& dtype, const std::vector<size_t>& shape,
              const void* data, size_t bytes) {
    std::string dims;
    for (size_t d : shape) dims += std::to_string(d) + ", ";
    if (shape.size() > 1 && !dims.empty()) dims.resize(dims.size() - 2);  // 複数次元は末尾の ", " を落とす
    std::string dict = "{'descr': '" + dtype + "', 'fortran_order': False, 'shape': (" + dims + "), }";
    size_t total = 10 + dict.size() + 1;
    size_t pad = (64 - total % 64) % 64;
    dict += std::string(pad, ' ');
    dict += '\n';

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("npy: cannot write " + path);
    FileCloser closer{f};
    unsigned char head[10] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
                              static_cast<unsigned char>(dict.size() & 0xFF),
                              static_cast<unsigned char>(dict.size() >> 8)};
    std::fwrite(head, 1, 10, f);
    std::fwrite(dict.data(), 1, dict.size(), f);
    std::fwrite(data, 1, bytes, f);
}

}  // namespace isdbt
