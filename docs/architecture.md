# 処理パイプライン設計

## 復調側（ref/isdbt）
```
I/Q (任意 fs)
  → resample to 512/63 MHz           [sync.py]
  → GI 相関によるシンボル同期・粗 AFC   [sync.py]
  → FFT (8192)                        [demod.py]
  → 精 AFC / サンプルクロック追従      [sync.py]  （SP の位相回転から推定）
  → SP 補間によるチャネル等化           [demod.py]
  → TMCC 復号（DBPSK, 同期語, 誤り訂正） [demod.py]  → 階層構成を確定
  → 階層ごとにデマップ（軟判定）         [demod.py]
  → 周波数デインターリーブ              [deinterleave.py]
  → 時間デインターリーブ                [deinterleave.py]
  → ビットデインターリーブ              [deinterleave.py]
  → デパンクチャ → Viterbi(軟判定)      [fec.py]
  → バイトデインターリーブ → RS(204,188)[fec.py]
  → エネルギー逆拡散 → TS パケット      [ts.py]
```

各段は「入力 numpy 配列 → 出力 numpy 配列」の純関数にし、`.npy` で中間データを保存できるようにする。
段ごとの単体テストは、変調器の対応する段の出力を正解として使う。

### 復調側の要点（2026-10-02 実装）
- 同期: GI 相関 |s| を 64 シンボルごとに畳んでピーク位置を直線フィット → クロック偏差（ppm）を
  多相 sinc 補間（1/256 サンプル分解能）で補正 → GI 相関位相で小数 CFO → 固定パイロット
  （TMCC/AC1/CP）電力が最大になるビンずれで整数 CFO。FFT 窓は GI 内を Ng/8 だけ早く開く。
- 等化: SP を前後 8 シンボルで平均（3 キャリアおきの格子）→ 格子の位相傾斜（バルク遅延）を除いて
  線形補間 → 傾斜を戻す → 各シンボルの SP で共通位相補正。CSI = |H|² を軟判定の重みにする。
- フレーム整合: TMCC で決めたフレーム先頭から、仕様の補遅延（時間 I·(95−m)、ビット 120−d、バイト 11−b）を
  入れた直後が TSP 境界（docs/isdbt-spec-notes.md 10 章）。
- Viterbi: 8192 ステップのブロックに分け、前 96 / 後 64 ステップの重なりで独立に復号して numpy で
  ブロック並列化（25M ビットで約 20 秒）。
- 出力 TS は復号できた全フレーム分で、先頭 11 TSP（バイトデインターリーブの履歴なし）は不定。
- 追従（2026-10-10）: `sync.TrackedCarrierSource` がタイミング・CFO を GI 相関チャンク（64 シンボル）
  ごとの区分線形で追従し、ロック判定（GI 相関比 > 0.5）で瞬断を区間に分ける。`demodulate(track=True)` は
  区間ごとにフレーム同期からやり直して TS を連結する。等化器はバースト雑音（インパルス・瞬断の残り）を
  SP 残差で検出し、該当シンボルを平均から除外 + CSI=0 の消失として扱う。
  受信品質は `measure.py`（MER・キャリア別 MER・遅延プロファイル・Mode/GI ブラインド判定、
  `tools/rx_report.py` で一発表示）。
- 長い録音: `sync.CarrierSource` が同期パラメータ（タイミング直線、クロック比、CFO）だけを全長から推定し、
  シンボル範囲ごとに分数リサンプル → 周波数補正 → FFT する。`demodulate(block_frames=6)` は
  6 フレームずつ処理し、各ブロックに時間デインターリーブの履歴（最大 I 分、2 フレーム）と等化の前後 4 シンボル、
  ビット/バイト/Viterbi の立ち上がり用に直前 1 フレームの重ねを付けて、重ね分の出力は捨てる。
  メモリは録音データ本体（complex64、30 秒で約 2 GB）+ ブロック分（数百 MB）で済む。

## 変調側（modulator.py）
復調の逆順。試験信号生成専用。TMCC は与えられた階層構成から生成する。

## リアルタイム化（rt/、2026-10-10 実装）
- `rt/isdbt/` が C++17 の復調コア（静的ライブラリ isdbt_core）。構成は ref/isdbt と 1:1 対応
  （params / tables / fft / io / npy / fec / pilots / tmcc / deinterleave / equalize / sync / demod）。
  アルゴリズムは ref/ と同一で、`tools/gen_rt_vectors.py` + `rt/tests/rt_test.cpp` が段ごとに
  一致を検証する（検証方針は docs/decisions.md 2026-10-10 の行を参照）。
