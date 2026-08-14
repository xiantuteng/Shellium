# Shellium Apple Silicon DMG 发布指南

本文说明 Shellium 在不通过 Mac App Store 上架的情况下，如何生成可供其他用户直接安装的
Apple Silicon DMG。正式发布采用 Apple 推荐的站外分发链路：Developer ID Application
签名、Hardened Runtime、安全时间戳、Apple 公证以及票据装订。

## 一、Apple 侧准备

发布者需要加入付费 Apple Developer Program，并在登录钥匙串中安装包含私钥的
`Developer ID Application` 证书。它不同于 Apple Development、Mac App Distribution
以及 Developer ID Installer；本项目发布 `.app` 和 `.dmg`，不需要 Installer 证书。

使用以下命令检查证书：

~~~bash
security find-identity -v -p codesigning
~~~

输出中必须存在类似以下身份，并且不能显示 `0 valid identities found`：

~~~text
Developer ID Application: 发布者名称 (TEAMID)
~~~

随后将 Apple ID 的 App 专用密码保存到钥匙串。不要把密码直接写进脚本、环境变量或命令行；
省略 `--password` 后，`notarytool` 会通过安全提示读取密码：

~~~bash
xcrun notarytool store-credentials "Shellium-notary" \
  --apple-id "你的 Apple ID" \
  --team-id "你的 Team ID"
~~~

这一步默认会立即连接 Apple 服务验证凭据。脚本之后只引用钥匙串配置名，不会读取或输出密码。

## 二、兼容性约束

当前开发机通过 Homebrew 安装的 libssh、libvterm、libsodium、spdlog 和 fmt 均以 macOS
26.0 为最低目标构建，因此默认安装包也明确要求 macOS 26.0。仅修改
`SHELLIUM_DEPLOYMENT_TARGET` 不能让现成动态库支持旧系统。

若要支持更早版本的 Apple Silicon macOS，必须先用目标 deployment target 重新编译全部
第三方依赖，再通过 `PKG_CONFIG_PATH` 和 `CMAKE_PREFIX_PATH` 让构建使用这套依赖。发布脚本会
检查 Bundle 内的架构和开发机绝对路径，但最低系统兼容性仍应在对应真实系统上做启动测试。

## 三、正式一键发布

默认 Bundle Identifier 为 `com.shellium.Shellium`。正式发布前建议换成发布者控制的稳定反向
域名；后续版本不要随意更改，否则 macOS 会把它视为不同应用。

~~~bash
SHELLIUM_BUNDLE_IDENTIFIER="com.yourcompany.shellium" \
./scripts/package_macos_dmg.sh
~~~

如果钥匙串内有多个 Developer ID Application 证书，应明确指定完整身份：

~~~bash
SHELLIUM_BUNDLE_IDENTIFIER="com.yourcompany.shellium" \
SHELLIUM_SIGNING_IDENTITY="Developer ID Application: 发布者名称 (TEAMID)" \
./scripts/package_macos_dmg.sh
~~~

成功后正式产物位于：

~~~text
dist/Shellium-0.1.8-macos-arm64.dmg
~~~

脚本只有在下列检查全部通过后才会报告完成：

1. 主程序及 Bundle 内所有 Mach-O 文件均包含 arm64 架构；
2. 动态库引用不包含 `/opt/homebrew`、`/usr/local` 或项目源码绝对路径；
3. App 的嵌套签名、Developer ID Authority 和 Hardened Runtime 均有效；
4. DMG 完整性及 Developer ID 签名有效；
5. Apple 公证状态为 `Accepted`；
6. 公证票据已装订，且 Gatekeeper 接受 DMG 主签名。

## 四、检查与本地验证模式

只检查正式发布前置条件，不产生构建文件：

~~~bash
./scripts/package_macos_dmg.sh --check
~~~

尚未取得 Developer ID 证书时，可以先验证 Release 编译、运行时依赖收集、arm64 架构和 DMG
制作流程：

~~~bash
./scripts/package_macos_dmg.sh --adhoc
~~~

该模式产物文件名包含 `-adhoc`，只具有临时本地签名，不能通过面向其他用户的 Developer ID
信任和 Apple 公证，严禁作为正式安装包发布。

在证书已经可用、但 Apple 公证服务暂时不可达时，可生成受控测试用的已签名未公证包：

~~~bash
./scripts/package_macos_dmg.sh --skip-notarization
~~~

未公证安装包可能被 Gatekeeper 阻止，因此该选项不属于正式发布成功路径。
