# 03 · 核心算法逐个拆

> 行号对应 commit `46b9ec3`。所有性能数字是本次实测（方法见 §11）。
> 指不到代码或测量的地方一律标【待验证】。

**目录**

| § | 组件 | 一句话 |
|---|---|---|
| 1 | `ItemStore`（SoA 布局） | embedding 与元数据分离的常驻候选库 |
| 2 | `dot_scalar` / `dot_simd` | 全引擎唯一的 hot kernel |
| 3 | `Operator`（统一接口） | template method：算子只写 transform，trace 由基类统一产出 |
| 4 | `DagScheduler` | 顺序执行链（**不是拓扑排序**） |
| 5 | `RecallOp` | 召回：source node，全量扫描 + top-k |
| 6 | `FeatureOp` | 特征挂载：不改变基数 |
| 7 | `ScoreOp` | 线性多目标打分 + 截断 |
| 8 | `RerankOp` | greedy MMR 多样性重排 |
| 9 | `MixOp` | new/seen 配比 + 探索保底 |
| 10 | `decayProfile` | 兴趣衰减（在 TypeScript 里） |

---

## 1. `ItemStore` — SoA 候选库

**解决什么问题**：让相似度 kernel 能连续流式读取 item 向量，同时不把元数据拖进 cache。

**输入/输出**：不是函数，是数据结构。

```cpp
struct ItemStore {
    static constexpr std::size_t DIM = 64;
    std::vector<float> embeddings;  // size == count() * DIM，item-major
    std::vector<Note>  notes;       // 平行数组，同一个下标
    std::size_t count() const { return embeddings.size() / DIM; }
    const float* vector_of(std::uint32_t index) const {
        return embeddings.data() + static_cast<std::size_t>(index) * DIM;
    }
};
```

**代码位置**：`src/item_store.hpp:20-49`；数据由 `src/synthetic.hpp:59-119` 构建。

**内部流程**：
- `embeddings` 是**一个**扁平 buffer，item i 的向量在 `[i*DIM, i*DIM+DIM)`（`:18, :46-48`）。
- `notes` 是平行数组，`Note` 只有 `id / category / popularity / age_days`（`note.hpp:15-26`），**embedding 不在里面**。
- `count()` 从 embedding buffer 长度推导（`:33-35`），注释说明理由：让"有多少向量"只有一个真相来源。
- `vector_of` 返回裸指针，零拷贝（`:46-48`）。

**规模**：3,000 items × 64 dim × 4 B = **768,000 字节 ≈ 750 KB** embedding，
外加 3,000 × `sizeof(Note)`（16 B）= 48 KB 元数据。**整个 store 不到 1 MB**，
稳稳落在 L2 里（M 系列 L2 通常 ≥ 4 MB）—— 这一点很重要，**意味着这里根本没有
访存瓶颈，所以 SIMD 的收益是纯计算收益，不是 cache 收益**。

**为什么这么设计 / 备选方案**：

| 方案 | 为什么没选 |
|---|---|
| AoS：`struct Item { float vec[64]; uint32 id; float pop; ... }` | 扫描时每个 cache line 都夹带元数据，有效带宽下降；`CLAUDE.md` 明令禁止 |
| dim-major SoA：`embeddings[d*N + i]` | **这才是能跨 item 向量化的布局**，但当前 kernel 是跨 dim 的，改布局必须同时重写 kernel。没做 |
| 每 item 一个 `std::vector<float>` | 3,000 次独立分配，指针追逐，完全放弃连续性 |

⚠️ **术语精确性（面试必须注意）**：代码叫它 "Structure-of-Arrays"，这在
**字段层面成立**（embedding 与 metadata 分成两个平行数组）。但 buffer **内部**
是 item-major（`:18` 白纸黑字 "row-major by item"）。在向量检索语境里，
"SoA" 常被默认为 dim-major。**别让面试官误以为你做了跨 item 向量化。**
安全说法见 §2 的追问 Q2。

**对应工业界**：在线特征/embedding 存储。真实系统里是分布式 embedding table
（参数服务器）或列式 feature store，带版本管理、增量更新、TTL、多副本。
**差距**：这里是单机只读、进程内、静态、一次构建永不更新——没有写路径、
没有一致性问题、没有淘汰策略，而那些恰恰是工业界的全部难点。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「你这个 SoA 到底 SoA 在哪？」 | 老实拆两层：字段层面 embedding/metadata 分离（真做了，收益是扫描不拖元数据）；buffer 内部是 item-major，不是 dim-major。**主动说出后者**，比被戳穿好 |
| Q2「750KB 全在 L2，SIMD 的收益是什么？」 | 正是因为不受访存限制，收益才是纯 ALU 吞吐 + 依赖链缩短（见 §2）。如果 store 是 10 GB，瓶颈会变成访存，那时该做的是量化（int8）而不是加宽 SIMD |
| Q3「item 增删怎么办？」 | 现在**做不了**——`count()` 由 buffer 长度推导，且 `synthetic.hpp:104-106` 靠 "id == 插入下标" 省掉了 id→index 映射。要支持删除得引入 tombstone + 定期 compaction + id 映射表。这是个诚实的"没做"，不要硬答 |

---

## 2. `dot_scalar` / `dot_simd` — 唯一的 hot kernel

**解决什么问题**：算 query 与 item 向量的内积（因为两边都是单位向量，内积即 cosine 相似度）。

**输入/输出**：`(const float* a, const float* b, size_t dim) → float`。

**代码位置**：`src/dot.hpp:31-37`（scalar）、`src/dot.hpp:48-91`（NEON）。

**内部流程（SIMD 路径，反汇编核对过）**：

