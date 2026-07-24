# algo_ch.md — 引擎内部实现讲解（中文学习版）

`Doc/algo.md` 的中文对照版，方便阅读与面试复习。每一节都用大白话讲清楚：**这块做什么、
关键设计决策与为什么（WHY）、取舍、时间/空间复杂度、以及面试官可能追问的术语**。读完这份
应该能把代码"讲出来"。顺序大致是"基础在前"：先讲引擎内核，再讲建立在其上的行为特性。

> 术语一律保留英文（SoA / SIMD / MMR / cosine / HNSW / embind …）并附中文解释——因为面试里
> 这些词就是用英文问的。代码片段保持与源码一致（英文注释不译）。

---

## 物品存储 —— 结构体数组 Structure-of-Arrays (SoA)

### 做什么

物品存储把每个候选物品的 embedding 向量常驻在内存里，供召回扫描使用。向量以 **SoA
（结构体数组）** 布局存放：一块扁平的 `float` 缓冲区，按物品行主序把所有向量首尾相接
（`embeddings`），外加一个平行的逐物品元数据数组（`notes`）。第 `i` 个物品的向量就是
`embeddings[i*DIM …]` 处的 `DIM` 个 float；`vector_of(i)` 返回指向它的裸指针（不拷贝）。
`DIM = 64`。

### 为什么用 SoA 而不是 AoS（面试常问）

- **AoS**（array-of-structs，结构体的数组）会是 `vector<Item>`，每个 `Item` 自己持有一个
  向量——写法简单，但向量散落在堆的各处。
- **SoA** 把所有数字放进一整块连续内存。召回内核从头到尾把它当成一条数据流来扫，于是
  (a) **对 cache 友好**——每条 cache line 都装满有用的 float；(b) **对 SIMD 友好**——连续的
  lane 直接加载进向量寄存器（见 SIMD 一节）。
- 经验法则：**当一个热点循环需要在很多物品上横扫同一个字段时，用 SoA**（召回正是如此）；
  当你要一次访问单个物品的很多字段时，用 AoS。
- 取舍：SoA 不方便"把第 i 个物品当成一个对象拿出来"，所以元数据放在平行的 `notes` 数组里，
  用相同的下标索引。

### 复杂度

访问是 O(1) 的指针算术；内存占用为 `count · DIM · 4` 字节（DIM=64 时每个向量 256 B），
所以 3000 个物品的 demo 存储不到 1 MB。

### 面试可能追问的术语

Structure-of-Arrays vs. array-of-structs；cache 局部性 / cache line；数据布局如何让
向量化（vectorization）成为可能。

## 合成数据 —— 类目质心 + 噪声

### 做什么

这里没有训练，所以物品向量是**人工造出来的**，但造的方式要让召回仍然有意义。配方：每个
类目分一个随机 **centroid（质心）**；该类目下每个物品 = `质心 + 小高斯噪声`，再做
**单位归一化（unit-normalize）**。

### 为什么这样召回才有意义

同类目的物品会落在共享质心附近，因此在向量空间里**聚成一簇（cluster）**；一个靠近 food
质心的 query 自然会召回 food。内容是假的，但**几何结构**（也就是排序机制）是真的——这正是
一个"没有训练管线的 serving demo"的意义所在。

- **单位归一化**让点积（dot product）等于 **cosine similarity（余弦相似度）**，于是"相似"
  就等于"方向相同"，内核保持为一个纯点积。
- **固定的随机数种子（PRNG seed）**让存储可复现——这是 naive/SIMD 一致性校验可信的前提。

### 面试可能追问的术语

embedding 空间；centroid / cluster；cosine vs. dot product；为什么要归一化；确定性
fixture；train/serve（训练/服务）拆分（本项目是 serve 这一半）。

## 召回 Recall —— 基于向量相似度的候选生成

### 做什么

召回是**漏斗最宽的一级**：用很低的成本，把一个大候选池收成几百个"看起来靠谱"的候选。它对
**每一个**物品用 query 与物品向量的点积打分，然后保留 **top-k**。

### 设计决策

