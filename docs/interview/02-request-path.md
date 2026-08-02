# 02 · 主链路追踪：点「Refresh recommendations」→ feed 渲染完成

> 所有行号对应 commit `46b9ec3`。本文里的性能数字全部是**本次实测**，
> 测量方法和命令附在 §7。

---

## 1. 全景流程图

```
┌─ 浏览器主线程（single thread，无 Web Worker —— grep 全仓库无 Worker 引用）─────┐
│                                                                              │
│  ① 用户点击 <button className="refresh-btn">                                 │
│     App.tsx:157-164                                                          │
│         │                                                                    │
│         ▼  [同步]                                                            │
│  ② handleRefresh()                          App.tsx:111-115                  │
│     ├─ decayProfile(profile)   ← 读画像      profile.ts:227-236              │
│     │     每个 tag 权重 × DECAY_FACTOR 0.5   profile.ts:225                  │
│     ├─ setProfile(decayed)     ← 写画像(内存)                                │
│     └─ runFeed(decayed)                                                      │
│         │                                                                    │
│         ▼  [同步]                                                            │
│  ③ runFeed(p)                               App.tsx:68-83                    │
│     ├─ setLoading(true)                                                      │
│     ├─ setDrivenBy(summarizeProfile(p))     profile.ts:253-266               │
│     ├─ runId = ++feedRunId.current  ← 竞态令牌  App.tsx:71                   │
│     └─ recommendFromProfile(                                                 │
│            categoryWeights(p),   ← 读画像 → 6 个 float   profile.ts:243-250  │
│            [...p.seenItemIds],   ← 读画像 → id 数组                          │
│            NEW_RATIO /* 80 */)                          profile.ts:43        │
│         │                                                                    │
│         ▼  [★ 唯一的 async 点]                                               │
│  ④ engine.ts:85-94                                                           │
│     await loadEngine()   ─── 首次：注入 <script src="/shuashua.js">          │
│         │                     engine.ts:48-79；之后是已 resolve 的缓存 promise│
│         ▼                                                                    │
│     ╔══════════════ JS → WASM 边界（同步、阻塞主线程）══════════════╗         │
│     ║  engine.recommendFromProfile(                              ║          │
│     ║      "1,0,0.5,0,0,0",   ← CSV string  (13 chars)           ║          │
│     ║      "1367,458,...",    ← CSV string  (随点击增长)          ║          │
│     ║      80)                ← int32                            ║          │
│     ╚════════════════════════════════════════════════════════════╝          │
│                              │                                               │
└──────────────────────────────┼───────────────────────────────────────────────┘
                               ▼
┌─ WASM 线性内存（C++，全程同步）──────────────────────────────────────────────┐
│  ⑤ recommend_from_profile_json()            bindings.cpp:25-55               │
│     ├─ 解析 weights CSV → vector<float>      bindings.cpp:28-39              │
│     └─ 解析 seen CSV   → vector<uint32_t>    bindings.cpp:41-53              │
│         │                                                                    │
│  ⑥ recommend_from_profile()                 api.hpp:149-167                  │
│     ├─ guard：size≠6 或全 0 → 退化成 uniform  api.hpp:155-162                │
│     └─ make_query()：6 权重 × 6 个 64 维 centroid → 1 个 64 维单位向量        │
│                                              api.hpp:67-78                   │
│  ⑦ run_recommendation()                     api.hpp:105-142                  │
│     ├─ shared_data()  ← 常驻 store 单例；仅首次构建 3000 条  api.hpp:60-63    │
│     ├─ 【每次请求都重新 new 5 个算子】        api.hpp:113-136                 │
│     └─ pipeline.run(full_pool(store), trace)  api.hpp:140                    │
│            └─ full_pool()：造 3000 个 Candidate = 72,000 B   api.hpp:82-91   │
│                                                                              │
│  ⑧ DagScheduler::run()                      scheduler.hpp:32-38              │
│     Batch batch = seed;   ← 又拷一份 72,000 B     scheduler.hpp:33           │
│     for (node : nodes_) batch = node->run(batch, trace);   scheduler.hpp:34-36│
│         │                                                                    │
│         ▼  每个 node 都走同一个 template method                               │
│  ⑨ Operator::run()                          operator.hpp:79-96               │
│     steady_clock start → transform(in) → end → push TraceEntry               │
│                                                                              │
│     ┌────────────┬───────────┬──────────┬──────────┬─────────┐               │
│     │ RecallOp   │ FeatureOp │ ScoreOp  │ RerankOp │ MixOp   │               │
│     │ 3000→300   │ 300→300   │ 300→50   │ 50→24    │ 24→12   │               │
│     │ recall_op  │ feature_op│ score_op │ rerank_op│ mix_op  │               │
│     │  :117-134  │  :30-58   │  :38-61  │  :41-81  │ :60-124 │               │
│     └────────────┴───────────┴──────────┴──────────┴─────────┘               │
│       ↑ 忽略入参！                                                            │
│                                                                              │
│  ⑩ to_json()                                api.hpp:186-221                  │
│     手写序列化 → std::string（本次实测 2,040 chars）                          │
└──────────────────────────────┬───────────────────────────────────────────────┘
                               ▼
┌─ 回到浏览器主线程 ───────────────────────────────────────────────────────────┐
│     ╔══════════════ WASM → JS 边界 ══════════════╗                           │
│     ║  返回 JS string（2,040 chars，embind 拷贝）  ║                          │
│     ╚═════════════════════════════════════════════╝                          │
│  ⑪ JSON.parse(...)                          engine.ts:91                     │
│         │                                                                    │
│         ▼  [microtask 里 resolve]                                            │
│  ⑫ .then(r => {...})                        App.tsx:73-77                    │
│     ├─ if (feedRunId.current !== runId) return;  ← 丢弃过期结果  App.tsx:74  │
│     ├─ setRec(r)                                                             │
│     └─ setLoading(false)                                                     │
│         │                                                                    │
│         ▼  React re-render                                                   │
│  ⑬ <TracePanel trace={rec.trace} drivenBy={drivenBy}/>   App.tsx:173         │
│     └─ 对数刻度漏斗 + 逐列 90ms 错峰动画     TracePanel.tsx:30-36, 62-92      │
│  ⑭ <Feed items={rec.feed} onCardClick={...}/>            App.tsx:174         │
│     └─ Masonry（5/4/3/2 列）× 12 个 NoteCard   Feed.tsx:33-46                │
│         └─ 每张卡：contentFor(item)  presentation.ts:190-215（纯前端编的内容） │
│                     coverFor(cat,id) covers.ts:37-44（本地图，lazy <img>）    │
│                                                                              │
│  ⑮ 副作用（不在这条链上，但由 setProfile 触发）                                │
│     useEffect([profile, storageMode]) → saveProfile()    App.tsx:59-61       │
│         └─ 写 sessionStorage（默认）或 localStorage      profile.ts:153-177   │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 逐步调用链（带行号的表格版）

| # | 位置 | 函数 | 做什么 | 同步/异步 |
|---|---|---|---|---|
| 1 | `App.tsx:157-164` | JSX `onClick` | 按钮 `disabled={loading \|\| rec === null}`，所以刷新期间点不动 | 同步 |
| 2 | `App.tsx:111-115` | `handleRefresh` | 衰减 → 存 state → 触发 feed | 同步 |
| 3 | `profile.ts:227-236` | `decayProfile` | 每个 tag 权重 ×0.5；`:232-234` 若全部 <1e-3 则退回 neutral（防零向量） | 同步纯函数 |
| 4 | `App.tsx:68-83` | `runFeed` | 拍快照 `drivenBy`、发竞态令牌、发起调用 | 同步 |
| 5 | `profile.ts:243-250` | `categoryWeights` | 6 个 tag 权重 → 按 `CATEGORY_ORDER` 排成 6 元素数组 | 同步 |
| 6 | `engine.ts:85-94` | `recommendFromProfile` | `await loadEngine()`；`.join(",")` 成 CSV；调 WASM；`JSON.parse` | **async 壳 + 同步核** |
| 7 | `engine.ts:68-79` | `loadEngine` | 首次注入 `<script>` 并 `factory()`；promise 缓存于 `:66` | 首次异步，之后已 resolve |
| 8 | `bindings.cpp:57-59` | embind 注册 | `emscripten::function("recommendFromProfile", …)` | — |
| 9 | `bindings.cpp:25-55` | `recommend_from_profile_json` | 两次 CSV 手工解析（`std::stof`/`std::stoul`，`catch(...)` 吞掉坏字段） | 同步 |
| 10 | `api.hpp:149-167` | `recommend_from_profile` | 防御 guard + 建 query | 同步 |
| 11 | `api.hpp:67-78` | `make_query` | Σ wᵢ·centroidᵢ，再 normalize 成单位向量 | 同步 |
| 12 | `api.hpp:105-142` | `run_recommendation` | 取常驻 store、**组装 pipeline**、跑 | 同步 |
| 13 | `api.hpp:60-63` | `shared_data` | function-local static，**整个 session 只构建一次** | 同步（首次贵） |
| 14 | `api.hpp:82-91` | `full_pool` | 3000 个 Candidate 的种子 batch | 同步 |
| 15 | `scheduler.hpp:32-38` | `DagScheduler::run` | `Batch batch = seed`（拷贝）+ for 循环 | 同步 |
| 16 | `operator.hpp:79-96` | `Operator::run` | 计时 → `transform` → 追加 TraceEntry | 同步 |
| 17 | `recall_op.hpp:117-134` | `RecallOp::transform` | 全量扫 3000 × dot(64) → sort → 取 300 | 同步 |
| 18 | `feature_op.hpp:30-58` | `FeatureOp::transform` | 挂 category_match / recency / popularity | 同步 |
| 19 | `score_op.hpp:38-61` | `ScoreOp::transform` | 线性加权 → sort → 取 50 | 同步 |
| 20 | `rerank_op.hpp:41-81` | `RerankOp::transform` | greedy MMR → 24 | 同步 |
| 21 | `mix_op.hpp:60-124` | `MixOp::transform` | new/seen 配比 10 + 2 个探索位 → 12 | 同步 |
| 22 | `api.hpp:186-221` | `to_json` | `ostringstream` 手写 JSON | 同步 |
| 23 | `engine.ts:91` | `JSON.parse` | 字符串 → JS 对象 | 同步 |
| 24 | `App.tsx:73-77` | `.then` | 竞态检查 → `setRec` | microtask |
| 25 | `App.tsx:171-176` | render | TracePanel + Feed | React 渲染 |
| 26 | `Feed.tsx:33-46` | `Feed` | Masonry × 12 `NoteCard` | 同步 |
| 27 | `NoteCard.tsx:31-33` | `NoteCard` | `contentFor` + `coverFor` | 同步 |

---

## 3. JS ↔ WASM 边界：格式、拷贝、绑定方式

### 3.1 绑定方式

只有**一个**导出函数，用 embind 的最简形式（`bindings.cpp:57-59`）：

```cpp
EMSCRIPTEN_BINDINGS(shuashua) {
    emscripten::function("recommendFromProfile", &recommend_from_profile_json);
}
```

没有 `class_<>`、没有 `register_vector`、没有 `value_object`、没有手动
`.delete()`。TS 侧就一个 interface（`engine.ts:36-38`）。

### 3.2 数据格式（实测）

| 方向 | 参数 | C++ 类型 | JS 类型 | 实测大小 |
|---|---|---|---|---|
| → | `weightsCsv` | `const std::string&` | `string` | `"1,0,0.5,0,0,0"` = **13 chars** |
| → | `seenCsv` | `const std::string&` | `string` | 冷启动 0 chars；每点一次 +≈5 chars，**无上限** |
| → | `newRatio` | `int` | `number` | 标量，`80` |
| ← | 返回值 | `std::string` | `string` | **2,040 chars**（12 条 feed + 5 条 trace） |

设计理由写在 `bindings.cpp:21-24`：6 个 float 用 CSV 比 `register_vector` 省掉
JS 侧手动释放的心智负担。

### 3.3 有几次拷贝？**至少 4 次**

一次请求里，payload 被完整物化 4 遍：

1. **JS string → WASM 线性内存**：embind 对 `std::string` 参数做 UTF-8 编码 +
   拷贝进堆（两个入参各一次）。
2. **CSV → `std::vector`**：`bindings.cpp:28-39` / `:41-53` 逐 token
   `std::stof` / `std::stoul` 重新解析成数值。**这是一次纯解析开销** ——
   6 个 float 先被 JS 格式化成十进制文本、再被 C++ 解析回二进制。
3. **C++ 结果 → `std::string`**：`to_json` 用 `ostringstream` 拼 2,040 字符。
4. **`std::string` → JS string**：embind 把它拷回 JS 堆。
5. **（第 5 次）`JSON.parse`**：`engine.ts:91` 再把 2,040 字符解析成 JS 对象图。

**没有共享内存、没有零拷贝、没有 `HEAPF32.subarray()` 视图。** 全部走
「序列化 → 反序列化」。

这在当前规模下**完全无所谓**（见 §5 的数字：边界开销 ≈ 30µs / 请求），但它是
Step 6 对标时的一个真实差距：工业 feature service 的 RPC 边界会用
Protobuf/FlatBuffers + arena，避免文本往返。**面试里这是个好答案，不是短板** ——
只要你说得出「我知道这是文本往返，在 3,000 candidate 下 30µs 不值得优化；
真到 BFS 那个量级就得换二进制协议 + 零拷贝视图」。

### 3.4 一个可以主动讲的细节：为什么没有零拷贝

即使想零拷贝也只能做入参那 6 个 float（可以用 `HEAPF32` 视图）。**返回方向做不了**
——feed + trace 是变长结构体数组，要么定死二进制布局手工解包，要么就是 JSON。
在 2KB 的量级上，JSON 是正确的工程选择。

---

## 4. 同步 / 异步的精确划分

### 异步的只有三件事

| # | 什么 | 位置 | 何时 |
|---|---|---|---|
| 1 | **引擎加载**（`<script>` 注入 + WASM 实例化） | `engine.ts:48-79` | **仅首次**。之后 `enginePromise`（`:66`）已 resolve，`await` 只花一个 microtask |
| 2 | 封面 manifest `fetch("/covers/manifest.json")` | `covers.ts:18-30` | 与引擎调用**完全并行**，互不依赖；失败则回退渐变色块（`:24-27`） |
| 3 | 封面图 `<img loading="lazy">` | `NoteCard.tsx:52-59` | 卡片进视口才加载 |

### 其余全是同步，而且**阻塞主线程**

`engine.ts:92` 的 `engine.recommendFromProfile(...)` 是一次**普通同步函数调用**。
从 CSV 解析、3,000 次点积、三次 `std::sort`、MMR、到 `to_json`，全部在
**浏览器 UI 线程上跑完才返回**。

**全仓库没有任何 Web Worker**（`grep -rn "Worker" web/src/` 无命中）。

这是一条要能自己说出口的事实。它现在没问题——实测整条链路 ≈ **114µs**，
远低于一帧 16.7ms。但它意味着：把 store 从 3,000 放大到 ~500,000，主线程就会
卡顿（线性外推 ≈ 19ms/请求，直接掉帧）。**正确的下一步是把引擎搬进 Web Worker，
而不是先去优化 kernel。** 这个判断本身就是面试里能加分的东西。

### React 层的时序细节

`handleRefresh` 里 `setProfile` 和 `runFeed` 是**同一个同步块**里发的，但
`runFeed` 拿到的是**参数 `decayed`（值捕获）**，不是 React state，所以不存在
「读到旧 state」的问题（`App.tsx:112-114`）。`saveProfile` 在 passive effect 里
跑（`App.tsx:59-61`），发生在 commit 之后；它和 WASM 调用谁先谁后**不影响正确性**，
因为两者读的是同一个不可变的 `decayed` 对象。

---

## 5. 性能实测：trace 显示的，和 trace 没显示的

### 5.1 WASM（浏览器实际跑的路径），warm-up 后 200 次平均

```
RecallOp    74.90us
FeatureOp    0.90us
ScoreOp      2.89us
RerankOp     1.99us
MixOp        2.73us
            -------