```cpp
float32x4_t acc = vdupq_n_f32(0.0f);          // dot.hpp:51  一个累加器
for (d = 0; d + 4 <= dim; d += 4) {           // dot.hpp:57  16 次迭代
    const float32x4_t va = vld1q_f32(a + d);  // dot.hpp:61
    const float32x4_t vb = vld1q_f32(b + d);  // dot.hpp:62
    acc = vmlaq_f32(acc, va, vb);             // dot.hpp:68  lane-wise MAC
}
float sum = vgetq_lane_f32(acc, 0) + ... ;    // dot.hpp:74-75  horizontal reduction
```

**向量化的轴是 dim，不是 item**：循环变量 `d` 走维度，`b` 是**单个** item 的向量。
所以每个 item 结束都要做一次 horizontal reduction —— 一次召回付 3,000 次。

**实测反汇编（`clang++ -O2`，arm64）**：

| | 指令构成 | 依赖链 |
|---|---|---|
| `dot_scalar` | 4 × `fmul.4s` + **64 × 串行 `fadd`** | **64 长** |
| `dot_simd` | 16 × `fmla.4s` + 3 × `dup.4s` + 3 × `fadd.4s` | **16 长** |

**两个反直觉但重要的事实**：

1. **`dot_scalar` 在 -O2 下不是全标量**——clang 把**乘法向量化了**（`fmul.4s`），
   只把**加法**留成串行（没有 `-ffast-math` 不许重排求和顺序）。
   `dot.hpp:26-30` 的注释说 "this compiles to scalar adds"，字面准确，
   但容易被读成"全标量"。
2. **于是 speedup 的真正来源是「归约被向量化」，不是「乘法被向量化」**——
   两边的乘法都是向量的。依赖链 64 → 16，理论上限 4×，实测 ~3× 。
   **这是能在白板上讲清楚、且有反汇编支撑的解释。**

**复杂度**：O(dim)。实际 dim=64 → 每次 64 次乘加；一次召回 3,000 次调用 =
**192,000 次乘加**，NEON 下是 **48,000 条 `fmla.4s`** + 3,000 次 horizontal reduction。

**为什么这么设计 / 备选方案**：

| 方案 | 为什么没选 / 代价 |
|---|---|
| 跨 item 向量化（4 个 item 同时算） | 需要 dim-major 布局，且**完全消除** horizontal reduction。**这是更快的方案，没做** |
| 多个累加器（4 个 `acc`） | 依赖链 16 → 4，latency-bound 变 throughput-bound。反汇编确认现在**只有 1 个累加器**（16 条 `fmla` 全写 `v0`）。**小改动、真收益，没做** |
| `vaddvq_f32` 做归约 | `dot.hpp:73` 注释说明：故意写成 4 次 `vgetq_lane` 让不懂 NEON 的人也读得懂。可读性优先，代价是几条指令 |
| `-ffast-math` 让编译器自动向量化 | 全局改变浮点语义，会让 parity check 的前提失效 |

**对应工业界**：ANN 检索里的距离计算。真实系统用 FAISS / ScaNN，
配 IVF-PQ 或 HNSW，把"全量算距离"降成"只算候选子集"，再加 int8/PQ 量化压缩访存。
**差距**：这里是暴力全扫，没有索引、没有量化、没有 GPU、单线程。
量级差距见 §5。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「你的 baseline 是不是被编译器偷偷优化过了？」 | **主动承认并给细节**：是，乘法被向量化了，只有加法是串行的。所以 3× 的收益来自归约的向量化。能说出这个，说明你真的看过汇编 |
| Q2「为什么不跨 item 向量化？」 | 因为 buffer 是 item-major。跨 item 需要 dim-major + 重写 kernel，收益是干掉 3,000 次 horizontal reduction。**这是我知道但没做的下一步**，不要假装是有意为之的最优解 |
| Q3「`vmlaq_f32` 是 fused 吗？会影响 parity 吗？」 | clang 实际发的是 `fmla`（fused，单次舍入），而 scalar 路径是 `fmul`+`fadd`（两次舍入）。所以 `2.98e-07` 的差异有**两个**来源：求和顺序重排 **+** fused/unfused。`main.cpp:143` 写的 "reassociation only" 不完整 |

---

## 3. `Operator` — 统一接口（template method）

**解决什么问题**：让每个算子只需要实现"算什么"，而"怎么被观测"由基类强制统一。

**输入/输出**：`Batch in → Batch out`，副作用是往 `vector<TraceEntry>&` 追加一条。

**代码位置**：`src/operator.hpp:66-115`。数据结构 `Candidate` `:22-29`、
`Batch` `:32-34`、`TraceEntry` `:41-48`。

**内部流程**：

```cpp
Batch run(const Batch& in, std::vector<TraceEntry>& trace) const {   // :79
    const auto start = std::chrono::steady_clock::now();             // :83
    Batch out = transform(in);                                       // :84  ← 子类实现
    const auto end = std::chrono::steady_clock::now();               // :85

    TraceEntry entry;
    entry.name       = name();                                       // :88
    entry.in_count   = in.items.size();                              // :89
    entry.out_count  = out.items.size();                             // :90
    entry.latency_us = duration<double, micro>(end - start).count(); // :91
    entry.sample_ids = first_ids(out);                               // :92
    entry.detail     = detail();                                     // :93
    trace.push_back(std::move(entry));                               // :94
    return out;
}
protected:
    virtual Batch transform(const Batch& in) const = 0;              // :100
```

`run()` 是 **non-virtual** 的，`transform()` 是 pure virtual 且 **protected** ——
这是教科书式的 template method：**子类没有能力绕过计时和 trace**。

**为什么这么设计 / 备选方案**：

| 方案 | 为什么没选 |
|---|---|
| 每个算子自己计时、自己 push TraceEntry | 样板代码复制 5 遍；任何一个算子都可能忘记填 `sample_ids` 或格式不一致。`:59-64` 的注释明确点名了这个被否决的方案 |
| 用装饰器/中间件包一层 | 更灵活，但多一层间接；在只有一种横切关注点（tracing）时不划算 |
| 编译期 CRTP 而非虚函数 | 省一次虚调用。但每次请求只有 5 次虚调用，`CLAUDE.md` 明确反对为炫技上模板 |