- **相似度度量：** 点积——因为向量已单位归一化，它**就是** cosine similarity。
- **Top-k：** 给全部 N 个打分，降序排序，取前 k；打平时按 id 升序。这个**确定性**的
  tie-break 很关键：naive 与 SIMD 两条路径求和顺序不同、结果可能差 ~1e-7，但按 id 打平让它们
  返回**完全相同**的排名。*暂时不做：* `partial_sort`/堆对小 k 更优，但在当前 N 下快不了多少，
  反而更难读。
- **全量线性扫描：** 每个物品都打分。生产环境里"别扫一百万个物品"的答案是
  **近似最近邻索引（ANN / HNSW）**——那是一个进阶目标（stretch goal），这里没做。

### 复杂度

`O(N·DIM)` 扫描（热点路径）+ `O(N log N)` 排序。

### 面试可能追问的术语

召回（candidate generation）vs. 排序（ranking）；cosine similarity；top-k；精确 vs. 近似最近邻
（ANN / HNSW / IVF）；为什么召回必须便宜。

## 算子 DAG —— 统一算子、调度器、trace

### 万物皆算子（everything is an operator）

每一级都实现同一个统一契约：**接收一个 batch，返回一个（通常更小的）batch，并记录一条
trace 条目。** 没有特判的"召回函数"或"排序函数"——只有接进图里的算子。正是这一点让流水线既
可以被**扩展**（加一个节点），又可以被**观测**（每个节点自我上报），而互不干扰。

- `Batch` 是 `Candidate` 的 **AoS（结构体数组）**（id + 逐步填充的特征/分数列）。这里用 AoS
  没问题——batch 很小、临时、且从不进入热点内核；SoA 那条规则只对存储适用。
- Tracing 用 **模板方法模式（template-method）**：基类 `run()` 计时、调用子类的
  `transform()`、再追加恰好一条 `TraceEntry` `{name, in, out, latency_us, sample_ids}`。
  **为什么集中在基类：** trace 是产品本身，所以它的形状必须对每一级都一模一样地产出、不能漂移。

### 级联漏斗（the cascade / funnel）

四个算子逐级收窄候选集（demo 下的基数）：

- **RecallOp** `3000 → 300` —— 相似度召回（见上）。
- **FeatureOp** `300 → 300` —— 给每个候选挂上排序特征：category match（画像/persona 的类目
  亲和度）、recency（物品年龄的指数衰减）、popularity（归一化热度）。只做丰富，不做过滤。
- **ScoreOp** `300 → 50` —— **加权多目标（multi-objective）**打分。生产排序器会融合学出来的
  各目标模型（pCTR / pLike / pSave）；这里没有训练模型，就把特征线性加权——对它是什么很诚实
  ——然后取 top-k。
- **RerankOp** `50 → 12` —— **多样性感知（diversity-aware）**的重排，用贪心 **MMR**
  （maximal marginal relevance，最大边际相关）：每次挑下一个物品时最大化
  `λ·score − (1−λ)·redundancy`，其中 redundancy 用类目重叠来衡量，避免一整页都是同一种东西。
  这就是缩微版的 **exploration/exploitation（探索/利用）**——已验证的相关性 vs. 多样性。

### DAG 调度器（scheduler）

调度器把算子当作节点持有并依次运行，把每一级的输出接进下一级、并收集有序的 trace。Shua Shua
的级联是一条**线性链**，所以这是一个**退化的 DAG**（只有一条路径）。一个通用 DAG 引擎
（拓扑排序、多输入合并）是**没有任何算子需要的投机式复杂度**，所以没做。之所以仍诚实地叫它
"DAG 调度器"，是因为统一的算子契约正是真正的 DAG 引擎所调度的东西。

### 面试可能追问的术语

算子/DAG 执行模型；拓扑序（topological order）；模板方法；多目标排序；MMR / 多样性；
探索 vs. 利用；把可观测性作为设计目标（observability by design）。

## SIMD 召回内核 + naive/SIMD 一致性校验

### 为什么要 SIMD、在哪用

热点路径是召回的点积——每个物品 `DIM` 次乘加。那个内层循环是向量化唯一真正划算的地方，所以
它在同一个签名背后有两套实现：

- `dot_scalar` —— 朴素的从左到右累加；**永久保留的参考实现（reference）**。即使 `-O2` 它也
  保持标量，因为不加 `-ffast-math` 的话 clang 不会自动向量化浮点归约（reassociation 会改变
  结果）——所以它是一个诚实的 baseline。
