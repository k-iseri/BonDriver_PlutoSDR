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
- Viterbi は AVX2（8 状態 × 8 ベクトル、枝メトリックは ±X±Y。arm64 は NEON で 4 状態 × 16 ベクトル）+ 窓付きトレースバック。
  復調全体で 2.1 s の I/Q を 2.0 s（1 コア、Release /arch:AVX2）。実時間比 ~1 倍だったため、
  ライブ受信では重複ウィンドウをワーカースレッド（既定 3）で並列に復調して余裕を作っている
  （下の「TVTest / EDCB 連携」「FPGA 前段経路」参照。raw 経路で約 2.7 コア、FPGA 前段で約 0.3 コア。
  2026-10-15 からライブ受信の raw 経路も 8bit ビタビ（`Demodulate(..., fast_fec=true)`、x86 は未再測定）。
  `--sigmf` の一括復調は既定で float 版のまま、`isdbtd --fast-fec` で同じ 8bit 版にして比べられる）。
- raw 経路の前段で重いのは分数リサンプル（48 タップ多相 sinc、`ResampleFractional`）と小数 CFO の回転。
  リサンプルは複素列を float 列として 4 レーン × 4 累算器で積和（NEON、他は同じ構造のスカラー）、CFO 回転は
  サンプルごとの cos/sin をやめて double の複素乗算で進め 4096 サンプルごとに位相を取り直す。Apple M5 の実機
  raw 経路で 1.3 → 0.7 コア。`ISDBT_PROFILE=1` で `[profile] symbols`（リサンプル+CFO / FFT）、`[profile] eq`
  （チャネル推定 / 等化 / デインターリーブ）、層ごとの FEC 行が出る。
- 同日さらに: FFT を基数 4 の Stockham で実部・虚部を明示して書き（std::complex の乗算は NaN 回復の分岐で遅い、
  23 µs / 8192 点）、等化の複素除算を y·conj(h)/|h|² に、チャネル推定のキャリアごとの cos/sin を漸化式に、ライブの
  干渉キャリア検出を FPGA 経路と同じ 1 パス版に、ライブのブロック復号を DecodeRange と同じ範囲復号（前後に助走だけ、
  前のウィンドウが出したフレームは FEC しない、末尾フレームも出す）にして raw 経路 0.7 → 約 0.5 コア、
  選局後の遅延も 1 フレーム短い。float 経路（録音・rt_test）と 8bit 範囲復号の TS は先頭 11 TSP（両方とも訂正不能）
  以外一致。
- `rt/demod/isdbtd.exe` が docs/daemon-protocol.md v2 のデーモン。--ts / --sigmf の再生（demod_server.py と
  同機能）に加え、`--sdr` で実機（libiio）からのライブ受信と逐次復調（長時間再追従込み）を行う
  （docs/hw-setup.md「SDR ライブ受信」）。
- ビルドは `make rt`（Windows: rt/build.ps1 → vswhere で CMake/MSVC を探す。出力は
  rt/build/rt-x64/Release/ の rt_test.exe / isdbtd.exe。Linux / macOS: CMake 直接、出力は rt/build/rt-native/）。
  SIMD は x86-64 の AVX2 と arm64 の NEON（`rt/isdbt/simd.h`。float ビタビ・8bit ビタビ・RS シンドロームの 3 カーネル、
  ISDB-C の積和はスカラーのまま）で、どちらも無いときのスカラー版と同じ結果を出す（rt_test、録音 10 秒の TS が md5 一致）。
  `-DISDBT_NO_NEON=ON` でスカラー版に戻せ、`rt_test --bench` で FEC 段の速さを比べられる。Mac で x86-64 の AVX2 経路を
  コンパイルだけ確かめるには `cmake -S rt -B rt/build/rt-x86 -DCMAKE_OSX_ARCHITECTURES=x86_64` → `--target rt_test`
  （libiio が arm64 なので isdbtd はリンクできない）。Rosetta を入れれば `arch -x86_64 rt/build/rt-x86/rt_test testdata/rt_vectors`
  で AVX2 経路のテストも回る（2026-10-15 に 44 件通過を確認。Rosetta 上の速度は実機の目安にならない）。
  OS 依存は isdbtd のソケット（Winsock / POSIX）と isdbc のサイクルカウンタだけで、復調コアは標準 C++17。

