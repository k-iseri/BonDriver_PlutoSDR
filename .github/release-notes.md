Pluto SDR で地上デジタル放送（ISDB-T フルセグ）を受信する TVTest / EDCB 用 BonDriver。
復調は DLL 内で行うため、添付の zip だけで動作します（復調デーモン不要）。

## インストール

1. Analog Devices の **PlutoSDR-M2k-USB-Drivers** をインストールし、Pluto SDR（互換機可）を USB で接続
2. zip の中身を**全部** TVTest（**x64 版**）の BonDriver フォルダへコピー
3. `BonDriver_PlutoSDR_T.ini.sample` を `BonDriver_PlutoSDR_T.ini` にリネーム（そのままで動く）
4. アンテナを Pluto の RX 端子へ（F型→SMA 変換。壁のアンテナ端子か屋外アンテナを推奨）
5. TVTest のドライバに `BonDriver_PlutoSDR_T.dll` を選び、チューニング空間 UHF でチャンネルスキャン

選局から映像が出るまで 10〜15 秒かかります（異常ではありません）。
B-CAS / スクランブル解除は TVTest 側の設定で、本パッケージの範囲外です。
詳細な手順とトラブルシュートは [docs/setup-tvtest.md](https://github.com/k-iseri/BonDriver_PlutoSDR/blob/main/docs/setup-tvtest.md) を参照してください。

## 動作要件

- Windows 11 x64 / 64bit 版 TVTest（32bit 版では読み込めません）
- Pluto SDR 互換機（AD9363、USB 2.0。Pluto+ / LibreSDR は ini の `Uri=ip:…` で LAN 接続可）
- CPU は AVX2 対応（Haswell 以降）が必要