- `dot_simd` —— 手写 **NEON**（arm64）。一个 128-bit 寄存器装 **4 个 float32**，所以我们每次
  迭代处理 4 个元素、累加进 4 个 lane 累加器，最后做水平求和（horizontal sum）。**为什么是 4：**
  一个寄存器就装得下这么多 float。一个**标量尾循环（tail loop）**处理 `dim % 4`（DIM=64 时是
  空操作，但保证内核对任意维度都正确）。用 `__ARM_NEON` 守卫；其他平台（包括 WASM 构建）回退到
  标量内核。

### 一致性 / diff 校验（the parity check）

两个内核跑同样的输入、走同样的打分+排序，唯一的区别就是内核本身。引擎随后**断言 top-k 排名
完全一致（`diff = 0`）**，并报告最大分数差（~1e-7，来自不同求和顺序）与加速比（扫描约 3.6×；
端到端更低，因为共享的 top-k 排序没有被向量化）。

**为什么这点很重要（一个很强的面试点）：** 它复刻了真实 serving 系统迁移时的纪律——证明一个
优化**既更快、又对结果零改变**。永远保留 naive 路径作为 oracle（对照真值）是一个特性，不是
死代码。

### 面试可能追问的术语

SIMD / 向量化；NEON vs. AVX2；寄存器宽度 / lane；水平归约（horizontal reduction）；循环尾部
处理；浮点非结合性（为什么我们比较的是排名而不是 bit）；迁移中的 parity/diff 校验；Amdahl
定律（为什么端到端加速比 < 内核加速比）。

---

## Item-based recall —— "看更多类似的"

### 做什么

引擎默认的 `recommend(persona)` 从一个 persona 的类目质心混合里构造一个 **query 向量**，
再跑级联（Recall → Feature → Score → Rerank）。**Item-based recall**
（`recommend_similar(itemId)`）跑**同一条**流水线，但 query 换成**被点击物品自己的 embedding
向量**（`store.vector_of(id)`）。于是"为这个 persona 推荐"变成了"推荐 embedding 空间里离这个
物品最近的东西"。（v1 里点卡片会调用它、feed 当场切换；从 v2·B3 起，点击改为构建用户画像——见
下文——feed 改为按需重排；`recommend_similar` 仍留在引擎里，只是不再挂在点击上。）

### 它如何建模"用户点了 X → 展示与 X 相似的"

一次点击是**隐式正反馈（implicit positive feedback）**。一个简单而有力的响应就是
**item-to-item 召回**：把被点物品的表示当作 query，检索它的最近邻。这是 item-item 协同过滤
（"喜欢 X 的人也喜欢…"）的 content-based（基于内容）表亲，只不过这里的相似度是**直接从物品
embedding 算出来的**（向量相似度），而不是从共同互动里学的。它不需要用户历史、也不需要重训
模型——只要物品存储和同一个召回内核。

### 关键设计决策 —— 复用完全相同的流水线

`recommend()` 和 `recommend_similar()` 都委托给 `run_recommendation(query,
category_weights, label)`；**唯一**的区别是 query 从哪来（persona 的质心混合 vs. 物品自己的
向量）。

- **为什么：** 无论 query 来自哪里，排序逻辑都完全相同——"query"不过是 embedding 空间里的
  一个点。把它们统一成一条代码路径，防止漂移。*被否掉的方案：* 给 item-based recall 单独写一份
  实现——多余的重复，还多一处要维护正确性。
- **类目权重：** item-based recall 里我们把被点物品**自身类目**的权重设为 `1.0`（隐式的
  "你就喜欢这一类"信号），于是 `FeatureOp` 的 `category_match` 会奖励同类目物品。被点物品自己
  排第一（单位向量与自身的相似度是 `1.0`），然后是它的邻居。

### 确定性 —— 是特性，不是 bug

相同 query → 每次都相同结果。`recommend_similar(100)` 永远返回同一个 feed。

- **为什么在 serving 里这是想要的：** 可复现（A/B 测试和 debug 需要固定输入下的稳定输出）、
  可缓存、可解释。个性化与新鲜感来自**输入**的变化——不同的被点物品、persona 或上下文——而不是
  往排序器里注入随机性。一个 serving 引擎是 `(query, store)` 的纯函数；store 固定后，行为完全由
  query 决定。
