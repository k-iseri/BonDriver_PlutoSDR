// bontest.cpp: BonDriver を TVTest 無しで叩く確認用コンソール。
//
//   bontest.exe [--interval N] [BonDriver.dll] [物理ch(13..52) | 空間/番号] [秒数] [出力.ts] [ch 秒数]...
//
//   --interval N  統計行の出力間隔（秒、既定 1）。長時間試験では 10〜60 にして行数を抑える。
//
//   チャンネルは地デジ物理チャンネル（空間 0）か "空間/チャンネル番号"（例: 1/55 = TC 版の CATV 空間、
//   BonDriver_BDA 互換の通し番号 55 = 56ch）。
// OpenTuner → SetChannel → 指定秒数 TS を受けて、受信レート・同期バイト整合・信号レベル・
// 継続カウンタ欠落（drop、TVTest の D 相当）・TEI（error、TVTest の E 相当）を表示する。
// 終了コードは 0 = 正常、4 = 無データ / 同期不良（drop / error は終了コードに含めず、集計値を見る）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "IBonDriver2.h"

typedef IBonDriver *(*CreateBonDriverFn)();

// 継続カウンタ（ISO/IEC 13818-1 2.4.3.3）の欠落を PID ごとに数える。ペイロード無しのパケットと
// NULL パケット（0x1FFF）は対象外。adaptation_field の discontinuity_indicator が立っていれば数えない。
struct TsCounter {
	int last[8192];
	ULONGLONG drops = 0, errors = 0, scrambled = 0;
	TsCounter() { for (int &v : last) v = -1; }
	void Feed(const BYTE *p)
	{
		if (p[0] != 0x47) return;
		if (p[1] & 0x80) { ++errors; return; }               // transport_error_indicator
		const int pid = ((p[1] & 0x1F) << 8) | p[2];
		if (pid == 0x1FFF) return;
		if (p[3] & 0xC0) ++scrambled;
		const bool hasPayload = (p[3] & 0x10) != 0;
		const bool hasAf = (p[3] & 0x20) != 0;
		const int cc = p[3] & 0x0F;
		if (!hasPayload) return;
		bool discontinuity = false;
		if (hasAf && p[4] > 0) discontinuity = (p[5] & 0x80) != 0;
		if (last[pid] >= 0 && !discontinuity && ((last[pid] + 1) & 0x0F) != cc) ++drops;
		last[pid] = cc;
	}
	void Reset() { for (int &v : last) v = -1; }
};