trace 之和   83.42us   ← UI 上 "…µs" 显示的就是这个
JS 侧墙钟   113.74us   ← 含 CSV 解析 + to_json + embind 编解码 + JSON.parse
```

→ **边界开销 ≈ 30.3µs，占墙钟的 27%，而 UI 完全没有展示它。**

> ⚠️ **冷调用会骗人**：未预热的第一次调用，RecallOp 报 383–413µs、
> MixOp 报 150–189µs（见 §7 命令）。那是 V8 的 JIT 预热，不是 C++ 变慢了。
> **面试里报数字一定要说清是不是预热后的。**

### 5.2 Native（arm64，`-O2`），200 次平均

```
full_pool()                        7.49us
scheduler 的 seed 拷贝              1.46us
trace 之和（5 个算子）              74.47us   ← UI 会显示的部分
recommend_from_profile() 总计       84.60us
  └─ 未被 trace 记录的开销          10.13us   (占整次调用的 12%)
to_json()（还在这之上）             18.22us
```

### 5.3 两条重要结论

**(a) trace 系统性低估真实成本。** 三处开销**结构性地**落在所有算子计时器之外：

- `full_pool()` 在 `api.hpp:140` 作为**实参**求值，发生在 `pipeline.run` 之前 →
  7.49µs 不计入任何算子。
- `scheduler.hpp:33` 的 `Batch batch = seed;` 又拷 72,000 字节 → 1.46µs 不计入。
- `to_json()` 在 `bindings.cpp:54` 于 pipeline 之后调用 → 18.22µs 不计入。

Native 上真实成本 ≈ 102.8µs，trace 只报 74.47µs → **trace 覆盖了 72%**。
这不是 bug（trace 的契约就是「每个算子自报」），但**你要知道，被问到时要能承认**。

**(b) 那 3,000 个 Candidate 的种子池，纯粹是给 trace 看的。**

`RecallOp::transform` 的签名是 `transform(const Batch& /*in*/)`
（`recall_op.hpp:117`）——**参数名被注释掉了，它根本不读入参**。理由写在
`:112-116`：召回是候选的源头，它直接流式扫 SoA buffer，走 id 列表反而是散乱 gather。

于是这 72,000 字节被构造一次（`full_pool`）、拷贝一次（`scheduler.hpp:33`），
然后被第一个算子完全忽略。它存在的唯一目的是让 trace 的 `in_count` 能报出
3,000，让 UI 的漏斗有个入口宽度（`TracePanel.tsx:30` 的 `maxN` 就是取
`max(e.in)`）。

**代价 ≈ 9µs / 请求（native），约占 11%。** 这是个真实的、可以坦白讲的设计取舍：
为了可观测性付了 11% 的成本。要去掉也容易——让 `RecallOp` 自报 `in_count =
store.count()`，种子传空 batch 即可。

### 5.4 一个佐证 WASM 走标量的行为证据

| 路径 | 召回耗时 |
|---|---|
| Native **SIMD** end-to-end recall | **44.08µs** |
| Native **scalar** end-to-end recall | **81.16µs** |
| **WASM** RecallOp（预热后） | **74.90µs** |

WASM 落在 native **scalar** 那一档，不在 SIMD 那一档。这和上一轮反汇编得到的
「v128 指令数 = 0」互相印证——**两条独立证据指向同一结论**。

---

## 6. 用户画像在这条链路上何时被读、何时被写

```
时间轴 ─────────────────────────────────────────────────────────────────►