- **本 demo 的数据：** 合成向量只编码了**类目**（每条 note = 类目质心 + 小噪声）。所以"与某个
  food 物品相似"返回的是其它 **food** 物品（同一簇）——feed 确实变了（不同物品、不同 trace
  `sample_ids`），但不会有"肉 vs. 沙拉"这类子类目结构，因为数据里根本没有。换成学出来的
  embedding，同一套机制就能浮现出真正的子主题邻域。

### 复杂度

与 persona 召回完全相同：

- **时间：** 召回扫描全部 `N` 个物品，每个是 `DIM` 维点积 → `O(N·DIM)`；再用全排序取 top-k →
  `O(N log N)`。本 demo（`N=3000`，`DIM=64`）原生运行远低于 1 毫秒。
- **空间：** 打分候选列表 `O(N)`。
- 亚线性召回（HNSW 这类近似最近邻索引）是大 `N` 下的生产答案，这里作为进阶目标。

### 面试可能追问的术语

- **Item-to-item / item-based 召回**；content-based vs. collaborative filtering。
- **Embedding / 向量相似度** —— cosine vs. dot product（这里向量已单位归一化，两者相等）。
- **隐式反馈（implicit feedback）**（点击）vs. 显式反馈（explicit，评分）。
- **确定性 serving** —— 为什么"固定输入 → 固定输出"是你想要的性质，以及新鲜感真正从哪来。
- **最近邻搜索**与 ANN 索引（HNSW）用于扩展召回。
- 统一的框架："query 只是 embedding 空间里的一个点"，这让 persona 召回和 item 召回成为同一个
  操作、只是 query 来源不同。

---

## 用户画像 & 隐式反馈 (v2 · B1)

### 是什么

v2 用一个**用户画像（user profile）**取代固定 persona——一个从用户行为构建的、持久化的"用户
喜欢什么"的小模型。这一块里它是三个字段（`profile.ts`）：

- `tagWeights` —— 每个兴趣 tag 一个权重（在 tag 空间里不断增长的"兴趣向量"）。
- `clickHistory` —— 被点物品的记录（tags + 时间戳），后面用于衰减和"已看集合"。
- `seenItemIds` —— 已展示/点击过的物品集合，用于"新/已看混合"。

后续 block 会把 `tagWeights` 变成一个 query 向量、跑同一条召回 DAG，所以画像**就是** query。
本 block 只引入模型与其持久化——feed 仍走 v1 路径。

### 隐式反馈

画像由**隐式反馈**长出来——从行为里推断的信号（一次点击），而不是用户刻意给出的**显式反馈**
（星标、点赞）。隐式反馈丰富得多，也是生产推荐系统主要依赖的东西，代价是更嘈杂、且只是弱正向
（点击不等于一定认可）。v1 的 persona 最接近显式输入（"我是个 Foodie"）；v2 的画像是隐式的
（"你老是点开美食帖子"）。

### 为什么用 local storage（客户端持久化）

画像存进浏览器的 `localStorage`、回访时重新加载，于是用户学到的兴趣能挺过一次刷新——**无需任何
账号、登录或后端**（一个刻意的范围选择；引擎保持在浏览器内 WASM）。

- **取舍：** 按设备、按浏览器（不跨设备同步），用户也能清掉——在这里可以接受，也如实说明了
  纯客户端持久化能买到什么。
- **健壮性：** 加载包在 try/catch 里。存储可能不存在、被禁用（隐私模式）、写满、或存了损坏的
  JSON，读一个被屏蔽的 key 还可能抛异常；任何失败都回退到一个**中性画像（neutral profile）**
  而不是崩溃。

### tag → 类目映射

八个冷启动 tag 折叠到引擎的六个物品类目上，集中在一处（`TAG_TO_CATEGORY`）。几个 tag 会共享
一个类目（如 News → tech，Outdoors → travel），因为合成数据只有六个类目质心——这是一个**有据可查
的近似**，不是建模主张。

### 中性画像（冷启动 → 预热）

没有历史时画像是**中性的**：每个 tag 等权重。后面变成 query 时，它给出的是一个多样化的采样
feed 而不是空 feed；头几次点击再把它特化。这就是**冷启动（cold-start）**问题及其最简单合理的
答案：从宽泛开始，用证据收窄。

### 面试可能追问的术语