- Viterbi は AVX2（8 状態 × 8 ベクトル、枝メトリックは ±X±Y）+ 窓付きトレースバック。
  復調全体で 2.1 s の I/Q を 2.0 s（1 コア、Release /arch:AVX2）。実時間比 ~1 倍なので、
  ストリーミング化の際に前段（FFT〜デインターリーブ）と復号（Viterbi〜RS）を
  スレッドでパイプライン化して余裕を作る（TODO）。
- `rt/demod/isdbtd.exe` が docs/daemon-protocol.md v1 のデーモン。現状は --ts / --sigmf の
  再生（demod_server.py と同機能）。実機ソース（libiio）と逐次復調は、長時間再追従
  （sync の閉ループ化）と合わせて入れる。
- ビルドは `make rt`（rt/build.ps1 → vswhere で CMake/MSVC を探す）。出力は
  rt/build/rt-x64/Release/ の rt_test.exe / isdbtd.exe。

## TVTest / EDCB 連携（BonDriver、2026-10-02 実装）
```
TVTest / EDCB
  └─ rt/bondriver/BonDriver_PlutoSDR.dll ── TCP(制御 JSON Lines) ──┐
                                           ── TCP(生 TS)          ──┤── 復調デーモン ── SDR / 録画 TS / SigMF
```
- BonDriver はアプリのプロセス内で動く薄い DLL（IBonDriver2）。復調はしない。選局と TS 受信を
  `docs/daemon-protocol.md` のプロトコルでデーモンに頼むだけ。デーモンのクラッシュや
  32/64 bit・DLL 依存の問題をアプリから切り離し、将来 GbE ボードの ARM 側や別 PC でデーモンを
  動かすこともできるようにしている。
- デーモンは 2 つある。`tools/demod_server.py`（Python の偽デーモン、録画 TS の再生、または
  SigMF を `ref/` で先に全部復調してから再生）と、`rt/demod/isdbtd.exe`（C++。同じ 2 ソースを
  rt/ の復調コアで処理。実時間比 ~1 倍）。プロトコルは同一で BonDriver からは区別できない。
  SDR 実機ソース（libiio）はボード到着後に isdbtd へ追加する。
- デーモンは階層 A/B の TS をフレーム順に 1 本へ戻し、訂正不能 TSP と先頭 11 TSP には
  transport_error_indicator を立てる。本来の多重フレーム（BTS）順への復元は TODO。
- `rt/bondriver/bontest.exe` で TVTest 無しに DLL を叩ける。

## デバイス抽象（hw/）
```
class IQSource:
    def open(uri: str)           # "usb:" / "ip:192.168.2.1" / "file:xxx.sigmf-meta"
    def configure(center_hz, fs, bw, gain)
    def read(n) -> complex64[]
```
Pluto 互換機は libiio（pylibiio）、将来の USB3 ボードは SoapySDR。file: はテスト用。

## モジュール対応（ref/isdbt、2026-10-02 時点）
| ファイル | 内容 | 状態 |
|---|---|---|
| params.py | 仕様定数、Mode 別諸元、遅延調整式、コンスタレーション、TMCC 符号表 | 実装済み（VERIFY 項目あり） |
| tables/carrier_randomize.py | セグメント内ランダマイズ表 Mode 1/2/3 | 転記済み（gr-isdbt 由来） |
| tables/pilot_positions.py | TMCC/AC1 絶対位置 Mode 1/2/3 | 転記済み（gr-isdbt 由来） |
| fec.py | RS(204,188) 符号化/復号（BM）、エネルギー拡散/逆拡散、バイトインターリーブ、畳み込み符号・パンクチャ、軟判定 Viterbi（ブロック並列） | 実装済み |
| tmcc.py | TMCC ビット列生成・パリティ・解析 | 実装済み（誤り訂正は未） |
| pilots.py | パイロット PRBS、SP/CP/データキャリア位置 | 実装済み |
| ts.py | ヌルパケット、フレーム単位パディング | 実装済み |
| modulator.py | 送信側全段 | 実装済み（同期変調のみ） |
| channel.py | 劣化モデル | 実装済み |
| sync.py | リサンプル、GI 相関によるシンボル同期・クロック偏差・小数 CFO、整数 CFO、シンボル同期 FFT | 実装済み |
| deinterleave.py | 周波数/時間/セグメント間/ビットのデインターリーブ、軟判定デマップ | 実装済み |
| demod.py | TMCC フレーム同期と解析、SP による等化、階層ごとの復号、demodulate() | 実装済み（Mode 3 / GI 1/8 既定、差動変調未対応） |
