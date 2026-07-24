# 核心引擎设计

本文描述 `src/` 中的 C++ 引擎，默认读者已了解 [Architecture](Architecture_ch.md) 中的系统概览。

## 1. 数据模型

### 1.1 Note（`note.hpp`）

一条 item（“note”）只携带级联需要读取的标量信号：

```cpp
struct Note {
    std::uint32_t id;         // 稳定的 item id（== store 中的下标）
    std::uint8_t  category;   // food / fashion / travel / tech / ...
    float         popularity; // 归一化到 [0, 1]
    std::uint32_t age_days;   // 驱动新鲜度特征
};
```

表现字段（标题、封面、作者）刻意**不**放这里——它们在前端合成。

### 1.2 Item store（`item_store.hpp`）

存储采用 **SoA（Structure-of-Arrays）**：一整块扁平 `float` buffer，按 item 行主序拼接所有 item
向量，再加一个平行的 `notes` 数组。

```cpp
struct ItemStore {
    static constexpr std::size_t DIM = 64;   // 固定 embedding 维度
    std::vector<float> embeddings;           // count()*DIM，行主序 (SoA)
    std::vector<Note>  notes;                // 平行元数据
    std::size_t  count() const;              // embeddings.size() / DIM
    const float* vector_of(std::uint32_t i); // &embeddings[i*DIM]
};
```

**为何 SoA：** 相似度内核以连续内存方式遍历 item 向量，这正是缓存表现好、SIMD 路径值得做的原因。
`vector_of` 返回指向既有 buffer 的裸指针——不拷贝。该布局是一项契约，不重构为 AoS。

### 1.3 合成数据（`synthetic.hpp`）

作为学习 embedding 替身的 fixture。每个品类有一个随机中心；每条 note 的向量为
`centroid[category] + 小噪声`，随后单位归一化。因此同品类 note 在向量空间聚类，使相似度召回
在语义上有意义。固定的 PRNG 种子让 store 可复现。

归一化到单位长度使点积等于余弦相似度，于是“相似”意味着“同方向”（同品类），与模长无关——
而内核保持为纯点积。

## 2. 算子接口（`operator.hpp`）

`Batch` 是在各阶段间流动的载荷——`Candidate` 的数组（AoS）（batch 小且短暂，AoS 读起来清晰；
SoA 规则只针对 item store / 热点内核）：

```cpp
struct Candidate {
    std::uint32_t id;
    float similarity, category_match, recency, popularity, score; // 逐级填充
};
struct Batch { std::vector<Candidate> items; };
```

`TraceEntry` 是每个阶段产出的固定记录（UI 依赖此形状）：

```cpp
struct TraceEntry {
    std::string name; std::size_t in_count, out_count;
    double latency_us; std::vector<std::uint32_t> sample_ids;
    std::string detail;  // 可选的一行备注（例如 MixOp 的 exploit/explore 拆分）
};
```

`Operator` 采用**模板方法**模式：基类 `run()` 只定义一次——它对该阶段计时、调用子类的
`transform()`、并追加恰好一条 `TraceEntry`。子类只实现 `transform()` 与 `name()`。

```cpp
class Operator {
public:
    virtual std::string name() const = 0;
    virtual std::string detail() const { return ""; }                 // 可选的 trace 备注
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) const; // 计时 + 记录
protected:
    virtual Batch transform(const Batch& in) const = 0;               // 算法
};
```

**为何把 trace 集中在基类：** `{name, in, out, latency_us, sample_ids, detail}` 契约对每个阶段以
完全相同的方式产出，不会漂移也不会被遗漏（阶段只是可选地覆写 `detail()` 来加一行备注）。trace 是
产物，因此在一处强制保证。

## 3. 算子

- **RecallOp**（`recall_op.hpp`）——源头阶段。按与 query 的点积相似度给 store 中每个 item 打分并保留
  top-k。它流式遍历连续的 SoA buffer（而非输入 id 列表），这正是 SIMD 内核见效之处。持有一个内核
  选择器（标量或 SIMD）。
- **FeatureOp**（`feature_op.hpp`）——为每个候选附加特征：`category_match`（画像——或 persona——对该
  item 品类的亲和度）、`recency`（`age_days` 的指数衰减）、`popularity`（透传）。基数不变。
- **ScoreOp**（`score_op.hpp`）——把特征做透明的加权线性组合得到一个分数，再保留 top-k。生产排序器
  会融合学习到的多目标模型（pCTR/pLike/pSave）；此处无训练模型，线性组合诚实地表达它的本质。
- **RerankOp**（`rerank_op.hpp`）——贪心 MMR（最大边际相关），以品类为冗余度轴：
  `value = λ·score − (1−λ)·redundancy`，使最终页在相关性与品类多样性之间权衡。在画像路径上，它产出
  一个**更宽**的池（`kRerankPool = 24`），好让下一阶段有多样性可混。