**这是整个项目里最强的一张牌**（对标 intern bullet 2/5 的 "operator migration"）：
5 个算子对 trace 的产出**结构上不可能不一致**。详见 Step 6(b)。

**对应工业界**：BFS 这类特征/算子服务的算子基类。工业界的算子接口还会有
`Init(config)` / `Close()`、超时与降级、错误码、算子级 metrics 上报、
输入输出的 schema 声明。**差距**：这里没有配置注入、没有生命周期、
没有错误传播（`transform` 不能失败——没有返回错误的通道）。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「算子失败了怎么办？」 | **现在没有失败通道**——`transform` 返回 `Batch`，不返回 status，也不抛异常。真实系统需要 `Status transform(const Batch&, Batch*)` 或异常 + 算子级降级策略（失败时透传输入）。这是个明确的缺口 |
| Q2「trace 能关吗？」 | 现在不能，永远全开。见 Step 6(d)——加编译期 flag 或运行时开关的成本评估 |
| Q3「为什么 `transform` 是 const？状态放哪？」 | 算子是无状态的，配置在构造时注入并 `const` 持有。好处是天然线程安全、可并发调度；代价是 MixOp 那种想记录"这次实际探索了几个"的信息只能靠 `detail()` 从**配置**推算（`mix_op.hpp:54-57` 的注释承认了这点）——所以 detail 说的是"配置意图"，不是"实际结果" |

---

## 4. `DagScheduler` — 顺序执行链

> ⚠️ **这一节是整份材料里你最需要小心措辞的地方。**

**解决什么问题**：按顺序跑完算子链，把每一步的输出接到下一步的输入，并收集 trace。

**输入/输出**：`(seed Batch, trace&) → final Batch`。

**代码位置**：`src/scheduler.hpp:24-42`。**整个类 19 行有效代码。**

**内部流程 —— 全文如下，没有省略**：

```cpp
class DagScheduler {
public:
    void add(std::unique_ptr<Operator> op) { nodes_.push_back(std::move(op)); }   // :26-28

    Batch run(const Batch& seed, std::vector<TraceEntry>& trace) const {          // :32
        Batch batch = seed;                                                       // :33
        for (const std::unique_ptr<Operator>& node : nodes_) {                    // :34
            batch = node->run(batch, trace);                                      // :35
        }
        return batch;                                                             // :37
    }
private:
    std::vector<std::unique_ptr<Operator>> nodes_;                                // :41
};
```

**没有**：依赖声明、边、入度表、拓扑排序、环检测、多输入节点、并行、
算子级超时。`nodes_` 就是一个 `vector`，`run` 就是一个 `for`。

**代码自己是诚实的**（`:11-22`）："The Shua Shua cascade is a linear chain …
It is a degenerate DAG: one path, no branches." 并给出了不做通用 DAG 引擎的理由：
没有分支管线要调度，做了就是投机性复杂度。**这个理由本身是站得住的工程判断。**

问题**不在代码，在命名**：类叫 `DagScheduler`、UI 叫 "DAG pipeline trace"
（`TracePanel.tsx:51`）、README 叫 "DAG of operators"。**对外暗示的能力大于实现。**

**复杂度**：O(节点数) = 5 次虚调用。加上 `:33` 那次 72,000 字节的 seed 拷贝
（实测 1.46µs，见 Step 2 §5.2）。

**为什么这么设计 / 备选方案**：

| 方案 | 权衡 |
|---|---|
| 真拓扑排序（Kahn / DFS）+ 依赖声明 + 环检测 | 当前**没有任何**算子需要多输入，做了确实是空转。但这正是 Step 6(a) 要评估的缺口——因为**面试语汇需要它** |
| 直接在 `api.hpp` 里硬编码 5 次函数调用，不要 scheduler 类 | 更诚实，但会丢掉"算子可插拔"这个真实优点，也丢掉统一 trace 的挂载点 |

**对应工业界**：BFS / Service Engine 的 DAG 执行引擎。工业界的版本有：
从 DSL 解析建图、拓扑排序、**同层并行执行**、算子级超时与熔断、
子图复用与缓存、按需的逐节点日志（正是 intern bullet 4）。
**差距：这是最大的一个，量级上不可比。**

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「你这个 DAG 和一个函数调用链有什么区别？」 | **不要辩解。** 正确答法：「实现上就是顺序执行，我在注释里写明了它是 degenerate DAG。它比函数调用链多出来的是**算子可插拔 + 统一的 trace 挂载点**——加一个算子不用改 scheduler。真正的 DAG 能力（多输入、拓扑序、并行）我没做，因为没有分支管线需要它。」然后**主动**转到你真做了的解耦上 |
| Q2「加拓扑排序要多久？」 | 见 Step 6(a) 的工作量评估。能报出具体数字比含糊其辞强 |
| Q3「为什么 `run` 要先拷一份 seed？」 | `:33` `Batch batch = seed;` 是为了不修改 const 入参。代价是 72KB 拷贝（1.46µs）。可以改成第一个节点直接吃 `seed`、后续用 move —— 小优化，没做 |

---

## 5. `RecallOp` — 召回（source node）

**解决什么问题**：从全库里挑出与用户兴趣向量最相似的 top-k 候选。

**输入/输出**：
- 输入：**无数据输入**（见下）。配置：`const ItemStore&`、64 维 query、k=300、kernel 选择。
- 输出：`Batch`，300 个 `Candidate`，只有 `id` 和 `similarity` 被填充。

**代码位置**：`src/recall_op.hpp:100-141`（算子）、`:40-95`（自由函数：
`score_all` / `rank_topk` / `recall_with` / `recall_naive` / `recall_simd`）。

