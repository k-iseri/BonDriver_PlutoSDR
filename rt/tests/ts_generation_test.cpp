#include "../demod/ts_queue.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <utility>

using namespace isdbt;

namespace {

bool TestQueuedBeforeTuneIsCleared() {
    ClientQueue q;
    q.AdvanceGeneration(1);
    if (q.TryPush(TsChunk{{0x47}, 1}, 8) != TsPushResult::kQueued) return false;
    q.AdvanceGeneration(2);
    std::lock_guard<std::mutex> lock(q.mu);
    return q.generation == 2 && q.chunks.empty();
}

bool TestPulledBeforeTuneIsRejectedAtOutput() {
    ClientQueue q;
    q.AdvanceGeneration(10);
    TsChunk pulled;
    {
        std::lock_guard<std::mutex> lock(q.mu);
        q.chunks.push_back(TsChunk{{0x47}, 10});
        pulled = std::move(q.chunks.front());  // PullerThread が上流から取り出した状態
        q.chunks.pop_front();
    }
    q.AdvanceGeneration(11);                  // SetChannel の世代更新
    if (q.TryPush(pulled, 8) != TsPushResult::kStale) return false;
    if (q.TryPush(TsChunk{{0x47}, 11}, 8) != TsPushResult::kQueued) return false;
    std::lock_guard<std::mutex> lock(q.mu);
    return q.chunks.size() == 1 && q.chunks.front().generation == 11;
}

TsChunk PsiMarker(int channel, int pid, uint8_t table_id) {
    std::vector<uint8_t> packet(188, 0xFF);
    packet[0] = 0x47;
    packet[1] = static_cast<uint8_t>(0x40 | ((pid >> 8) & 0x1F));
    packet[2] = static_cast<uint8_t>(pid);
    packet[3] = 0x10;
    packet[4] = 0x00;             // pointer_field
    packet[5] = table_id;         // PAT / SDT の table_id を模した識別値
    packet[6] = static_cast<uint8_t>(channel);
    return TsChunk{std::move(packet), 0};
}

bool TestRapid23To25PsiTransition() {
    ClientQueue q;
    TsGeneration gen = NextTsGeneration();
    q.AdvanceGeneration(gen);
    TsChunk pulled;
    for (int channel : {23, 24, 25}) {
        if (channel != 23) {
            ++gen;
            q.AdvanceGeneration(gen);
            if (channel == 24 || channel == 25) {
                // SetChannel 前に PullerThread が握った旧 PAT/SI も次の世代では入らない。
                if (q.TryPush(pulled, 8) != TsPushResult::kStale) return false;
            }
        }
        TsChunk pat = PsiMarker(channel, 0x0000, 0x00);
        TsChunk sdt = PsiMarker(channel, 0x0011, 0x42);
        pat.generation = sdt.generation = gen;
        if (q.TryPush(pat, 8) != TsPushResult::kQueued ||
            q.TryPush(sdt, 8) != TsPushResult::kQueued) return false;
        if (channel != 25) {
            std::lock_guard<std::mutex> lock(q.mu);
            pulled = std::move(q.chunks.front());
            q.chunks.pop_front();
        }
    }
    std::lock_guard<std::mutex> lock(q.mu);
    if (q.chunks.size() != 2) return false;
    for (const TsChunk& chunk : q.chunks) {
        if (chunk.generation != gen || chunk.bytes[6] != 25) return false;
    }
    return true;
}

bool TestTuneRacingWithProducer() {
    ClientQueue q;
    constexpr TsGeneration kIterations = 2000;
    for (TsGeneration i = 0; i < kIterations; ++i) {
        const TsGeneration old_gen = 100 + i * 2;
        const TsGeneration new_gen = old_gen + 1;
        q.AdvanceGeneration(old_gen);
        std::atomic<bool> go{false};
        TsPushResult result = TsPushResult::kQueued;
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            result = q.TryPush(TsChunk{{static_cast<uint8_t>(i)}, old_gen}, 8);
        });
        std::thread tuner([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            q.AdvanceGeneration(new_gen);
        });
        go.store(true, std::memory_order_release);
        producer.join();
        tuner.join();
        {
            std::lock_guard<std::mutex> lock(q.mu);
            if (q.generation != new_gen || !q.chunks.empty()) return false;
        }
        if (result != TsPushResult::kQueued && result != TsPushResult::kStale) return false;
    }
    return true;
}

}  // namespace

int main() {
    const bool cleared = TestQueuedBeforeTuneIsCleared();
    const bool pulled = TestPulledBeforeTuneIsRejectedAtOutput();
    const bool rapid = TestRapid23To25PsiTransition();
    const bool raced = TestTuneRacingWithProducer();
    std::printf("%s queued-before-tune\n", cleared ? "ok" : "FAIL");
    std::printf("%s puller-output-race\n", pulled ? "ok" : "FAIL");
    std::printf("%s rapid-23-24-25-psi-generation\n", rapid ? "ok" : "FAIL");
    std::printf("%s tune-producer-race (2000 iterations)\n", raced ? "ok" : "FAIL");
    return cleared && pulled && rapid && raced ? 0 : 1;
}
