# 実機セットアップ（Windows + Pluto SDR nano）

2026-10-10、Pluto SDR nano（互換機）をこの PC に USB 接続して疎通確認済み。

## ボード情報（実機で確認）
- hw_model: Analog Devices PlutoSDR Rev.B (**Z7010-AD9363A**)
- ファームウェア: v0.39-dirty（互換機の独自ビルド。カーネル 2025-10-09 付）
- XO 偏差: 実測 **約 -16 ppm**（クロック・CFO の両方で一致。sync が自動補正するので問題なし）
- URI: `usb:`（libusb 直結）。本機は Windows 11 24H2 以降のため RNDIS の
  `ip:192.168.2.1` は使わず、`usb:` を標準とする。

## ドライバ / ライブラリの構成
1. **USB ドライバ**: ADI の PlutoSDR-M2k-USB-Drivers.exe（導入済み）。
   デバイスマネージャーに「IIO」(USBDevice)、「PlutoSDR Serial Console (COM3)」等が出れば OK。
2. **libiio v1.0.0 ネイティブ DLL**: GitHub リリースの `Windows.zip` →
   `Windows-VS-2022-x64/Release` の DLL 一式と `iio_info.exe` / `iio_attr.exe` を
   **`.venv\Scripts\` にコピー**してある。
   - venv を作り直したら再コピーが必要:
     https://github.com/analogdevicesinc/libiio/releases/download/v1.0.0/Windows.zip
   - pylibiio は `ctypes.util.find_library`（= PATH 検索）で `libiio1.dll` を探すため、
     venv 非 activate で実行するスクリプトは自分で
     `os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]`
     を import adi より前に行う（tools/iq_record.py、hw/scripts/scan_channels.py 参照）。
3. **pyadi-iio 0.0.21 + pylibiio 1.0**（pip、venv 内）。
   - **pylibiio 1.0 の Windows バグに手当て済み**: `iio/__init__.py` の
     `_libc = _cdll(find_library("c"))` が Python 3.12+ の Windows で None になり落ちる。
     venv 内のファイルを直接パッチして `ucrtbase` にフォールバックさせた
     （`# Patched for Windows (isdbt-sdr)` で検索）。venv 再作成時は再パッチが必要。
     上流: https://github.com/analogdevicesinc/libiio の python バインディング。

## 動作確認コマンド
```powershell
# コンテキスト情報（ファーム・モデル確認）
.venv\Scripts\iio_info.exe -u usb:        # ※ PATH に .venv\Scripts が要る（DLL 検索）

# チャンネルスキャン（GI 相関で ISDB-T 判定）
.venv\Scripts\python hw\scripts\scan_channels.py

# 録音 → 品質レポート → 復調
.venv\Scripts\python tools\iq_record.py --ch 25 --sec 5 --out testdata\captured\ch25
.venv\Scripts\python tools\rx_report.py testdata\captured\ch25 --plot testdata\captured\report\ch25
.venv\Scripts\python tools\demod_sigmf.py testdata\captured\ch25 --track
```

## 2026-10-10 の初回受信結果（付属小型アンテナ・室内）
- スキャン: 物理 25ch が最良（GI 相関 0.231）、26〜30ch にも微弱な ISDB-T。
- 25ch 復調: ブラインド判定 Mode 3 / GI 1/8、**TMCC パリティ OK**
  （A: QPSK 2/3 I=4 ×1seg、B: 64QAM 3/4 I=2 ×12seg、部分受信フラグあり＝典型構成）。
- ただし MER が A 階層 -2.2 dB / B 階層 0.0 dB（必要目安 A≈8 dB、B≈19 dB）で
  **RS 訂正は全滅（TS 出力不可）**。キャリア MER は帯域中央（セグメント 0 付近）が
  特に悪い。原因は付属アンテナの利得不足（＋室内の周波数選択性フェージング）。
- ソフトウェア側は録音 → 同期 → TMCC → 復調の全段が実波で動作することを確認済み。
  次の改善はアンテナ（屋外アンテナ端子への接続、または八木/ロッドの窓際設置）。

## SDR ライブ受信（isdbtd --sdr、2026-10-10 実機検証済み）
```powershell
# 事前に一度だけ: libiio を rt/third_party へ取得（ビルド時に --sdr が有効化される）
.\rt\third_party\fetch_libiio.ps1
make rt

# ライブ受信デーモン（アンテナ → 復調 → BonDriver/TVTest）
rt\build\rt-x64\Release\isdbtd.exe --sdr usb: --ch 24 --no-quit
```
- デーモンを手動で常駐させる場合は **--no-quit を付け、BonDriver_PlutoSDR_T.ini は Spawn=0** にする。
  Spawn=1 のままだと TVTest 終了時に BonDriver が quit を送ってデーモンごと落とす
  （Spawn=1 は録画 TS 再生デーモンを DLL に起動させる開発用の設定）。