- **MixOp**（`mix_op.hpp`）——**仅画像路径。** 从两个池组装最终页：**利用（exploit）**——new/seen
  混合（`new_ratio`），此处强偏好合理地占主导——以及一个从主导品类*之外*抽样的、保证的
  **探索（explore）**保底位（`kExploreFloor = 2`）。这是探索/利用权衡的缩影：对高度集中的 query，
  召回只返回约一个品类，因此多样性无法从已排序的池内部涌现，必须被注入。探索 item 刻意绕过
  Recall/Feature/Score（它们未排序——这正是要点），因此其分数列保持为零；MixOp 在 trace `detail`
  里报告它的拆分（例如 `"10 exploit · 2 explore"`）。

## 4. 调度器（`scheduler.hpp`）

`DagScheduler` 持有算子并按加入顺序执行，将每一级输出接入下一级并收集 trace。Shua Shua 的级联是
线性链，因此这是一个退化的 DAG（单路径、无分支）。

**为何暂不做通用 DAG 引擎（拓扑排序、多入节点）：** 目前没有需要调度的分支流水线，那会是带来
未定义语义（例如如何合并两个输入 batch）的投机复杂度。等真的出现分支流水线，再演进为拓扑执行。

## 5. 相似度内核与奇偶校验（`dot.hpp`）

唯一的热点计算是 query 与每个 item 向量的点积。

- `dot_scalar`——参考实现。朴素的从左到右累加。即便在 `-O2` 下它也保持标量，因为没有
  `-ffast-math` 时 clang 不会向量化浮点归约（重结合会改变结果）。
- `dot_simd`——手写 NEON。每次迭代处理 4 个 float（128 位寄存器 = 4× `float32`），保留四个 lane
  累加器，最后横向求和。一个标量尾循环处理 `dim % 4`（DIM=64 时空转，但让内核对任意维度都正确）。
  由 `__ARM_NEON` 保护；其它平台（含 WASM）回退到 `dot_scalar`。

召回把两个内核都走同一套 `score_all` + `rank_topk`，因此朴素/SIMD 对比恰好隔离出内核这一处差异。
由于 SIMD 以不同顺序求和，分数在浮点重结合层面（约 1e-7）有差异；排序通过确定性的 id 并列打破而
保持稳健。

**奇偶校验纪律。** 朴素与 SIMD 路径必须产出完全一致的输出。`main.cpp` 在同一输入上跑两者并报告：
top-k 排序一致（`result diff = 0`）、逐 item 最大分数差（约 3e-7），以及加速比（仅扫描约 3.6×；
端到端更低，因为 top-k 排序被共享且未向量化）。朴素路径永久保留作参考。

## 6. API 边界（`api.hpp`、`bindings.cpp`）

`api.hpp` 是原生驱动与 WASM 绑定共用的唯一编排层——它是胶水，不是排序逻辑：

- `shared_data()`——常驻 item store，只构建一次（函数局部静态）并跨请求复用。
- `make_query(weights, centroids)`——把按品类权重变成单位归一化 query 向量的加权中心向量混合。被每个
  入口共用，因此 persona query 与画像 query 的构建方式完全一致，不会漂移。
- `run_recommendation(query, weights, label, seen, new_ratio, explore_floor)`——共享核心：组装
  `RecallOp → FeatureOp → ScoreOp`，随后要么 `RerankOp` 直接产出该页，要么——当有探索保底位 / 已看
  集合需要照顾时——`RerankOp` 走更宽的池 → `MixOp`。返回 `{ persona_label, feed, trace }`。
- 三个入口喂给它，区别仅在 query 来源：`recommend(personaId)`（persona 的混合中心向量）、
  `recommend_similar(itemId)`（被点击 item 自身的向量）、以及
  `recommend_from_profile(categoryWeights, seen, newRatio)`（v2 实时路径——画像*就是*召回 query；
  总是走 `MixOp` 路径）。
- `to_json(rec)`——对 feed + trace 的手写 JSON 序列化（前端消费的形状）。保持零依赖。
- `personas()`——为 persona 路径保留的可切换 personas（标签 + 按品类兴趣权重）。

`bindings.cpp` 通过 embind 向 JavaScript 暴露 `recommendFromProfile`（实时路径）、`recommend`、
`recommendSimilar`、`personaCount`、`personaLabel`。画像权重与已看 id 集合以 CSV 字符串跨界（引擎侧
解析）——对一个小的固定向量，这是最简单稳健的跨界方式。绑定中没有任何引擎逻辑。

## 7. 设计原则

- **可读性优先于聪明。** 写一个中级工程师会写的、最简单的正确版本。无模板元编程、无生僻技巧。
- **朴素参考永久保留。** 零差异奇偶校验是一项功能，因此 `dot_scalar` / `recall_naive` 永不删除。
- **确定性。** 固定种子；id 并列打破。结果与奇偶校验可复现。
- **保持 trace 契约。** `{name, in_count, out_count, latency_us, sample_ids, detail}` 固定不变；UI 依赖它。
