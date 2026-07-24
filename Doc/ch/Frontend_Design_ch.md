# 前端设计

前端（`web/`）是一个 Vite + React + TypeScript 单页应用，把 WASM 引擎的输出渲染为小红书风格的
feed，并带一个实时的 DAG trace 面板。它是纯表现层——JS 侧没有任何排序逻辑。它*确实*持有的是
**用户画像（profile）**（v2）：它用冷启动标签与点击构建画像，并把按品类的权重交给引擎去做推荐。

## 1. 设计语言

视觉设计沿用小红书网页版，参考开源项目 XiaoShiLiu 的比例（组件为全新编写，非拷贝）：

- **配色**（CSS 变量）：暖红强调色 `#ff2442`，克制使用（logo、关键动作如刷新、out 计数），配以
  一套中性的文本/背景/边框 token。
- **布局：** 固定左侧栏（约 232px）+ 主内容区——网页版小红书的形态，而非移动端底部标签。
- **feed：** 响应式瀑布流，无边框、圆角卡片。
- **深色模式：** 通过 `[data-theme="dark"]` 提供完整第二主题，可切换。

## 2. 布局与组件

```
┌────────────┬───────────────────────────────────────────────┐
│  侧边栏     │  Explore · For you            [↻ Refresh]      │
│  ┌──────┐  │  ┌─────────────────────────────────────────┐  │
│  │ 刷    │  │  │ DAG pipeline trace（可折叠漏斗）          │  │
│  │ 品牌  │  │  └─────────────────────────────────────────┘  │
│  ├──────┤  │  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐            │
│  │ 画像  │  │  │卡片│ │卡片│ │卡片│ │卡片│ │卡片│  瀑布流    │
│  │ 面板  │  │  └────┘ └────┘ └────┘ └────┘ └────┘  (2–5 列) │
│  ├──────┤  │  ...                                            │
│  │主题   │  │                                                │
│  └──────┘  │                                                │
└────────────┴───────────────────────────────────────────────┘
```

| 组件 | 文件 | 职责 |
|---|---|---|
| `App` | `src/App.tsx` | 持有状态：用户画像（加载、持久化、播种、衰减）、当前推荐结果、主题。仅在**显式事件**上重跑 feed——首次加载与刷新按钮——绝不在每次点击时重跑。画像未完成 onboarding 前显示冷启动选择器。 |
| `ColdStart` | `src/components/ColdStart.tsx` | 首访标签选择器（可选）。选中的标签为初始画像播种；跳过则回退到中性（多样）画像。无论哪种，画像都被标记为已 onboarding。 |
| `Sidebar` | `src/components/Sidebar.tsx` | 品牌、**实时画像面板**（取代 v1 的 persona 切换器）、明暗切换。 |
| `ProfilePanel` | `src/components/ProfilePanel.tsx` | 画像的标签权重以条形展示，按当前最大值归一化；随点击增权而实时增长、重排序。 |
| `Feed` | `src/components/Feed.tsx` | `react-masonry-css` 瀑布流；响应式列数；加载封面 manifest。 |
| `NoteCard` | `src/components/NoteCard.tsx` | 单卡：封面 → 两行标题 → "why" 说明 → 作者 + 点赞。点击卡片是隐式反馈（为该 item 的品类标签增权）。 |
| `TracePanel` | `src/components/TracePanel.tsx` | 可折叠 DAG 漏斗：逐级 in→out、延迟、可选 detail、sample ids；标出驱动本次运行的画像；数值变化时重放错峰揭示 + 闪烁。 |

## 3. 引擎集成（`engine.ts`）

引擎以单文件、经典脚本形式发布（`public/shuashua.js`，wasm 内嵌），挂载一个全局 `ShuaShua()`
工厂。`engine.ts` 用 `<script>` 标签加载它，并暴露带类型的封装：

