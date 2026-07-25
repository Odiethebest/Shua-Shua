# Algorithm — the math behind the engine

The formal companion to the code: each algorithm stated with notation, the key
derivations, and complexity. For plain-language intuition, the design WHYs, and
interview framing, see **[Interview](Interview.md)** — this document deliberately does
*not* repeat that; it is formula-first. It is a **growing** doc: stretch topics (HNSW,
quantization) get their math here as they land.

> Math renders on GitHub (MathJax): inline like $\langle q,x\rangle$, and display blocks
> below. In a plain editor the `$…$` shows as source.

## Notation

| symbol | meaning |
|---|---|
| $n$ | items in the store ($=3000$) |
| $d$ | embedding dimension ($=64$) |
| $x_i\in\mathbb{R}^d$ | item $i$'s embedding, unit-normalized ($\lVert x_i\rVert_2=1$) |
| $q\in\mathbb{R}^d$ | query vector, unit-normalized |
| $K$ | categories ($=6$); $c_k\in\mathbb{R}^d$ category $k$'s unit centroid |
| $u$ | float32 unit roundoff, $2^{-24}\approx 5.96\times10^{-8}$ |

## 1. Similarity — cosine as a dot product

Cosine similarity is

$$\cos\angle(q,x)=\frac{\langle q,x\rangle}{\lVert q\rVert\,\lVert x\rVert}.$$

Every stored vector and every query is unit-normalized, so $\lVert q\rVert=\lVert x\rVert=1$
and the kernel collapses to a bare inner product:

$$\operatorname{sim}(q,x)=\langle q,x\rangle=\sum_{i=1}^{d} q_i\,x_i\ \in[-1,1].$$

Normalizing is what makes "similar" mean "same direction" (small angle), independent of
magnitude, while keeping the hot path a single multiply–add loop.

## 2. Why recall is meaningful — synthetic geometry

Item $i$ in category $k$ is $x_i=\operatorname{normalize}(c_k+\varepsilon_i)$ with
$\varepsilon_i\sim\mathcal{N}(0,\sigma^2 I_d)$, $\sigma\approx 0.1$; centroids are
approximately independent random directions.

**Centroids are near-orthogonal in high $d$.** For independent random unit vectors,
$\mathbb{E}\langle c_j,c_k\rangle=0$ and $\operatorname{Var}\langle c_j,c_k\rangle\approx 1/d$,
so $\langle c_j,c_k\rangle=O(1/\sqrt{d})\approx 0.125$ at $d=64$.

**Intra- vs inter-category similarity separates.** With $\lVert c_k\rVert=1$ and
$\mathbb{E}\lVert\varepsilon\rVert^2=\sigma^2 d$, each normalizer is
$\lVert c_k+\varepsilon\rVert\approx\sqrt{1+\sigma^2 d}$, so in expectation

$$\operatorname{sim}_{\text{intra}}\approx\frac{\lVert c_k\rVert^2}{1+\sigma^2 d}=\frac{1}{1+\sigma^2 d}\approx 0.61,
\qquad
\operatorname{sim}_{\text{inter}}\approx\frac{\langle c_j,c_k\rangle}{1+\sigma^2 d}\approx 0\ (\pm 0.08),$$

at $\sigma=0.1,\ d=64$ (so $\sigma^2 d=0.64$). The gap ($\approx 0.61$ vs $\approx 0$) is why
a top-$k$ dot-product scan returns same-category items: recall is genuinely selective even
though the content is fabricated.

## 3. Recall — the top-$k$ scan and numerical reassociation

Recall scores every item and keeps the top $k$ ($=300$) by similarity — a full linear
scan, cost $O(nd)$. (An ANN index — HNSW — would make this sublinear; a stretch goal, see
the Future section at the end.)

The kernel is a floating-point reduction, and **FP addition is not associative**:
$(a\oplus b)\oplus c\neq a\oplus(b\oplus c)$ in general. The scalar path sums left-to-right
with one accumulator; the SIMD path keeps four lane accumulators (strided partial sums) and
combines them at the end — a different summation order, hence different rounding.

For a naive length-$d$ sum with summands $t_i=q_i x_i$, the standard forward error bound is

$$\bigl|\hat S-S\bigr|\ \le\ (d-1)\,u\sum_{i=1}^{d}\lvert t_i\rvert\ +\ O(u^2).$$

