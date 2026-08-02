# 01 · 代码地图（Code Map）

> 本文档由**逆向阅读代码**产出，不采信 README / Doc 的说法。所有结论后面都标注了
> 代码依据（`文件:行`）。文档与代码不一致的地方集中在文末「文档 vs 实现 差异表」。
>
> 盘点时的 HEAD：`27c34c9`（`fix(web): mobile-responsive layout`）

---

## 1. 一句话概括

这是一个**只做 serving、不做 training** 的推荐引擎：C++20 header-only 写的
cascade pipeline（Recall → Feature → Score → Rerank → Mix），通过 Emscripten
编译成单文件 WASM，被一个 React SPA 在浏览器里直接调用；没有后端、没有数据库、
没有网络请求（`src/api.hpp:60-63` 的 resident store 是进程内单例）。

**规模**：C++ 引擎 1,393 行（其中代码 799 行，注释 594 行 ≈ 43%），前端 TS/TSX
1,314 行 + CSS 1,007 行，脚本 226 行。

---

## 2. 目录结构树（含职责）

```
Shua Shua/
├── src/                        ← C++ 引擎（唯一的「核心」目录）
│   ├── note.hpp          (26)  item 元数据 POD：id / category / popularity / age_days
│   ├── item_store.hpp    (49)  SoA 候选库：一个扁平 float buffer + 平行 notes 数组
│   ├── synthetic.hpp    (119)  离线数据 fixture：按 category 生成 centroid + 噪声向量
│   ├── dot.hpp           (91)  内积 kernel：dot_scalar（参考）+ dot_simd（NEON）
│   ├── operator.hpp     (115)  算子统一接口 + Candidate / Batch / TraceEntry 定义
│   ├── pipeline.hpp     (42)  Pipeline：顺序执行 nodes_，串联 batch
│   ├── recall_op.hpp    (141)  Stage 1 召回：全量打分 + top-k
│   ├── feature_op.hpp    (63)  Stage 2 特征：给候选挂 3 个特征列
│   ├── score_op.hpp      (66)  Stage 3 打分：线性加权 + top-k 截断
│   ├── rerank_op.hpp     (87)  Stage 4 重排：greedy MMR（category 冗余）
│   ├── mix_op.hpp       (132)  Stage 5 混排：new/seen 配比 + exploration floor
│   ├── api.hpp          (221)  编排层：配置常量 + 组装 pipeline + 手写 JSON 序列化
│   ├── bindings.cpp      (59)  embind 绑定（仅 Emscripten 编译）
│   └── main.cpp         (182)  native driver + M2 parity/speedup 诊断
│
├── web/                        ← React 前端（presentation + 用户画像状态）
│   ├── index.html              入口；含防闪烁的 pre-paint theme 脚本
│   ├── vite.config.ts          Vite 配置；dev/preview 注入 COOP+COEP
│   ├── package.json            react 18 / react-masonry-css / vite 6 / ts 5.6
│   ├── public/
│   │   ├── shuashua.js  (219KB) **已提交的 WASM 产物**（wasm base64 内嵌）
│   │   ├── _headers            Cloudflare Pages 的 COOP/COEP + 缓存策略
│   │   └── covers/             650 张本地封面图 + manifest.json
│   └── src/
│       ├── main.tsx      (10)  React 挂载
│       ├── App.tsx      (185)  顶层状态机：profile / feed / 冷启动门禁 / 竞态防护
│       ├── engine.ts     (94)  **JS↔WASM 边界**：<script> 注入 + 工厂 + JSON.parse
│       ├── profile.ts   (266)  **用户画像模型**：tag 权重 / 点击累加 / 衰减 / 持久化
│       ├── presentation.ts(215) 纯前端伪内容（标题/作者/likes/why 文案）
│       ├── covers.ts     (44)  封面 manifest 读取 + 按 id 确定性选图
│       ├── styles.css  (1007)  全部样式
│       └── components/
│           ├── ColdStart.tsx    (89)  冷启动 tag picker + remember me
│           ├── Feed.tsx         (47)  masonry 布局容器
│           ├── NoteCard.tsx     (93)  单卡片；点击 = implicit feedback
│           ├── Sidebar.tsx      (95)  品牌 / 画像面板容器 / 重置 / 主题
│           ├── ProfilePanel.tsx (72)  实时画像条形图
│           └── TracePanel.tsx  (103)  **Pipeline trace 漏斗可视化**
│
├── scripts/
│   ├── build-wasm.sh     (29)  单条 emcc 命令产出 web/public/shuashua.js
│   ├── wasm_smoke.mjs    (36)  Node 里加载 WASM、校验 JSON 契约
│   └── fetch-covers.mjs (161)  一次性抓 Unsplash 封面（构建期，产物已提交）
│
├── Doc/en/ + Doc/ch/           6 篇双语文档（Architecture / Core_Design /
│                               Algorithm / Interview / Frontend / Operations）
├── README.md            (367)  项目门面
├── CMakeLists.txt        (28)  **仅为 IDE 服务**，不参与任何官方构建
└── cmake-build-debug/          CLion 生成物（未 gitignore，属于噪音）
```