### 5.1 它是 source node —— 这不是 bug，是 DAG 里的一个正经概念

```cpp
Batch transform(const Batch& /*in*/) const override {   // recall_op.hpp:117
```

参数名被注释掉了。**在 DAG 术语里，RecallOp 是一个 source node（源节点）：
它没有数据前驱，它的输入来自图外部的资源（`ItemStore`），而不是上游算子的输出。**

理由写在 `:112-116`：召回是候选的**产生者**。它直接顺序流式扫描 SoA buffer，
这正是连续内存布局的收益所在；如果改成"遍历入参给的 id 列表再去取向量"，
就变成了散乱 gather，把布局优势全部浪费掉。

那 seed batch 存在的意义是什么？——**给 trace 一个漏斗口径**。
`Operator::run` 统一用 `in.items.size()` 填 `in_count`（`operator.hpp:89`），
所以要让 trace 报出"从 3,000 里选"，就得喂一个 3,000 的 seed。
UI 的漏斗宽度取 `max(e.in)`（`TracePanel.tsx:30`）。

**这是一个真实的设计取舍，说法应该是**：「统一接口要求每个节点都有 in_count，
而 source node 天然没有数据输入。我选择用一个全量 seed batch 来满足这个契约，
代价是每次请求多 ~9µs 的构造+拷贝（约占 11%）。另一种做法是让 source node
自报 `in_count = store.count()`、seed 传空——更省，但会在基类里开一个特例口子。」

**不要**把它说成"我写漏了"。**也不要**假装它零成本。

### 5.2 内部流程

```cpp
// :40-50  score_all —— 全量扫描，不排序
for (i = 0; i < n; ++i) {
    const float* vec = store.vector_of(i);
    scored.push_back(Scored{i, dot(query, vec, ItemStore::DIM)});
}

// :63-73  rank_topk —— 全排序后截断
std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;   // 相似度降序
    return a.id < b.id;                                 // 确定性 tie-break
});
if (k < scored.size()) scored.resize(k);
```

`:81-86` 的 `recall_with` 把两个 kernel 都走同一条路径 —— 扫描不同，**排序共享**，
所以 parity diff 只可能来自 kernel 本身。这个设计是 parity check 可信的前提。

`:66-68` 的 tie-break 按 id 升序：因为 SIMD 求和顺序不同可能产生极小差异，
若两项相等而排序不稳定，两条路径会给出不同顺序，把 parity 弄脏。

### 5.3 复杂度与实际运算量

| 阶段 | 复杂度 | 实际（N=3000, DIM=64, k=300） |
|---|---|---|
| 扫描 | O(N·DIM) | **192,000 次乘加** |
| 排序 | O(N log N) | 约 **34,650 次比较**（3000 × log₂3000 ≈ 3000×11.55） |
| 截断 | O(1) amortized | resize 到 300 |

### 5.4 ⚠️ 实测：SIMD 之后，排序才是瓶颈

本次实测（arm64 -O2，40 轮 × 100 次取最小值，五个变体交错测量）：

| # | 变体 | 耗时 |
|---|---|---|
| 1 | scalar 扫描（仅扫描） | 26.7 µs |
| 2 | **SIMD 扫描（仅扫描）** | **9.0 µs** |
| 3 | scalar 扫描 + `std::sort`（M2 之前） | 64.1 µs |
| 4 | **SIMD 扫描 + `std::sort`（今天的代码）** | **46.0 µs** |
| 5 | SIMD 扫描 + `nth_element` + sort(k) | **17.7 µs** |

由此得到三个数字：

- 扫描本身的 speedup：**~3.0×**（这与 README 报的 3.6× 属于同一件事，
  不同 harness 下 2.9–3.7× 浮动）
- **top-k 排序在今天的 recall 里占 37 µs = 80%**
- **M2 真正带来的端到端 recall speedup 只有 1.39×**（64.1 → 46.0），
  不是 3.6×。`main.cpp` 自己也诚实地报了这个（它报 1.84×，同样远小于 3.6×）
- **把 `std::sort` 换成 `nth_element` 还能再快 2.6×（46.0 → 17.7）——
  比 SIMD 带来的收益还大**

`recall_op.hpp:59-62` 的注释已经预见到了这点（"partial_sort is a fair
optimization once n grows large — premature now"）。**实测表明它现在就不 premature 了。**

> 这不是"SIMD 白做了"。M2 之前扫描占 64.1µs 中的 26.7µs（42%），
> 优化它是合理的第一步；优化完之后瓶颈转移到排序 —— **这就是 profiling 的正常节奏。
> 能把这个故事讲完整，比报一个 3.6× 强得多。**

### 5.5 为什么这么设计 / 备选方案

| 方案 | 为什么没选 |
|---|---|
| ANN 索引（HNSW / IVF） | `:35-39` 明说是 stretch goal，故意不做。3,000 条全扫已经够快，且全扫是最清晰的参照实现 |
| `std::partial_sort` / `nth_element` | `:59-62` 判断"在这个规模上差别可忽略"。**实测推翻了这个判断**（见 §5.4） |
| 堆（`priority_queue`）维护 top-k | O(N log k)，同样比全排序好。但需要自己处理 tie-break 的确定性 |

### 5.6 对应工业界

对应**召回层**。真实系统是：多路召回并行（i2i / u2i / 向量召回 / 热门 / 运营池）
→ 归并去重 → 统一截断；向量那一路走 ANN 索引 + 分片 + GPU；
候选池 10⁶–10⁹，召回 10³–10⁴。

**差距（必须说清）**：
- 池子 3,000 vs 工业界 10⁶⁺ —— **差 3 个数量级以上**
- 单路召回 vs 多路归并
- 全量暴力扫 vs ANN 索引
- 单机单线程 vs 分布式分片

### 5.7 三个追问

