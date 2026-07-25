# Algorithm — 引擎背后的数学（中文学习版）

`Doc/en/Algorithm.md` 的中文对照版：每个算法配上记号、关键推导与复杂度。直觉、设计 WHY、面试
话术在 **[Interview](Interview_ch.md)**——本文刻意*不*重复那些，只做"公式优先"。这是一份**成长型**
文档：stretch 主题（HNSW、量化）的数学落地时补进来。

> 数学在 GitHub 上会被渲染（MathJax）：行内如 $\langle q,x\rangle$，下面是块级公式。纯文本编辑器里
> `$…$` 显示为源码。术语保留英文。

## 记号

| 符号 | 含义 |
|---|---|
| $n$ | store 里的物品数（$=3000$） |
| $d$ | embedding 维度（$=64$） |
| $x_i\in\mathbb{R}^d$ | 物品 $i$ 的 embedding，单位归一化（$\lVert x_i\rVert_2=1$） |
| $q\in\mathbb{R}^d$ | query 向量，单位归一化 |
| $K$ | 类目数（$=6$）；$c_k\in\mathbb{R}^d$ 为类目 $k$ 的单位质心 |
| $u$ | float32 的 unit roundoff，$2^{-24}\approx 5.96\times10^{-8}$ |

## 1. 相似度 —— 余弦即点积

余弦相似度为

$$\cos\angle(q,x)=\frac{\langle q,x\rangle}{\lVert q\rVert\,\lVert x\rVert}.$$

每个存储向量与每个 query 都单位归一化，于是 $\lVert q\rVert=\lVert x\rVert=1$，内核塌缩成一个纯内积：

$$\operatorname{sim}(q,x)=\langle q,x\rangle=\sum_{i=1}^{d} q_i\,x_i\ \in[-1,1].$$

归一化让"相似"等于"同方向"（夹角小）、与模长无关，同时把热点路径保持为一条乘加循环。

## 2. 为什么召回有意义 —— 合成数据的几何

类目 $k$ 中的物品 $i$ 是 $x_i=\operatorname{normalize}(c_k+\varepsilon_i)$，其中
$\varepsilon_i\sim\mathcal{N}(0,\sigma^2 I_d)$，$\sigma\approx 0.1$；质心近似为相互独立的随机方向。

**高维下质心近正交。** 对独立随机单位向量，$\mathbb{E}\langle c_j,c_k\rangle=0$、
$\operatorname{Var}\langle c_j,c_k\rangle\approx 1/d$，所以 $\langle c_j,c_k\rangle=O(1/\sqrt{d})\approx 0.125$（$d=64$）。

**类内 vs 类间相似度被拉开。** 由 $\lVert c_k\rVert=1$、$\mathbb{E}\lVert\varepsilon\rVert^2=\sigma^2 d$，
每个归一化因子约为 $\lVert c_k+\varepsilon\rVert\approx\sqrt{1+\sigma^2 d}$，于是期望上

$$\operatorname{sim}_{\text{intra}}\approx\frac{\lVert c_k\rVert^2}{1+\sigma^2 d}=\frac{1}{1+\sigma^2 d}\approx 0.61,
\qquad
\operatorname{sim}_{\text{inter}}\approx\frac{\langle c_j,c_k\rangle}{1+\sigma^2 d}\approx 0\ (\pm 0.08),$$

此处 $\sigma=0.1,\ d=64$（即 $\sigma^2 d=0.64$）。这道缝隙（$\approx 0.61$ vs $\approx 0$）正是为什么
一次 top-$k$ 点积扫描会返回同类目物品：即便内容是造的，召回也真的有区分度。

## 3. 召回 —— top-$k$ 扫描与数值重结合

召回给每个物品打分、按相似度保留 top $k$（$=300$）——一次全量线性扫描，代价 $O(nd)$。（用 ANN
索引 HNSW 可做到亚线性；stretch 目标，见文末 Future 一节。）

内核是一次浮点归约，而**浮点加法不满足结合律**：一般地 $(a\oplus b)\oplus c\neq a\oplus(b\oplus c)$。
标量路径用单个累加器从左到右求和；SIMD 路径保留四个 lane 累加器（跨步的部分和）、最后再合并——
求和顺序不同，因此舍入不同。

对一个长度为 $d$、加数 $t_i=q_i x_i$ 的朴素求和，标准前向误差界为

$$\bigl|\hat S-S\bigr|\ \le\ (d-1)\,u\sum_{i=1}^{d}\lvert t_i\rvert\ +\ O(u^2).$$