**注意**：用户在任务里提到的 `docs/algo.md` 在当前仓库里**不存在**——它在
`aa0f063` / `33cebf4` 两个 commit 里被移动并改名成了 `Doc/en/Algorithm.md`。

---

## 3. 模块依赖关系与三层边界

### 3.1 C++ 内部 include 图（真实的，逐文件核对过 `#include`）

```
                        note.hpp  (无依赖)
                           ↑
                    item_store.hpp
                     ↑     ↑     ↑
        synthetic.hpp │     │     │
                      │     │     │
   dot.hpp ─────→ recall_op.hpp   │
                      │  feature_op.hpp  rerank_op.hpp  mix_op.hpp
   operator.hpp ──────┴──────┴──────────┴──────────────┴──── score_op.hpp
        ↑
   pipeline.hpp
        ↑
      api.hpp  ←── 汇聚点：include 了上面所有东西
        ↑
    ┌───┴────┐
 main.cpp   bindings.cpp
 (native)   (emscripten only)
```

关键点：

- **全部 header-only**（`#pragma once`，12 个 `.hpp` 全是 inline / 模板类），
  只有 2 个 translation unit：`main.cpp` 和 `bindings.cpp`。这就是为什么
  native 构建能是一条 `clang++ src/main.cpp` 命令。
- `operator.hpp` 是零依赖的接口层（只 include 标准库），5 个算子全部只依赖它 +
  `item_store.hpp`，**算子之间互不依赖**——这是后面 Step 6「算子解耦」要核查的
  重要事实。
- `api.hpp` 是唯一的汇聚点，也是唯一知道「pipeline 长什么样」的地方
  （`src/api.hpp:113-136`）。

### 3.2 三层边界在哪

| 层 | 物理位置 | 语言 | 边界形式 |
|---|---|---|---|
| **引擎层** | `src/*.hpp`（除 `api.hpp` 的 JSON 部分） | C++20 | 纯内存计算，无 IO |
| **编排 + 序列化层** | `src/api.hpp` + `src/bindings.cpp` | C++20 + embind | **CSV 字符串进 / JSON 字符串出** |
| **前端层** | `web/src/**` | TS + React | `JSON.parse` 后是普通 JS 对象 |

边界的**精确签名**（`src/bindings.cpp:25-27, 57-59`）：

```cpp
static std::string recommend_from_profile_json(const std::string& weights_csv,
                                               const std::string& seen_csv,
                                               int new_ratio);
EMSCRIPTEN_BINDINGS(shuashua) {
    emscripten::function("recommendFromProfile", &recommend_from_profile_json);
}
```

对应 TS 侧（`web/src/engine.ts:36-38, 85-94`）：

```ts
interface EngineModule {
  recommendFromProfile(weightsCsv: string, seenCsv: string, newRatio: number): string;
}
```

**整个跨语言边界只有一个函数、三个参数、全部是字符串/整数标量。** 没有共享内存、
没有指针、没有 `register_vector`。理由写在 `src/bindings.cpp:21-24`：6 个 float
用 CSV 比 embind 的 vector 绑定省掉手动 `.delete()` 的麻烦。

### 3.3 一个必须记住的边界事实（面试会被问）

> **用户画像的模型本身不在 C++ 里，在 TypeScript 里。**

- 画像数据结构、点击累加、衰减因子、持久化，全部在 `web/src/profile.ts`
  （`Profile` 接口 `:51-56`、`recordClick` `:197-207`、`decayProfile` `:227-236`、
  `DECAY_FACTOR = 0.5` `:225`、`NEW_RATIO = 80` `:43`、localStorage/sessionStorage
  读写 `:103-191`）。
