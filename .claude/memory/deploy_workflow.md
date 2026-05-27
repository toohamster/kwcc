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