[点击 Refresh]
   │
   ├─ 读  profile (React state)                        App.tsx:112
   ├─ 读  profile.tagWeights → 衰减                     profile.ts:229
   ├─ 写  profile (内存/React state)  = decayed        App.tsx:113
   │
   ├─ 读  decayed.tagWeights → summarizeProfile         App.tsx:70
   ├─ 读  decayed.tagWeights → categoryWeights (6 float) App.tsx:72
   ├─ 读  decayed.seenItemIds → CSV                     App.tsx:72
   │
   │      ╔═══ 跨进 WASM ═══╗
   │      ║  C++ 全程只读传进去的那份快照，                ║
   │      ║  【不保存、不回写任何用户状态】                 ║
   │      ║  唯一的常驻状态是只读的 item store 单例         ║
   │      ║  （api.hpp:60-63）                          ║
   │      ╚═════════════════╝
   │
   └─ 写  持久化 → sessionStorage / localStorage        App.tsx:59-61
             → saveProfile()                            profile.ts:153-177
```

### 三条要点

1. **引擎侧零写入。** C++ 从头到尾没有任何用户状态的写路径。画像的读写全部在
   TypeScript 里。这条链上 C++ 是**纯函数**：`(weights, seen, ratio) → JSON`。

2. **画像的另一条写路径不在这条链上。** 点击卡片走
   `handleCardClick`（`App.tsx:94-96`）→ `recordClick`（`profile.ts:197-207`），
   它只 bump 权重和 seen 集合，**故意不重跑 feed**（`App.tsx:92-93` 的注释明写
   "The FEED does not move — it re-ranks only on refresh"）。
   所以「点击」和「刷新」是两个独立的写入时机：**点击写画像，刷新读画像**。

3. **`seenItemIds` 会无界增长。** 每次点击 `+1`（`profile.ts:205`），从不清理，
   而且每次请求都全量序列化成 CSV 跨边界（`App.tsx:72`）。点 200 次 ≈ 1KB CSV。
   实际用不到那么多，但这是个**真实的、没做上限的地方** —— 被问到要认。

---

## 7. 复现命令

```bash
# (1) Native：pipeline + trace + parity/speedup
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua

# (2) WASM 冷调用（会看到 JIT 预热的虚高数字）
node scripts/wasm_smoke.mjs

# (3) WASM 预热后 200 次平均（§5.1 的数字）—— 见下方脚本
# (4) Native traced vs untraced 拆解（§5.2 的数字）—— 见下方脚本
```

§5.1 / §5.2 的两个测量脚本是本次盘点临时写的，**不在仓库里**（避免往
业务代码里塞测量代码）。它们的逻辑分别是：

- **WASM 版**：`new Function` eval `web/public/shuashua.js` 拿到工厂 → 预热
  50 次 → 计时 200 次 `recommendFromProfile("1,0,0.5,0,0,0","",100)`，
  同时累加每次返回 JSON 里的 `trace[].latency_us`，与 `process.hrtime` 墙钟对比。
- **Native 版**：`#include "api.hpp"`，分别单独计时 `full_pool()`、
  `Batch b = seed`、`recommend_from_profile()` 全程、`to_json()`，
  并累加 trace 之和。

> 【待验证】如果要把这两个测量固化成可复现的产物，需要在仓库里加
> `bench/` 目录（约 1 小时）。当前它们是一次性脚本，**结论可信但不可一键复现**。

---

## 8. 这一步最重要的四个发现

1. **整条 pipeline 同步阻塞浏览器主线程，没有 Web Worker。**
   现在无害（114µs ≪ 16.7ms 一帧），但这是扩容时第一个会撞墙的地方。
   能主动说出「下一步该搬进 Worker，而不是继续优化 kernel」是加分项。

2. **UI 显示的延迟只覆盖真实成本的 ~72%。** `full_pool`（7.5µs）、
   scheduler 的 seed 拷贝（1.5µs）、`to_json`（18.2µs）结构性地落在所有算子
   计时器之外，WASM 侧还有 30µs 的边界开销完全不可见。

3. **那个 3,000 元素的种子 batch 是「trace 道具」。**
   `RecallOp::transform` 把入参名注释掉了（`recall_op.hpp:117`），根本不读它。
   72KB 构造 + 72KB 拷贝，只为了让漏斗图有个入口宽度。约 11% 的成本换可观测性
   —— 这是个真实取舍，讲出来比藏着好。

4. **WASM 的召回耗时（74.9µs）精确落在 native scalar（81.2µs）那一档，而非
   native SIMD（44.1µs）那一档。** 这是继反汇编之后的第二条独立证据，
   再次确认线上跑的是标量路径。

---

**下一步**：Step 3 — 核心算法逐个拆（5 个算子 + DAG 调度器 + SIMD kernel +
SoA 存储 + 兴趣衰减，含复杂度、备选方案、工业界对应、各三个追问）。