- C++ 引擎**每次调用都是无状态的**：它只收到当次的 6 个 category 权重 +
  seen id 列表，不保存任何用户状态。唯一的进程内状态是那个只读的 item store 单例
  （`src/api.hpp:60-63`）。

也就是说：**两个「排序策略参数」（衰减 0.5、new_ratio 80）住在前端，而不是引擎里。**
这在 Step 4（设计决策）和 Step 6（对标 User Data Accessor）都会再出现。

---

## 4. 代码量统计与「核心文件」标注

### 4.1 C++（`src/`）

| 文件 | 总行 | 代码行 | 注释占比 | 核心？ |
|---|---:|---:|---:|:---:|
| `api.hpp` | 221 | 152 | 31% | **★ 核心（编排 + 全部配置常量）** |
| `main.cpp` | 182 | 132 | 27% | ☆ 半核心（parity check 在这） |
| `recall_op.hpp` | 141 | 79 | 44% | **★ 核心** |
| `mix_op.hpp` | 132 | 88 | 33% | **★ 核心** |
| `synthetic.hpp` | 119 | 62 | 48% | ★ 核心（数据 fixture，决定引擎看到什么） |
| `operator.hpp` | 115 | 59 | 49% | **★★ 最核心（接口契约 + trace 契约）** |
| `dot.hpp` | 91 | 31 | 66% | **★★ 最核心（唯一的 hot kernel）** |
| `rerank_op.hpp` | 87 | 46 | 47% | **★ 核心** |
| `score_op.hpp` | 66 | 41 | 38% | **★ 核心** |
| `feature_op.hpp` | 63 | 30 | 52% | **★ 核心** |
| `bindings.cpp` | 59 | 36 | 39% | ☆ 胶水 |
| `item_store.hpp` | 49 | 16 | 67% | **★★ 最核心（SoA 布局契约）** |
| `pipeline.hpp` | 42 | 19 | 55% | **★ 核心** |
| `note.hpp` | 26 | 8 | 69% | ☆ 数据定义 |
| **合计** | **1,393** | **799** | **43%** | |

「核心 = 改了会让引擎行为变」的判定：

- **★★ 三个契约文件**：`operator.hpp`（改了 5 个算子全要改 + 前端 trace 渲染
  要改）、`item_store.hpp`（SoA 布局，改了 kernel 要改）、`dot.hpp`（改了要重跑
  parity check）。
- **★ 行为文件**：5 个算子 + `pipeline.hpp` + `api.hpp`。特别注意
  `api.hpp:45-52` 那 8 个 `constexpr`——**pipeline 的全部可调参数都硬编码在这 8 行里**：

  ```cpp
  constexpr std::uint32_t kPerCategory   = 500;   // 每类 500 条 → 总共 3000
  constexpr std::uint32_t kSeed          = 42;
  constexpr std::size_t   kRecallK       = 300;
  constexpr std::size_t   kScoreK        = 50;
  constexpr std::size_t   kPageSize      = 12;
  constexpr float         kRerankLambda  = 0.7f;
  constexpr std::size_t   kRerankPool    = 24;
  constexpr std::size_t   kExploreFloor  = 2;
  ```

  改任何一个都要**重新编译 + 重新 build WASM + 重新 commit `shuashua.js`**。
  这一点直接对应 Step 6(c) 要评估的「图/配置外置」缺口。

### 4.2 前端（`web/src/`）

| 文件 | 行数 | 核心？ |
|---|---:|:---:|
| `styles.css` | 1,007 | 纯展示 |
| `profile.ts` | 266 | **★ 核心（画像模型 + 两个排序参数）** |
| `presentation.ts` | 215 | 纯展示（全部是编的内容） |
| `App.tsx` | 185 | **★ 核心（调用时序、竞态防护）** |
| `TracePanel.tsx` | 103 | ★ 半核心（trace 是产品的一半） |
| `engine.ts` | 94 | **★ 核心（WASM 边界）** |
| `Sidebar.tsx` / `NoteCard.tsx` / `ColdStart.tsx` / `ProfilePanel.tsx` / `Feed.tsx` / `covers.ts` / `main.tsx` | 95 / 93 / 89 / 72 / 47 / 44 / 10 | 展示 |
| **TS/TSX 合计** | **1,314** | |

