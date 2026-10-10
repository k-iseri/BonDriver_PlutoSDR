// IBonDriver2.h: BonDriver インターフェース（第 2 版）
//
// TVTest リポジトリ src/Include/IBonDriver2.h と同じ宣言順。vtable 互換のため変更禁止。
// 出自については IBonDriver.h 冒頭と docs/decisions.md を参照。
#pragma once

#include "IBonDriver.h"

class IBonDriver2 : public IBonDriver
{
public:
	virtual LPCTSTR GetTunerName(void) = 0;

	virtual const BOOL IsTunerOpening(void) = 0;

	virtual LPCTSTR EnumTuningSpace(const DWORD dwSpace) = 0;
	virtual LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) = 0;

	virtual const DWORD GetCurSpace(void) = 0;
	virtual const DWORD GetCurChannel(void) = 0;

// IBonDriver
	virtual void Release(void) = 0;
};
