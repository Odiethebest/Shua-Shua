# 运维

Shua Shua 的构建、运行、验证与部署。系统概览见 [Architecture](Architecture_ch.md)。

## 1. 工具链

| 工具 | 用于 | 备注 |
|---|---|---|
| Apple clang / 任意 C++20 编译器 | 原生引擎构建 + 奇偶校验 | macOS 默认自带。 |
| Emscripten (`emcc`) | WASM 构建 | 例如 `brew install emscripten`。原生构建不需要。 |
| Node.js 20+ | 前端、构建脚本、冒烟测试 | — |
| CMake | 可选（仅 IDE） | 非必需；所有构建都是单条命令。 |

## 2. 构建与运行

### 2.1 原生引擎（开发 + 奇偶校验）

```bash
clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
```

构建合成数据、运行流水线，打印 feed、DAG trace、`recommend()` JSON，以及朴素-对-SIMD 召回的
奇偶校验 + 加速比。预期在 `-Wall -Wextra` 下无告警。（原生驱动跑的是 **persona** 路径加奇偶校验；
带 `MixOp` 的**画像**路径在浏览器应用里演练。）

### 2.2 WebAssembly 构建

```bash
scripts/build-wasm.sh        # 需要 PATH 中有 emcc
```

单条 `emcc` 命令把 `src/bindings.cpp` 编译为单文件模块 `web/public/shuashua.js`（`.wasm` 内嵌）。
无需 CMake。用 Node 验证：

```bash
node scripts/wasm_smoke.mjs  # 加载模块，打印 personas + trace
```

### 2.3 前端

```bash
cd web
npm install
npm run dev        # 开发服务器 (http://localhost:5173)
npm run build      # 类型检查 + 生产构建 → web/dist
npm run preview    # 提供生产构建
```

### 2.4 封面图（构建期）

封面照片在本地一次性抓取并提交到仓库，因此部署后的站点运行时对 Unsplash 零 API 调用、也不带 key：

```bash
UNSPLASH_KEY=你的access_key node scripts/fetch-covers.mjs
```

默认为每个品类下载**约 120 张随机**竖版照片（`PER_CATEGORY` 环境变量，建议 ≤ 约 180），用
`/photos/random`，**每次跑都是不同的一批**，存到 `web/public/covers/<category>/` 并写出
`manifest.json`（含署名）。抓取是**失败安全**的：它先下载到一个 `covers.new/` 暂存目录，仅在成功时
才原子地换入正式位置，因此一次被限流或失败的运行绝不会毁掉已提交的图库。key 仅在抓取时从
`UNSPLASH_KEY` 环境变量读取。想刷新随时重跑；想扩充图库就调大 `PER_CATEGORY`。

> Unsplash Demo 档为 50 次/小时。阶段 1 每类做 `ceil(N/30)+2` 次随机抓取（`/photos/random` 的
> `count` 上限 30）——默认 N=120 时约 36 次请求，稳稳低于上限。阶段 2 随后为每张保留的图发一次下载
> 追踪 ping；这些也计入上限，因此超过约 50 次后 ping 可能被拒——但图片此时已保存（CDN 不受限流）。

## 3. 配置与密钥

- **运行时无 API key。** 唯一用到 key 的地方是本地 `fetch-covers.mjs` 脚本（一个环境变量，不写入
  文件、不提交、不下发到浏览器）。`.env*` 文件已 gitignore；提交的 manifest 与图片都不含 key。
- **被提交的构建产物：** `web/public/covers/`（图片 + manifest）会提交。生成的引擎
  `web/public/shuashua.js`、`web/dist/`、`node_modules/` 已 gitignore。

## 4. 部署

交付物是 `npm run build` 的静态产物（`web/dist/`），可由任意静态托管提供。两点要求：

1. **文档上（最好是所有响应上）的跨源隔离头**：
   ```
   Cross-Origin-Opener-Policy: same-origin
   Cross-Origin-Embedder-Policy: require-corp
   ```
   它们解锁浏览器高精度计时器，使 trace 延迟为真实微秒。所有资源同源，无需其它设置。
   （Netlify `_headers`、Vercel `headers`、或 nginx `add_header` 均可。）
2. **正确的 MIME 与静态资源服务**（`.wasm`/`.js`/`.json`；多数托管默认即可）。引擎与封面从站点根
   加载。

## 5. 验证清单

- 原生构建在 `-Wall -Wextra` 下干净；`./shuashua` 打印 feed + trace。
- 召回奇偶校验：`result diff = 0`，最大分数差约 1e-7，SIMD 扫描快于标量。
- `node scripts/wasm_smoke.mjs` 打印 personas、合法 JSON、以及漏斗。
- `npm run build` 类型检查并打包干净。
- 构建产物中不含 `api.unsplash.com`——运行时对 Unsplash 零调用。

## 6. 路线图与状态

**v1** — 内核 → DAG → SIMD + diff → WASM → feed UI：

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 — Kernel | `Note`、SoA store、合成数据、朴素召回、stdout | 完成 |
| M1 — DAG | 算子接口、调度器、算子、trace | 完成 |
| M2 — SIMD + diff | NEON 召回内核、朴素/SIMD 奇偶校验（diff = 0）、加速比 | 完成 |
| M3 — WASM | Emscripten 构建、`recommend` 绑定到 JS、JSON 边界 | 完成 |
| M4 — Feed UI | React feed、persona 切换、”why”、DAG trace 面板；小红书网页版重构；深色模式；构建期封面图 | 完成 |
| M5 — Ship | 部署静态构建（带上文的 COOP/COEP 头） | 待办 |
| Stretch | HNSW 索引、int8 量化、学习 embedding、WASM SIMD 召回 | 以后 |

**v2** — 行为驱动画像（规格见 [v2design](../v2design.md)）。用一个实时、会衰减的用户画像取代 v1 的
persona 切换器；画像向量即召回 query。全部已交付：

| 区块 | 内容 | 状态 |
|---|---|---|
| B1 — 画像状态 | 画像模型、标签权重、点击历史、local-storage 持久化 | 完成 |
| B2 — 冷启动 | onboarding 标签选择器；可选；中性画像回退 | 完成 |
| B3 — 实时画像面板 | 点击 = 隐式反馈；兴趣条实时增长；feed 不移动 | 完成 |
| B4 — 兴趣衰减 | 按刷新的乘性衰减（recency） | 完成 |
| B5 — 引擎入口 | `recommend_from_profile` + 重建 WASM；画像*就是*召回 query | 完成 |
| B6 — 刷新 + new/seen 混合 | 手动重排；`MixOp` 组装 exploit + explore（探索/利用） | 完成 |
| B7 — trace 打磨 | 驱动画像标签、重跑闪烁、诚实的 COOP/COEP 延迟 | 完成 |
| — 多样性修复 | `MixOp` 保证的探索保底位（过滤气泡修复）；更强的衰减（0.5）；更大的封面图库 | 完成 |

### 工程约定

- **可读性优先于聪明**；朴素召回路径永久保留作奇偶校验参考。
- **SIMD 内核**以清晰注释的标量参考 + 一条最小 NEON 路径的形式发布，后者必须产出一致的排序输出。
- **注释解释“为什么”**（设计决策与被否决的替代方案），而非“做了什么”。
