// TS チャンクに選局世代を付け、選局境界をまたいだ古いデータを拒否する。
#pragma once
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace isdbt {

using TsGeneration = uint64_t;

inline TsGeneration NextTsGeneration() {
    static std::atomic<TsGeneration> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

struct TsChunk {
    std::vector<uint8_t> bytes;
    TsGeneration generation = 0;
};

enum class TsPushResult { kQueued, kStale, kFull };

struct ClientQueue {
    std::deque<TsChunk> chunks;
    std::mutex mu;
    std::condition_variable cv;
    TsGeneration generation = 0;  // mu 下。全クライアント共通の世代トークン。

    // 選局側と送出側が同じ mutex で直列化されるため、クリア後に古いチャンクが
    // 再投入される隙間がない。古い Tune 要求が遅れて到着しても世代を巻き戻さない。
    void AdvanceGeneration(TsGeneration next) {
        std::lock_guard<std::mutex> lock(mu);
        if (next <= generation) return;
        generation = next;
        chunks.clear();
    }

    TsPushResult TryPush(const TsChunk& chunk, std::size_t max_chunks) {
        std::lock_guard<std::mutex> lock(mu);
        if (chunk.generation != generation) return TsPushResult::kStale;
        if (chunks.size() >= max_chunks) return TsPushResult::kFull;
        chunks.push_back(chunk);
        cv.notify_one();
        return TsPushResult::kQueued;
    }
};

}  // namespace isdbt
