// recpluto のチャンネル表記（Mirakurun の channels.yml の channel がそのまま来る）:
//   "27" / "27ch"  … UHF の物理チャンネル 13〜62。方式のヒントが無ければ地デジ（ISDB-T、473 + 1/7 MHz + 6 MHz × (ch − 13)）、
//                     ヒントが ISDB-C ならケーブルの UHF 帯トラモジ（473 + 6 MHz × (ch − 13)、1/7 MHz オフセット無し）
//   "C56" / "C56ch" … ケーブルの C 帯 C13〜C63（ISDB-C）
// 方式のヒントは Mirakurun の <type>（GR = 地デジ、BS / CS / SKY = ケーブル）か --system で与える。
// ISDB-C の番号は isdbc::CatvChannel の通し番号（BonDriver_BDA 互換: UHF 13〜62 = 12〜61、C13〜C22 = 62〜71、
// C23〜C63 = 72〜112）に変換する。
// --scan の対象指定: "gr"（地デジ 13〜62）、"catv"（ケーブル C13〜C63 と UHF 帯 13〜62 を ISDB-C で）、"all"、
// 範囲は "gr:24-27" / "catv:C13-C22" / "catv:50-60"（数字だけなら UHF 帯の ISDB-C）
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

#include "../demod/live_switch.h"
#include "../isdbc/params.h"

namespace isdbt {
namespace mirakurun {

enum class SystemHint { kAuto, kIsdbt, kIsdbc };

// "GR" → 地デジ、"BS" / "CS" / "SKY" → ケーブル（Mirakurun の channel type）。他は false
inline bool HintFromType(const std::string& type, SystemHint* out) {
    if (type == "GR") { *out = SystemHint::kIsdbt; return true; }
    if (type == "BS" || type == "CS" || type == "SKY") { *out = SystemHint::kIsdbc; return true; }
    return false;
}
inline bool HintFromSystem(const std::string& s, SystemHint* out) {
    if (s == "auto" || s.empty()) { *out = SystemHint::kAuto; return true; }
    if (s == "isdbt") { *out = SystemHint::kIsdbt; return true; }
    if (s == "isdbc") { *out = SystemHint::kIsdbc; return true; }
    return false;
}

inline bool ParseChannel(std::string s, SystemHint hint, System* sys, int* ch, double* freq) {
    if (s.size() > 2 && (s.compare(s.size() - 2, 2, "ch") == 0 || s.compare(s.size() - 2, 2, "CH") == 0)) s.resize(s.size() - 2);
    if (s.empty()) return false;
    if (s[0] == 'C' || s[0] == 'c') {
        if (hint == SystemHint::kIsdbt) return false;
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
    if (hint == SystemHint::kIsdbc) {
        *sys = System::kIsdbc;
        *ch = 12 + (n - 13);
        return isdbc::CatvChannel(*ch, freq, nullptr);
    }
    *sys = System::kIsdbt;
    *ch = n;
    *freq = ChannelToHz(n);
    return true;
}

// 表示用: 地デジは "27"、ケーブルは "C56"（C 帯）/ "56"（UHF 帯）
inline std::string ChannelLabel(System sys, int ch) {
    if (sys == System::kIsdbt) return std::to_string(ch);
    std::string name;
    isdbc::CatvChannel(ch, nullptr, &name);        // "C56ch" / "56ch"
    if (name.size() > 2) name.resize(name.size() - 2);
    return name;
}

// --scan の対象。各要素は {channel 文字列, 方式のヒント}
struct ScanTarget {
    std::string channel;
    SystemHint hint;
};

inline bool ScanTargets(const std::string& spec, std::vector<ScanTarget>* out) {
    std::string kind = spec, range;
    const size_t colon = spec.find(':');
    if (colon != std::string::npos) { kind = spec.substr(0, colon); range = spec.substr(colon + 1); }
    // range: "24-27" / "C13-C22"。prefix_c: 範囲の C の有無（両端で一致していること）
    auto parse_range = [&](int* lo, int* hi, bool* prefix_c) {
        const size_t dash = range.find('-');
        if (dash == std::string::npos) return false;
        std::string a = range.substr(0, dash), b = range.substr(dash + 1);
        const bool ca = !a.empty() && (a[0] == 'C' || a[0] == 'c'), cb = !b.empty() && (b[0] == 'C' || b[0] == 'c');
        if (ca != cb) return false;
        if (ca) { a.erase(0, 1); b.erase(0, 1); }
        *prefix_c = ca;
        *lo = std::atoi(a.c_str());
        *hi = std::atoi(b.c_str());
        return *lo > 0 && *hi >= *lo;
    };
    auto add = [&](int lo, int hi, bool prefix_c, SystemHint hint) {
        for (int n = lo; n <= hi; ++n) out->push_back({(prefix_c ? "C" : "") + std::to_string(n), hint});
    };
    if (kind == "gr") {
        int lo = 13, hi = 62;
        bool c = false;
        if (!range.empty() && (!parse_range(&lo, &hi, &c) || c)) return false;
        add(lo, hi, false, SystemHint::kIsdbt);
        return true;
    }
    if (kind == "catv") {
        if (range.empty()) {
            add(13, 63, true, SystemHint::kIsdbc);
            add(13, 62, false, SystemHint::kIsdbc);
            return true;
        }
        int lo, hi;
        bool c;
        if (!parse_range(&lo, &hi, &c)) return false;
        add(lo, hi, c, SystemHint::kIsdbc);
        return true;
    }
    if (kind == "all" || kind.empty()) {
        if (!range.empty()) return false;
        add(13, 62, false, SystemHint::kIsdbt);
        add(13, 63, true, SystemHint::kIsdbc);
        add(13, 62, false, SystemHint::kIsdbc);
        return true;
    }
    return false;
}

}  // namespace mirakurun
}  // namespace isdbt