| 追问 | 答法要点 |
|---|---|
| Q1「你候选池才 3,000，凭什么说自己懂大规模检索？」 | **不要硬扛。**「我不懂大规模检索的工程，我做的是 serving pipeline 的骨架。3,000 这个规模下我能讲清楚的是：为什么全扫是合理的参照实现、SIMD 收益从哪来、以及**扫描优化完之后瓶颈会转移到 top-k 排序**（有实测）。真到 10⁶ 我知道要换 ANN + 分片，但那部分我没做过。」—— 用一个你**有实测**的洞察换掉你没有的经验 |
| Q2「为什么 RecallOp 不读输入？」 | 按 §5.1 讲 source node，说清代价（~9µs / 11%）和替代方案 |
| Q3「top-k 为什么用全排序？」 | 承认这是当前最大的单点浪费（80% 的 recall 耗时），已实测 `nth_element` 能再快 2.6×，注释里的"premature"判断在这个规模下已经不成立 |

---

## 6. `FeatureOp` — 特征挂载

**解决什么问题**：给存活候选补齐打分器要读的特征列。**不改变基数**（enrich，不 filter）。

**输入/输出**：300 个 `Candidate`（只有 id/similarity）→ 300 个 `Candidate`
（补上 `category_match` / `recency` / `popularity`）。

**代码位置**：`src/feature_op.hpp:19-63`，核心 `:30-58`。

**内部流程**：

```cpp
Batch out = in;                                   // :35  拷贝，因为入参是 const
for (Candidate& c : out.items) {
    const Note& note = store_.notes[c.id];

    c.category_match = category_weights_[note.category];              // :43
    const float kHalfLifeDays = 30.0f;                                // :50
    c.recency = std::exp(-static_cast<float>(note.age_days) / kHalfLifeDays);  // :51
    c.popularity = note.popularity;                                   // :55
}
```

三个特征的性质完全不同：
- `category_match`：**直接读 profile 权重**，不做任何变换或归一化
- `recency`：指数半衰减，半衰期硬编码 30 天（`:50`）。`:44-49` 解释为何用指数
  而非线性：指数天然落在 (0,1] 无需 clamp，且符合"先快后慢"的衰减形状
- `popularity`：建库时已归一到 [0,1]，直接透传

**复杂度**：O(M)，M=300。实际：**300 次 `std::exp` 调用** + 300 次数组索引。
实测 FeatureOp 全程 0.90 µs（WASM）/ 约 2.4 µs（native 冷调用）。

### ⚠️ 一个真实的量纲问题（自己先知道，别等被问）

`category_match` 读的是**未归一化的累计权重**。点击每次 +1 无上限
（`profile.ts:200`），而 `similarity ∈ [-1,1]`、`recency ∈ (0,1]`、
`popularity ∈ [0,1]`。ScoreOp 给 `category_match` 的权重是 0.5（`api.hpp:118`）。

推演：冷启动选了 Food，权重 = 1；连点 3 个 food，权重 = 4；此时
`0.5 × 4 = 2.0`，而 similarity 最大只有 1.0 —— **类目项完全压过相似度**。
好在每次刷新会 ×0.5 衰减（`profile.ts:225`），稳态大致收敛到"每轮点击数"，
所以实际值通常在 1–5 之间，不会失控。

**但这确实是没有做特征归一化的后果。** 真实系统里所有特征进模型前都会
归一/分桶。**被问到就承认：「特征没有做量纲统一，靠衰减隐式把它压在小范围内，
这是个薄弱点。」**

**为什么这么设计 / 备选方案**：

| 方案 | 为什么没选 |
|---|---|
| 原地修改而非 `Batch out = in` | 接口规定输入 const（每个 stage 的输入不可变）。`:32-35` 的注释说明：特征阶段概念上就是产出一个 enriched copy |
| 线性 recency `1 - age/max` | `:44-49`：需要 clamp，且形状不符合真实的兴趣衰减 |
| 把 recency 预计算存进 `Note` | `note.hpp:20-25` 明确反对：原始 age 是数据，衰减曲线是排序逻辑，分开放才能各自演进 |
| 特征归一化 / 分桶 | **没做**，见上 |

**对应工业界**：**这就是 BFS（Bytedance Feature Service）在做的事** ——
特征抽取。工业界版本：几百上千个特征算子、特征版本管理、在线/离线一致性校验、
特征回填、长序列特征（用户近 N 次行为）、特征 DSL 描述抽取逻辑。
**差距**：这里是 3 个硬编码特征、无配置、无版本、无一致性校验。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「特征怎么加？」 | 现在要改 3 个地方：`Candidate` 加字段（`operator.hpp:22-29`）、`FeatureOp::transform` 填值、`ScoreOp::Weights` 加权重 + `api.hpp:118` 传值。**再加上 `to_json`（`api.hpp:195-201`）和前端的 `FeedItem` 类型（`engine.ts:10-18`）** —— 一共 5 处，跨两种语言。这正是 DSL 化想解决的问题 |
| Q2「特征量纲不统一有什么后果？」 | 见上，主动讲 |
| Q3「半衰期 30 天怎么来的？」 | **没有依据，是拍的**（`:50` 硬编码，无注释解释为何是 30）。诚实说"这是个 demo 常数，真实系统应该由数据决定"。不要编一个理由 |

---

## 7. `ScoreOp` — 线性多目标打分

**解决什么问题**：把多个特征融合成一个可排序的分，并截断到精排规模。

**输入/输出**：300 个带特征的 `Candidate` → 50 个（填了 `score`，按分降序）。

**代码位置**：`src/score_op.hpp:16-66`，核心 `:38-61`。权重在 `api.hpp:117-120`。

**内部流程**：

```cpp
c.score = weights_.similarity     * c.similarity        // 1.0
        + weights_.category_match * c.category_match    // 0.5
        + weights_.recency        * c.recency           // 0.3
        + weights_.popularity     * c.popularity;       // 0.2
// score_op.hpp:41-44，权重值来自 api.hpp:118-119

std::sort(...);        // :49-55  同样按 score 降序、id 升序 tie-break
if (k_ < out.items.size()) out.items.resize(k_);   // :57-59  k=50
```

