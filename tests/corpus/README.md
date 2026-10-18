# Adversarial regression corpus

Persisted divergence reproducers from `arbiter fuzz --target adversarial`
(minimizer-reduced op streams, `.ops` text format). Replayed by
`arbiter fuzz --target corpus` and test_diff's corpus_replays_clean.

Currently EMPTY: every adversarial search to date found zero divergences
between the reference and optimized books. A find would appear here as
`adv_<seed>_<opidx>.ops`; the contract is that entries parse, run
deterministically, and produce zero divergences once fixed.