## FPGA 前段経路（pluto-fw-isdbt v2、2026-10-15 実装）
```
Pluto SDR Nano（XC7Z010）: ADC → GI 相関同期 → CFO 回転 → FFT 8192 → SP 等化（CSI 付き）→ {I8, Q8, CSI8} × 5617 / シンボル
  ── USB 14.9 MB/s ──
ホスト rt/: 32bit 詰めのまま 干渉検出 → 論理変換+セグメント間（1 gather）→ 時間デインターリーブ（必要行だけ）
  → デマップ+ビットデインターリーブ+±31 量子化（1 パス、int8）→ ビタビ（8bit コスト × 32 レーン AVX2 / 16 レーン × 4 NEON、デパンクチャ融合）
  → バイトデインターリーブ → 逆拡散 → RS（32 TSP 同時 AVX2 / NEON）→ TS
```
- `rt/demod/sdr_source` が CAPS bit3 で前段の有無を見て自動で使う（ini `[SDR] FpgaEq=auto`）。手順は raw でソフト
  AGC 約 1 秒 → モード 3（同期 FFT）の 16 窓から整数 CFO δ → モード 0 を 260 ms → EQ_CTRL / モード 4。モード 3/4 中は
  ファームの ADC レベル計（0x54）でソフト AGC を回し続ける。CATV（ISDB-C）は raw のまま（前段を止める）。
- `rt/demod/live_demod` はウィンドウ（文脈 3 + 新規 8 + 1 フレーム + 16 シンボル）を `DemodulateEqualizedPacked`
  に渡し、**範囲復号**（`DecodeRangePacked`）で出力する 8 フレーム + 助走（先頭 102・末尾 8 シンボル）だけを
  デマップ〜RS にかける。先頭助走はバイトデインターリーブの履歴 11 TSP とビタビの立ち上がりを含み、どの階層でも
  TSP 境界に揃う長さ。出力は完全なフレームだけ（`DemodResult::emit_first / emit_end`）なので Emit がそのまま継ぐ。
- ロック前の信号レベルは等化済みシンボルの TMCC / AC1 キャリア（DBPSK ±4/3）から 64 シンボルごとに MER を出す
  （TVTest のスキャンは選局 5 秒後に 7 dB 未満だと次へ進むため）。
- CPU（isdbtd、ch 24）: raw ホスト復調 2.7 コア → 前段あり 1.04 → 範囲復号 0.72 → ビタビ 8bit 0.58 → RS SIMD 0.51
  → 量子化 1 パス 0.43 → 干渉検出 1 パス 0.38 → 32bit 詰め 0.30 コア。残りはビタビ 0.19・デマップ 0.12・
  デインターリーブ 0.06 s / 1.8 s。`ISDBT_PROFILE=1` で段ごとの時間、`ISDBT_VITERBI_CHECK=1` で float 版ビタビとの突き合わせ。
- **弱い入力で FPGA 経路が raw より落ちていた問題（ファーム v2.0.1 以前）**: 原因はファームの GI 相関器の移動和に
  残る固定オフセット（リセット直後の N 個の積を引き算が取り除かない）で、信号が弱いほど相関位相 = CFO 推定が傾き、
  FFT 前に補正されない残留 CFO が ICI になっていた（27ch ゲイン 24 dB: 推定 +83 Hz / 真値 +42 Hz → ICI 床 23 dB）。
  切り分けは `fe_carrier_demod.py --gain` でモード 3 のビンを取り、ホスト等化・`fe_eq_model.py --fixed`・モード 4 の
  MER が一致（= 等化器は白）、NCO の小数 CFO（ヘッダ w6）とホストで測る残留の和が一定（= 同期の推定が偏る）で
  確定した。FFT 出力は BFP でビンの大きさはレベルに依らない（量子化ではなかった）。詳細は pluto-fw-isdbt
  docs/design-v2.md 3g。v2.0.2 で修正。
- 7010 で残る選択肢（デマップの FPGA 化、TMCC 復号）は数 % の効果。後段（ビタビ・RS）の FPGA 化は時間デインターリーブ
  （6.4 MB）が BRAM に入らず 7020 向け（pluto-fw-isdbt docs/design-v2.md）。
