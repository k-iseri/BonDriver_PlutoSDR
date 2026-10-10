# 8 bit パッキング（README の対策 2）— tezuka_fw の採用

状態: **未検証**（ボード未着。実機での確認項目は末尾）。

## 課題

8.126984 MSPS × 16bit I/Q = 32.5 MB/s は USB 2.0 の実効スループット（実測報告はおおむね
30〜35 MB/s、SDR 用途の安定域はさらに下）を超えるため、純正ファームでは連続受信できない。
8bit に詰めれば 16.3 MB/s で余裕を持って通る。AD9363 の ADC は 12bit なので、失うのは
下位 4bit（約 24 dB 分のダイナミックレンジ）。地デジの C/N 要求（64QAM 3/4 で約 20 dB）に
対し、AGC が適切なら 8bit（理論 SQNR ≈ 50 dB）で十分。

## 判断: 自前で plutosdr-fw をフォークせず tezuka_fw を使う

[tezuka_fw](https://github.com/F5OEO/tezuka_fw)（F5OEO、GPL-3.0）が complex 8bit モードを
実装済みで、USB で 14 MHz、GbE で 45 MHz の安定ストリーミングを謳う。対応ボードに
Pluto（Zynq-7010）、Pluto+、LibreSDR（Zynq-7020）が含まれ、本プロジェクトの移行予定
（CLAUDE.md）と一致する。自前フォークは Vivado + Linux ビルド環境と実機デバッグが必要で、
正しさの検証コスト（本来の開発対象は復調器）に見合わない。

- このリポジトリには tezuka_fw のコードを**複製しない**。ホスト側（`pack8.py`、
  `tools/iq_record.py --bits 8`）はデータ配置というプロトコルに合わせて独立に書いた。
- ファームウェアは別アーティファクト（SD カード / フラッシュイメージ）として各自導入する。

## 8bit モードの仕組み（ホストから見た動作）

[SDR++ の議論](https://github.com/AlexandreRouma/SDRPlusPlus/issues/1580) によると:

1. IIO デバイス `cf-ad9361-lpc` の `voltage1`（rx0_q）を無効化し `voltage0` のみ有効にする
2. FPGA が voltage0 の 16bit 枠に (i8, q8) を詰めて送る
3. ホストはバッファを int8 の対として解釈する（`pack8.unpack8`）

`# VERIFY:` バイト順（下位 = I）・切り捨て方式・モード切替条件は実機未確認。`pack8.py` 参照。

## 導入手順（ボード到着後）

1. [リリース](https://github.com/F5OEO/tezuka_fw/releases) から対象ボード用イメージを取得。
   自前ビルドする場合は Linux ホスト（WSL2 可。開発本体は Windows ネイティブのままで、
   ファームビルドだけの例外）で:
   ```
   git clone https://github.com/F5OEO/tezuka_fw && cd tezuka_fw
   ./getbuildroot.sh
   ./build.sh <board>   # pluto / plutoplus / libresdr など
   ```
2. 書き込みは SD ブートが推奨（文鎮化しにくい）。ただし **無印 Pluto 互換機には SD スロットが
   無い**ので DFU / マスストレージ更新になる。必ず純正ファームイメージを手元に置いてから行う。
   `# VERIFY:` 手元の互換機の更新経路（frm ファイル対応か）は個体確認。
3. 動作確認: `python tools/iq_record.py --uri usb: --ch 27 --fs 8126984 --bits 8 --sec 10 --out ...`
   → `tools/iq_inspect.py` で GI 相関ピーク間隔が一定（= サンプル落ちなし）を確認。

## 実機での確認項目（到着後に消していく）

- [ ] tezuka_fw で 8.127 MSPS / 8bit / USB 2.0 が録音落ちなしで通るか（iq_inspect の GI 相関）
- [ ] voltage1 無効化だけで 8bit モードに入るか（追加の IIO 属性が要るか）
- [ ] 16bit ワード内のバイト順（下位 = I）と符号（int8）— 既知トーンで確認
- [ ] 8bit 録音の TS 完全一致（ループバックではなく実波で、12bit 録音の 7 MSPS 版と比較）
- [ ] AGC（slow_attack）が 8bit でクリップ/量子化不足を起こさないか

## 自前 HDL 改造へ戻す場合の入口（やらない理由の記録を兼ねる）

tezuka_fw が使えない事態（ボード非対応・保守停止）になったら:
[plutosdr-fw](https://github.com/analogdevicesinc/plutosdr-fw) を再帰 clone し、
[ADI の HDL Pluto プロジェクト](https://analogdevicesinc.github.io/hdl/projects/pluto/index.html)
の RX 経路（axi_ad9361 → util_cpack → axi_dmac）の cpack 手前に 12→8bit 切り捨て +
2 サンプル/ワード詰めを挿入し、Linux 側は cf_axi_adc の scan 形式（storagebits）を合わせる。
ビルド手順は [ADI wiki: building the image](https://wiki.analog.com/university/tools/pluto/building_the_image)。
Vivado の要求版は hdl リポジトリの該当タグの tcl（`required_vivado_version`）に従う。
