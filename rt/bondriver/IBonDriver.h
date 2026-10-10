// IBonDriver.h: BonDriver インターフェース（第 1 版）
//
// 宣言の順序と型は TVTest リポジトリ（DBCTRADO/TVTest, src/Include/IBonDriver.h）と
// 同一でなければならない。vtable 互換性に関わるので並べ替え・追加禁止。
// 原本はコメントが Shift-JIS で、ライセンス表記は無い（2004 年の BonTest 由来、
// 互換 BonDriver 群が広く再利用している）。本ファイルは宣言を転記しコメントを UTF-8 で書き直したもの。
// 経緯は docs/decisions.md を参照。
#pragma once

#include <windows.h>

class IBonDriver
{
public:
	virtual const BOOL OpenTuner(void) = 0;
	virtual void CloseTuner(void) = 0;

	virtual const BOOL SetChannel(const BYTE bCh) = 0;
	virtual const float GetSignalLevel(void) = 0;

	virtual const DWORD WaitTsStream(const DWORD dwTimeOut = 0) = 0;
	virtual const DWORD GetReadyCount(void) = 0;

	virtual const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) = 0;
	virtual const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) = 0;

	virtual void PurgeTsStream(void) = 0;

	virtual void Release(void) = 0;
};

// インスタンス生成。DLL 側は BONDRIVER_EXPORTS を定義してビルドする。
#ifdef BONDRIVER_EXPORTS
extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver();
#else
extern "C" __declspec(dllimport) IBonDriver * CreateBonDriver();
#endif
