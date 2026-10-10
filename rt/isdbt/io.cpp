#include "io.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace isdbt {

namespace {

std::string BasePath(const std::string& path) {
    auto ends_with = [&](const char* suf) {
        size_t n = std::strlen(suf);
        return path.size() >= n && path.compare(path.size() - n, n, suf) == 0;
    };
    if (ends_with(".sigmf-meta")) return path.substr(0, path.size() - 11);
    if (ends_with(".sigmf-data")) return path.substr(0, path.size() - 11);
    return path;
}

// メタは 1 階層の JSON として既知キーだけ拾う（BonDriver と同じ最小実装方針）
std::string FindJsonString(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return {};
    p = json.find(':', p + pat.size());     // キー自体に ':' を含むことがある（core:datatype）
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    size_t q = json.find('"', p + 1);
    return json.substr(p + 1, q - p - 1);
}

double FindJsonNumber(const std::string& json, const std::string& key, double def) {
    const std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return def;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return def;
    return std::strtod(json.c_str() + p + 1, nullptr);
}

}  // namespace

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(got);
    return buf;
}

void WriteFileBytes(const std::string& path, const uint8_t* data, size_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fwrite(data, 1, bytes, f);
    std::fclose(f);
}

std::vector<cfloat> ReadSigmf(const std::string& path, SigmfMeta* meta) {
    std::string base = BasePath(path);
    std::vector<uint8_t> mraw = ReadFileBytes(base + ".sigmf-meta");
    std::string mjson(mraw.begin(), mraw.end());
    SigmfMeta m;
    m.datatype = FindJsonString(mjson, "core:datatype");
    m.sample_rate = FindJsonNumber(mjson, "core:sample_rate", 0.0);
    m.frequency = FindJsonNumber(mjson, "core:frequency", 0.0);
    if (meta) *meta = m;

    std::vector<uint8_t> raw = ReadFileBytes(base + ".sigmf-data");
    std::vector<cfloat> x;
    if (m.datatype.rfind("cf32", 0) == 0) {
        size_t n = raw.size() / 8;
        x.resize(n);
        std::memcpy(x.data(), raw.data(), n * 8);
    } else if (m.datatype.rfind("ci16", 0) == 0) {
        const int16_t* s = reinterpret_cast<const int16_t*>(raw.data());
        size_t n = raw.size() / 4;
        x.resize(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = cfloat(s[2 * i] / 2048.0f, s[2 * i + 1] / 2048.0f);
        }
    } else if (m.datatype.rfind("ci8", 0) == 0) {
        const int8_t* s = reinterpret_cast<const int8_t*>(raw.data());
        size_t n = raw.size() / 2;
        x.resize(n);
        for (size_t i = 0; i < n; ++i) {
            x[i] = cfloat(s[2 * i] / 128.0f, s[2 * i + 1] / 128.0f);
        }
    } else {
        throw std::runtime_error("unsupported sigmf datatype: " + m.datatype);
    }
    return x;
}

}  // namespace isdbt