用户画像 / 兴趣向量；隐式 vs. 显式反馈；冷启动问题；客户端持久化及其取舍；"画像即 query"的
框架——它把行为与召回这一步连起来。

---

## 冷启动 & 标签选择器 (v2 · B2)

### 做什么

首次访问（存储里没有 onboarded 画像）时，应用展示一个单屏的**标签选择器**——八个兴趣 tag
做成可切换的 chip。选择是**可选的**："Continue" 用所选 tag 给画像播种；"Skip" 播一个中性的。
无论哪种，画像都被标记为 `onboarded` 并持久化，于是这个浏览器上选择器再不出现。

### 冷启动问题

一个**没有历史**的推荐系统无法个性化——这就是**冷启动问题**。真实系统用 onboarding
（问几个兴趣）、popularity prior（热度先验）或上下文信号来对付它。这里 onboarding 给画像第一个
信号：

- **已播种（选了 tag）：** 那些 tag 有权重、其余为零——一旦画像开始驱动召回（B5），feed 就偏向
  它们。
- **中性回退（跳过）：** 每个 tag 等权重 → 一个**多样化采样**的 feed 而不是空 feed。头几次点击
  再特化它（B3）。这就是**冷启动 → 预热**，被显式地展示出来。

### 为什么 `onboarded` 是一个存储的标志位

画像从 B1 起就持久化，所以"我们 onboard 过了吗？"不能靠"存储是不是空的"来推断。一个显式的
`onboarded` 布尔（默认 false——包括任何 B2 之前存下的画像）干净地把守选择器、并挺过刷新。

### 范围说明

B2 给画像播种并展示它；feed 仍走 v1 persona 路径。把画像向量接进召回是 B5，所以 onboarding
现在体现在画像读数上、之后才驱动 feed——刻意分步。

### 面试可能追问的术语

冷启动问题；onboarding / 兴趣征询（interest elicitation）；popularity prior；显式 onboarding
信号 vs. 隐式点击信号；冷启动 → 预热。

---

## 实时画像 + 隐式反馈累积 (v2 · B3)

### 做什么

点击一张卡片现在是**隐式反馈**：它给被点物品的 tag 加权、追加到点击历史、并把物品标记为已看
——全部**实时**发生。侧边栏的实时**画像面板**立即重渲染，tag 条随点击增长（并重新排序）。关键
在于：**feed 不动。**

### 实时画像 vs. 按需 feed（关键决策）

两个相互独立的更新频率：

- **画像每次点击都更新**——即时、清晰的反馈（"我的操作被记下了"），条子肉眼可见地长。
- **feed 只在按需时重排**（"Refresh recommendations" 按钮，B6），点击时绝不动。

**为什么拆开：** 每次点击都跳 feed 会摧毁因果感（你看不出哪次点击改了什么）、而且很跳；而在
手动刷新前冻结一切又会丢掉"它在学习"这个信号。拆开两者兼得——实时的画像增长 **和** 一次刻意的
"揭晓"。它也复刻了生产系统：行为**实时记录**，但推荐**批量重算**。

### 不可变性（immutability）

`recordClick` 返回一个**新的**画像对象（拷贝权重、历史、已看集合）而不是原地修改，于是 React
看到引用变了、重渲染面板；改动也被持久化（B1 的保存 effect）。

### tag→类目 的耦合

一个物品只带一个类目，而几个 tag 折叠到同一个类目，所以点击（比如）一个 tech 物品会给映射到
tech 的每个 tag 都加权（Tech **和** News）——这是粗粒度合成类目空间的后果，不是建模选择。

### 面试可能追问的术语

隐式反馈 / 行为信号；实时记录 vs. 批量重算；在线 vs. 批量更新；为什么点击即刻改 feed 会损害
可读性；UI 里的不可变状态更新。

---

## 兴趣衰减 (v2 · B4)

### 做什么

兴趣会褪色，好让画像能漂移向用户**当下**关心的东西。`decayProfile` 在每次**刷新**时把每个 tag
权重乘以 `DECAY_FACTOR`（0.7）。因为新点击以满权重进入（B3）、而旧权重不断被乘小，你不再喂的
tag 会缩水、近期点击逐渐主导——"近期点击比旧的更重"。

### 半衰期 vs. 每次刷新（这个决策）

- **基于时间的半衰期** —— `weight = base · exp(-λ·Δt)`：一次点击的影响随物理时间连续衰减。更
  "真实"的模型。
