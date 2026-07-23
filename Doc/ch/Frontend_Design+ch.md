# 前端设计

前端（`web/`）是一个 Vite + React + TypeScript 单页应用，把 WASM 引擎的输出渲染为小红书风格的
feed，并带一个实时的 DAG trace 面板。它是纯表现层——JS 侧没有任何排序逻辑。

## 1. 设计语言

视觉设计沿用小红书网页版，参考开源项目 XiaoShiLiu 的比例（组件为全新编写，非拷贝）：

- **配色**（CSS 变量）：暖红强调色 `#ff2442`，克制使用（logo、当前 persona、out 计数），配以一套
  中性的文本/背景/边框 token。
- **布局：** 固定左侧栏（约 232px）+ 主内容区——网页版小红书的形态，而非移动端底部标签。
- **feed：** 响应式瀑布流，无边框、圆角卡片。
- **深色模式：** 通过 `[data-theme="dark"]` 提供完整第二主题，可切换。

## 2. 布局与组件

```
┌────────────┬───────────────────────────────────────────────┐
│  侧边栏     │  Explore  ·  <persona>                         │
│  ┌──────┐  │  ┌─────────────────────────────────────────┐  │
│  │ 刷    │  │  │ DAG pipeline trace（可折叠漏斗）          │  │
│  │ 品牌  │  │  └─────────────────────────────────────────┘  │
│  ├──────┤  │  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐            │
│  │persona│  │  │卡片│ │卡片│ │卡片│ │卡片│ │卡片│  瀑布流    │
│  │ 导航  │  │  └────┘ └────┘ └────┘ └────┘ └────┘  (2–5 列) │
│  ├──────┤  │  ...                                            │
│  │主题   │  │                                                │
│  └──────┘  │                                                │
└────────────┴───────────────────────────────────────────────┘
```

| 组件 | 文件 | 职责 |
|---|---|---|
| `App` | `src/App.tsx` | 持有状态：persona 列表、当前 persona、推荐结果、主题。persona 变化时重跑流水线；应用并持久化主题。 |
| `Sidebar` | `src/components/Sidebar.tsx` | 品牌、persona 切换器（导航；当前项用强调色）、明暗切换。 |
| `Feed` | `src/components/Feed.tsx` | `react-masonry-css` 瀑布流；响应式列数；加载封面 manifest。 |
| `NoteCard` | `src/components/NoteCard.tsx` | 单卡：封面 → 两行标题 → “why” 说明 → 作者 + 点赞。 |
| `TracePanel` | `src/components/TracePanel.tsx` | 可折叠 DAG 漏斗：逐级 in→out、延迟、sample ids，更新时错峰揭示。 |

## 3. 引擎集成（`engine.ts`）

引擎以单文件、经典脚本形式发布（`public/shuashua.js`，wasm 内嵌），挂载一个全局 `ShuaShua()`
工厂。`engine.ts` 用 `<script>` 标签加载它，并暴露带类型的封装：

- `getPersonas()` → `Persona[]`
- `recommend(personaId)` → `Recommendation`（`{ persona, feed[], trace[] }`）

**为何用 `<script>` 标签而非 ESM `import`：** 引擎位于 `public/`，而 Vite 开发服务器拒绝以 ESM 方式
`import` `public/` 资源（它们只能通过 HTML 标签引用）。单文件经典构建 + 脚本标签在 dev、preview、
生产下表现一致。模块只加载一次并缓存（因此 C++ store 每会话只构建一次）。

## 4. 表现层

引擎没有标题、作者、封面图或点赞数——这些都是前端职责。

- **`presentation.ts`** 依据 note id 确定性地合成每条 note 的内容（同卡刷新 → 同内容）：标题（取自
  按品类的词库）、作者 + 头像、点赞数（由 `popularity` 得出）。**“为什么推荐”一行由引擎的真实特征
  值推导**——它在 `category_match` / `recency` / `popularity` 中挑出最强的排序信号（按 `ScoreOp`
  的权重加权），使这行说明反映真正把该 item 推上来的因素。
- **`covers.ts`** 一次性加载本地封面 manifest（`/covers/manifest.json`），按 id 确定性为每条 note 选
  一张封面。每张封面显示必需的“Photo by … on Unsplash”署名。缺图/缺品类 → 优雅回退到渐变 + 品类
  emoji。

封面图在**构建期**抓取并提交；运行时对 Unsplash 零 API 调用（见 [Operations](Operations+ch.md)）。

## 5. 主题

- 主题是若干 CSS 变量集合：浅色在 `:root`，深色在 `[data-theme="dark"]` 下。
- `index.html` 中一小段内联脚本在**首次绘制前**解析主题（顺序：`?theme=` 查询参数 → 已保存选择 →
  操作系统偏好），避免闪烁。
- `App` 读取解析出的主题，切换按钮把选择持久化到 `localStorage`。`?theme=` 参数也让主题可分享/可
  截图。

## 6. trace 可视化

DAG trace 面板是本项目可见的那一半——“trace 就是产品”。它把四个阶段渲染成从左到右的漏斗，展示
`in → out`、延迟、sample ids，每次新推荐时重放错峰的 CSS 揭示动画。它可折叠，好让 feed 保持焦点。

关于延迟：只有当页面处于跨源隔离（COOP/COEP）时，浏览器才报告清晰的亚毫秒计时——本应用会请求
该隔离。用虚拟时间预算拍的无头截图可能显示 `0.0µs`——这是截图工具的假象，不是运行时 bug；真实
浏览器显示真实微秒。

## 7. 构建

标准 Vite：`npm install`、`npm run dev`（开发服务器）、`npm run build`（类型检查 + 生产打包到
`dist/`）、`npm run preview`。引擎（`public/shuashua.js`）与封面（`public/covers/`）是被拷入构建的
静态资源。见 [Operations](Operations+ch.md)。