**复杂度**：O(M log M)。实际：300 × (4 乘 + 3 加) = **1,200 乘 + 900 加**，
排序约 **2,470 次比较**（300 × log₂300 ≈ 300 × 8.23）。实测 2.89 µs（WASM）。

**为什么这么设计 / 备选方案**：

`:18-25` 的注释是全项目最坦诚的一段：真实精排用学习模型预测 pCTR/pLike/pSave
再融合；这里没有模型（训练不在范围内），所以用线性加权。
**"fabricated sigmoid 'model' curves would add mystery without adding a real model"** ——
拒绝造假模型，这个态度本身在面试里是加分的。

| 方案 | 为什么没选 |
|---|---|
| 假的 DNN / sigmoid 打分 | 见上，故意拒绝 |
| 真训一个小模型 | `CLAUDE.md` 明确：不做 training |
| 权重可配置 | 现在硬编码在 `api.hpp:118-119`。改一个数要重编 + 重 build WASM |

**对应工业界**：精排（ranking）。真实系统：多目标 DNN（MMoE / PLE 等）
预测多个 head，再用融合公式（常带业务调权、时长/完播/互动的加权）；
在线特征几百维，模型几亿参数，用 GPU 推理，还有 A/B 实验框架控制融合公式。
**差距**：线性 4 项 vs 学习模型；无 A/B；无在线学习。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「四个权重怎么定的？」 | **拍的，没有依据。** `api.hpp:118-119` 硬编码。诚实说：「没有离线评估集，所以没法调参；这些值是让 demo 看起来合理的手调值。真实做法是用离线 AUC/GAUC + 线上 A/B。」 |
| Q2「为什么不做归一化再融合？」 | 见 §6 的量纲问题，承认 |
| Q3「为什么 ScoreOp 和 RecallOp 都要排序？重复了吗？」 | 不重复：RecallOp 按 similarity 排（粗筛 3000→300），ScoreOp 按融合分排（精排 300→50）。这是 cascade 的标准形态——每一级用更贵的模型处理更少的候选。**这个能答好会显得你真懂 cascade** |

---

## 8. `RerankOp` — greedy MMR 多样性重排

**解决什么问题**：避免整页都是同一个类目，在相关性和多样性之间做权衡。

**输入/输出**：50 个已打分候选 → 24 个（profile 路径）或 12 个（无 MixOp 时）。

**代码位置**：`src/rerank_op.hpp:33-87`，核心 `:41-81`。λ 在 `api.hpp:50`。

**内部流程**：贪心 MMR，每轮挑边际价值最高的一个。

```cpp
std::vector<std::size_t> category_picks(256, 0);      // :48  已选类目计数

for (placed = 0; placed < page; ++placed) {           // :54
    for (i = 0; i < remaining.size(); ++i) {          // :58
        const std::uint8_t category = store_.notes[c.id].category;
        const float redundancy = static_cast<float>(category_picks[category]);   // :61
        const float value = lambda_ * c.score - (1.0f - lambda_) * redundancy;   // :62
        if (value > best_value) { best_value = value; best_index = i; }
    }
    out.items.push_back(chosen);                                       // :70
    category_picks[store_.notes[chosen.id].category] += 1;             // :71
    remaining.erase(remaining.begin() + best_index);                   // :77
}
```

**关键**：redundancy 用的是**同类目已选个数**，不是经典 MMR 的"与已选集合的向量相似度"。
`:26-31` 说明理由：这个 feed 的"单调"轴就是类目，直接按类目算既准确又不用回到点积 kernel。

λ = 0.7（`api.hpp:50`）：0.7 × score − 0.3 × 同类已选数。
所以每多选一个同类，该类下一个候选的价值就减 0.3。

**复杂度**：O(P × M)，P=页大小，M=池大小。
实际：Σ(50→27) = **924 次候选评估** + 24 次 `erase`（平均搬 ~19 个元素 ≈ 450 次移动）。
实测 1.99 µs（WASM）。

`:73-77` 明说 `erase` 的 O(n) 搬移在 50 个候选下是免费的，`swap-and-pop` 是
不划算的微优化 —— 这个判断在这个规模下是对的（不像 §5.4 里那个）。

**为什么这么设计 / 备选方案**：

| 方案 | 为什么没选 |
|---|---|
| 经典 MMR（向量相似度做 redundancy） | `:26-31`：更通用，但这里买不到什么，代价是重新进 kernel |
| DPP（行列式点过程） | 理论上更优的多样性建模，但复杂度和解释成本都高 |
| 硬规则打散（同类间隔 ≥ N） | 更简单但不可调；MMR 的 λ 提供连续权衡 |
| 256 桶固定数组 | `:44-47` 解释：`category` 是 uint8，256 桶覆盖所有取值，省掉"有几个类目"的参数传递。代价是每次调用分配 2KB |

**对应工业界**：重排层。真实系统：MMR / DPP 做多样性，
加上打散规则（同作者、同类目、同话题的间隔约束）、
广告与内容的混排、以及业务硬规则（置顶、屏蔽、频控）。
**差距**：单一多样性维度（类目），无业务规则，无频控。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「λ=0.7 怎么定的？」 | **拍的。** 没有多样性指标（如 ILD / category entropy）来评估。诚实说，并补一句"要定它需要先定义多样性指标 + 用户满意度代理指标" |
| Q2「贪心 MMR 不是最优解，你知道吗？」 | 知道。MMR 是次模函数最大化的贪心近似，有 (1−1/e) 的理论保证（在次模假设下）。选它是因为 O(P·M) 且可解释；精确解是 NP-hard |
| Q3「为什么 profile 路径要出 24 个而不是直接 12 个？」 | 因为 MixOp 还要在后面做 new/seen 配比和探索注入，需要一个比页面大的**多样化池**（`api.hpp:51` 注释 "diverse pool MixOp draws from (> page)"）。如果 rerank 直接给 12 个，MixOp 就没有腾挪空间 |