- **每次刷新的乘法衰减** —— 每次刷新 `weight *= factor`：基于事件，衰减在用户动作时发生。

我们选**每次刷新**。在一个点击驱动的 demo 里几乎没有物理时间流逝，所以基于时间的衰减看起来会
像什么都没褪色——恰恰在我们想展示它的地方隐形了。基于事件的衰减把褪色绑在用户动作上、保持因果
可读，而且只有一个可解释的参数、没有 λ 要调（§9：别过度设计衰减）。

### 为什么效果是"近因性"而不是"条子缩短"

一个统一的乘法把每个权重等比缩放，所以它本身不改变**相对**条长。可见的效果来自**不对称性**：
刷新衰减旧权重，而新点击以满强度进入。点主题 A，然后一边刷新一边点主题 B，即使点击次数相同 B
也会反超 A——那个反超就是衰减被看见的样子。

### 兜底（§6）

如果每个权重都衰减到 ~0（多次刷新、无点击），`decayProfile` 回退到中性画像，于是由它构造的召回
query（B5）永远不会是零/NaN 向量。

### 触发说明

本 block 里的"刷新"事件是切换 persona（当前唯一的 feed 重跑）；B6 把衰减触发移到专门的
"Refresh recommendations" 按钮上。

### 面试可能追问的术语

兴趣衰减 / 近因性（recency）；指数半衰期 vs. 基于事件的衰减；为什么近因重要；单参数的简洁；
防止退化（零）画像向量。

---

## 画像向量 = 召回 query (v2 · B5)

这里画像不再只是侧边栏的装饰，而成了驱动召回的东西。整个 v2 的论点一句话：**推荐 = 把画像
向量当作召回 query，然后跑现有的 DAG。**

### 一串翻译（the pipeline of translations）

```
tagWeights (8)  ──►  categoryWeights (6)  ──►  profile vector (DIM=64)  ──►  RecallOp query
   profile.ts           profile.ts                   api.hpp                  (DAG 不变)
```

三次刻意的跳转，每一次都发生在拥有那份数据的语言里：

**1. tags → categories（TypeScript）。** 画像存 8 个 tag 权重；引擎只认 6 个物品类目。
`categoryWeights` 通过唯一的 `TAG_TO_CATEGORY` 映射把前者折叠成后者——这是全应用**唯一**的
tag→类目翻译：

```ts
// web/src/profile.ts — categoryWeights()
const w = new Array<number>(CATEGORY_ORDER.length).fill(0);
for (const tag of TAGS) {
  const idx = (CATEGORY_ORDER as readonly string[]).indexOf(TAG_TO_CATEGORY[tag]);
  if (idx >= 0) w[idx] += profile.tagWeights[tag] ?? 0;
}
return w;
```

`CATEGORY_ORDER` = `["food","fashion","travel","tech","fitness","beauty"]`，钉死成与 C++ 的
`CATEGORY_NAMES`（`src/api.hpp`）一致——这是两种语言**必须**在顺序上达成一致的那一处，因为
C++ 那边是按位置索引质心的。

**2. weights → vector（C++）。** 向量空间的数学完全放在 C++ 里，这样它不会与 persona 路径
漂移。`recommend_from_profile` 复用 persona 用的那个 `make_query`——query 就是
`normalize(Σ wᶜ · centroidᶜ)`：

```cpp
// src/api.hpp — make_query()（persona 与画像共用）
for (std::size_t c = 0; c < category_weights.size(); ++c)
  for (std::size_t d = 0; d < ItemStore::DIM; ++d)
    query[d] += category_weights[c] * centroids[c][d];
normalize(query.data(), ItemStore::DIM);
```

```cpp
// src/api.hpp — v2 入口
inline Recommendation recommend_from_profile(std::vector<float> category_weights) {
  // ... §6 兜底：尺寸不对或全零权重 → 均匀（中性）混合 ...
  return run_recommendation(make_query(category_weights, shared_data().centroids),
                            category_weights, "For you");
}
```

所以 `recommend_from_profile` 与 `recommend`（persona）只有一处不同——权重从哪来。下游的一切
（`run_recommendation` 以及 Recall→Feature→Score→Rerank DAG）都是同一份代码。

