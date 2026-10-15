# 復調デーモン制御プロトコル（v2）

BonDriver（TVTest / EDCB 内で動く DLL）と復調プロセスを分離するためのプロトコル。
復調プロセスは `rt/demod/isdbtd.exe`（SDR ライブ / 録画 TS / SigMF の再生）か、開発用の Python 偽デーモン
`tools/demod_server.py`（録画 TS の再生、または SigMF をオフライン復調してから再生）。
BonDriver 側はどちらでも同じ。

## 版の履歴

| 版 | 変更 |
|---|---|
| 1 | 初版（地デジのみ） |
| 2 | `tune` / `status` に `system`（`isdbt` = 地デジ、`isdbc` = ケーブル TV の 64QAM トランスモジュレーション）、`tune` に `ts`（TSMF の相対 TS 番号）、`status` に ISDB-C の統計（`tsmf` など）と SDR のゲイン情報。すべて省略可で既定は v1 と同じなので、v1 クライアント（`system` を送らない BonDriver_PlutoSDR_T.dll）はそのまま動く |

## 構成

```
TVTest / EDCB
  └─ BonDriver_PlutoSDR_T.dll / _TC.dll ── TCP(制御) ──┐
                                        ── TCP(TS)  ──┤── 復調デーモン ── SDR / ファイル
```

- トランスポートは TCP。既定は `127.0.0.1:47188`。デーモンが別ホスト
  （Pluto+ / LibreSDR の ARM 側や別 PC）で動く場合も同じ。
- 1 本の接続は「制御セッション」か「TS ストリーム」のどちらかになる。
  クライアントが最初に送るコマンドで決まる（`stream` なら TS ストリーム）。
- 制御は JSON Lines（UTF-8、1 行 1 オブジェクト、`\n` 終端）。要求 1 行に応答 1 行。
- 応答は必ず `"ok": true|false` を含む。失敗時は `"error": "<説明>"`。
- 未知のキーは無視する。クライアントは未知のキーがあっても壊れてはいけない（前方互換）。

## コマンド

### hello
```
→ {"cmd": "hello"}
← {"ok": true, "name": "isdbtd", "protocol": 2, "source": "sdr:usb:"}
```
`protocol` はこの文書の版。`source` は人間向けの説明文字列。

### tune
```
→ {"cmd": "tune", "channel": 27}
→ {"cmd": "tune", "system": "isdbc", "channel": 55}
→ {"cmd": "tune", "system": "isdbc", "freq_hz": 731000000, "ts": 1}
← {"ok": true, "system": "isdbc", "channel": 55, "freq_hz": 731000000.0}
```
- `system`（省略時 `isdbt`）:
  - `isdbt`: `channel` は地デジ物理チャンネル（13〜62）。中心周波数は
    473 + 1/7 MHz + 6 MHz × (ch − 13)（`tools/iq_record.py` と同じ式）。
  - `isdbc`: `channel` はケーブル TV のチャンネル番号（BonDriver_BDA の TransModulation 自動生成と同じ
    通し番号 0〜112: 0〜2 = VHF 1〜3ch、3〜11 = 4〜12ch、12〜61 = UHF 13〜62ch、62〜71 = C13〜C22、
    72〜112 = C23〜C63）。中心周波数は 6 MHz 格子（UHF 13ch = 473 MHz、+1/7 MHz オフセット無し）。
    `docs/isdbc-spec-notes.md` 1 章。
- `channel` と `freq_hz` の両方がある場合は `freq_hz` を優先する。
- `ts`（`isdbc` のみ、省略可）: TSMF で複数 TS が多重されているときに出す相対 TS 番号（1〜15）。
  0 または省略で自動（有効 TS のうち最小番号）。
- 方式が変わる選局では SDR 側のパイプラインを作り直すため、TS が流れ始めるまで 1〜2 秒余計にかかる。
- 選局後、TS ストリームにはそれ以降に復調したデータだけが流れる（デーモン側で
  送信待ちを捨てる）。BonDriver 側も `PurgeTsStream` 相当を行う。
- 再生モードのデーモンは選局を記録するだけで、同じファイルを先頭から再生し直す。

### status
```
→ {"cmd": "status"}
← {"ok": true, "system": "isdbt",
   "locked": true,
   "signal_db": 28.5,
   "channel": 27, "freq_hz": 557142857.0,
   "mode": 3, "gi": "1/8",
   "layers": [{"name": "A", "mod": "QPSK",  "rate": "2/3", "I": 4, "segments": 1},
              {"name": "B", "mod": "64QAM", "rate": "3/4", "I": 2, "segments": 12}],
   "ts_packets": 1234567,
   "rs_uncorrectable": 12,
   "dropped": 0,
   "stream_clients": 1,
   "gain_db": 31, "rx_level_dbfs": -24.3}
```
- `signal_db` は BonDriver の `GetSignalLevel` にそのまま返す値。実時間復調では
  MER（等化後コンスタレーションの誤差ベクトルから算出、dB）を入れる。再生モードでは
  起動オプションの固定値。
