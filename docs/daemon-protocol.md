# 復調デーモン制御プロトコル（v1）

BonDriver（TVTest / EDCB 内で動く DLL）と復調プロセスを分離するためのプロトコル。
復調プロセスは今は Python の偽デーモン `tools/demod_server.py`（録画 TS の再生、
または SigMF をオフライン復調してから再生）で、将来 `rt/` の実時間復調デーモンが
同じプロトコルを実装して置き換わる。BonDriver 側は差し替えに気付かない。

## 構成

```
TVTest / EDCB
  └─ BonDriver_PlutoSDR_T.dll ── TCP(制御) ──┐
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
← {"ok": true, "name": "isdbt-sdr demod_server", "protocol": 1, "source": "replay:C:/rec/ch27.ts"}
```
`protocol` はこの文書の版。`source` は人間向けの説明文字列。

### tune
```
→ {"cmd": "tune", "channel": 27}
→ {"cmd": "tune", "freq_hz": 557142857}
← {"ok": true, "channel": 27, "freq_hz": 557142857.0}
```
- `channel` は地デジ物理チャンネル（13〜62）。中心周波数は
  473 + 1/7 MHz + 6 MHz × (ch − 13)（`tools/iq_record.py` と同じ式）。
- `channel` と `freq_hz` の両方がある場合は `freq_hz` を優先する。
- 選局後、TS ストリームにはそれ以降に復調したデータだけが流れる（デーモン側で
  送信待ちを捨てる）。BonDriver 側も `PurgeTsStream` 相当を行う。
- 再生モードのデーモンは選局を記録するだけで、同じファイルを先頭から再生し直す。

### status
```
→ {"cmd": "status"}
← {"ok": true,
   "locked": true,
   "signal_db": 28.5,
   "channel": 27, "freq_hz": 557142857.0,
   "mode": 3, "gi": "1/8",
   "layers": [{"name": "A", "mod": "QPSK",  "rate": "2/3", "I": 4, "segments": 1},
              {"name": "B", "mod": "64QAM", "rate": "3/4", "I": 2, "segments": 12}],
   "ts_packets": 1234567,
   "rs_uncorrectable": 12,
   "dropped": 0,
   "stream_clients": 1}
```
- `signal_db` は BonDriver の `GetSignalLevel` にそのまま返す値。実時間復調では
  MER（等化後コンスタレーションの誤差ベクトルから算出、dB）を入れる。再生モードでは
  起動オプションの固定値。
- `locked` は TMCC フレーム同期が取れているか。
- `rs_uncorrectable` は起動（または選局）以降の RS 訂正不能 TSP 数。
- `dropped` はストリーム送信が実時間に追いつかず捨てた TSP 数。

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
- TS ストリームの送信は復調と同じ速度（Mode 3 / GI 1/8、QPSK 2/3 + 64QAM 3/4 で
  約 16 Mbps）。再生モードでは PCR から推定したレート、PCR が無ければ固定値。

## BonDriver 側の対応

| IBonDriver2 | 動作 |
|---|---|
| OpenTuner | 制御接続（必要なら `Command` でデーモンを起動して再試行）→ `hello` → ストリーム接続 → 受信スレッド開始 |
| SetChannel(space, ch) | space 0 = UHF、ch 0..39 → 物理 13..52ch → `tune` → 受信キューを空にする |
| GetSignalLevel | `status.signal_db`（500 ms キャッシュ） |
| WaitTsStream / GetReadyCount / GetTsStream / PurgeTsStream | 受信スレッドが 188 の倍数に揃えて積むチャンク列 |
| CloseTuner | 受信停止、起動したデーモンには `quit` |

## 将来の拡張（v1 では未定義）

- `layer` フィルタ（ワンセグだけ欲しいクライアント向け）
- `tune` への `mode` / `gi` 指定（海外 ISDB-T 派生方式）
- IBonDriver3（`GetTotalDeviceNum` など）は SDR 1 台では意味が無いので実装しない
