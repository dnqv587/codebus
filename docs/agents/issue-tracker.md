# Issue tracker：本地 Markdown

这个 repo 的 issues 和 specs 作为 markdown 文件存放在 `docs/scratch/` 中。

## 约定

- 每个 feature 一个目录：`docs/scratch/<feature-slug>/`
- Spec 是 `docs/scratch/<feature-slug>/spec.md`
- Implementation issues 每个 ticket 一个文件，路径为 `docs/scratch/<feature-slug>/issues/<NN>-<slug>.md`，从 `01` 开始编号；绝不要写成一个 combined tickets file
- Triage state 记录为每个 issue file 顶部附近的 `Status:` 行（role 字符串见 `triage-labels.md`）
- Comments 和 conversation history 追加到文件底部的 `## Comments` heading 下

## 当 skill 说 "publish to the issue tracker" 时

在 `docs/scratch/<feature-slug>/` 下创建新文件（必要时创建目录）。

## 当 skill 说 "fetch the relevant ticket" 时

读取引用路径处的文件。用户通常会直接传入路径或 issue number。

## Wayfinding 操作

供 `/wayfinder` 使用。**map** 是一个文件，每个 ticket 对应一个 **child** 文件。

- **Map**: `docs/scratch/<effort>/map.md`（Notes / Decisions-so-far / Fog body）。
- **Child ticket**: `docs/scratch/<effort>/issues/NN-<slug>.md`，从 `01` 开始编号，body 中是问题。`Type:` 行记录 ticket 类型（`research`/`prototype`/`grilling`/`task`）；`Status:` 行记录 `claimed`/`resolved`。
- **Blocking**: 顶部附近的 `Blocked by: NN, NN` 行。当它列出的每个文件都是 `resolved` 时，ticket 即为 unblocked。
- **Frontier**: 扫描 `docs/scratch/<effort>/issues/` 中 open、unblocked 且 unclaimed 的文件；按编号第一个胜出。
- **Claim**: 在任何工作开始前设置 `Status: claimed` 并保存。
- **Resolve**: 把答案追加到 `## Answer` heading 下，设置 `Status: resolved`，然后向 `map.md` 中 map 的 Decisions-so-far 追加 context pointer（gist + link）。

## 与既有文档体系的分工

- `docs/known-issues.md`：D 编号已知缺陷登记 + Q 疑点清单，与 `DISABLED_` 测试用例对应——是缺陷登记簿，不是 issue tracker；skills 不要往里写 ticket。
- `docs/spec/`：正式设计文档与计划（如 H 轮设计文档），长期保留。
- `docs/scratch/`：skills 的工作区 issues/specs（to-tickets / triage / to-spec / wayfinder 读写），属过程性工作文件。