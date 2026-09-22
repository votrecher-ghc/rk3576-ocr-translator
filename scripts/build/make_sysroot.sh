#!/bin/bash
# =====================================================================
# 目标 sysroot 组装脚本 (Ubuntu 22.04 jammy / arm64)
#
# 野火 RK3576 SDK 的 rootfs 是 Ubuntu 22.04 arm64 (RK_UBUNTU=y)，但 SDK
# 本身不提供可直接用于交叉编译的 sysroot。本脚本从 ports.ubuntu.com 拉取
# 目标架构的 .deb 并解包，得到与板端 ABI 一致 (glibc 2.35) 的 sysroot，
# 供 build_app.sh 使用。全程不需要 root / dpkg --add-architecture。
#
# 输出: output/sysroot
# 用法: bash scripts/build/make_sysroot.sh
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SYSROOT_DIR="${SYSROOT_DIR:-${PROJECT_ROOT}/output/sysroot}"
CACHE_DIR="${CACHE_DIR:-${PROJECT_ROOT}/output/.deb-cache}"
MIRROR="${UBUNTU_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"
SUITE="${UBUNTU_SUITE:-jammy}"
TARGET_ARCH="arm64"
MULTIARCH="aarch64-linux-gnu"

# app/CMakeLists.txt 要求的目标库 + 其链接期依赖 + glibc
PACKAGES=(
    libc6 libc6-dev linux-libc-dev libgcc-s1 libstdc++6
    libdrm2 libdrm-dev libdrm-common
    libjpeg-turbo8 libjpeg-turbo8-dev libjpeg8-dev
    libfreetype6 libfreetype-dev
    libpng16-16 libpng-dev
    zlib1g zlib1g-dev
    libbrotli1 libbrotli-dev
    libbz2-1.0 libbz2-dev
    libcjson1 libcjson-dev
)

log() { echo -e "[Sysroot $(date '+%H:%M:%S')] $*"; }
err() { echo -e "[Sysroot ERROR] $*" >&2; exit 1; }

check_prerequisites() {
    command -v curl    >/dev/null 2>&1 || err "未找到 curl"
    command -v dpkg-deb>/dev/null 2>&1 || err "未找到 dpkg-deb"
    command -v python3 >/dev/null 2>&1 || err "未找到 python3"
}

# 解析 Packages 索引, 下载所需 .deb
fetch_debs() {
    mkdir -p "${CACHE_DIR}"
    log "解析 ${SUITE} (+updates/security) main+universe 的 ${TARGET_ARCH} 索引"
    MIRROR="${MIRROR}" SUITE="${SUITE}" TARGET_ARCH="${TARGET_ARCH}" \
    CACHE_DIR="${CACHE_DIR}" PACKAGES="${PACKAGES[*]}" python3 - <<'PY'
import gzip, os, subprocess, sys, urllib.request

mirror   = os.environ["MIRROR"]
suite    = os.environ["SUITE"]
arch     = os.environ["TARGET_ARCH"]
cache    = os.environ["CACHE_DIR"]
wanted   = set(os.environ["PACKAGES"].split())

# 后面的 suite 版本更新, 允许覆盖前面的条目
suites     = [suite, f"{suite}-updates", f"{suite}-security"]
components = ["main", "universe"]
found = {}

for su in suites:
    for comp in components:
        url = f"{mirror}/dists/{su}/{comp}/binary-{arch}/Packages.gz"
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                data = gzip.decompress(r.read()).decode("utf-8", "replace")
        except Exception as e:
            print(f"  跳过 {su}/{comp}: {e}", file=sys.stderr)
            continue
        for stanza in data.split("\n\n"):
            if not stanza.strip():
                continue
            fields = {}
            for line in stanza.splitlines():
                if line[:1] not in (" ", "\t") and ":" in line:
                    k, _, v = line.partition(":")
                    fields[k] = v.strip()
            name = fields.get("Package")
            if name in wanted and "Filename" in fields:
                found[name] = (fields["Filename"], fields.get("Version", "?"))

missing = wanted - set(found)
if missing:
    sys.exit("索引中未找到这些包: " + " ".join(sorted(missing)))

for name in sorted(found):
    path, version = found[name]
    dest = os.path.join(cache, os.path.basename(path))
    if not os.path.exists(dest):
        print(f"  下载 {name} {version}")
        subprocess.run(["curl", "-fsSL", "-o", dest + ".part",
                        f"{mirror}/{path}"], check=True)
        os.replace(dest + ".part", dest)
    else:
        print(f"  已缓存 {name} {version}")
PY
}

