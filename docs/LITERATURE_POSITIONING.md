# Literature positioning: fixed-total latency allocation

Verified against arXiv records on 2026-09-09 (abstracts read; no reference
is cited from memory). Scope: what prior work exists on the concepts the
flagship touches, where the flagship question sits relative to it, and
what is NOT claimed as novel.

## Prior work by concept

### Total latency cost / latency as risk (closest neighbors)

* **Gao & Wang (2018), "Optimal Market Making in the Presence of Latency"
  (arXiv:1806.05849).** MDP formulation for large-tick assets; latency is a
  delay between decision and execution treated as a single parameter, shown
  to be an additional source of risk that hurts MM performance and can
  flip profitability criteria.
* **Lehalle & Mounjid (2016), "Limit Order Strategic Placement with Adverse
  Selection Risk and the Role of Latency" (arXiv:1610.00261).** The closest
  conceptual neighbor: shows the VALUE of predictive information (liquidity
  imbalance) is eroded by latency, and measures the added value of latency
  to limit-order placement via stochastic control.
* **Bonart & Gould (2015), "Latency and liquidity provision in a limit
  order book" (arXiv:1511.04116).** Empirical Nasdaq study of order flow
  phases around market orders; strategic liquidity providers weigh adverse
  selection against waiting costs.
* **Barzykin (2026), "Dynamic slippage control and rejection feedback in
  spot FX market making" (arXiv:2603.07752).** Adverse selection modelled
  explicitly as latency-driven price moves over a delay window (Avellaneda-
  Stoikov tradition); slippage tolerance is an optimized control.

**What these do NOT do**: all treat latency as a scalar (a delay window, or
decision-to-execution lag). None ask whether redistributing a FIXED total
budget across the components of a trader's own pipeline changes economics.
That decomposition, observation vs decision vs order-transit at fixed
total, is the flagship's specific question.

### Queue position value

* **Huang, Pulido, Rosenbaum, Saliba & Sfendourakis (2019),
  "From Glosten-Milgrom to the whole limit order book..." (arXiv:1902.10743).**
  Agent-based LOB model that quantitatively VALUES queue position (relevant
  to our queue-inference layer and its documented accuracy bound).
* **Danyliv, Bland & Argenson (2019), "Random walk model from the point of
  view of algorithmic trading" (arXiv:1908.04333).** Under a random-walk
  price, all limit levels have equal execution cost, a useful null-model
  contrast to our queue mechanics.

### Speed bumps / market-design latency

* **Khapko & Zoican (2019), "Do speed bumps curb low-latency trading?
  Evidence from a laboratory market" (arXiv:1910.03068).** Laboratory
  (experimental) market where participants invest in speed under imposed
  delays. Methodologically the closest spirit to ours: a controlled
  laboratory for a latency question.
* **Kelly & Yudovina (2015), "A Markov model of a limit order book..."
  (arXiv:1504.00579).** Includes analysis of HFT Nash equilibria when
  continuous trading is replaced by frequent batch auctions (the canonical
  latency-flattening design).
* **Gong, Liu & Kate (2023), "The Case of FBA as a DEX Processing Model"
  (arXiv:2302.01177).** Discrete batching vs continuous matching welfare
  comparison.

**Relation to the flagship**: speed bumps and FBAs are EXOGENOUS, market-wide
delays, the venue moves latency for everyone. The flagship studies
ENDOGENOUS allocation of a fixed budget INSIDE one trader's pipeline. These
are complementary framings, and our engine can express both (venue-side
delays exist as exchange_processing/publish stages; the allocation study
deliberately holds them fixed).

### Self-exciting / clustered order flow

* **Alfonsi & Blanc (2014), "Dynamic optimal execution in a mixed-market-
  impact Hawkes price model" (arXiv:1404.0648).** Optimal execution when
  other takers follow Poisson vs Hawkes flows, shows self-excitation
  changes viability/manipulation conditions.
* **Kumar (2021), "Deep Hawkes Process for High-Frequency Market Making"
  (arXiv:2109.15110).** Self/cross-excitation feedback between order
  arrivals and the LOB state in MM strategy design.
* **Mahseredjian (2025), "Diffusive Limit of Hawkes Driven Order Book
  Dynamics With Liquidity Migration" (arXiv:2511.18117).** Hawkes-driven LOB
  mesoscopic theory.

**Relation to the flagship**: these calibrate or theorize under Hawkes-type
flow. Our simulator deliberately does NOT implement a full Hawkes process,
it uses a minimal one-shot self-excitation with a bit-exact Poisson null
(docs/SIMULATION.md) so the mechanism question is testable without a
calibration claim. The placebo design (Poisson null vs clustered) mirrors
the Poisson-vs-Hawkes contrast in Alfonsi & Blanc, at the level of
controlled simulation rather than closed-form theory.

### Counterfactual / laboratory methodology

* **Khapko & Zoican (2019)** (above) — laboratory markets for latency
  policy questions.
* **Dewhurst, Arnold & Van Oort (2018), "Selection mechanisms affect
  volatility in evolving markets" (arXiv:1812.05657).** Agent-based market
  with a batch-auction substrate, agent-based + mechanism-level
  experimentalism.

## Where the flagship sits

The fixed-total allocation question, "is total latency a sufficient
statistic for execution economics, or does placement within the trader's
pipeline matter independently", was not found in the verified literature
above. Prior latency work (i) varies the total/magnitude, (ii) treats
latency as a scalar delay, or (iii) studies venue-imposed delays
(speed bumps/FBAs). The flagship's contribution is the QUESTION plus a
deterministic, engine-verified, paired-counterfactual instrument for it,
not a positive empirical finding (the current verdict is
UNCERTAIN/UNDERPOWERED; docs/EXPERIMENTS/flagship_allocation.md).

## What is explicitly NOT claimed as novel

* Latency hurts execution (known; see Gao & Wang, Lehalle & Mounjid).
* Queue-position value (Huang et al.; our layer even quantifies its own
  inference bound).
* Self-exciting flow effects (Hawkes literature).
* Counterfactual simulation methodology per se (laboratory-market
  tradition).
* Any real-market calibration or external validity, this is a simulator
  property study.

## Gaps in this audit

arXiv-only coverage (journals like Mathematical Finance, JFM, and
industry white papers e.g. exchange speed-bump filings were not searched,
API access). Finance literature outside arXiv may contain closer work; a
full novelty claim would require a wider search. The README and flagship
docs therefore phrase the contribution as "a precise, falsifiable question
with a deterministic instrument", never as "the first to study latency".