- `locked` は同期が取れているか（地デジ: TMCC フレーム同期。ISDB-C: バイト同期かつ MER > 15 dB）。
- `rs_uncorrectable` は起動（または選局）以降の RS 訂正不能 TSP 数。
- SDR ライブ受信で FPGA 前段（pluto-fw-isdbt v2 の等化器）を使っているときは `fpga_eq`（true = モード 4 で
  等化済みキャリアを受信中）、`fpga_delta`（整数 CFO [ビン]）、`fpga_symbols`（受けたシンボル数）、
  `fpga_symbol_gaps`（連番の途切れ）が付く（2026-10-15 追加。すべて省略可で、無いクライアントは無視してよい）。
  同日追加: `fpga_last_start`（直近の窓開始サンプル番号。ホストで 64bit に伸ばした値）、`fpga_start_hi`（ファームが出した
  上位 32bit そのまま。v2.0.1 以降は周回ごとに増える）、`splice_resets`（TS 継ぎ合わせの安全弁が働いた回数）、`fpga_timeouts`（モード 3/4 で窓が来ずタイムアウトして raw に戻した回数）。
- `dropped` はストリーム送信が実時間に追いつかず捨てた TSP 数。
- ISDB-C（`"system": "isdbc"`）では `mode` / `gi` / `layers` の代わりに
  `"mer_db"`, `"acquired"`, `"byte_sync"`, `"inverted"`（スペクトル反転を検出した）, `"cfo_hz"`, `"clock_ppm"`,
  `"rs_packets"`, `"sync_losses"`, `"reacquisitions"`,
  `"tsmf": {"frames": N, "bad_headers": N, "selected": 1, "streams": [{"rel": 1, "tsid": 16625, "onid": 4}]}`
  を返す。`channel_name`（"56ch" / "C47ch"）も付く。
- SDR ソースでは `sdr_connected` / `sdr_reconnects` / `hw_ovf` / `rx_rate` / `rx_slow` /
  `gain_db`（現在の hardwaregain）/ `rx_level_dbfs`（帯域内平均電力）も返す。
  pluto-fw-isdbt ファーム（サンプルカウンタ付き）に接続しているときだけ `hw_dropped`（ストリーム開始後に
  FPGA の DMA が捨てた複素サンプル数）と `hw_ovf_events`（overflow パルス数。現行ファームでは捨てたビートごとに
  パルスが立つので `hw_dropped` / 2 と同じ値になり、バースト回数ではない）が付く（任意項目。
  受信側は無いものとして扱ってよい）。実機確認 2026-10-14: 20 MSPS 指定で USB を飽和させると到着 15.83 MSPS、
  `hw_dropped` の増分 4.1M/s で差分と一致。

### stream
```
→ {"cmd": "stream"}
← {"ok": true}
（以降この接続は生の TS バイト列。切断まで続く）
```
- 188 バイト TSP の連結。デーモンは TSP 境界で区切って送るが、TCP の受信境界は
  任意なので、受信側は 188 の倍数になるまで溜めてから使う。
- 同時に複数のストリーム接続を受け付ける（TVTest と録画の併用など）。
  各接続は独立したキューを持ち、遅い接続だけが TSP を落とす。
- 訂正不能 TSP は捨てずに transport_error_indicator（2 バイト目の 0x80）を立てて送る。
  デインターリーブ履歴の無い先頭 11 TSP も同じ扱い。
- ISDB-C では TSMF で選んだ 1 本の TS だけを流す（ヘッダパケット PID 0x002F は除く）。
  `ts` に `all` 相当（isdbtd `--tsmf all` / ini `[CATV] Ts=all`）を指定したときだけ全スロットを流す。

### quit
```
→ {"cmd": "quit"}
← {"ok": true}
```
デーモンを終了する。BonDriver が自分で起動した場合に `CloseTuner` で送る。
起動オプションで禁止できる（`--no-quit`）。

## 時間制約

- 制御コマンドは 2 秒以内に応答する（BonDriver 側の受信タイムアウト）。
  実機の選局で PLL ロックや再同期に時間がかかる場合でも `tune` は即座に `ok` を返し、
  同期の成否は `status.locked` で伝える。
- TS ストリームの送信は復調と同じ速度（地デジ Mode 3 / GI 1/8、QPSK 2/3 + 64QAM 3/4 で
  約 16 Mbps。ISDB-C 64QAM は 1 キャリア 29.16 Mbps、TSMF で選んだ TS はその一部）。
  再生モードでは PCR から推定したレート、PCR が無ければ固定値。

## BonDriver 側の対応

| IBonDriver2 | 動作 |
|---|---|
| OpenTuner | 制御接続（必要なら `Command` でデーモンを起動して再試行）→ `hello` → ストリーム接続 → 受信スレッド開始 |
| SetChannel(space, ch) | space 0 = UHF、ch 0..39 → 物理 13..52ch → `tune`。TC 版は space 1 = CATV、ch 0..112 → `tune` with `"system": "isdbc"` → 受信キューを空にする |
| GetSignalLevel | `status.signal_db`（500 ms キャッシュ） |
| WaitTsStream / GetReadyCount / GetTsStream / PurgeTsStream | 受信スレッドが 188 の倍数に揃えて積むチャンク列 |
| CloseTuner | 受信停止、起動したデーモンには `quit` |

## 将来の拡張（v2 では未定義）

- `layer` フィルタ（ワンセグだけ欲しいクライアント向け）
- `tune` への `mode` / `gi` 指定（海外 ISDB-T 派生方式）
- ISDB-C 256QAM（J.83 Annex C の第 2 の変調方式）
- IBonDriver3（`GetTotalDeviceNum` など）は SDR 1 台では意味が無いので実装しない