- 取り込みは既定 7,111,111 Hz（公称の 7/8）。16bit×公称レートは USB 2.0 の実効上限
  （実測 ~31.3 MB/s）を超えるため。品質劣化はない（同時刻の公称レート録音と MER 一致）。
- 復調は重複ウィンドウ（文脈 4 + 新規 8 フレーム、選局直後の 1 本だけ文脈 2 + 新規 4）× ワーカー 3 並列（ini `Workers`）。選局から TS まで約 5 秒、連続再生の安定まで約 10 秒（WinFrames=16 なら raw 経路の CPU は軽いが約 13 秒）。
  pluto-fw-isdbt v2 のファームなら同期・FFT・等化を FPGA が行い、ホストは約 0.3 コア（docs/architecture.md「FPGA 前段経路」）。
- 実測: 2.5 分以上連続でフレーム欠落ゼロ、ワンセグ映像 PID は 75 秒ストリームで連続性ジャンプ 0。
  B 階層（64QAM 3/4）は C/N ≥ 19 dB のとき安定（夕方の計測では 18.0 dB で 2 割欠け。
  これはアンテナ/伝搬の問題で、受信系の限界ではない）。
- 排出はペーサーで 1 フレーム（≈231 ms）周期に平滑化済み（実測で毎秒 16〜20 Mbps の安定供給）。
- チャンネル切替はパイプライン再構築のため映像まで ~10 秒かかる。同一チャンネルへの
  tune（クライアント再接続）はリセットしない。

## トラブルシュート: ストリームが壊れたら（stream error -138 / 全ウィンドウ sync failed）
isdbtd を強制終了（プロセス kill）で繰り返し落とすと、Pluto 側の iiod セッションが残って
以後のストリームがサンプル落ちだらけ・または即エラーになることがある。ソフト再起動では直らない。
**シリアルコンソール（COM3）から再起動すると復旧する**（USB 抜き差しと同等）:
```powershell
$p = New-Object System.IO.Ports.SerialPort "COM3",115200,None,8,One
$p.Open(); $p.Write("`n"); Start-Sleep -m 800
$p.Write("root`n"); Start-Sleep -m 800; $p.Write("analog`n"); Start-Sleep -m 1200
$p.Write("reboot`n"); $p.Close()   # 約 30 秒で再接続される
```
isdbtd を止めるときは quit コマンド（またはプロセスへの穏当な終了）を優先する。

## 受信ゲイン（2026-10-13 実測、重要）

AD936x 内蔵の slow_attack AGC は帯域内レベルを約 −11 dBFS に保とうとするが、ケーブル TV 端子のように
全チャンネルが強い入力ではフロントエンド（LNA/ミキサ）が合計電力で圧縮され、復調できても MER が
20 dB 前後に張り付く。手動ゲインの掃引（56ch トラモジ 64QAM、同じ端子）:

| hardwaregain | 帯域内レベル | MER |
|---|---|---|
| AGC (42) / 38 / 36 / 33 dB | −11 / −16 / −18 / −21 dBFS | 20.0〜20.1 dB |
| 30 dB | −22 dBFS | 33.1 dB |
| 27 dB | −25 dBFS | 33.0 dB |
| 24 / 20 / 10 dB | −28 / −32 / −42 dBFS | 31.2 / 29.9 / 24.8 dB |

地デジも同様で、同じ端子の 24ch が AGC で A 17.9 / B 19.8 dB → ゲイン 30 dB で A 23.6 / B 27.0 dB、
27ch は A 24.5 / B 30.1 dB。このため isdbtd / BonDriver の既定はソフトウェア AGC（帯域内 −24 dBFS 目標、
`SdrConfig::agc_target_dbfs`）になっている。録音ツールで同じ条件にするには `--gain 30` 程度を指定する
（`tools/iq_record.py`）。弱い空中線では目標に届かずゲインが上限（73 dB）まで上がるので、
ソフト AGC は従来の AGC の上位互換になる。

## 未検証のまま残っているもの
- `--bits 8`（tezuka_fw の complex 8bit モード）: 本機は独自ビルド v0.39 のため
  モードがあるか不明。また record_8bit は libiio 0.x の Buffer API で書かれており、
  pylibiio 1.0 では API が変わっているため **要改修**（hw/pluto/fw-8bit.md 参照）。
- 長時間録音での USB 2.0 帯域（16bit は 32.5 MB/s）。5 秒録音では
  68/68 チャンクがロックしており、少なくとも短時間ではサンプル落ちは観測されず。