---

## 5. 构建链路：C++ 源码 → 浏览器运行

### 5.1 两条构建路径（两个 front door，同一份 `api.hpp`）

```
                        src/api.hpp（唯一编排入口）
                          ↙                    ↘
        ┌─────────────────┐                  ┌──────────────────┐
        │  main.cpp       │                  │  bindings.cpp    │
        │  (native driver)│                  │  (embind glue)   │
        └────────┬────────┘                  └────────┬─────────┘
                 │                                     │
   clang++ -std=c++20 -O2                 emcc -std=c++20 -O2 -lembind
   src/main.cpp -o shuashua               -sMODULARIZE=1 -sEXPORT_NAME=ShuaShua
                 │                        -sSINGLE_FILE=1 -sENVIRONMENT=web,node
                 ↓                                     ↓
          ./shuashua（97KB 可执行）        web/public/shuashua.js（219,827 B）
          打印 feed + trace + JSON                     │  ← 已 git 提交
          + M2 parity/speedup 诊断                     │
                                                       ↓
                                          node scripts/wasm_smoke.mjs（可选自检）
                                                       ↓
                                          web/index.html
                                            → /src/main.tsx
                                              → App.tsx
                                                → engine.ts 用 <script> 注入
                                                  /shuashua.js
                                                  → window.ShuaShua() 工厂
                                                    → recommendFromProfile(...)
                                                       返回 JSON string
                                                       ↓
                                       npm run build = tsc --noEmit && vite build
                                                       ↓
                                                   web/dist/
                                                       ↓
                                          Cloudflare Pages → shuashua.odieyang.com
```

### 5.2 每一步的配置文件与关键事实

| 步骤 | 命令 / 文件 | 关键事实（代码依据） |
|---|---|---|
| 1. Native 构建 | `clang++ -std=c++20 -O2 src/main.cpp -o shuashua` | 只编一个 TU，因为其余全是 header-only |
| 2. IDE 用的 CMake | `CMakeLists.txt` | 注释自陈「keep this to a single target」，**不参与官方构建**；且它列的头文件清单里**漏了 `mix_op.hpp`**（`CMakeLists.txt:14-26`） |
| 3. WASM 构建 | `scripts/build-wasm.sh:24-27` | 一条 `emcc`，无 CMake。`-sSINGLE_FILE=1` 把 .wasm 变成 base64 内嵌进 .js，所以运行时只有一个文件要拿 |
| 4. WASM 产物 | `web/public/shuashua.js` | **被有意提交**（`.gitignore:19-24` 明写理由：CF Pages 的 CI 镜像没有 Emscripten）。当前与 `src/` 同属 commit `b3d39b9`，**未过期** |
| 5. WASM 自检 | `node scripts/wasm_smoke.mjs` | 用 `new Function` eval 那个 classic script 拿到工厂（`scripts/wasm_smoke.mjs:19-22`），因为它不是 ESM |
| 6. 前端类型检查+打包 | `web/package.json:8` → `tsc --noEmit && vite build` | 类型检查和打包是分开的两步 |
| 7. 跨域隔离（开发） | `web/vite.config.ts:12-15, 24-25` | dev/preview 都注入 COOP `same-origin` + COEP `require-corp` |
| 8. 跨域隔离（生产） | `web/public/_headers` | Vite 原样拷进 `dist/`，CF Pages 读它 |
| 9. 部署 | Cloudflare Pages（无配置文件在仓库里） | 从仓库直接 build，所以第 4 步的「提交产物」是必需的 |

### 5.3 COOP/COEP 为什么在这个项目里是**功能性**的，不是安全摆设

`TracePanel.tsx:25` 读 `window.crossOriginIsolated`；如果为 false，面板会显示
"latencies read ~0µs here — the browser coarsens its timer"（`:93-98`）。
原因是 WASM 里 `std::chrono::steady_clock` 最终映射到 `performance.now()`，
未跨域隔离时浏览器把它粗化到毫秒级，**µs 级的算子耗时会全部读成 0**。
所以这两个 header 是「让 trace 有数字」的前提条件，不是可选的加固。

---

## 6. 运行时加载时序（一句话版，Step 2 会展开）