两种顺序都满足它；它们*彼此*之间相差 $O(d\,u)$——此处
$\lesssim 64\cdot 6\times10^{-8}\approx 4\times10^{-6}$，而实测最大的逐物品差为 $\approx 3\times10^{-7}$。

**为什么奇偶校验报告 diff $=0$。** top-$k$ 的*集合与顺序*完全一致，因为 (i) 不同物品之间的分数差
$\gg 10^{-6}$，且 (ii) 精确并列按 id 确定性打破。所以 $\sim10^{-7}$ 的扰动无法改变排名。（pairwise
或 Kahan 求和能把界收紧到 $O(\log d\cdot u)$ 或 $O(u)$——这里不需要。）

## 4. 特征与打分

FeatureOp 给每个候选附加：类目亲和度 $a$（画像对该物品类目的权重）、新鲜度、热度
$p\in[0,1]$。新鲜度是年龄 $t$（天）的指数衰减，半衰期 $\tau=30$：

$$\operatorname{rec}(t)=e^{-t/\tau}\in(0,1].$$

ScoreOp 是一个透明的线性融合，然后保留 top $k'$（$=50$）：

$$s_i=w_{\text{sim}}\operatorname{sim}_i+w_{\text{cat}}\,a_i+w_{\text{rec}}\operatorname{rec}_i+w_{\text{pop}}\,p_i,
\qquad (w)=(1,\ 0.5,\ 0.3,\ 0.2).$$

没有学习到的多目标模型（pCTR / pLike / pSave）——训练不在范围内，所以融合是线性的、对自己是什么很诚实。

## 5. 重排 —— 贪心 MMR 做多样性

RerankOp 用 Maximal Marginal Relevance 把高分候选按类目多样性重排。从 $S=\varnothing$ 贪心地建页；
每步加入

$$i^\star=\underset{i\notin S}{\arg\max}\ \Bigl[\,\lambda\,s_i-(1-\lambda)\,r_S(i)\,\Bigr],
\qquad r_S(i)=\bigl|\{\,j\in S:\operatorname{cat}(j)=\operatorname{cat}(i)\,\}\bigr|,$$

$\lambda=0.7$。$\lambda=1$ 纯相关；$\lambda=0$ 纯打散。冗余度用*类目重叠*（而非向量相似度），因为 feed
最显眼的单调轴就是类目。在大小为 $m$ 的池上贪心填满 $P$ 张的页，代价 $O(mP)$。

## 6. 兴趣衰减

每次刷新，所有 tag 权重被缩放，两次刷新之间的新点击以全权重进入：

$$w\ \leftarrow\ \gamma\,w,\qquad \gamma=0.5.$$

一个连续 $m$ 次刷新没被喂的 tag 衰减为 $\gamma^m w_0\to 0$——几何（指数）遗忘，等价于*事件*时间上的
指数移动平均。选它而非墙钟时间的 $e^{-\lambda\,\Delta t}$，是因为点击驱动的 demo 里几乎不流逝真实
时间，按事件步进的衰减才看得见。$\gamma$ 是可塑性旋钮：越小 $\Rightarrow$ 记忆越短。

## 7. 探索 vs. 利用

大小为 $P$（$=12$）的页拆成 $P-\phi$ 个 **利用（exploit）** 位（从已排序池按 new/seen 混合填充）与
$\phi$ 个被保证的 **探索（explore）** 位（$\phi=2$，从主导类目*之外*均匀采样）。这是一个固定下限的
$\epsilon$-greedy，

$$\epsilon=\frac{\phi}{P}=\frac{2}{12}\approx 0.17.$$

对强偏好来说利用可以占主导（这是正确行为），但这个下限保证页面永远不会 100% 单一类目——过滤气泡的
兜底。探索位天然未排序（绕过 Recall / Feature / Score）。

## 8. 复杂度与内存

| 阶段 | 时间 | 说明 |
|---|---|---|
| 召回扫描 | $O(nd)$ | 热点路径；SIMD 在乘加上约 4× 常数因子 |
| 扫描后 top-$k$ | $O(n\log n)$ | 全排序；用堆做到 $O(n\log k)$ 是合理优化 |
| 重排（MMR） | $O(mP)$ | 池 $m$、页 $P$，都很小 |
| 存储（SoA） | $O(nd)$ 字节 | 一整块连续 float32 buffer，$4nd=4\cdot 3000\cdot 64\approx 768\text{ KB}$ |

## Future（成长中的文档）

- **HNSW** —— 亚线性召回：在可导航小世界图上贪心下降，期望 $O(\log n)$ 跳；替代 §3 的线性扫描。
- **int8 量化** —— 向量小 4×；点积变成整数加一个 scale，用一个有界的量化误差换取更便宜的加载与带宽。