**3. 边界（embind）。** JS 把这 6 个权重当作一个 CSV 字符串递过去——对一个固定的、极小的
float 向量来说，这是穿越 embind 边界最简单也最稳的方式（不用 `register_vector`、也不用手动
`.delete()`）：

```cpp
// src/bindings.cpp
static std::string recommend_from_profile_json(const std::string& weights_csv) {
  // 按 ',' 切分、对每个字段 std::stof，然后 recommend_from_profile(...)
}
emscripten::function("recommendFromProfile", &recommend_from_profile_json);
```

```ts
// web/src/engine.ts
export async function recommendFromProfile(categoryWeights: number[]) {
  const engine = await loadEngine();
  return JSON.parse(engine.recommendFromProfile(categoryWeights.join(","))) as Recommendation;
}
```

### feed 什么时候跑（什么时候不跑）

`App.runFeed(profile)` 调 `recommendFromProfile(categoryWeights(profile))`。它是一个普通函数、
**不是**一个以画像为依赖的 effect，因为 feed 必须只在显式事件时重跑：

- **挂载时**，对一个回访的、已 onboard 的用户（`useEffect([], …)`）；
- **onboarding 完成时**，对一个新用户（`finishOnboarding` → 播种 → `runFeed`）；
- **刷新按钮** —— 在 B6 加入。

它**绝不**因点击而重跑——这正是 B3 的"实时画像 / 按需 feed"拆分，所以 `handleCardClick` 只做
`setProfile(recordClick(...))`。

### 这一块删掉了什么

v1 的 persona 切换器没了：feed 现在是画像的了，所以固定选择器不再契合这个模型。`personas()`
仍留在 `api.hpp` 里（`recommend` / `recommendSimilar` 也仍然绑定着），只是 UI 不再调用它们。
B4 的衰减触发挂在 persona 切换上，所以这一块衰减是休眠的、到 B6 才拿到它真正的触发——刷新按钮。

### 重新构建步骤

因为这一块改了 C++，WASM 必须重建：`scripts/build-wasm.sh` → `web/public/shuashua.js`
（单文件、内嵌 wasm、通过 `<script>` 标签加载）。过期的 WASM 会抛 "recommendFromProfile is
not a function"。

### 面试可能追问的术语

query/embedding 向量；把用户画像看成物品空间里的一个点；质心混合；把向量数学放在一处以避免
serving 偏移；FFI 边界（embind）与 marshalling；为什么推荐能归约成"画像 → query → DAG"。

---

## 刷新 + 新/已看混合 —— 探索/利用 (v2 · B6)

### 刷新按钮

feed 只在用户按下 **"Refresh recommendations"** 时重排——这是 B3"实时画像 / 按需 feed"拆分
里"按需"的那一半。`App.handleRefresh` 做两件事：

```ts
// web/src/App.tsx
const handleRefresh = () => {
  const decayed = decayProfile(profile);  // B4 的衰减现在在这里触发
  setProfile(decayed);
  runFeed(decayed);                        // 对老化后的画像重排
};
```

一次刷新既**老化**画像（你不再喂的兴趣褪色——B4 的触发，自 B5 起休眠、现在落在这里），又
**重算** feed。这就是"批量重算"；刷新之间的点击只增长画像。

### 混合要解决的问题

如果刷新只是对画像重跑召回，它会返回**用户已经点过的那些物品**——它们打分最高恰恰**因为**被
点过（它们匹配画像）。feed 就永远走不动了。真实系统用 **探索/利用（exploration/exploitation）**
的取舍来避免这点：大部分展示新东西（探索），保留几个已验证的偏好（利用）。

### MixOp —— 被保证的配额

`MixOp`（`src/mix_op.hpp`）是一个新的末级算子。它把已排序的候选池切成**新**（id ∉ seen）与
**已看**（id ∈ seen），然后主要用新的来填页面、并留一个由 `new_ratio`（默认 80 → 12 里约 2–3
个已看）设定的小额已看配额：

```cpp
// src/mix_op.hpp — MixOp::transform（核心）
std::size_t new_target = llround(page_size_ * new_ratio_ / 100.0);
std::size_t take_new  = std::min(new_target, new_items.size());
std::size_t take_seen = std::min(page_size_ - new_target, seen_items.size());
// ... 任一池不够时从另一池回填（优先补新的）...
// 先发新的，再发预留的已看
```

