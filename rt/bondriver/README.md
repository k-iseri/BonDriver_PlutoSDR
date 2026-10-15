# BonDriver_PlutoSDR

TVTest / EDCB から isdbt-sdr の復調器を使うための BonDriver（IBonDriver2 実装）。
同じソース（`BonDriver_PlutoSDR.cpp`）から 2 つの DLL を作る。ユーザーはどちらか片方を入れる:

| DLL | 空間 0 | 空間 1 | 備考 |
|---|---|---|---|
| `BonDriver_PlutoSDR_T.dll` | UHF（地デジ 13〜52ch） | — | 従来どおり |
| `BonDriver_PlutoSDR_TC.dll` | UHF | CATV（ケーブル TV の BS/CS トラモジ、ISDB-C 64QAM） | `BONDRIVER_CATV` 定義でビルド。CATV のチャンネル番号は BonDriver_BDA の TransModulation 自動生成と同じ通し番号 0..112（VHF 1〜12、UHF 13〜62、C13〜C63）。既存の `.ch2` は空間番号を 0 → 1 にすれば流用できる |

動作モードは 2 つ（ini の `[SDR] Enable` で選ぶ）:

```
スタンドアロン: TVTest ── BonDriver_PlutoSDR_T(C).dll（内蔵復調 + libiio）── Pluto SDR
デーモン:       TVTest ── BonDriver_PlutoSDR_T(C).dll ── TCP ── isdbtd / demod_server.py ── SDR / 録画 TS / SigMF
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

## リリース（配布 zip）

`v` で始まるタグを push すると GitHub Actions（`.github/workflows/release.yml`）が
x64 Release をビルドし、配布 zip（DLL + libiio 一式 + ini.sample + bontest.exe +
INSTALL.txt）を GitHub Release に添付する。本文は `.github/release-notes.md`:

```
git tag v1.0.1
git push origin v1.0.1
```

同じ zip をローカルで作るにはビルド後に `.\tools\package_release.ps1 -Version v1.0.1`
（`dist\` に出力。dist/ は gitignore）。

## 配置（スタンドアロン = 通常の視聴）

1. `BonDriver_PlutoSDR_T.dll` を TVTest（または EDCB）の BonDriver フォルダへ
2. `BonDriver_PlutoSDR.ini.sample` を同じ場所に `BonDriver_PlutoSDR_T.ini` としてコピー
   （サンプルは `[SDR] Enable=1` のスタンドアロン設定になっている）
3. TVTest でチューナ「ISDB-T SDR」を選ぶ。チューニング空間は UHF、チャンネルは 13〜52ch

選局から映像が連続して出るまで **約 10 秒**、番組情報（PSI/SI）だけなら約 5 秒かかる
（isdbtd の `--sdr` と同じ。その間 TVTest は無信号表示。選局直後の最初のウィンドウだけ文脈 2 + 新規 4 フレームに
縮めて初回ロックを早め、以降は `WinFrames`（既定 8）のウィンドウで回る。ペーサーは最初の 1 本を出した後に空になったら、次の 2 本が揃うまで待ってから連続送出に入る）。信号レベル表示は最悪階層の MER [dB]。
TVTest のチャンネルスキャンは 1 チャンネル約 9 秒で次へ進むので、これより遅いと局が見つからない
（2026-10-14 に実際に起きた。docs/setup-tvtest.md 7 章）。

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
copy ..\..\..\bondriver\BonDriver_PlutoSDR.ini.sample BonDriver_PlutoSDR_T.ini   # 編集する
.\bontest.exe BonDriver_PlutoSDR_T.dll 27 5 out.ts
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
- スタンドアロンモードは AD936x コアの UI_OVF レジスタを約 1 秒周期で監視し、USB 帯域不足による
  サンプル落ちをハード側で確定検出する（ログ統計の `hw_ovf`。純正ファームで動作、
  対応レジスタが無いデバイスでは自動で無効化）。
- **カスタムファーム（[pluto-fw-isdbt](https://github.com/k-iseri/pluto-fw-isdbt)）に自動適応する。**
  同ファームは I/Q を 8bit で転送する（scan-element が `le:s8/8`）ので、DLL は接続時にサンプル幅を
  申告から読み、8bit なら公称 8.127 MSPS で取り込む（純正 16bit ファームは従来どおり 7/8 レート。
  `[SDR] Fs` 未指定時）。さらにファームのサンプルカウンタ（コアのレジスタ 0x3000、MAGIC "ISDB"）が
  読めれば、捨てられたサンプル数 `hw_dropped` と overflow イベント数 `hw_ovf_events` をログ統計に加える。
  ファーム識別子では分岐しない（申告ベース）。
- **FPGA 前段（pluto-fw-isdbt v2 の同期・FFT・等化器、CAPS bit3）があれば既定でそれを使う**
  （`[SDR] FpgaEq=auto`。`0` で従来の raw I/Q ホスト復調、`1` で必須）。選局のたびに raw I/Q で
  ソフト AGC を約 1 秒走らせ、モード 3（同期 FFT 8bit）の 16 窓から整数 CFO δ を求めて EQ_CTRL に書き、
  いったんモード 0 に戻してからモード 4（等化済みキャリア I/Q/CSI 各 8bit、ビン順 → キャリア順に並べ直し）
  を受ける。DLL 内の復調はデインターリーブ以降だけになり、さらにウィンドウの重なり分は復号し直さない（範囲復号）
  ので、CPU 使用率は raw の約 2.7 コアから約 0.4 コアに下がる（ビタビと RS は 8bit / 32 レーンの AVX2）。
  信号レベル（MER）・TS の継ぎ方は raw 経路と同じ。FPGA が窓を欠かすと（連番の途切れ）その区間の
  ウィンドウは捨てる（ログ統計 `fpga_symbol_gaps`）。モード 4 の間はソフト AGC が止まる（レベル
  情報が無い）ので、ゲインは選局直後の値で固定される。地デジ専用で、CATV（ISDB-C）は常に raw。
- スタンドアロンモードは USB 瞬断などで SDR のストリームが切れても自動で再接続する
  （`[SDR] Reconnect=1` 既定。バックオフ 0.5→5 秒、選局中の周波数は再接続時に再適用）。
  切断中は信号レベルが 0 になり、復帰後 10〜15 秒でパイプラインが再同期する。
  実機検証済み（2026-10-12、TVTest 視聴中の USB 抜き差しで自動復帰を確認）。
  ログには `sdr: reconnected (#1)` が出る（`[Debug] Log=1` で `sdr_reconnects` も伸びる）。
- `[Debug] Log=1` で DLL の隣に `BonDriver_PlutoSDR_T.log` を書く。`OutputDebugString` にも出る。
- デーモンモードの選局（`SetChannel`）は物理チャンネル番号をデーモンに渡すだけ。周波数換算はデーモン側。
- スタンドアロンモードの実体は `rt/demod/live_switch.{h,cpp}`（isdbtd の `--sdr` と同じ `LiveSwitch`。
  地デジは `LiveDemod`、ISDB-C は `LiveIsdbc` を選局先の空間に応じて差し替える）を DLL に
  静的リンクしたもの。ini の `WinFrames` / `CtxFrames` / `Workers` は
  isdbtd の `--win-frames` / `--ctx-frames` / `--workers` に対応する。
  方式が変わる選局（UHF ↔ CATV）は SDR を開き直すので 1〜2 秒余計にかかる。
- 受信ゲインの既定はソフトウェア AGC（帯域内 −24 dBFS 目標、`[SDR] AgcTargetDbfs`）。AD936x 内蔵の
  slow_attack（`Gain=hw`）はケーブル端子のような強い全帯域入力で圧縮し MER が 20 dB に張り付く
  （docs/hw-setup.md「受信ゲイン」）。
- `_TC` 版の `[CATV] Ts=` で TSMF の相対 TS 番号を固定できる（既定は自動 = 有効 TS の最小番号、
  `all` で BonDriver_BDA 互換の全スロット通過）。
- 32/64 bit は TVTest 本体に合わせる（違うと LoadLibrary で失敗する）。
