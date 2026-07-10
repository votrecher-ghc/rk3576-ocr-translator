# Git仓库与分支策略

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 仓库组织](#2-仓库组织)
- [3. 分支策略](#3-分支策略)
- [4. 提交规范](#4-提交规范)
- [5. 发布流程](#5-发布流程)

---

## 1. 概述

Git 仓库组织与分支管理策略，规范团队协作。

## 2. 仓库组织

| 仓库           | 内容                           |
| -------------- | ------------------------------ |
| ocr-uboot      | U-Boot 定制                    |
| ocr-kernel     | Linux 内核定制                 |
| ocr-buildroot  | Buildroot 配置                 |
| ocr-app        | 应用程序源码                   |
| ocr-docs       | 文档                           |
| ocr-models     | AI 模型与转换脚本              |

## 3. 分支策略

```
main        ← 稳定发布分支
develop     ← 开发集成分支
feature/*   ← 功能分支
hotfix/*    ← 紧急修复分支
release/*   ← 发布准备分支
```

- `main`：仅合并 release，打 tag
- `develop`：日常开发集成
- `feature/*`：从 develop 拉出，完成后合并回 develop

## 4. 提交规范

```
<type>(<scope>): <subject>

<body>

<footer>
```

| type    | 说明           |
| ------- | -------------- |
| feat    | 新功能         |
| fix     | 修复           |
| docs    | 文档           |
| style   | 格式           |
| refactor| 重构           |
| test    | 测试           |
| chore   | 构建/工具      |

示例：`feat(pipeline): 支持拍照模式动态切换`

## 5. 发布流程

```
1. develop → 创建 release/x.y.z 分支
2. release 分支测试验证
3. 合并到 main，打 tag vx.y.z
4. 合并回 develop
```

---

> 相关文档：[03-代码审查清单.md](03-代码审查清单.md)