- `recommendFromProfile(categoryWeights, seenIds, newRatio)` → `Recommendation`
  ——**实时路径。** 画像的按品类权重与已看 item 集合以 **CSV 字符串**跨越边界（一个固定、极小的
  float 向量——最简单稳健的 embind 跨界方式，无需 `register_vector` 的繁文缛节）；C++ 构建 query
  向量并运行 DAG。
- `recommend(personaId)` / `recommendSimilar(itemId)` → `Recommendation` —— v1 的 persona 与 item
  相似度路径。作为备用 query 来源保留在引擎中；**当前 UI 不调用它们。**
- `getPersonas()` → `Persona[]`。

三者返回相同的 JSON 形状（`{ persona, feed[], trace[] }`）。

**为何用 `<script>` 标签而非 ESM `import`：** 引擎位于 `public/`，而 Vite 开发服务器拒绝以 ESM 方式
`import` `public/` 资源（它们只能通过 HTML 标签引用）。单文件经典构建 + 脚本标签在 dev、preview、
生产下表现一致。模块只加载一次并缓存（因此 C++ store 每会话只构建一次）。

## 4. 用户画像（v2 · `profile.ts`）

v2 用一个**行为驱动的画像**取代 v1 的固定 personas——一个活的、会衰减的兴趣模型。`profile.ts`
是这套模型的全部；向量空间的数学留在 C++。

**状态。**

```ts
interface Profile {
  tagWeights: Record<string, number>; // 每个标签累积、衰减后的权重
  clickHistory: ClickRecord[];         // 用于已看集合 + 历史
  seenItemIds: Set<number>;            // 支持刷新时的 new/seen 混合
  onboarded: boolean;                  // 是否已通过冷启动选择器
}
```

**标签 → 品类（集中一处）。** 八个冷启动标签（Travel、Food、Tech、News、Art、Sports、Literature、
Outdoors）经 `TAG_TO_CATEGORY` 折叠到引擎的六个 item 品类（如 News → tech、Art → fashion、
Sports → fitness、Literature → beauty、Outdoors → travel）。标签集刻意比品类集大——一个有文档记录
的近似，因为合成数据里并没有单独的 "news"/"literature" item 向量。`categoryWeights(profile)` 把八个
标签权重折叠成 `CATEGORY_ORDER`（`food, fashion, travel, tech, fitness, beauty`）下的六个按品类权重
——这是两种语言**唯一**必须就顺序达成一致的地方，因为 C++ 以此为下标索引中心向量。加权中心向量的
混合本身发生在 C++（`api.hpp make_query`），因此画像 query 的构建方式与 persona query 完全一致，
不会漂移。

**冷启动（`ColdStart`，B2）。** 一个可选选择器为初始画像播种：选中的标签得权重 1；**跳过**则回退到
*中性*画像（每个标签相等），使首个 feed 是一个多样的采样器而非空白——随后头几次点击很快让它专门化
（冷启动 → 预热）。

**隐式反馈（`recordClick`，B3）。** 点击卡片会给映射到该 item 品类的每个标签增权（+1），追加到
`clickHistory`，并把该 item 标记为已看。它返回一个**新**画像（不可变更新），因此 React 立刻重渲染
实时兴趣条。

**兴趣衰减（`decayProfile`，B4）。** 衰减是**按刷新的乘性衰减**：每次刷新把每个标签权重乘以
`DECAY_FACTOR = 0.5`。我们否决了时间式 `exp(-λΔt)` 衰减，因为在点击驱动的 demo 里几乎不流逝墙钟时间，
那样看起来会像什么都不衰减；事件式衰减让淡出恰好发生在用户动作时。新点击以全权重进入，因此你持续
喂的标签保持高位，而你停止喂的标签每次刷新减半——近期兴趣压过陈旧兴趣。一个保护在一切衰减到约 0 时
重置为中性，因此召回 query 永不为零/NaN 向量。