- 検証ツール: `tools/fe_fft_test.py`（FFT テストモードのループバック）、`fe_sync_test.py`（同期モード）、
  `fe_carrier_demod.py`（同期 FFT 出力 → ref/ 後段で TS 一致）、`fe_eq_model.py`（等化器の固定小数点モデル、
  `--vectors` で RTL の検証ベクタ）、`fe_eq_test.py`（モード 4 の取り込み → ref/ 後段で MER / RS / TS 一致。
  δ 推定・EQ_STATUS の読み出し付き）。デーモン側は `ISDBT_FE_DUMP=path` で前段フレーム列をそのまま書ける。
- 落とし穴: ファーム v2.0.0 の窓開始サンプル番号は下位 32bit だけ（528 秒で一周）。ホストで周回を数え（v1.3.1）、
  ファーム v2.0.1 で 64bit 化。モード 3 → 4 を直接切り替えると時間軸がずれるのでモード 0 を挟む。

## TVTest / EDCB 連携（BonDriver、2026-10-02 実装）
```
TVTest / EDCB
  └─ rt/bondriver/BonDriver_PlutoSDR_T.dll ── TCP(制御 JSON Lines) ──┐
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

## ケーブル TV トランスモジュレーション（ISDB-C、2026-10-13 実装）
```
I/Q (任意 fs)
  → 2 sps へリサンプル → 帯域端で粗 CFO → RRC 整合フィルタ（α=0.13）     [ref/isdbc/demod.py]
  → Oerder–Meyr タイミング（ブロック推定 → 直線フィット）→ シンボル抽出
  → z^4 で微 CFO → ブロック 4 乗法位相 → 判定指向 LS 等化（T 間隔 FFE）→ 判定指向位相
  → 硬判定 → 2 MSB 差動復号 → ビット境界探索（204 周期の同期バイト）     [ref/isdbc/mapping.py]
  → デインターリーブ → RS(204,188) → 逆ランダマイズ（8 パケット単位）      [ref/isdbc/framing.py]
  → TSMF 分離（PID 0x002F ヘッダ、53 スロット、相対 TS 番号）→ TS           [ref/isdbc/tsmf.py]
```
- 内符号が無い（RS のみ）ので 64QAM の実用限界は MER ≈ 22 dB。実放送（ケーブル端子）は MER 33 dB。
- `rt/isdbc/` は同じ推定器を逐次化（入力レートの多相 RRC 補間でシンボル直接抽出、O&M をブロックごと、
  LS 等化をチャンクごとに再解）。`rt/demod/live_isdbc` は 1 ワーカーの逐次復調（ウィンドウ分割不要、
  レイテンシ < 0.5 s）。`rt/demod/live_switch` が地デジ / ISDB-C を 1 台の SDR で切り替える。
- rt 側の軽量化（2026-10-15）: RS は 32 TSP ずつ溜めて AVX2 版に渡す（Feed ブロック末尾で端数を Flush）、LS の
  R 蓄積は位相ブロック末尾で float 積和、FFE の実部・虚部積和を 1 ループに、AGC 利得は 64 シンボルごと。
  CFO 回転は 8 サンプルずつ float、位相ブロック（判定指向位相 2 パス・語生成の硬判定と MER・R の積和）は AVX2、
  補間は 8 シンボルまとめて先に計算して補間 → FFE → 出力の直列連鎖を切る（これが最も効いた: 0.54 → 0.35）。
  ch 55 で **約 0.35 コア**（復調スレッド 1 本。SDR 取り込みスレッドは 1% 程度）。残りは FFE と補間の積和そのもの。
  FPGA 化は 7010 の資源が無く、CATV 専用ビットストリームの PL リロードか 7020 が要る。
- 変調器 `ref/isdbc/modulator.py` と `tsmf.multiplex` でループバック（AWGN / CFO / クロック / 反射 /
  スペクトル反転 / TSMF 2 本）を検証。実放送キャプチャは `ref/tests/test_isdbc_real.py` と rt_test の
  `isdbc_real_catv56ch`。

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
