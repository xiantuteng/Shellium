#!/usr/bin/env bash
set -euo pipefail

# 发布者必须提供固定依赖包和校验和；Apple Silicon 同样强制使用 amd64 容器生成 x64 PE。
bundle_url="$(printenv SHELLIUM_CROSS_BUNDLE_URL || true)"
bundle_sha256="$(printenv SHELLIUM_CROSS_BUNDLE_SHA256 || true)"
test -n "$bundle_url" || { echo "缺少 SHELLIUM_CROSS_BUNDLE_URL" >&2; exit 2; }
test -n "$bundle_sha256" || { echo "缺少 SHELLIUM_CROSS_BUNDLE_SHA256" >&2; exit 2; }

script_directory="$(cd "$(dirname "$0")" && pwd)"
repository_root="$(cd "$script_directory/.." && pwd)"
artifact_directory="$repository_root/dist/windows"
project_version="$(sed -n 's/^project(Shellium VERSION \([0-9.]*\).*/\1/p' \
  "$repository_root/CMakeLists.txt" | head -n 1)"
test -n "$project_version" || { echo "无法读取 Shellium 项目版本" >&2; exit 2; }
mkdir -p "$artifact_directory"

docker buildx build \
  --platform linux/amd64 \
  --file "$repository_root/packaging/windows/Dockerfile" \
  --build-arg "CROSS_BUNDLE_URL=$bundle_url" \
  --build-arg "CROSS_BUNDLE_SHA256=$bundle_sha256" \
  --build-arg "SHELLIUM_VERSION=$project_version" \
  --target artifacts \
  --output "type=local,dest=$artifact_directory" \
  "$repository_root"
