#include "tsmf.h"

namespace isdbc {

namespace {

struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
            t[i] = c;
        }
    }
};

const CrcTable& Crc() {
    static const CrcTable c;
    return c;
}

}  // namespace

uint32_t Crc32Mpeg(const uint8_t* data, size_t n) {
    const CrcTable& tb = Crc();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ tb.t[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

bool IsTsmfHeaderCandidate(const uint8_t* p) {
    return p[0] == kSyncByte && (((p[1] & 0x1F) << 8) | p[2]) == kTsmfHeaderPid && (p[3] & 0xF0) == 0x10;
}

bool ParseTsmfHeader(const uint8_t* p, TsmfHeader* h) {
    if (!IsTsmfHeaderCandidate(p)) return false;
    const int sync = ((p[4] << 8) | p[5]) & kTsmfSyncMask;
    bool inverted;
    if (sync == kTsmfSync) inverted = false;
    else if (sync == kTsmfSyncInverted) inverted = true;
    else return false;
    if (Crc32Mpeg(p + 4, kTsPacket - 4) != 0) return false;
    if ((p[6] >> 4) & 1) return false;              // 相対 TS 番号方式以外は未対応
    h->continuity_counter = p[3] & 0x0F;
    h->sync_inverted = inverted;
    h->version = (p[6] >> 5) & 7;
    h->frame_type = p[6] & 0x0F;
    const int avail = (p[7] << 8) | p[8];
    for (int i = 0; i < kTsmfMaxTs; ++i) {
        h->available[i] = ((avail >> (15 - i)) & 1) != 0;
        h->ts_id[i] = static_cast<uint16_t>((p[9 + 4 * i] << 8) | p[10 + 4 * i]);
        h->onid[i] = static_cast<uint16_t>((p[11 + 4 * i] << 8) | p[12 + 4 * i]);
    }
    const uint32_t rs = (static_cast<uint32_t>(p[69]) << 24) | (p[70] << 16) | (p[71] << 8) | p[72];
    for (int i = 0; i < kTsmfMaxTs; ++i) h->receive_status[i] = static_cast<uint8_t>((rs >> (30 - 2 * i)) & 3);
    h->emergency = p[72] & 1;
    for (int s = 0; s < kTsmfDataSlots; ++s) {
        const uint8_t nib = p[73 + s / 2];
        h->slot_ts[s] = (s % 2 == 0) ? (nib >> 4) : (nib & 0x0F);
    }
    return true;
}

void TsmfDemux::Reset() {
    have_ = false;
    slot_ = -1;
    frames_ = bad_ = 0;
    selected_ = 0;
}

int TsmfDemux::Resolve() const {
    if (sel_.tsid >= 0) {
        for (int i = 0; i < kTsmfMaxTs; ++i) {
            if (hdr_.available[i] && hdr_.ts_id[i] == sel_.tsid &&
                (sel_.onid < 0 || hdr_.onid[i] == sel_.onid)) {
                return i + 1;
            }
        }
        return 0;
    }
    if (sel_.rel >= 1 && sel_.rel <= kTsmfMaxTs) return hdr_.available[sel_.rel - 1] ? sel_.rel : 0;
    for (int i = 0; i < kTsmfMaxTs; ++i) {
        if (hdr_.available[i]) return i + 1;
    }
    return 0;
}

bool TsmfDemux::Push(const uint8_t* pkt) {
    if (sel_.pass_all) return true;
    if (IsTsmfHeaderCandidate(pkt)) {
        TsmfHeader h;
        if (ParseTsmfHeader(pkt, &h)) {
            hdr_ = h;
            have_ = true;
            slot_ = 0;
            ++frames_;
            selected_ = Resolve();
        } else {
            ++bad_;
        }
        return false;
    }
    if (!have_) return true;                        // TSMF 無し（単一 TS）: そのまま通す
    if (slot_ < 0 || slot_ >= kTsmfDataSlots) return false;   // ヘッダを見失っている区間
    const int s = slot_++;
    return selected_ > 0 && hdr_.slot_ts[s] == selected_;
}

}  // namespace isdbc