---

## 9. `MixOp` — new/seen 配比 + 探索保底

**解决什么问题**：刷新时不要总是重放看过的内容；同时保证页面永远不会 100% 单一类目。

**输入/输出**：24 个重排后候选 → 12 个（10 exploit + 2 explore）。

**代码位置**：`src/mix_op.hpp:40-132`，核心 `:60-124`。
参数：`kPageSize=12`、`kExploreFloor=2`（`api.hpp:49,52`）、`NEW_RATIO=80`（`profile.ts:43`）。

**内部流程 —— 分两段**：

**(a) Exploit（10 个槽位）**：按 seen 集合把候选分成 new / seen 两堆，
按 `new_ratio` 取，不够则从另一堆回填（优先补 new）。

```cpp
const std::size_t exploit_slots = page_size_ - explore_floor_;    // :61  12-2 = 10
for (const Candidate& c : in.items)
    (seen_.count(c.id) != 0 ? seen_items : new_items).push_back(c);   // :67

std::size_t new_target = llround(exploit_slots * new_ratio_ / 100.0);  // :70-71  10*0.8 = 8
std::size_t take_new  = std::min(new_target, new_items.size());        // :74
std::size_t take_seen = std::min(exploit_slots - new_target, seen_items.size());  // :75
// :76-84  shortfall 回填，优先补 new
```

**(b) Explore（2 个槽位，保底）**：从**主导类目之外**随机采样。

```cpp
// :94-103  先找出 exploit 部分的主导类目
// :109     RNG 用 seen 集合大小做种子 —— 同一状态可复现，随点击轮换
std::mt19937 rng(static_cast<std::uint32_t>(seen_.size() + 1));
for (tries = 0; added < explore_floor_ && tries < n * 2; ++tries) {   // :111
    const std::uint32_t id = rng() % n;                               // :112
    if (picked.count(id) != 0 || seen_.count(id) != 0) continue;      // :113
    if (store_.notes[id].category == dominant) continue;              // :114
    Candidate c;
    c.id = id;   // 探索项故意不排序，score 列保持 0                     // :116
    out.items.push_back(c);
}
```

**探索项的 score/similarity/recency/popularity 全是 0** —— 这是**有意的**
（`:116` 注释 "intentionally unranked"），因为它们绕过了 Recall/Feature/Score。
这在 native 输出里直接可见（最后两条 `score=0 sim=0 rec=0 pop=0`），
在前端会让 `whyFor` 走到 popularity 分支 → 显示 "Trending right now"
（`presentation.ts:165-181`）—— **这是个小的表里不一：一个探索项被标成"正在流行"。**

**复杂度**：O(M) 分堆 + O(256) 找主导 + 期望 O(explore_floor / (1−主导占比)) 次采样。
实际：24 次分堆 + 256 次扫描 + **期望约 2.4 次采样**（3,000 个 item 里约 5/6 非主导类目）。
最坏被 `tries < n*2 = 6000` 兜住。实测 2.73 µs（WASM）。

**为什么这么设计 / 备选方案**：

`:29-33` 的理由很扎实：对一个集中的 query，recall 返回 ~300 个同类目 item，
**排序阶段看到的每一个候选都是那个类目** —— 多样性不可能从已排序的池子里"涌现"，
必须注入。所以保留固定槽位是唯一可靠的办法。

| 方案 | 为什么没选 |
|---|---|
| 靠 RerankOp 的 MMR 产生多样性 | `:29-32`：池子里全是同类，MMR 无从下手 |
| ε-greedy / Thompson sampling / UCB | 真正的 EE 算法，但需要反馈闭环和收益估计。这里没有 reward 信号 |
| 在召回阶段就多路召回保证多样性 | 更接近工业做法，但要改成多路召回架构 |

**对应工业界**：探索/EE + 冷启动扶持 + 多样性保量。真实系统用
Thompson sampling / UCB / contextual bandit，探索比例由线上实验决定，
且有专门的冷启动流量池给新内容。
**差距**：这里是固定 2/12 = 16.7% 的硬保底，**比例是拍的**，没有 bandit，没有 reward。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「探索比例 2/12 怎么定的？」 | **拍的**（`api.hpp:52`）。诚实说：定它需要线上实验测"探索带来的长期留存收益 vs 短期 CTR 损失"，我没有这个闭环。能讲清楚**为什么需要保底**（§9 的注释理由）比编一个数字强 |
| Q2「探索项的 score 是 0，会不会有问题？」 | 会，两处：(a) 它们排在页面最后（因为是 append 的）；(b) 前端的 "why" 文案会误标成 "Trending right now"。**主动讲这个小瑕疵**，显示你真的端到端看过 |
| Q3「RNG 用 `seen_.size()` 做种子，合理吗？」 | 优点是同一 profile 状态可复现（对 demo 和调试友好），且随点击轮换。缺点是：**用户点击数相同就会看到相同的探索项**，且不同用户之间完全没有区分。真实系统应该用 user_id + 时间片做种子 |

---

## 10. `decayProfile` — 兴趣衰减（在 TypeScript 里）

> ⚠️ **这个不在 C++ 引擎里。** 用户画像的全部逻辑住在 `web/src/profile.ts`。

**解决什么问题**：让近期行为比陈旧行为更有分量，避免早期点击永久锁死画像。

**输入/输出**：`Profile → Profile`（不可变，返回新对象）。

**代码位置**：`web/src/profile.ts:227-236`，常数 `:225`。
触发点：`App.tsx:112`（只在点「Refresh」时触发，不是定时器）。

**内部流程**：

