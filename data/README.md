# data/

Market-data inputs for the research layer (`mktdata_v1` CSV; see docs/DATA.md).

- `golden/` — tiny hand-verifiable fixtures (imbalance/OFI/book cases) with
  manually computed expectations asserted in tests/test_quant.cpp and
  tools/research/reference_features.py. Never extend without versioning.
- Bring your own licensed captures as CSV: `arbiter research validate --csv F`
  then `arbiter research run EXP-001 --csv F`. Sequence gaps and session
  boundaries must be preserved, never interpolated.
- The deterministic synthetic generator (`pipeline::synthesize`, `--synthetic`)
  needs no external data and is the default control (KNOWN alpha when
  `--biased`, null otherwise).