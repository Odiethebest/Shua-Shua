# 架构

## 1. 目标与范围

Shua Shua 是一个教学性质的**推荐服务引擎（serving engine）**。工业级推荐系统分为两半：
**训练**（离线、通常用 Python——学习 embedding 和模型权重）与**服务**（在线、通常用
C++——在毫秒级把庞大的候选池收敛成一小页排好序的 feed）。Shua Shua 实现的是**服务**这一半，
并让它**可观测**：产生 feed 的同一次请求也会产出一条执行 trace，在 UI 中渲染成一列逐级点亮的
流水线漏斗。

引擎用 C++20 编写，经 Emscripten 编译为 WebAssembly，前端用 React 呈现为小红书风格的 feed。
整个产品是一个静态站点，**没有后端、请求路径上没有任何网络调用**。

## 2. 系统上下文

```
   浏览器（单一静态页面，无服务器）
   ┌──────────────────────────────────────────────────────────────┐
   │                                                                │
   │   React UI (web/)                    WASM 模块 (src/ → C++)     │
   │   ┌────────────────────┐  recommend  ┌───────────────────────┐ │
   │   │ 侧边栏 personas     │  (persona)  │  DAG 调度器             │ │
   │   │ 瀑布流 feed         │ ──────────▶ │  拓扑顺序执行            │ │
   │   │ DAG trace 面板      │             ├───────────────────────┤ │
   │   │                    │  JSON feed  │  算子：                 │ │
   │   │                    │ ◀────────── │  Recall→Feature→       │ │
   │   │                    │  + trace    │  Score→Rerank          │ │
   │   └────────────────────┘             ├───────────────────────┤ │
   │                                       │ 内存 item store         │ │
   │                                       │ (SoA float 向量)        │ │
   │                                       └───────────────────────┘ │
   └──────────────────────────────────────────────────────────────┘
```

- **React UI**（`web/`）：表现层。加载已编译的引擎，调用 `recommend(persona)`，渲染返回的
  feed 以及逐算子 trace。封面图是构建期抓取的静态资源。
- **WASM 模块**（`src/` 编译产物）：引擎。通过 embind 暴露唯一入口 `recommend`（外加 persona
  元信息）。全部在浏览器标签页内运行。

## 3. 运行时组件模型

| 组件 | 位置 | 职责 |
|---|---|---|
| DAG 调度器 | `src/scheduler.hpp` | 以节点形式持有算子，按顺序执行，将每一级输出接入下一级，收集 trace。 |
| 算子 | `src/{recall,feature,score,rerank}_op.hpp` | 自包含的流水线阶段，接口统一（batch 进 → batch 出，外加一条 trace）。 |
| Item store | `src/item_store.hpp` | 内存候选存储。item 向量以 SoA（一整块扁平 `float` buffer）存放，利于缓存与 SIMD 访问。 |
| 相似度内核 | `src/dot.hpp` | 热点内积：一个标量参考实现 + 一个手写 NEON 路径。 |
| 合成数据 | `src/synthetic.hpp` | 用品类中心向量构建内存 store（作为学习 embedding 的替身 fixture）。 |
| API / 边界 | `src/api.hpp`、`src/bindings.cpp` | `recommend(persona)` 编排、JSON 序列化，以及暴露给 JS 的 embind 绑定。 |
| 前端 | `web/src/**` | React 应用：侧边栏、瀑布流、trace 面板、引擎加载、表现层。 |

## 4. 算子 DAG（请求路径）

级联由四个算子组成。下表基数为演示规模（store = 3,000 条合成 note）：

| 阶段 | 算子 | 进 → 出 | 作用 | 热点 |
|---|---|---|---|---|
| 召回 | `RecallOp` | 3,000 → 300 | 向量相似度候选生成 | **SIMD 内积** |
| 特征 | `FeatureOp` | 300 → 300 | 附加特征（品类匹配、新鲜度、热度） | — |
| 打分 | `ScoreOp` | 300 → 50 | 加权多目标打分，保留 top-k | — |
| 重排 | `RerankOp` | 50 → 12 | 多样性重排（MMR），产出该页 | — |

每个算子产出一条 trace 记录 `{ name, in_count, out_count, latency_us, sample_ids }`。调度器把它们
拼接成 UI 动画所用的 trace。**trace 是一等产物**，从设计之初就在，而非事后加装。

## 5. 请求生命周期

1. UI 跨 WASM 边界调用 `recommend(personaId)`。
2. `api.hpp` 把 persona 解析为按品类的兴趣权重，用品类中心向量构建单位归一化的 query 向量，
   并组装算子流水线（`RecallOp → FeatureOp → ScoreOp → RerankOp`）。
3. `DagScheduler` 用完整候选池作为种子输入，按序执行每个算子，逐级收集 `TraceEntry`。
4. `to_json` 把最终 feed 加 trace 序列化为 JSON 字符串。
5. UI 解析后渲染瀑布流 feed（本地封面图 + 由引擎特征推导的“为什么推荐”一行），并播放 DAG
   trace 漏斗动画。

item store 在首次使用时**只构建一次**（常驻单例）并跨请求复用——没有逐请求的数据加载，与生产
服务存储常驻内存的方式一致。

## 6. 关键架构决策

- **一切皆算子。** 统一的 `run(batch) → batch` 接口加一个 DAG 调度器。正是这一点让流水线既可
  扩展（加节点）又可观测（每个节点自我 trace），无需对任何阶段做特殊处理。
- **引擎返回的是 trace，而不仅是结果。** 可观测性是被设计出来的输出；它是把“隐形后端”变成
  “一眼可见的 demo”的桥梁。
- **内存 SoA 存储；无数据库、无网络。** 向量存于一整块扁平 buffer，使相似度内核可流式访问
  连续内存。这就是服务热点该有的样子；持久化与特征生产属于（不在范围内的）离线那一半。
- **编译为 WebAssembly，而非搭一个 C++ HTTP 服务。** 彻底去掉网络接缝，使整体成为一个静态页。
- **优化下的正确性。** SIMD 召回路径必须与朴素标量参考产出完全一致；奇偶校验报告差异（为零）
  并附带加速比。朴素路径永久保留作参考。
- **静态资源，运行时零第三方调用。** 封面图在构建期一次性从 Unsplash 抓取并提交；运行时站点对
  Unsplash 零 API 调用、也不带 key。

## 7. 横切关注点

- **确定性。** 合成数据使用固定 PRNG 种子；排序并列按 id 打破。运行结果——以及朴素/SIMD 奇偶
  校验——可复现。
- **跨源隔离。** 站点以 `COOP: same-origin` 和 `COEP: require-corp` 提供，解锁浏览器高精度计时器，
  使逐算子 trace 延迟为真实微秒。所有资源同源（引擎 + 本地封面），隔离很直接。
- **内核可移植。** SIMD 路径由 `__ARM_NEON` 保护；在无 NEON 的目标（包括 WASM 构建）上回退到
  标量内核，因此行为处处正确，仅性能不同。

## 8. 技术栈

- **引擎：** C++20，仅标准库；WASM 构建用 Emscripten + embind；召回内核用 ARM NEON intrinsics。
- **前端：** Vite、React 18、TypeScript、`react-masonry-css`。
- **工具链：** Apple clang（原生构建与奇偶校验）、Node.js（构建脚本与无头冒烟测试）。

引擎内部见 [Core_Design](Core_Design_ch.md)，UI 见 [Frontend_Design](Frontend_Design_ch.md)，
构建/运行/部署见 [Operations](Operations_ch.md)。
