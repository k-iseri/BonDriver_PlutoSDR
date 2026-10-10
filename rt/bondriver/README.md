# BonDriver_PlutoSDR

TVTest / EDCB から isdbt-sdr の復調器を使うための BonDriver（IBonDriver2 実装）。
動作モードは 2 つ（ini の `[SDR] Enable` で選ぶ）:

```
スタンドアロン: TVTest ── BonDriver_PlutoSDR.dll（内蔵復調 + libiio）── Pluto SDR
デーモン:       TVTest ── BonDriver_PlutoSDR.dll ── TCP ── isdbtd / demod_server.py ── SDR / 録画 TS / SigMF
```

- **スタンドアロン（`[SDR] Enable=1`）**: DLL が rt/ の復調コアと libiio で Pluto SDR を
  直接開き、DLL 内で復調する。デーモン不要。TVTest の BonDriver フォルダに置くのは
  DLL と ini だけでよい（libiio は遅延ロード。下記「libiio の解決」参照）。
- **デーモン（既定 `Enable=0`）**: 復調は別プロセス（`isdbtd.exe` または
  `tools/demod_server.py`）が行い、DLL は `docs/daemon-protocol.md` のプロトコルで
  TCP 越しに選局と TS 受信をするだけ。録画 TS / SigMF の再生、別ホストの復調器、
  複数クライアント（視聴 + 録画）はこちら。

## ビルド

```
.\rt\bondriver\build.ps1              # x64 Release → rt\build\bondriver-x64\Release\
.\rt\bondriver\build.ps1 -Arch Win32  # 32 bit TVTest 用
```
Visual Studio 2022（Build Tools 可）の C++ ツールセットと CMake が要る。CRT は静的リンク。

ビルドに `rt/third_party/libiio` があれば（`.\rt\third_party\fetch_libiio.ps1` で取得）
スタンドアロンモードが有効になる。無くてもビルドは通り、デーモンモードだけの DLL になる。
libiio の配布 .lib が x64 のみなので、Win32 ビルドはデーモンモード専用。

## 配置（スタンドアロン = 通常の視聴）

1. `BonDriver_PlutoSDR.dll` を TVTest（または EDCB）の BonDriver フォルダへ
2. `BonDriver_PlutoSDR.ini.sample` を同じ場所に `BonDriver_PlutoSDR.ini` としてコピー
   （サンプルは `[SDR] Enable=1` のスタンドアロン設定になっている）
3. TVTest でチューナ「ISDB-T SDR」を選ぶ。チューニング空間は UHF、チャンネルは 13〜52ch

選局からウィンドウ並列復調パイプラインが立ち上がるまで **10〜15 秒** かかる
（isdbtd の `--sdr` と同じ。その間 TVTest は無信号表示）。信号レベル表示は最悪階層の MER [dB]。

### libiio の解決

libiio1.dll は遅延ロードなので、無くても DLL 自体は読み込める（デーモンモードは動く）。
スタンドアロンモードの OpenTuner 時に次の順で探す:

1. ini の `[SDR] LibiioDir`
2. DLL と同じフォルダ（`rt/build/bondriver-x64/Release/` にビルド時にコピーされる
   libiio の DLL 一式を TVTest フォルダへ入れてもよい）
3. 通常の DLL 検索パス（ADI の libiio インストーラが PATH に入れた場所など）

## 配置（デーモンモード）

ini で `[SDR] Enable=0` にし、`[Daemon]` を設定する。`Spawn=1` なら OpenTuner 時に
DLL がデーモンを起動し、CloseTuner で `quit` を送る。デーモンを手で起動しておく
（`isdbtd --sdr usb:` / `python tools/demod_server.py --ts x.ts --loop`）か
別ホストで動かす場合は `Spawn=0` にして `Host` / `Port` を合わせる。

## TVTest 無しで確認する

```
cd rt\build\bondriver-x64\Release
copy ..\..\..\bondriver\BonDriver_PlutoSDR.ini.sample BonDriver_PlutoSDR.ini   # 編集する
.\bontest.exe BonDriver_PlutoSDR.dll 27 5 out.ts
```
1 秒ごとに受信レート・信号レベル・同期バイト不整合数を表示する。終了コード 0 が正常。

## 実装メモ

- IBonDriver / IBonDriver2 のヘッダは TVTest リポジトリ `src/Include/` の宣言を転記したもの
  （vtable 互換のため順序変更禁止）。IBonDriver3 は LNB 電源・複数デバイス用で SDR 1 台では不要。
- `GetTsStream` は 188 の倍数のチャンク（最大 48128 バイト）を返す。受信キューは 16 MB で、
  TVTest 側が止まっていたら古い方から捨てる。
- `GetSignalLevel` は `status.signal_db` を 500 ms キャッシュして返す。
- 制御接続が切れたら次のコマンドで 1 回だけ張り直す。TS ストリームが切れたら受信が止まる
  （TVTest 上では無信号に見える）。チャンネルを切り替え直すか、チューナを開き直す。
- `[Debug] Log=1` で DLL の隣に `BonDriver_PlutoSDR.log` を書く。`OutputDebugString` にも出る。
- デーモンモードの選局（`SetChannel`）は物理チャンネル番号をデーモンに渡すだけ。周波数換算はデーモン側。
- スタンドアロンモードの実体は `rt/demod/live_demod.{h,cpp}`（isdbtd の `--sdr` と同じ
  `LiveDemod`）を DLL に静的リンクしたもの。ini の `WinFrames` / `CtxFrames` / `Workers` は
  isdbtd の `--win-frames` / `--ctx-frames` / `--workers` に対応する。
- 32/64 bit は TVTest 本体に合わせる（違うと LoadLibrary で失敗する）。