`index.html` 里的 pre-paint 脚本先定主题 → React 挂载 → `App` 从
`loadProfile()` 恢复画像（`App.tsx:38`）→ 若 `onboarded` 为 false 直接渲染
`<ColdStart/>`（`App.tsx:137-139`）→ 否则 mount effect 调 `runFeed`
（`App.tsx:87-90`）→ `engine.ts` 首次调用时才注入 `<script>` 并 `await` 工厂
（`engine.ts:66-79`，之后 promise 被缓存）→ C++ 侧首次调用时才 lazily 构建
3,000 条 item store（`api.hpp:60-63` 的 function-local static）。

**即：item store 的构建成本只在整个 session 的第一次请求上付一次。**

---

## 7. 文档 vs 实现 差异表

> 规则：只列**代码里能指到反证**的条目。

> 状态标注：`✅ 已修` = 已在 commit `46b9ec3`
> (`docs: distinguish native vs wasm targets, correct planning-era numbers`) 中改掉。

| # | 出处 | 文档/注释怎么说 | 代码实际是什么 | 严重度 | 状态 |
|---|---|---|---|---|---|
| 1 | `README.md:7` | SIMD badge 写 `NEON / AVX2` | `dot.hpp:9-11, 48-90` **只有 `__ARM_NEON` 一条路径**，`#else` 直接 fallback 到 `dot_scalar`。仓库里搜不到任何 AVX2/`__m256`/immintrin 代码 | **高** | ✅ 已修（badge 改为 `NEON (native only)`） |
| 1b | `README.md:206-214` | 未区分 native / WASM target | **线上 WASM 里点积走标量**：emcc 既不定义 `__ARM_NEON` 也不定义 `__wasm_simd128__`（实测），且 `build-wasm.sh` 无 `-msimd128`。从提交的 `shuashua.js` 里抽出内嵌 wasm 反汇编，**v128/f32x4 指令数 = 0** | **高**（"浏览器里 SIMD 加速"是红线话术） | ✅ 已修（README 明写 native-only + WASM 走 scalar fallback） |
| 1c | `README.md:214` | 只贴 `speedup 3.6x` | 3.6× 是 **scan-only**；end-to-end recall 只有 **1.84×**（top-k `std::sort` 两条路径共享且未向量化） | 中 | ✅ 已修（两个数都贴，并说明差异来源） |
| 2 | `src/recall_op.hpp:15` | `// RecallOp — Target: ~1,000,000 -> ~5,000` | 实际 `3,000 → 300`（`api.hpp:45` `kPerCategory=500` ×6 类；`api.hpp:47` `kRecallK=300`） | **高**（数量级差 300 倍，且这是**代码注释**，比 README 更容易被面试官读到） | ✅ 已修 |
| 3 | `src/feature_op.hpp:14` | `// Target: ~5,000 -> ~5,000` | 实际 `300 → 300` | 中 | ✅ 已修 |
| 4 | `src/score_op.hpp:11` | `// Target: ~5,000 -> ~50` | 实际 `300 → 50`（`api.hpp:48` `kScoreK=50`） | 中 | ✅ 已修 |
| 5 | `src/rerank_op.hpp:14` | `// Target: ~50 -> ~12` | profile 路径下实际是 `50 → 24`（`api.hpp:131` 传 `kRerankPool=24`），再由 MixOp `24 → 12` | 中 | ✅ 已修 |
| 6 | `src/pipeline.hpp:8-23` 类名 `Pipeline` | 注释自己承认："The Shua Shua cascade is a linear chain… It is a degenerate DAG: one path, no branches" | **代码诚实**，但**类名叫 `Pipeline`、UI 叫 "DAG pipeline trace"（`TracePanel.tsx:39-51`）、README 叫 "DAG of operators"**。命名对外暗示的能力 > 实现 | **高**（Step 6(a) 的核查对象，直接决定能不能说「DAG 调度」） | 未处理（等 Step 6 决策） |
| 7 | `CMakeLists.txt:14-26` | 头文件清单 | 漏了 `src/mix_op.hpp`（第 5 个算子） | 低（只影响 IDE 索引） | 未处理 |
| 8 | `README.md:214` | `dot scan: naive 62us \| simd 17us \| speedup 3.6x` | 实测可复现：**naive 64.89us / simd 17.95us / 3.61×**（同一台机、200 次平均），run-to-run 在 3.6–3.7× 之间浮动 | 【已验证】 | ✅ 已修（换成实测输出 + 标注浮动区间） |
| 9 | 任务描述 | `docs/algo.md` | 该文件不存在，已改名为 `Doc/en/Algorithm.md`（commit `33cebf4`） | 低 | n/a |
| 10 | `src/api.hpp:23-30` 注释 | "This is the presentation/glue layer, NOT engine algorithm" | 但 `api.hpp:105-142` 的 `run_recommendation` **是 pipeline 拓扑的唯一定义处**，`api.hpp:45-52` 是全部超参。这不是 glue，是**图定义 + 配置中心** | 中（自我定位偏低，实际它是核心） | 未处理 |
| 11 | `Doc/en/*.md`（6 篇） | 尚未逐篇核对 | 只核了 README。Doc/ 下若也写了 AVX2 / 规划期数字，同样要改 | 待办 | 未处理 |

