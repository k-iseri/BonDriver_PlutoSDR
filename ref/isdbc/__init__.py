"""ISDB-C（ケーブルテレビの 64QAM トランスモジュレーション）の参照実装。

伝送路符号化・変調は ITU-T J.83 Annex C（= JCTEA STD-002 の伝送方式。フレーム構成と
誤り訂正は Annex A と同一、ロールオフ 0.13・シンボルレートが 6 MHz チャンネル向け）、
複数 TS の多重（TSMF）は ITU-T J.183 Appendix I。根拠は docs/isdbc-spec-notes.md。

ref/ なので速度より可読性と仕様との対応を優先する（isdbt パッケージと同じ方針）。
"""
