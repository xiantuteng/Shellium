# syntax=docker/dockerfile:1.7

# Docker 负责 MinGW 编译与未签名打包。Qt/第三方 sysroot 由发布环境提供固定 URL 和 SHA-256，
# 避免镜像在不同日期静默拉取不同 ABI 或密码学依赖。
ARG BUILD_IMAGE=ubuntu:24.04
FROM $BUILD_IMAGE AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates cmake curl mingw-w64 ninja-build nsis zip zstd binutils-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*

ARG CROSS_BUNDLE_URL
ARG CROSS_BUNDLE_SHA256
ARG SHELLIUM_VERSION
RUN test -n "$CROSS_BUNDLE_URL" \
    && test -n "$CROSS_BUNDLE_SHA256" \
    && curl --fail --location --retry 3 "$CROSS_BUNDLE_URL" --output /tmp/cross.tar.zst \
    && echo "$CROSS_BUNDLE_SHA256  /tmp/cross.tar.zst" | sha256sum --check - \
    && mkdir -p /opt/shellium-cross \
    && tar --extract --zstd --file /tmp/cross.tar.zst --directory /opt/shellium-cross \
    && rm /tmp/cross.tar.zst

WORKDIR /work/source
COPY . .

# 依赖包约定 host 为 Linux moc/uic/rcc，target 为 Windows Qt 和第三方库，二者不能混用。
RUN cmake -S . -B /work/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DCMAKE_TOOLCHAIN_FILE=/work/source/cmake/toolchains/mingw-x86_64.cmake \
      -DSHELLIUM_WINDOWS_SYSROOT=/opt/shellium-cross/target \
      -DQT_HOST_PATH=/opt/shellium-cross/host \
    && cmake --build /work/build --parallel 4 \
    && cmake --install /work/build --prefix /work/staging

# 交叉环境不能运行 windeployqt。先收集锁定 sysroot 中的 DLL/插件，再递归验证所有 PE imports。
RUN find /opt/shellium-cross/target/bin -maxdepth 1 -type f -iname '*.dll' \
      -exec cp -f '{}' /work/staging/ ';' \
    && mkdir -p /work/staging/plugins \
    && cp -a /opt/shellium-cross/target/plugins/platforms /work/staging/plugins/ \
    && cp -a /opt/shellium-cross/target/plugins/sqldrivers /work/staging/plugins/ \
    && if test -d /opt/shellium-cross/target/plugins/imageformats; then \
         cp -a /opt/shellium-cross/target/plugins/imageformats /work/staging/plugins/; \
       fi \
    && if test -d /opt/shellium-cross/target/plugins/tls; then \
         cp -a /opt/shellium-cross/target/plugins/tls /work/staging/plugins/; \
       fi \
    && cp /work/source/packaging/windows/qt.conf /work/staging/qt.conf \
    && bash /work/source/scripts/verify_windows_dependencies.sh /work/staging

RUN mkdir -p /work/artifacts \
    && cd /work/staging \
    && cmake -E tar cf /work/artifacts/Shellium-$SHELLIUM_VERSION-win64.zip --format=zip -- . \
    && makensis -DSTAGING_DIR=/work/staging -DSHELLIUM_VERSION=$SHELLIUM_VERSION \
       -DOUTPUT_FILE=/work/artifacts/Shellium-$SHELLIUM_VERSION-setup-x64.exe \
       /work/source/packaging/windows/Shellium.nsi \
    && cd /work/artifacts \
    && sha256sum Shellium-$SHELLIUM_VERSION-win64.zip \
       Shellium-$SHELLIUM_VERSION-setup-x64.exe \
       > Shellium-$SHELLIUM_VERSION-SHA256SUMS.txt

FROM scratch AS artifacts
COPY --from=builder /work/artifacts/ /
