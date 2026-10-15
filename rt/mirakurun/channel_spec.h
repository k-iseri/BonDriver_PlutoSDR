// recpluto のチャンネル表記（Mirakurun の channels.yml の channel がそのまま来る）:
//   "27" / "27ch"  … 地デジ UHF の物理チャンネル 13〜62（ChannelToHz）
//   "C56" / "C56ch" … ケーブル TV の物理チャンネル C13〜C63（BS/CS 再送信、ISDB-C）。isdbc::CatvChannel の
//                     通し番号（BonDriver_BDA 互換: C13〜C22 = 62〜71、C23〜C63 = 72〜112）に変換する
// --scan の対象指定: "gr" / "catv" / "all"、範囲は "gr:24-27" / "catv:C13-C22"
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

#include "../demod/live_switch.h"
#include "../isdbc/params.h"

namespace isdbt {
namespace mirakurun {

inline bool ParseChannel(std::string s, System* sys, int* ch, double* freq) {
    if (s.size() > 2 && (s.compare(s.size() - 2, 2, "ch") == 0 || s.compare(s.size() - 2, 2, "CH") == 0)) s.resize(s.size() - 2);
    if (s.empty()) return false;
    if (s[0] == 'C' || s[0] == 'c') {
        for (size_t i = 1; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
        const int n = std::atoi(s.c_str() + 1);
        int index;
        if (n >= 13 && n <= 22) index = 62 + (n - 13);
        else if (n >= 23 && n <= 63) index = 72 + (n - 23);
        else return false;
        *sys = System::kIsdbc;
        *ch = index;
        return isdbc::CatvChannel(index, freq, nullptr);
    }
    for (char c : s) if (c < '0' || c > '9') return false;
    const int n = std::atoi(s.c_str());
    if (n < 13 || n > 62) return false;
    *sys = System::kIsdbt;
    *ch = n;
    *freq = ChannelToHz(n);
    return true;
}

// 表示用: 地デジは "27"、CATV は "C56"
inline std::string ChannelLabel(System sys, int ch) {
    if (sys == System::kIsdbt) return std::to_string(ch);
    std::string name;
    isdbc::CatvChannel(ch, nullptr, &name);        // "C56ch"
    if (name.size() > 2) name.resize(name.size() - 2);
    return name;
}

inline bool ScanTargets(const std::string& spec, std::vector<std::string>* out) {
    std::string kind = spec, range;
    const size_t colon = spec.find(':');
    if (colon != std::string::npos) { kind = spec.substr(0, colon); range = spec.substr(colon + 1); }
    auto add = [&](bool catv) {
        int lo = 13, hi = catv ? 63 : 62;
        if (!range.empty()) {
            const size_t dash = range.find('-');
            if (dash == std::string::npos) return false;
            std::string a = range.substr(0, dash), b = range.substr(dash + 1);
            if (!a.empty() && (a[0] == 'C' || a[0] == 'c')) a.erase(0, 1);
            if (!b.empty() && (b[0] == 'C' || b[0] == 'c')) b.erase(0, 1);
            lo = std::atoi(a.c_str());
            hi = std::atoi(b.c_str());
            if (lo <= 0 || hi < lo) return false;
        }
        for (int n = lo; n <= hi; ++n) out->push_back((catv ? "C" : "") + std::to_string(n));
        return true;
    };
    if (kind == "gr") return add(false);
    if (kind == "catv") return add(true);
    if (kind == "all" || kind.empty()) {
        if (!range.empty()) return false;
        return add(false) && add(true);
    }
    return false;
}

}  // namespace mirakurun
}  // namespace isdbt
