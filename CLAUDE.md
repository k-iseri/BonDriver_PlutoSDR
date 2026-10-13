# isdbt-sdr — Claude Code 向け作業指示

## 目的
日本の地上デジタル放送（ISDB-T、ARIB STD-B31）のフルセグ（13セグメント）を
汎用SDRボードで受信し、PC上のソフトウェアで復調して MPEG-2 TS を取り出す。
ワンセグ（部分受信、セグメント0のみ）は副次目標であり、フルセグより先に動く想定。
2026-10-13 から、ケーブルテレビの BS/CS トランスモジュレーション（ISDB-C = ITU-T J.83 Annex C 64QAM
5.274 Mbaud + J.183 TSMF）も対象（`ref/isdbc/`、`rt/isdbc/`、仕様ノート `docs/isdbc-spec-notes.md`）。
BonDriver は同じソースから `_T`（地デジのみ）と `_TC`（地デジ + CATV 空間）の 2 DLL を出す。
CATV 空間のチャンネル番号は BonDriver_BDA の TransModulation と同じ通し番号 0..112 を崩さない。

## ハードウェアの状況
- 対象ボード: Pluto SDR 互換機（Zynq-7010 + AD9363、USB 2.0）。手元にあり実機検証済み（2026-10-10〜）。
- 後に Gigabit Ethernet 付きの Pluto+ / LibreSDR（Zynq-7020）へ移行する可能性があるため、
  デバイス接続は `ip:` と `usb:` の両方を同じインターフェースで扱えるよう抽象化する。
- 受信ゲインは AD936x 内蔵 AGC を使わず、帯域内 −24 dBFS 目標のソフトウェア AGC が既定
  （強い全帯域入力でフロントエンドが圧縮するため。`docs/hw-setup.md`「受信ゲイン」）。

## 開発方針（重要）
1. **まず `ref/` の Python 参照実装で正しさを確立する。** numpy/scipy のみ。
   `ref/` では速度最適化を行わない。可読性と仕様との対応関係を優先する。
2. 参照実装が全テストを通過してから `rt/`（C++ または Rust）に移植する。
   `rt/` の出力は `ref/` の出力とビット一致（または許容誤差内）であることをテストで保証する。
3. **ループバックが開発の中心。** `ref/isdbt/modulator.py`（送信側）→ `channel.py`（劣化）→
   復調器 → 元の TS と比較。これにより実機なしで全段を検証する。
4. 変調器と復調器は、同じ誤解を両方に埋め込まないよう、仕様書の該当箇所を
   **章・表番号つきで** コメントに引用しながら独立に実装する。
5. 不確かなパラメータ（特にキャリア配置表、ランダマイズ表）は `params.py` に
   `# VERIFY:` マークを付け、`docs/isdbt-spec-notes.md` に根拠（STD-B31 の表番号）を書く。
   推測で埋めない。分からない場合は TODO として残し、テストを `skip` にする。

## 固定仕様（日本の標準運用）
- 帯域 6 MHz、13 セグメント、サンプルレート 512/63 MHz ≈ 8.126984 MSPS
- Mode 3（FFT 8192、有効キャリア 5617）、GI 1/8 が全国の実運用
- 階層: A = セグメント0（ワンセグ）QPSK 符号化率 2/3 時間インターリーブ I=4、
  B = 残り12セグメント 64QAM 符号化率 3/4 I=2（局により異なりうる。TMCC から読む）
- 入力 I/Q は固定レートに依存しない設計にする。8.126984 MSPS 以外（例: 7.0 MSPS）で
  録音したファイルを内部でリサンプルして処理できること。

