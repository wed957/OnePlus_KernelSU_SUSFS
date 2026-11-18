#!/usr/bin/env python3
"""
Local repository validation suite for wed957/OnePlus_KernelSU_SUSFS.

Validates:
  1. GitHub Actions YAML workflows and composite actions syntax + key invariants.
  2. Cache save action resilience (gh release view check, no error swallowing, ::warning:: on upload failure).
  3. Device JSON configs (single OP-ACE-2-PRO A16 config) and XML manifests.
  4. Action, script, duck-patch, and kernel_patches file references.
  5. Python SUSFS compat framework compilation and android13-5.15 Stage 1 + Stage 2 smoke test.
  6. Unified diff format, v2.4.0 fix patch set, selinux_hide guard, and hide_adb_jdwp_sockets.patch placement.
"""

from __future__ import annotations

import json
import py_compile
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent


class ValidationError(AssertionError):
    """Raised when a repository validation check fails."""


def check(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def validate_yaml_files() -> int:
    yaml_files = sorted(
        list((REPO_ROOT / ".github" / "workflows").glob("*.yml"))
        + list((REPO_ROOT / ".github" / "actions").rglob("action.yml"))
    )
    check(len(yaml_files) > 0, "No workflow or action YAML files found")

    for path in yaml_files:
        rel = path.relative_to(REPO_ROOT)
        try:
            data = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:
            raise ValidationError(f"YAML syntax error in {rel}: {exc}") from exc
        check(isinstance(data, dict), f"Expected YAML mapping in {rel}")

        if path.parent.name == "workflows":
            jobs = data.get("jobs", {})
            check(isinstance(jobs, dict) and jobs, f"No jobs defined in workflow {rel}")
            for job_id, job in jobs.items():
                runs_on = job.get("runs-on")
                if runs_on is not None:
                    check(
                        runs_on == "ubuntu-24.04",
                        f"{rel} job '{job_id}' must pin runs-on: ubuntu-24.04 (found {runs_on!r})",
                    )
    return len(yaml_files)


def validate_workflows_and_actions() -> None:
    build_release_text = (REPO_ROOT / ".github" / "workflows" / "build-kernel-release.yml").read_text(
        encoding="utf-8"
    )
    check("liblz4-tool" not in build_release_text, "liblz4-tool must not appear in build-kernel-release.yml (use lz4)")
    check("lz4" in build_release_text, "lz4 package missing in build-kernel-release.yml")
    check("mlgo:" in build_release_text, "mlgo workflow input missing in build-kernel-release.yml")
    check(
        "github-token: ${{ inputs.github_token }}" not in build_release_text,
        "build-kernel-release.yml must pass secrets.GITHUB_TOKEN (not inputs.github_token) to fetch_ksu_manager",
    )
    check(
        "github-token: ${{ secrets.GITHUB_TOKEN }}" in build_release_text,
        "fetch_ksu_manager must receive secrets.GITHUB_TOKEN in build-kernel-release.yml",
    )

    build_kernel_text = (REPO_ROOT / ".github" / "actions" / "build-kernel" / "action.yml").read_text(
        encoding="utf-8"
    )
    check(
        "50_add_susfs_in_gki-android13-5.15.patch" not in build_kernel_text,
        "build-kernel/action.yml still hardcodes 50_add_susfs_in_gki-android13-5.15.patch",
    )
    check(
        'MAIN_PATCH="$SUSFS_FOLDER/kernel_patches/50_add_susfs_in_${SUSFS_KERNEL_BRANCH}.patch"' in build_kernel_text,
        "build-kernel/action.yml must dynamically select MAIN_PATCH via SUSFS_KERNEL_BRANCH",
    )
    check(
        '"v2.2.0"|"v2.3.0"|"v2.4.0"' in build_kernel_text,
        "build-kernel/action.yml missing v2.4.0 case branch",
    )
    check(
        'if [ "$susfs_version" != "v2.4.0" ]' in build_kernel_text,
        "build-kernel/action.yml must skip v2.3.0 fix_selinux_hide_timing.patch on v2.4.0",
    )
    check(
        "apply_kernel_compat.py" in build_kernel_text,
        "build-kernel/action.yml must invoke Python apply_kernel_compat.py",
    )
    check(
        "CONFIG_KSU_SUSFS_PRIORITY_UID_FILTER=y" in build_kernel_text,
        "build-kernel/action.yml missing CONFIG_KSU_SUSFS_PRIORITY_UID_FILTER=y",
    )
    check(
        "hide_adb_jdwp_sockets.patch" in build_kernel_text,
        "build-kernel/action.yml missing hide_adb_jdwp_sockets.patch",
    )
    check(
        "--fuzz=0 < \"$KERNEL_PATCHES_FOLDER/common/hide_adb_jdwp_sockets.patch\"" in build_kernel_text,
        "build-kernel/action.yml should apply hide_adb_jdwp_sockets.patch with --fuzz=0",
    )

    cache_save_text = (REPO_ROOT / ".github" / "actions" / "cache" / "save" / "action.yml").read_text(
        encoding="utf-8"
    )
    check(
        'gh release view "$TAG_NAME" --repo "$TARGET_REPO"' in cache_save_text,
        "cache/save/action.yml must verify release existence with gh release view",
    )
    check(
        re.search(r"gh release upload[^\n]*>/dev/null|gh release upload[^\n]*> /dev/null", cache_save_text) is None,
        "cache/save/action.yml must not swallow gh release upload output with > /dev/null",
    )
    check(
        re.search(r"gh release create[^\n]*\|\|\s*true", cache_save_text) is None,
        "cache/save/action.yml must not swallow gh release create failure with || true",
    )
    check(
        "::error::Failed to upload" not in cache_save_text and "exit 1" not in cache_save_text,
        "cache/save/action.yml must emit ::warning:: rather than failing the build on cache upload failure",
    )
    check(
        "::warning::Failed to upload" in cache_save_text,
        "cache/save/action.yml must emit ::warning:: when cache upload fails",
    )


def validate_configs_and_manifests() -> None:
    config_files = sorted((REPO_ROOT / "configs").rglob("*.json"))
    expected = [REPO_ROOT / "configs" / "a16" / "OP-ACE-2-PRO.json"]
    check(
        config_files == expected,
        f"Expected only configs/a16/OP-ACE-2-PRO.json, found {[str(p.relative_to(REPO_ROOT)) for p in config_files]}",
    )

    cfg = json.loads(expected[0].read_text(encoding="utf-8"))
    required_keys = {
        "model", "soc", "branch", "manifest", "android_version", "kernel_version",
        "os_version", "lto", "zyc_compiler", "c_compiler", "rust_compiler", "bindgen",
        "rust_build", "disk_cleanup", "hmbird", "susfs", "opt", "ds", "bbg",
        "bbr", "bbr3", "ttl", "ip_set", "unicode", "ntsync", "blacklist_modules", "uname",
    }
    missing = required_keys - set(cfg.keys())
    check(not missing, f"OP-ACE-2-PRO.json missing keys: {sorted(missing)}")
    check(cfg["model"] == "OP-ACE-2-PRO", f"Unexpected model: {cfg['model']}")
    check(cfg["os_version"] == "A16", f"Unexpected os_version: {cfg['os_version']}")
    check(cfg["android_version"] == "android13" and cfg["kernel_version"] == "5.15", "Unexpected KMI")
    check(cfg["susfs"] is True, "susfs must be true in OP-ACE-2-PRO.json")

    manifest_path = REPO_ROOT / "manifests" / cfg["os_version"].lower() / cfg["manifest"]
    check(manifest_path.is_file(), f"Missing manifest file: {manifest_path}")
    ET.parse(manifest_path)

    for xml_path in (REPO_ROOT / "manifests").rglob("*.xml"):
        try:
            ET.parse(xml_path)
        except ET.ParseError as exc:
            raise ValidationError(f"Invalid XML in {xml_path.relative_to(REPO_ROOT)}: {exc}") from exc


def validate_references_and_patches() -> int:
    # Check local action references in workflows and composite actions
    for yml_path in list((REPO_ROOT / ".github" / "workflows").glob("*.yml")) + list(
        (REPO_ROOT / ".github" / "actions").rglob("action.yml")
    ):
        text = yml_path.read_text(encoding="utf-8")
        for ref in re.findall(r"uses:\s*\./([^\s#]+)", text):
            normalized = ref.removeprefix("action-source/")
            target = REPO_ROOT / normalized
            if normalized.startswith(".github/actions/setup-android-build"):
                # Refers to upstream Duck-Detector-Refactoring checkout root
                continue
            check(target.exists(), f"Broken local action reference './{ref}' in {yml_path.relative_to(REPO_ROOT)}")

    check(
        (REPO_ROOT / "scripts" / "kernel_fix_patches" / "apply_kernel_compat.py").is_file(),
        "Missing scripts/kernel_fix_patches/apply_kernel_compat.py",
    )

    for duck_patch in ("enhanced_timing_probes.patch", "adb_diagnostics.patch"):
        check((REPO_ROOT / "tools" / "duck_patches" / duck_patch).is_file(), f"Missing duck patch: {duck_patch}")

    # Ensure stale v2.3.0 hardcoded kernel patch was removed
    stale_50 = REPO_ROOT / "kernel_patches" / "next" / "50_add_susfs_in_gki-android13-5.15.patch"
    check(not stale_50.exists(), "Stale v2.3.0 kernel_patches/next/50_add_susfs_in_gki-android13-5.15.patch should be removed")

    # Verify v2.4.0 fix patches exist
    v240_dir = REPO_ROOT / "kernel_patches" / "next" / "susfs_fix_patches" / "v2.4.0"
    required_v240 = [
        "fix_Kbuild.patch",
        "fix_dispatch.c.patch",
        "fix_init.c.patch",
        "fix_kernel_umount.c.patch",
        "fix_rules.c.patch",
        "fix_setuid_hook.c.patch",
        "fix_sucompat.c.patch",
        "fix_supercall.c.patch",
        "fix_50_add_susfs_in_gki-android16-6.12.patch",
        "ksu_toolkit.patch",
        "overwrite_hook_mode.patch",
    ]
    for name in required_v240:
        check((v240_dir / name).is_file(), f"Missing v2.4.0 patch: {name}")

    # Verify hide_adb_jdwp_sockets.patch structure & v2.4.0 placement
    hide_patch_path = REPO_ROOT / "kernel_patches" / "common" / "hide_adb_jdwp_sockets.patch"
    check(hide_patch_path.is_file(), "Missing kernel_patches/common/hide_adb_jdwp_sockets.patch")
    hide_raw = hide_patch_path.read_bytes()
    check(b"\r\n" not in hide_raw, "hide_adb_jdwp_sockets.patch should use LF line endings")
    hide_text = hide_raw.decode("utf-8")
    check("if (!inode->i_mapping)" in hide_text, "hide_adb_jdwp_sockets.patch fs/susfs.c hunk missing 'if (!inode->i_mapping)' trailing context")
    check(
        hide_text.index("S_ISSOCK(inode->i_mode)") < hide_text.index("if (!inode->i_mapping)"),
        "hide_adb_jdwp_sockets.patch must place S_ISSOCK check before 'if (!inode->i_mapping)' (not inside it)",
    )
    check("susfs_is_current_proc_umounted_app()" not in hide_text, "hide_adb_jdwp_sockets.patch still references removed v2.3.0 context")

    # Validate all .patch files have unified diff headers
    patch_files = sorted(
        list((REPO_ROOT / "kernel_patches").rglob("*.patch"))
        + list((REPO_ROOT / "tools" / "duck_patches").rglob("*.patch"))
    )
    check(len(patch_files) > 0, "No .patch files found")
    for pf in patch_files:
        content = pf.read_text(encoding="utf-8", errors="replace")
        check(content.strip() != "", f"Empty patch file: {pf.relative_to(REPO_ROOT)}")
        check(
            ("--- " in content and "+++ " in content and "@@ " in content) or ("diff --git " in content),
            f"Malformed unified diff in {pf.relative_to(REPO_ROOT)}",
        )
    return len(patch_files)


def validate_compat_framework() -> None:
    compat_py = REPO_ROOT / "scripts" / "kernel_fix_patches" / "apply_kernel_compat.py"

    with tempfile.TemporaryDirectory() as tmp:
        py_compile.compile(str(compat_py), cfile=str(Path(tmp) / "apply_kernel_compat.pyc"), doraise=True)

        help_proc = subprocess.run(
            [sys.executable, str(compat_py), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        check(help_proc.returncode == 0, f"apply_kernel_compat.py --help failed: {help_proc.stderr}")

        # Smoke test Stage 1 + Stage 2 on a mock android13-5.15.180 kernel tree
        root = Path(tmp) / "common"
        (root / "fs" / "proc").mkdir(parents=True)
        (root / "mm").mkdir(parents=True)
        (root / "include" / "linux").mkdir(parents=True)

        (root / "fs" / "namespace.c").write_text('#include "internal.h"\n#include <linux/mnt_namespace.h>\n', encoding="utf-8")
        (root / "fs" / "proc" / "task_mmu.c").write_text(
            '#include "internal.h"\n#include <linux/pkeys.h>\n\nstatic int smaps_pte_range(void)\n{\n\tif (!vma_pages(vma))\n\t\tgoto show_pad;\nshow_pad:\n\treturn 0;\n}\n',
            encoding="utf-8",
        )
        (root / "mm" / "memory.c").write_text("#include <linux/vmalloc.h>\n#include <linux/swap_slots.h>\n", encoding="utf-8")
        (root / "mm" / "Makefile").write_text("obj-y += memory.o\n", encoding="utf-8")

        state_file = Path(tmp) / "state.json"

        s1 = subprocess.run(
            [
                sys.executable, str(compat_py),
                "--stage", "1",
                "--root", str(root),
                "--kmi", "android13-5.15",
                "--sublevel", "180",
                "--soc", "kalama",
                "--state", str(state_file),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        check(s1.returncode == 0, f"apply_kernel_compat.py stage 1 failed: {s1.stderr}")
        check("#include <trace/hooks/blk.h>" in (root / "fs" / "namespace.c").read_text(encoding="utf-8"), "Stage 1 did not add trace/hooks/blk.h")
        check("#include <trace/hooks/mm.h>" in (root / "fs" / "proc" / "task_mmu.c").read_text(encoding="utf-8"), "Stage 1 did not add trace/hooks/mm.h")
        check("#include <linux/swap_slots.h>" not in (root / "mm" / "memory.c").read_text(encoding="utf-8"), "Stage 1 did not temporarily remove swap_slots.h")

        s2 = subprocess.run(
            [
                sys.executable, str(compat_py),
                "--stage", "2",
                "--root", str(root),
                "--kmi", "android13-5.15",
                "--sublevel", "180",
                "--soc", "kalama",
                "--state", str(state_file),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        check(s2.returncode == 0, f"apply_kernel_compat.py stage 2 failed: {s2.stderr}")
        check("#include <trace/hooks/blk.h>" not in (root / "fs" / "namespace.c").read_text(encoding="utf-8"), "Stage 2 did not revert trace/hooks/blk.h")
        check("#include <trace/hooks/mm.h>" not in (root / "fs" / "proc" / "task_mmu.c").read_text(encoding="utf-8"), "Stage 2 did not revert trace/hooks/mm.h")
        check("#include <linux/swap_slots.h>" in (root / "mm" / "memory.c").read_text(encoding="utf-8"), "Stage 2 did not restore swap_slots.h")


def main() -> int:
    try:
        yaml_count = validate_yaml_files()
        print(f"[PASS] YAML syntax & runner pins validated ({yaml_count} files)")

        validate_workflows_and_actions()
        print("[PASS] Workflow, build-kernel, and cache/save action checks passed")

        validate_configs_and_manifests()
        print("[PASS] Device config (OP-ACE-2-PRO A16 only) & XML manifests passed")

        patch_count = validate_references_and_patches()
        print(f"[PASS] References, v2.4.0 patch set, and unified diffs passed ({patch_count} patches)")

        validate_compat_framework()
        print("[PASS] Python SUSFS compat framework compilation & android13-5.15 smoke test passed")
        return 0
    except ValidationError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