```ts
export const DECAY_FACTOR = 0.5;                            // :225

export function decayProfile(profile: Profile, factor = DECAY_FACTOR): Profile {
  const tagWeights: Record<string, number> = {};
  for (const [tag, w] of Object.entries(profile.tagWeights)) tagWeights[tag] = w * factor;  // :229
  if (Math.max(0, ...Object.values(tagWeights)) < 1e-3) {   // :232  全衰减到 0 的保护
    return { ...profile, tagWeights: neutralProfile().tagWeights };
  }
  return { ...profile, tagWeights };
}
```

配套的增长在 `recordClick`（`:197-207`）：点一次 **+1，无上限**。

**动力学**（这个能讲出来很加分）：设每轮刷新之间点击某类目 c 次，则
`w ← (w + c) × 0.5`，不动点是 **w\* = c**。所以稳态权重 ≈ 每轮点击数，
天然有界；而停止点击后，权重每次刷新减半，**约 3–4 次刷新后基本消失**。

**为什么是 per-refresh 而不是 per-time**：`:209-215` 的注释解释得很好 ——
在一个点击驱动的 demo 里几乎不流逝真实时间，用 `exp(-λΔt)` 会看起来"什么都不会淡"；
事件驱动的衰减让"淡出"恰好发生在用户操作时。**这是个针对场景的正确取舍。**

**为什么是 0.5 而不是 0.7**：commit `c62d0b6` 的历史 + `:216-224` 的注释：
0.7 时早期点击太久不掉，画像感觉被"锁死"（entrenchment）；改成 0.5 后
"a sustained change of interest can take over within a few refreshes"。
**这是少数几个有明确调参理由和 commit 记录的参数**，值得在面试里讲。

**复杂度**：O(标签数) = **6 次乘法**。

**对应工业界**：用户画像的时间衰减 / 长短期兴趣建模。
真实系统会分长期画像（月级，稳定）和短期画像（会话级，敏感），
并用**用户行为序列**（近 N 次点击/播放）直接喂给模型做 target attention
（DIN/SIM 那一类）——**这正是 intern bullet 7 说的 "long-sequence features"**。
**差距**：这里是 6 维标量权重 + 一个全局衰减因子，没有序列建模、
没有长短期分离、没有负反馈（不喜欢/划走）。

**三个追问**

| 追问 | 答法要点 |
|---|---|
| Q1「为什么衰减放在前端而不是引擎里？」 | 因为画像状态本身就在浏览器（无后端、无 DB）。C++ 引擎每次调用是**无状态纯函数**。这个边界是有意的：引擎只做 serving，状态归调用方。**但要承认代价：两个排序策略参数（0.5 和 80）住在 TS 里，和引擎的 8 个 constexpr 分居两地** |
| Q2「点击 +1 无上限，会不会失控？」 | 不会，因为衰减把不动点钉在"每轮点击数"（讲上面那个动力学）。**能推出 w\*=c 这个结论会很加分** |
| Q3「只有正反馈，没有负反馈？」 | 对，只有点击加权，没有"划过不点"的负信号，也没有"不感兴趣"按钮。真实系统会用曝光未点击作为弱负样本。这是个明确的没做 |

---

## 11. 测量方法（复现说明）

§2 / §5.4 的数字来自本次盘点写的临时程序（**不在仓库里**）：

- **反汇编**：`clang++ -std=c++20 -O2 -Isrc -c` 单独编译只含 `dot_scalar` /
  `dot_simd` 的 TU，`objdump -d` 查看指令构成。隔离 TU 是为了排除 libc 噪音。
- **top-k 对比**：`#include "api.hpp"`，五个变体（scalar/SIMD 扫描、
  三种 top-k 策略）在同一个进程里**交错**测量 40 轮 × 100 次，
  取**每轮最小值**（min 是这里的稳健统计量——噪声只会让时间变长）。
  两次独立运行结果一致（37 µs / 80% / 1.39× / 2.6×，误差 < 3%）。
- **WASM 分算子耗时**：见 Step 2 §7。

> 【待验证】这些测量目前不可一键复现。要固化需在仓库加 `bench/` 目录
> （约 1 小时），Step 5 会重新讨论要不要做。

---

## 12. 这一步最重要的四个发现

1. **SIMD 之后，top-k 排序占了 recall 的 80%（37 µs / 46 µs）。**
   M2 真正带来的端到端 recall speedup 只有 **1.39×**，不是 3.6×（3.6× 是扫描单独的）。
   而把 `std::sort` 换成 `nth_element` 还能再快 **2.6×** —— 比 SIMD 的收益还大。
   **这个"优化完瓶颈就转移"的完整故事，是回答"SIMD 有没有意义"这类压力问题的最佳弹药。**

2. **`dot_scalar` 的乘法其实被编译器向量化了**，只有加法是串行的。
   所以 speedup 的来源是**归约被向量化**（依赖链 64 → 16），不是"从标量变向量"。
   反汇编可证。这个细节会让面试官相信你真的看过生成代码。

3. **RecallOp 不读入参，正确的框架是 source node**（无数据前驱，从图外资源产出），
   而不是"漏了"。但要同时说清它为满足统一 trace 契约付出的代价：
   ~9 µs / 请求，约 11%。

4. **好几个关键参数是拍的，且没有评估手段**：ScoreOp 的四个权重、
   RerankOp 的 λ=0.7、探索比例 2/12、recency 半衰期 30 天。
   唯一有明确调参理由 + commit 记录的是 `DECAY_FACTOR` 0.7→0.5（commit `c62d0b6`）。
   **面试时把这一个讲透，胜过把四个都含糊带过。**

---

**下一步**：Step 4 — 设计决策与权衡表（DAG vs 硬编码、SoA vs AoS、
为什么 WASM、为什么无数据库、为什么只做 serving、exploration 比例、衰减参数，
每条写 决策 / 理由 / 代价 / 重做会不会改）。