int wmain(int argcIn, wchar_t **argvIn)
{
	// 先頭のオプションを取り除き、残りを従来どおりの位置引数として扱う
	int interval = 1;
	wchar_t *argv[64];
	int argc = 0;
	argv[argc++] = argvIn[0];
	for (int i = 1; i < argcIn && argc < 64; ++i) {
		if (wcscmp(argvIn[i], L"--interval") == 0 && i + 1 < argcIn) {
			interval = _wtoi(argvIn[++i]);
			if (interval < 1) interval = 1;
		} else {
			argv[argc++] = argvIn[i];
		}
	}
	const wchar_t *dll = argc > 1 ? argv[1] : L"BonDriver_PlutoSDR_T.dll";
	DWORD space = 0;
	unsigned phys = 27;
	DWORD chIndex = 27 - 13;
	if (argc > 2) {
		const wchar_t *slash = wcschr(argv[2], L'/');
		if (slash) {
			space = (DWORD)_wtoi(argv[2]);
			chIndex = (DWORD)_wtoi(slash + 1);
			phys = chIndex;
		} else {
			phys = (unsigned)_wtoi(argv[2]);
			chIndex = phys - 13;
		}
	}
	int seconds = argc > 3 ? _wtoi(argv[3]) : 5;
	FILE *out = nullptr;
	int extraArg = 4;                                   // 追加の「チャンネル 秒数」の組（連続選局の確認用）
	if (argc > 4 && wcsstr(argv[4], L".ts")) {
		out = _wfopen(argv[4], L"wb");
		extraArg = 5;
	}

	HMODULE h = LoadLibraryW(dll);
	if (!h) {
		fwprintf(stderr, L"LoadLibrary(%s) failed: %lu\n", dll, GetLastError());
		return 1;
	}
	auto create = (CreateBonDriverFn)GetProcAddress(h, "CreateBonDriver");
	if (!create) {
		fwprintf(stderr, L"CreateBonDriver not exported\n");
		return 1;
	}
	IBonDriver *bon = create();
	IBonDriver2 *bon2 = static_cast<IBonDriver2 *>(bon);   // TVTest と同じく IBonDriver2 実装を前提にする
	wprintf(L"tuner: %s\n", bon2->GetTunerName());
	for (DWORD sp = 0; bon2->EnumTuningSpace(sp); ++sp) {
		wprintf(L"space %lu: %s :", sp, bon2->EnumTuningSpace(sp));
		DWORD n = 0;
		for (; bon2->EnumChannelName(sp, n); ++n) {
			if (n < 3) wprintf(L" %s", bon2->EnumChannelName(sp, n));
		}
		wprintf(L" ... (%lu channels)\n", n);
	}

	if (!bon2->OpenTuner()) {
		fwprintf(stderr, L"OpenTuner failed (ini / daemon を確認。Debug Log=1 で BonDriver_PlutoSDR_T.log)\n");
		bon2->Release();
		return 2;
	}
	ULONGLONG total = 0, badSync = 0, pkts = 0;
	TsCounter tsc;
	int ret = 0;
	int segment = 0;
	for (;;) {
	if (!bon2->SetChannel(space, chIndex)) {
		fwprintf(stderr, L"SetChannel(space %lu, %lu) failed\n", space, chIndex);
		bon2->CloseTuner();
		bon2->Release();
		return 3;
	}
	wprintf(L"tuned %s (space %lu ch %lu) for %d s\n", bon2->EnumChannelName(space, chIndex), bon2->GetCurSpace(), bon2->GetCurChannel(), seconds);
	tsc.Reset();                                        // 選局直後の継続カウンタ不連続は欠落に数えない
	const ULONGLONG stepMs = (ULONGLONG)interval * 1000;
	ULONGLONG t0 = GetTickCount64(), tNext = t0 + stepMs, secBytes = 0;
	ULONGLONG lastDrops = tsc.drops, lastErrors = tsc.errors;
	while ((int)((GetTickCount64() - t0) / 1000) < seconds) {
		if (bon2->WaitTsStream(1000) != WAIT_OBJECT_0 && bon2->GetReadyCount() == 0) {
			wprintf(L"  (no data)\n");
		}
		BYTE *p = nullptr;
		DWORD size = 0, remain = 0;
		while (bon2->GetTsStream(&p, &size, &remain) && size) {
			total += size;
			secBytes += size;
			for (DWORD i = 0; i + 188 <= size; i += 188) {
				++pkts;
				if (p[i] != 0x47) ++badSync;
				tsc.Feed(p + i);
			}
			if (size % 188) ++badSync;
			if (out) fwrite(p, 1, size, out);
			if (!remain) break;
		}
		ULONGLONG now = GetTickCount64();
		if (now >= tNext) {
			SYSTEMTIME st;
			GetLocalTime(&st);
			wprintf(L"%02d:%02d:%02d %7.1f s  %7.3f Mbps  level %5.1f  pkts %llu  bad %llu  drop %llu (+%llu)  error %llu (+%llu)\n",
			        st.wHour, st.wMinute, st.wSecond,
			        (now - t0) / 1000.0, secBytes * 8.0 / 1e6 / ((now - tNext + stepMs) / 1000.0),
			        bon2->GetSignalLevel(), pkts, badSync,
			        tsc.drops, tsc.drops - lastDrops, tsc.errors, tsc.errors - lastErrors);
			fflush(stdout);
			lastDrops = tsc.drops;
			lastErrors = tsc.errors;
			secBytes = 0;
			tNext = now + stepMs;
		}
	}
	// 追加の「チャンネル 秒数」があれば続けて選局する（TVTest のスキャンのような連続選局の再現）
	++segment;
	const int ai = extraArg + 2 * (segment - 1);
	if (ai + 1 >= argc) break;
	const wchar_t *slash = wcschr(argv[ai], L'/');
	if (slash) {
		space = (DWORD)_wtoi(argv[ai]);
		chIndex = (DWORD)_wtoi(slash + 1);
	} else {
		space = 0;
		chIndex = (DWORD)_wtoi(argv[ai]) - 13;
	}
	seconds = _wtoi(argv[ai + 1]);
	}
	wprintf(L"total %llu bytes, %llu packets, %llu bad sync, %llu drop, %llu error (TEI), %llu scrambled\n",
	        total, pkts, badSync, tsc.drops, tsc.errors, tsc.scrambled);
	if (total == 0 || badSync) ret = 4;
	bon2->CloseTuner();
	bon2->Release();
	if (out) fclose(out);
	FreeLibrary(h);
	return ret;
}
