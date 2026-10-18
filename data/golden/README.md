# Golden datasets (hand-verifiable; see docs/DATA.md)

Tiny deterministic inputs with manually computed expectations (asserted in
tests/test_quant.cpp). Never shuffle, never extend without versioning.

- simple_book_001.csv — minimal valid L1 stream (parse/validate/hash).
- imbalance_case_001.csv — book {bid 100@1500, 50@1499 / ask 50@1501, 150@1502}:
  mid=1500.5 spread=1 imb_1=+1/3 imb_2=-50/350 microprice=1500.6667.
- ofi_case_001.csv — top-level OFI: +50 (bid 100->150) then +30 (ask 100->70),
  lifetime total +80, windowed(+,inf)=+80.