## TVTest / EDCB 連携（BonDriver）
- `rt/bondriver/` の BonDriver は 2 モード。デーモンモードは復調を持たず、復調デーモンと
  TCP で話す（プロトコルは `docs/daemon-protocol.md` v2、変更時は版番号を上げ、
  `tools/demod_server.py` と `rt/demod/isdbtd.cpp` と DLL をすべて更新する）。ini の `[SDR] Enable=1`
  （ini.sample の既定）でスタンドアロンモードになり、`rt/demod/live_switch.{h,cpp}`（isdbtd --sdr と共用。
  地デジ LiveDemod / ISDB-C LiveIsdbc を選局先の空間で差し替える）を静的リンクして DLL 内で直接
  SDR 取り込み + 復調する（libiio は遅延ロード）。
- デーモンは `rt/demod/isdbtd.exe`（SDR ライブ / 録画 TS / SigMF）と開発用の `tools/demod_server.py`。
- `rt/bondriver/IBonDriver*.h` は TVTest の宣言を転記したもので、宣言順を変えてはいけない。
- DLL のビルドは `make bondriver`（または `.\dev.ps1 bondriver`）。TVTest 無しの確認は `bontest.exe`。

## 入出力フォーマット
- I/Q: SigMF（`.sigmf-meta` + `.sigmf-data`）、データ型は `cf32_le`・`ci16_le`・`ci8`（8bit 録音）
- TS: 188 バイト固定長パケットの連結。同期バイト 0x47。
- 中間データ（FFT 後のキャリア行列など）は `.npy` で保存可能にし、段ごとの単体テストに使う。

## テスト要件
- `make test` が常に通ること。新機能にはテストを必ず付ける。
- ループバック基準:
  - AWGN、C/N 30 dB、オフセットなし: TS が完全一致
  - 周波数オフセット ±50 kHz、サンプルクロック偏差 ±30 ppm: TS が完全一致
  - 64QAM 3/4 で C/N を振った BER 曲線が理論値から 3 dB 以内
  - マルチパス（2 波、遅延 20 µs、-10 dB）で TS が一致
- 遅いテスト（フル TS フレーム複数分）は `@pytest.mark.slow` を付け、`make test-fast` から除外する。

## 範囲外
- B-CAS / ACAS によるスクランブル解除（libaribb25 等）はこのリポジトリに含めない。
  TS を出力した時点で本リポジトリの責務は終わる。
- 送信機能。変調器は試験信号生成専用で、実際の送信には使わない。

## 参考にしてよいもの
- ARIB STD-B31（公開。日本語版・英語版）。一次情報として最優先。
- gr-isdbt（GNU Radio 用、GPL）。実装の照合先として読んでよいが、コードをコピーする場合は
  ライセンス表記を守り `docs/decisions.md` に記録する。

## 開発環境
- **Windows ネイティブ**（WSL ではない）。venv は `.venv\Scripts\`。
- `make` が無い場合は `.\dev.ps1 <task>` を使う（Makefile と同じタスク名）。
- パスは `pathlib` で扱い、`/` 固定の文字列結合をしない。改行コードは LF で統一（.gitattributes）。
- 実機接続は Windows 側の libiio / pyadi-iio（ADI のインストーラでドライバを入れる）。

## コマンド
```
make setup       # venv 作成と依存インストール（または .\dev.ps1 setup）
make test-fast   # 高速テストのみ
make test        # 全テスト
make lint
make synth       # testdata/synthetic/ に試験信号を再生成
make bondriver   # rt/build/bondriver-x64/Release/ に BonDriver_PlutoSDR_T.dll と bontest.exe
make rt          # rt/build/rt-x64/Release/ に rt_test.exe（ref 一致テスト）と isdbtd.exe（復調デーモン）
make rt-vectors  # testdata/rt_vectors/ に rt 検証用ベクタを ref/ から生成
make rt-test     # rt_test.exe をベクタに対して実行（pytest からは test_rt.py 経由でも走る）
```

## 大きなファイル
- `testdata/captured/` は gitignore。実波録音は手元に置き、生成手順を README に書く。
- `testdata/synthetic/` も原則コミットしない。`make synth` で再現する。
