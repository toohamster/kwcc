# GitHub 发布流程

> 项目已部署到 GitHub，以下为完整的发布部署流程。

## GitHub Token 提取

调用 GitHub API 前，必须从 git 配置中提取 token：

```bash
git config --get remote.origin.url
# 输出：https://username:token@github.com/owner/repo.git
# 提取 token 后用于：curl -H "Authorization: Bearer $TOKEN" ...
```

## 版本发布指令

用户说"**发布到 main**"或"**发布新版本**"时，执行完整发布流程：

### A. 有功能分支时

1. 检查未提交修改，如有则先提交
2. 推送功能分支到远程
3. **从 git config 提取 GitHub Token**
4. 创建 Pull Request 到 main
5. 自动合并 PR
6. 自动生成版本号并打标签
7. 创建 GitHub Release（含发布说明）

### B. 已在 main 时

1. 检查未提交修改，如有则先提交
2. 推送到远程 main
3. 自动生成版本号并打标签
4. 创建 GitHub Release（含发布说明）

## 版本号规则

基于最新标签自动递增（格式 `vX.Y.Z`）：

| 变更类型 | 递增规则 | 示例 |
|---------|---------|------|
| 小修复 | 第三位递增 | v2.0.2 → v2.0.3 |
| 新功能 | 第二位递增，第三位置 0 | v2.0.2 → v2.1.0 |
| 重大变更 | 第一位递增，后两位置 0 | v2.0.2 → v3.0.0 |

## 注意事项

- 默认**保留远程功能分支**，除非用户明确说"删除远程分支"
- 完成后**留在当前功能分支**，除非用户明确说"切换到 main"

## GitHub Release 创建步骤（无 gh CLI）

项目环境没有 `gh` CLI，使用 curl + GitHub API 创建 Release：

### 1. 打标签并推送

```bash
git tag v0.0.3
git push origin v0.0.3
```

### 2. 提取 Token

```bash
TOKEN=$(git config --get remote.origin.url | sed 's|https://[^:]*:\([^@]*\)@.*|\1|')
```

### 3. 创建 Release

```bash
curl -s -X POST \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "tag_name": "v0.0.3",
    "target_commitish": "main",
    "name": "v0.0.3",
    "draft": false,
    "prerelease": false,
    "body": "## 发布说明内容\n\n支持 Markdown 格式"
  }' \
  "https://api.github.com/repos/toohamster/kwcc/releases"
```

### 发布说明格式

参考 v0.0.2/v0.0.3 的格式：

```markdown
## 开发时间线
| # | 方案 | 状态 | 演进关系 | 测试 |

## 新功能
### 每个主要功能一节
- 子功能点列表
- 代码示例（如适用）
- 关键约束（如适用）

## 测试
| 测试套件 | 数量 | 说明 |

## 设计沉淀
（如有新增 design/ 内容）

## 文档更新
（如有文档变更）
```

### 查看已有 Release

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/toohamster/kwcc/releases/tags/v0.0.2" | python3 -c "import sys,json; print(json.load(sys.stdin)['body'])"
```