**实时 vs. 按需。** 画像（及面板）在**每次**点击时更新；**feed** 只在**刷新**时重排——刷新也是施加一个
衰减步的时机。这一拆分是 v2 的核心 UX 决策（见
[Architecture §6](Architecture_ch.md#6-关键架构决策)）。

**刷新时的 new/seen 混合（探索/利用）。** `NEW_RATIO = 80` 传给引擎：刷新后的页面约 80% 是**新**（未看）
item，为高分的已看"favorites"保留一个约 12 选 2–3 的小配额，之外还有来自主导品类之外的、保证的探索
保底位。混合逻辑本身位于引擎的 `MixOp`（见 [Core_Design](Core_Design_ch.md)）；前端只提供已看集合与
比例。

**持久化。** 画像与点击历史在每次变化时保存到 `localStorage`（`seenItemIds` 存为数组，因为 `Set`
不可 JSON 序列化，加载时再水合）。加载包在 try/catch 里：缺失、禁用或损坏的存储会回退到一个全新的
中性画像，而非崩溃。

## 5. 表现层

引擎没有标题、作者、封面图或点赞数——这些都是前端职责。

- **`presentation.ts`** 依据 note id 确定性地合成每条 note 的内容（同卡刷新 → 同内容）：标题（取自
  按品类的词库）、作者 + 头像、点赞数（由 `popularity` 得出）。**"为什么推荐"一行由引擎的真实特征
  值推导**——它在 `category_match` / `recency` / `popularity` 中挑出最强的排序信号（按 `ScoreOp`
  的权重加权），使这行说明反映真正把该 item 推上来的因素。
- **`covers.ts`** 一次性加载本地封面 manifest（`/covers/manifest.json`），按 id 确定性为每条 note 选
  一张封面。每张封面显示必需的"Photo by … on Unsplash"署名。缺图/缺品类 → 优雅回退到渐变 + 品类
  emoji。

封面图在**构建期**抓取并提交；运行时对 Unsplash 零 API 调用（见 [Operations](Operations_ch.md)）。

## 6. 主题

- 主题是若干 CSS 变量集合：浅色在 `:root`，深色在 `[data-theme="dark"]` 下。
- `index.html` 中一小段内联脚本在**首次绘制前**解析主题（顺序：`?theme=` 查询参数 → 已保存选择 →
  操作系统偏好），避免闪烁。
- `App` 读取解析出的主题，切换按钮把选择持久化到 `localStorage`。`?theme=` 参数也让主题可分享/可
  截图。

## 7. trace 可视化

DAG trace 面板是本项目可见的那一半——"trace 就是产品"。它把流水线各阶段渲染成从左到右的漏斗，展示
`in → out`、延迟、可选的 `detail` 一行、以及 sample ids，旁边配一份"驱动本次运行的画像"简摘
（"driven by …"）。在画像路径上共有**五**个阶段（多出来的是 `MixOp`，其 detail 形如
`"10 exploit · 2 explore"`）。每当计数变化，一段错峰的 CSS 揭示——外加一次短暂闪烁——会重放，把刷新
按下与重算绑在一起。它可折叠，好让 feed 保持焦点。

关于延迟：只有当页面处于跨源隔离（COOP/COEP）时，浏览器才报告清晰的亚毫秒计时——本应用会请求
该隔离。当它未隔离时（例如无法发送这些头的静态托管，或用虚拟时间预算拍的无头截图），面板会如实
说明——`0.0µs` 读作"计时被钳制"，而非"C++ 瞬间完成"。真实、已隔离的浏览器显示真实微秒。

## 8. 构建

标准 Vite：`npm install`、`npm run dev`（开发服务器）、`npm run build`（类型检查 + 生产打包到
`dist/`）、`npm run preview`。引擎（`public/shuashua.js`）与封面（`public/covers/`）是被拷入构建的
静态资源。见 [Operations](Operations_ch.md)。