### 7.1 关键 kernel 事实（实测，供后续步骤引用）

| 事实 | 值 | 依据 |
|---|---|---|
| `dot` 的 `dim` | **64**（item embedding 维度，不是那 6 个兴趣权重） | `item_store.hpp:21`；`recall_op.hpp:47` 传 `ItemStore::DIM` |
| 6 个 float 的角色 | **权重**，被 `make_query` 折成 1 个 64 维单位向量 | `api.hpp:67-78` |
| 一次召回的 dot 次数 | **3,000**（= 全量扫描） | `recall_op.hpp:44-49`，`store.count()` = 500×6 |
| 一次召回的乘加次数 | **192,000**（3,000 × 64） | 同上 |
| 一次 feed 请求的召回次数 | **1** | `api.hpp:114` |
| WASM 里 v128 指令数 | **0** | 从提交的 `shuashua.js` 抽出内嵌 wasm（175,497 B，SHA-256 `fde6772a…`，与当前源码重建**逐字节一致**），`wasm-dis` 后 grep `v128\|f32x4\|i32x4` = 0 |
| 加 `-msimd128` 能否救回 | **不能**。隔离 TU 实测：baseline 与 `-msimd128` 都是 `8 f32.load / 4 f32.mul / 4 f32.add`；只有再加 `-ffast-math` 才出现 `v128.load / f32x4.mul / f32x4.add` | 印证 `dot.hpp:26-30` 自己的说法（clang 不在无 fast-math 时向量化浮点归约） |
| parity check 位置 | `src/main.cpp:70-147`（`run_recall_diagnostics`），由 `main.cpp:180` 调用。**仓库无 `tests/` 目录** | 实跑通过：diff=0，max delta 2.98e-07，verdict PASS |
| parity check 的两个短板 | (a) FAIL 时只打印、无 `assert`、退出码仍为 0 → 不能直接 gate CI；(b) WASM 上两条路径是同一个函数，parity 是平凡真 | `main.cpp:145-146` |

---

## 8. 这一步最重要的三个发现

1. **`Pipeline` 不是 DAG 调度器，是一个 `for` 循环**
   （`src/pipeline.hpp:32-38`：`for (node : nodes_) batch = node->run(batch, trace);`）。
   代码注释自己写清楚了「degenerate DAG: one path, no branches」，但类名、UI 文案、
   README 都在用 "DAG" 这个词。**这是本次盘点里最需要你决定怎么办的一件事**——
   Step 6(a) 会给出「改成真拓扑排序要多少工作量」的评估。

2. **图结构和全部超参都硬编码在 `src/api.hpp` 45–52 行和 113–136 行**，改一个数字
   要重编 C++ + 重 build WASM + 重 commit 产物。对标字节 BFS 的 **DAG-DSL** 时，
   这是差距最大的一环，也是 Step 6(c) 要重点评估投入产出比的地方。

3. **代码里的算子头注释全部是「规划期的数量级」，和实际配置差 1–2 个数量级**
   （`~1,000,000 -> ~5,000` vs 实际 `3,000 → 300`）。这些注释在代码里，面试官如果
   看仓库会直接读到。README badge 写的 `NEON / AVX2` 同理——**仓库里没有一行 AVX2 代码**。
   这两条建议在准备期就顺手改掉（改注释和 badge 不算改业务代码）。

---

**下一步**：Step 2 — 主链路追踪（点「重新推荐」→ feed 渲染完成的完整调用链，
含 JS↔WASM 边界的数据格式与拷贝分析）。
