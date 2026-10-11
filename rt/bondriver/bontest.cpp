// bontest.cpp: BonDriver を TVTest 無しで叩く確認用コンソール。
//
//   bontest.exe [BonDriver.dll] [物理ch(13..52)] [秒数] [出力.ts]
//
// OpenTuner → SetChannel → 指定秒数 TS を受けて、受信レート・同期バイト整合・信号レベルを 1 秒ごとに表示する。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "IBonDriver2.h"

typedef IBonDriver *(*CreateBonDriverFn)();

int wmain(int argc, wchar_t **argv)
{
	const wchar_t *dll = argc > 1 ? argv[1] : L"BonDriver_PlutoSDR_T.dll";
	unsigned phys = argc > 2 ? (unsigned)_wtoi(argv[2]) : 27;
	int seconds = argc > 3 ? _wtoi(argv[3]) : 5;
	FILE *out = nullptr;
	if (argc > 4) out = _wfopen(argv[4], L"wb");

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
	DWORD chIndex = phys - 13;
	if (!bon2->SetChannel((DWORD)0, chIndex)) {
		fwprintf(stderr, L"SetChannel(%uch) failed\n", phys);
		bon2->CloseTuner();
		bon2->Release();
		return 3;
	}
	wprintf(L"tuned %s (space %lu ch %lu)\n", bon2->EnumChannelName(0, chIndex), bon2->GetCurSpace(), bon2->GetCurChannel());

	ULONGLONG total = 0, badSync = 0, pkts = 0;
	ULONGLONG t0 = GetTickCount64(), tNext = t0 + 1000, secBytes = 0;
	int ret = 0;
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
			}
			if (size % 188) ++badSync;
			if (out) fwrite(p, 1, size, out);
			if (!remain) break;
		}
		ULONGLONG now = GetTickCount64();
		if (now >= tNext) {
			wprintf(L"  %5.1f s  %7.3f Mbps  level %5.1f  pkts %llu  bad %llu\n",
			        (now - t0) / 1000.0, secBytes * 8.0 / 1e6 / ((now - tNext + 1000) / 1000.0),
			        bon2->GetSignalLevel(), pkts, badSync);
			secBytes = 0;
			tNext = now + 1000;
		}
	}
	wprintf(L"total %llu bytes, %llu packets, %llu bad sync\n", total, pkts, badSync);
	if (total == 0 || badSync) ret = 4;
	bon2->CloseTuner();
	bon2->Release();
	if (out) fclose(out);
	FreeLibrary(h);
	return ret;
}