Both summation orders satisfy it; they differ *from each other* by $O(d\,u)$ — here
$\lesssim 64\cdot 6\times10^{-8}\approx 4\times10^{-6}$, and empirically the largest per-item
gap is $\approx 3\times10^{-7}$.

**Why the parity check reports diff $=0$.** The top-$k$ *set and order* are identical
because (i) score gaps between distinct items are $\gg 10^{-6}$, and (ii) exact ties break
deterministically by id. A $\sim10^{-7}$ perturbation therefore cannot reorder the ranking.
(Pairwise or Kahan summation would tighten the bound to $O(\log d\cdot u)$ or $O(u)$ —
unnecessary here.)

## 4. Features and scoring

FeatureOp attaches, per candidate: category affinity $a$ (the profile/persona weight for the
item's category), recency, and popularity $p\in[0,1]$. Recency is exponential decay of age
$t$ (days) with half-life $\tau=30$:

$$\operatorname{rec}(t)=e^{-t/\tau}\in(0,1].$$

ScoreOp is a transparent linear blend, then keeps the top $k'$ ($=50$):

$$s_i=w_{\text{sim}}\operatorname{sim}_i+w_{\text{cat}}\,a_i+w_{\text{rec}}\operatorname{rec}_i+w_{\text{pop}}\,p_i,
\qquad (w)=(1,\ 0.5,\ 0.3,\ 0.2).$$

There are no learned per-objective models (pCTR / pLike / pSave) — training is out of scope,
so the blend is linear and explicit about what it is.

## 5. Rerank — greedy MMR for diversity

RerankOp reorders the top scorers for category diversity via Maximal Marginal Relevance.
Build the page $S$ greedily (from $S=\varnothing$); at each step add

$$i^\star=\underset{i\notin S}{\arg\max}\ \Bigl[\,\lambda\,s_i-(1-\lambda)\,r_S(i)\,\Bigr],
\qquad r_S(i)=\bigl|\{\,j\in S:\operatorname{cat}(j)=\operatorname{cat}(i)\,\}\bigr|,$$

with $\lambda=0.7$. $\lambda=1$ is pure relevance; $\lambda=0$ is pure spread. Redundancy is
*category overlap* (not vector similarity) because the feed's salient monotony axis is
category. Greedy selection over a pool of size $m$ to fill a page of $P$ costs $O(mP)$.

## 6. Interest decay

On each refresh every tag weight is scaled, and fresh clicks enter at full weight between
refreshes:

$$w\ \leftarrow\ \gamma\,w,\qquad \gamma=0.5.$$

A tag not fed for $m$ refreshes decays as $\gamma^m w_0\to 0$ — geometric (exponential)
forgetting, equivalently an exponential moving average in *event* time. Chosen over
wall-clock $e^{-\lambda\,\Delta t}$ because in a click-driven demo little real time passes, so
event-stepped decay is what is actually observable. $\gamma$ is the plasticity knob: smaller
$\Rightarrow$ shorter memory.

## 7. Exploration vs. exploitation

The page of size $P$ ($=12$) splits into $P-\phi$ **exploit** slots (filled from the ranked
pool as a new/seen mix) and $\phi$ guaranteed **explore** slots ($\phi=2$) sampled uniformly
from *outside* the dominant category. This is a fixed-floor $\epsilon$-greedy with

$$\epsilon=\frac{\phi}{P}=\frac{2}{12}\approx 0.17.$$

Exploitation may dominate for a strong preference (correct behavior), but the floor
guarantees the page is never 100% one category — the filter-bubble guard. Explore items are
unranked by construction (they bypass Recall / Feature / Score).

## 8. Complexity and memory

| stage | time | notes |
|---|---|---|
| Recall scan | $O(nd)$ | the hot path; SIMD ≈ 4× constant-factor on the multiply–add |
| Top-$k$ after scan | $O(n\log n)$ | full sort; $O(n\log k)$ with a heap is a fair optimization |
| Rerank (MMR) | $O(mP)$ | pool $m$, page $P$; both small |
| Store (SoA) | $O(nd)$ bytes | one contiguous float32 buffer, $4nd=4\cdot 3000\cdot 64\approx 768\text{ KB}$ |

## Future (as the doc grows)

- **HNSW** — sublinear recall via greedy descent over a navigable small-world graph,
  expected $O(\log n)$ hops; replaces the §3 linear scan.
- **int8 quantization** — 4× smaller vectors; the dot product becomes integer plus a scale,
  trading a bounded quantization error for cheaper loads and bandwidth.