extract_debs() {
    local deb
    log "解包到 ${SYSROOT_DIR}"
    rm -rf "${SYSROOT_DIR}"
    mkdir -p "${SYSROOT_DIR}"
    for deb in "${CACHE_DIR}"/*.deb; do
        dpkg-deb -x "${deb}" "${SYSROOT_DIR}"
    done
}

# ARM 官方工具链是 aarch64-none-linux-gnu, 不搜索 Debian 的 multiarch 目录,
# 因此把 multiarch 内容并到 sysroot 的标准路径上。
flatten_multiarch() {
    log "展平 multiarch 目录 (${MULTIARCH} -> 标准路径)"
    # usrmerge 下 lib/ 与 usr/lib/ 可能互指, 且 libc6 自带
    # lib/ld-linux-aarch64.so.1 -> aarch64-linux-gnu/... 这类相对链接,
    # 直接 cp -a 会因源与目标同为一个文件而失败, 因此逐项判重。
    SYSROOT_DIR="${SYSROOT_DIR}" MULTIARCH="${MULTIARCH}" python3 - <<'FLATTEN'
import os, shutil

root = os.environ["SYSROOT_DIR"]
ma = os.environ["MULTIARCH"]
copied = skipped = 0

for base in ("usr/include", "usr/lib", "lib"):
    src_root = os.path.join(root, base, ma)
    if not os.path.isdir(src_root):
        continue
    dst_root = os.path.join(root, base)
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel_dir = os.path.relpath(dirpath, src_root)
        out_dir = dst_root if rel_dir == "." else os.path.join(dst_root, rel_dir)
        os.makedirs(out_dir, exist_ok=True)
        link_dirs = [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
        for name in filenames + link_dirs:
            src = os.path.join(dirpath, name)
            dst = os.path.join(out_dir, name)
            # 源与目标已是同一文件 (相对链接回指 multiarch 目录) 则跳过
            if os.path.lexists(dst):
                try:
                    if os.path.realpath(src) == os.path.realpath(dst):
                        skipped += 1
                        continue
                except OSError:
                    pass
                os.remove(dst)
            if os.path.islink(src):
                os.symlink(os.readlink(src), dst)
            else:
                shutil.copy2(src, dst, follow_symlinks=False)
            copied += 1
        # 已按链接处理过的目录不再递归
        dirnames[:] = [d for d in dirnames if d not in link_dirs]

print(f"  展平 {copied} 项, 跳过同名同源 {skipped} 项")
FLATTEN
}

# .deb 里的 .so 开发符号链接是绝对路径 (指向 /lib/...), 直接用会落到宿主机根目录
relativize_symlinks() {
    log "将绝对符号链接改写为 sysroot 内相对路径"
    SYSROOT_DIR="${SYSROOT_DIR}" python3 - <<'PY'
import os
root = os.environ["SYSROOT_DIR"]
fixed = 0
for dirpath, dirnames, filenames in os.walk(root):
    for name in dirnames + filenames:
        p = os.path.join(dirpath, name)
        if not os.path.islink(p):
            continue
        target = os.readlink(p)
        if not target.startswith("/"):
            continue
        # 绝对目标按 sysroot 为根重新计算
        abs_in_sysroot = os.path.join(root, target.lstrip("/"))
        rel = os.path.relpath(abs_in_sysroot, os.path.dirname(p))
        os.remove(p)
        os.symlink(rel, p)
        fixed += 1
print(f"  改写 {fixed} 个符号链接")
PY
}

verify() {
    log "校验 sysroot 关键内容"
    local f
    for f in usr/include/stdio.h usr/include/xf86drm.h usr/include/jpeglib.h \
             usr/include/freetype2/ft2build.h usr/include/cjson/cJSON.h \
             usr/lib/libdrm.so usr/lib/libjpeg.so usr/lib/libfreetype.so \
             usr/lib/libcjson.so usr/lib/libc.so; do
        [ -e "${SYSROOT_DIR}/${f}" ] || err "sysroot 缺少: ${f}"
    done
    log "=== sysroot 就绪: ${SYSROOT_DIR} ==="
    du -sh "${SYSROOT_DIR}"
}

main() {
    log "=== 目标 sysroot 组装开始 (${SUITE}/${TARGET_ARCH}) ==="
    check_prerequisites
    fetch_debs
    extract_debs
    flatten_multiarch
    relativize_symlinks
    verify
    log "=== 目标 sysroot 组装结束 ==="
}

main "$@"