**为什么单独一个算子、且放在最后**（`src/api.hpp` 的 `run_recommendation`）：RerankOp 的职责是
类目**多样性**（MMR）；MixOp 的职责是**曝光近因**平衡——各司一职。它跑在**最后**，这样已看配额
才被保证；若"先混合再重排"，RerankOp 的多样性裁剪可能把预留的已看丢掉。画像路径因此长出第五级
——但只在有已看集合可混时才长：

```cpp
// src/api.hpp — run_recommendation
if (!seen_ids.empty() && new_ratio < 100) {
  pipeline.add(std::make_unique<RerankOp>(data.store, kRerankPool, kRerankLambda)); // 50 -> 24 多样池
  pipeline.add(std::make_unique<MixOp>(std::move(seen_ids), kPageSize, new_ratio)); // 24 -> 12 新/已看
} else {
  pipeline.add(std::make_unique<RerankOp>(data.store, kPageSize, kRerankLambda));   // 50 -> 12
}
```

于是 DAG trace 本身就显出差别：首个、没点过任何东西的 feed 是 **4 个算子**；点击后的一次刷新是
**5 个**——MixOp 出现，把探索/利用这一步变得可观测（trace 就是产品）。

### 边界

已看集合以与权重相同的 CSV 方式穿到 C++——
`recommendFromProfile(categoryWeights.join(","), [...seenItemIds].join(","), NEW_RATIO)`
（`web/src/engine.ts`）——在 `bindings.cpp` 里解析回一个 `std::vector<std::uint32_t>`。

### 面试可能追问的术语

探索 vs. 利用；陈旧 / "信息茧房（filter bubble）"这个失败模式；为什么已看物品打分高、必须被
限额；预留配额的页面拼装；单一职责算子；批量重算 vs. 实时信号。

---

## 可观测性：trace 面板 + 为什么延迟显示为 0 (v2 · B7)

DAG trace 就是产品——本项目的意义就在于流水线是**可见的**。B7 把这个故事收尾：展示是什么驱动了
每一次运行、让一次重算显而易见、并让延迟变真实。

### 展示驱动本次运行的画像

trace 现在带一个 "driven by …" 标签——产出本次 feed 的那个画像的摘要。它是在 feed 运行**当时**
抓拍的，不是实时读的：

```ts
// web/src/App.tsx — runFeed(p)
setDrivenBy(summarizeProfile(p));  // 抓拍驱动了"本次"feed 的那个画像
```

**为什么抓拍：** 点击会即刻改变实时画像（B3），但 feed 只在刷新时重排（B6）。如果标签读的是实时
画像，它就会与屏幕上的 feed 失步——宣称 feed 来自一个还没产出它的画像。在 `runFeed` 时抓拍让
标签保持诚实。

### 让一次重算可见

`TracePanel` 用 `flowKey`（算子名 + out 计数）给那一行 stage 做 key，于是任何变化——新的计数、
或 MixOp 出现——都会重挂载那一行、重放交错的显现动画；每个 stage 还会短暂闪一下边框强调色
（`stage-flash`）。按下 Refresh 会在漏斗上产生一道可见的涟漪，而流水线**长出**一个 MixOp
级（B6）是你能亲眼看着发生的。

### 为什么延迟显示为 0（以及修复）

算子用浏览器的高精度时钟给自己计时。浏览器出于 Spectre 缓解会把那个时钟**粗化到 ~0**——**除非**
页面是**跨源隔离（cross-origin isolated）**的，这需要两个响应头：

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`web/vite.config.ts` 在 dev server 和 preview 上都发这两个头，所以本地 trace 显示真实的微秒
（RecallOp ~380µs）而不是 0.0µs——正是这一点让"C++ 很快"的故事根本可见。静态生产宿主必须发同样
的两个头；如果它做不到，`TracePanel` 会检查 `crossOriginIsolated` 并打印一行提示，于是一个
0.0µs 读起来是"计时器被夹住了"、而不是"瞬间完成"。

### 面试可能追问的术语

可观测性 / 把 tracing 作为一等输出；`performance.now()` 计时器粗化与 Spectre 缓解；跨源隔离
（COOP/COEP）；让 UI 暴露"测量降级"状态而不是显示一个误导性的零；把一次 UI 动作与一次重算
绑起来。
