# 更新日志 (Changelog)

## 2026-11-18 — 同步上游至 SuSFS v2.4.0 并修复 CI 缓存与补丁链路

### 上游同步与版本升级
- **同步 `WildKernels/OnePlus_KernelSU_SUSFS` 上游最新代码**（保留单机 `OP-ACE-2-PRO` A16 `android13-5.15` 配置、`.gitattributes`、`.github/workflows/build-enhanced-duck.yml` 与 `tools/duck_patches/`）。
- **升级内置 `kernel_patches/` 至 `WildKernels/kernel_patches` 最新版**：
  - 新增 `next/susfs_fix_patches/v2.4.0/` 与 `stable/v2.4.0/`，支持 **SuSFS v2.4.0**、`overwrite_hook_mode.patch` 与 `ksu_toolkit.patch`。
  - 引入 `common/hide_adb_jdwp_sockets.patch` 与 `CONFIG_KSU_SUSFS_PRIORITY_UID_FILTER=y`。
  - 引入 `scripts/kernel_fix_patches/apply_kernel_compat.py` Python 两阶段 SUSFS 兼容框架，取代原先在 workflow 中的硬编码 bash `sed`/fake-patch 逻辑。
  - 引入 `mlgo` 编译优化开关（默认开启，并带编译器不支持时的安全降级探测）、`ubuntu-24.04` runner 固定、`Droidspaces` `CONFIG_MACVLAN=y` 及发布流程自动抓取对应驱动版本的 KSUN/KSU Manager APK。

### 定制适配与问题修复
- **修复 `kernel_patches/common/hide_adb_jdwp_sockets.patch`**：
  - 上游原补丁针对 `v2.3.0` 的 `fs/susfs.c` 上下文生成，在 `v2.4.0` tree 下会因 `fuzz=2` 错位插进 `if (!inode->i_mapping)` 死代码分支。已修正为精准匹配 `v2.4.0` `susfs_is_inode_sus_path()` 中 `if (uid < 10000)` 之后的位置，并在 `.github/actions/build-kernel/action.yml` 使用 `--fuzz=0` 强制零模糊应用。
- **适配 `fix_selinux_hide_timing.patch` 至 SuSFS v2.4.0 架构**：
  - 在 `v2.4.0` 中，`10_enable_susfs_for_ksu.patch` 已将 `my_setprocattr` 等运行时 hook 从 `KernelSU-Next/kernel/feature/selinux_hide.c` 移除，改由 `50_add_susfs_in_*.patch` 直接在 `security/selinux/hooks.c`（`susfs_selinux_setprocattr`）与 `security/selinux/ss/services.c`（`susfs_string_to_context_struct`）中以单策略查表实现，天然消除双策略查询导致的时间侧信道，不再需要针对 `v2.3.0` `selinux_hide.c` 的 `fix_selinux_hide_timing.patch`；已在 `action.yml` 中限制仅 `v2.2.0`/`v2.3.0` 分支应用该补丁，避免 `v2.4.0` 构建冲突。
- **修复 `.github/actions/build-kernel/action.yml`**：
  - 移除此前硬编码的 `kernel_patches/next/50_add_susfs_in_gki-android13-5.15.patch`，改为按 `${SUSFS_KERNEL_BRANCH}` 动态从 `susfs4ksu` 取主补丁并配合 `apply_kernel_compat.py` 与可选 companion fix overlay。
  - 保留本仓内 `kernel_patches/` 优先使用逻辑与 `lz4` 依赖替换。
- **修复 `.github/actions/cache/save/action.yml`**：
  - 此前仅用 `git rev-parse` 判断 `ccache-cache` 是否存在，导致当远程仅有 git tag 而缺少 GitHub Release 时跳过建 release 并在 `gh release upload` 时用 `> /dev/null 2>&1` 静默报错并直接 `exit 1`，使已产出的内核构建被误标红。
  - 现改用 `gh release view` 校验 GitHub Release 真实存在性，缺失时自动补建 Release；移除吞错重定向；若缓存上传仍偶发失败则输出 `::warning::` 并清理临时文件，不再将已成功的内核构建标记为失败。
- **修复 `.github/workflows/build-kernel-release.yml`**：
  - 修正 `fetch_ksu_manager` 的 `github-token` 参数由空值 `inputs.github_token` 改为 `secrets.GITHUB_TOKEN`（并设置 `continue-on-error: true`）。
- **同步更新 `.github/workflows/debug-chain.yml` 与新增 `tests/validate_repo.py`**：
  - `debug-chain.yml` 默认版本更新为 `KSUN dev` / `SuSFS v2.4.0`，Stage 3 集成 `apply_kernel_compat.py` 与 `hide_adb_jdwp_sockets.patch`。
  - 新增 `tests/validate_repo.py` 本地自动化校验脚本，覆盖 YAML 语法、runner 固定、JSON 配置与 XML manifest、`apply_kernel_compat.py` 编译与 Stage 1/2 冒烟测试、补丁格式与 `v2.4.0` 关键补丁规则检查。
