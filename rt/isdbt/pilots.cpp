#include "pilots.h"

#include <cassert>
#include <map>
#include <mutex>

namespace isdbt {

std::vector<uint8_t> PilotPrbs(Mode mode) {
    // reg の bit10 が D1（最新）、bit0 が D11（出力）。D11 xor D9（bit0 xor bit2）を D1 へ
    int n = ModeParamsOf(mode).total_carriers;
    unsigned reg = kPilotPrbsInit;
    std::vector<uint8_t> w(n);
    for (int k = 0; k < n; ++k) {
        w[k] = static_cast<uint8_t>(reg & 1);
        unsigned nu = ((reg >> 2) ^ reg) & 1;
        reg = (reg >> 1) | (nu << 10);
    }
    return w;
}

namespace {

Layout BuildLayout(Mode mode) {
    const ModeParams& mp = ModeParamsOf(mode);
    Layout lo;
    lo.mode = mode;
    lo.dc = mp.data_carriers_per_segment;

    std::vector<uint8_t> w = PilotPrbs(mode);
    lo.pilot_values.resize(mp.total_carriers);
    for (int k = 0; k < mp.total_carriers; ++k) {
        lo.pilot_values[k] = static_cast<float>(kPilotAmplitude * (1.0 - 2.0 * w[k]));
    }

    int n_tmcc = 0, n_ac1 = 0;
    const int16_t* tm = TmccPositionsAbs(mode, &n_tmcc);
    const int16_t* ac = Ac1PositionsAbs(mode, &n_ac1);
    lo.tmcc_abs.assign(tm, tm + n_tmcc);
    lo.ac1_abs.assign(ac, ac + n_ac1);

    const int n_active = mp.total_carriers - 1;    // 右端 CP を除く
    for (int p = 0; p < kSpSymbolPeriod; ++p) {
        // SP: k mod 12 == 3p
        for (int k = 3 * p; k < n_active; k += kSpSpacing) lo.sp_positions[p].push_back(k);
        // データキャリア = SP/TMCC/AC1 以外
        std::vector<bool> is_pilot(n_active, false);
        for (int k : lo.sp_positions[p]) is_pilot[k] = true;
        for (int k : lo.tmcc_abs) is_pilot[k] = true;
        for (int k : lo.ac1_abs) is_pilot[k] = true;
        lo.dpos4[p].reserve(static_cast<size_t>(mp.data_carriers_total()));
        for (int k = 0; k < n_active; ++k) {
            if (!is_pilot[k]) lo.dpos4[p].push_back(k);
        }
        assert(static_cast<int>(lo.dpos4[p].size()) == mp.data_carriers_total());
    }

    int n_rand = 0;
    const int16_t* perm = CarrierRandomizeTable(mode, &n_rand);
    assert(n_rand == lo.dc);
    lo.randomize_inv.resize(lo.dc);
    for (int i = 0; i < lo.dc; ++i) lo.randomize_inv[perm[i]] = i;
    return lo;
}

}  // namespace

const Layout& LayoutOf(Mode mode) {
    static std::map<int, Layout> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(static_cast<int>(mode));
    if (it == cache.end()) it = cache.emplace(static_cast<int>(mode), BuildLayout(mode)).first;
    return it->second;
}

}  // namespace isdbt
