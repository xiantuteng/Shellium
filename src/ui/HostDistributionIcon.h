#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QSvgRenderer>

#include <algorithm>

/**
 * @brief 为已识别的远端 Linux 发行版生成侧栏图标：真实官方标志、按品牌色着色。
 *
 * 图标矢量取自 simple-icons 项目（CC0 授权，见 resources/icons/distros/NOTICE.md），
 * 原始文件都是不含颜色信息的单色轮廓。运行时按各品牌的官方色重新着色后直接展示在透明
 * 背景上——不叠加圆形底板，保持和真实 Logo 一致的观感；多数发行版的官方标志本身就是
 * 单一颜色（如 Ubuntu 一直是纯橙色），这是它们真实的设计，不是本程序简化的结果。
 * 识别依据始终是 /etc/os-release 的 ID，版本号只用于提示文本，不影响图标映射。
 */
namespace HostDistributionIcon {

/**
 * @brief 把 os-release 的发行版 ID 归类为侧栏图标样式。
 *
 * 只有确有对应真实官方标志矢量（resources/icons/distros/）的家族才单独枚举；没有可靠
 * 矢量来源的发行版（包括无法验证授权的私有/企业发行版）一律落到 GenericLinux 的手绘
 * 企鹅兜底，绝不用别的品牌图形冒充，也不会因新增或私有发行版而退回成"尚未识别"的普通
 * 服务器图标——两者在视觉上必须能区分开。
 */
enum class Family {
    Ubuntu,
    Debian,
    Fedora,
    RedHat,
    CentOS,
    Rocky,
    AlmaLinux,
    Arch,
    Manjaro,
    Alpine,
    OpenSuse,
    Suse,
    LinuxMint,
    Kali,
    Nix,
    Gentoo,
    Slackware,
    Void,
    PopOS,
    Elementary,
    Zorin,
    Deepin,
    RaspberryPi,
    Devuan,
    Solus,
    MXLinux,
    EndeavourOS,
    ArtixLinux,
    Photon,
    /** @brief OpenCloudOS 专用图标家族；该发行版的 ID 是 opencloudos。 */
    OpenCloudOS,
    /** @brief 银河麒麟专用图标家族；/etc/os-release 使用的 ID 是 kylin。 */
    Kylin,
    /** @brief 统信 UOS 专用图标家族；常见 ID 为 uos 或 uniontechos。 */
    UOS,
    GenericLinux
};

/** @brief 根据规范化 ID 返回最贴近的发行版视觉家族。 */
[[nodiscard]] inline Family familyForId(const QString &distributionId)
{
    const QString id = distributionId.trimmed().toLower();
    if (id == QStringLiteral("ubuntu")) {
        return Family::Ubuntu;
    }
    if (id == QStringLiteral("pop")) {
        return Family::PopOS;
    }
    if (id == QStringLiteral("elementary")) {
        return Family::Elementary;
    }
    if (id == QStringLiteral("debian")) {
        return Family::Debian;
    }
    if (id == QStringLiteral("raspbian")) {
        return Family::RaspberryPi;
    }
    if (id == QStringLiteral("devuan")) {
        return Family::Devuan;
    }
    if (id == QStringLiteral("fedora")) {
        return Family::Fedora;
    }
    // Oracle Linux 没有可靠授权来源的独立矢量，沿用红帽系视觉家族。
    if (id == QStringLiteral("rhel") || id == QStringLiteral("ol")) {
        return Family::RedHat;
    }
    if (id == QStringLiteral("centos")) {
        return Family::CentOS;
    }
    if (id == QStringLiteral("rocky")) {
        return Family::Rocky;
    }
    if (id == QStringLiteral("almalinux")) {
        return Family::AlmaLinux;
    }
    if (id == QStringLiteral("arch")) {
        return Family::Arch;
    }
    if (id == QStringLiteral("manjaro")) {
        return Family::Manjaro;
    }
    if (id == QStringLiteral("alpine")) {
        return Family::Alpine;
    }
    if (id == QStringLiteral("opensuse") || id == QStringLiteral("opensuse-leap")
        || id == QStringLiteral("opensuse-tumbleweed")) {
        return Family::OpenSuse;
    }
    if (id == QStringLiteral("sles")) {
        return Family::Suse;
    }
    if (id == QStringLiteral("linuxmint")) {
        return Family::LinuxMint;
    }
    if (id == QStringLiteral("kali")) {
        return Family::Kali;
    }
    if (id == QStringLiteral("nixos")) {
        return Family::Nix;
    }
    if (id == QStringLiteral("gentoo")) {
        return Family::Gentoo;
    }
    if (id == QStringLiteral("slackware")) {
        return Family::Slackware;
    }
    if (id == QStringLiteral("void")) {
        return Family::Void;
    }
    if (id == QStringLiteral("zorin")) {
        return Family::Zorin;
    }
    if (id == QStringLiteral("deepin") || id == QStringLiteral("deepin linux")) {
        return Family::Deepin;
    }
    if (id == QStringLiteral("solus")) {
        return Family::Solus;
    }
    if (id == QStringLiteral("mx")) {
        return Family::MXLinux;
    }
    if (id == QStringLiteral("endeavouros")) {
        return Family::EndeavourOS;
    }
    if (id == QStringLiteral("artix")) {
        return Family::ArtixLinux;
    }
    if (id == QStringLiteral("photon")) {
        return Family::Photon;
    }
    if (id == QStringLiteral("opencloudos") || id == QStringLiteral("opencloud")) {
        return Family::OpenCloudOS;
    }
    if (id == QStringLiteral("kylin") || id == QStringLiteral("kylin linux")
        || id == QStringLiteral("neokylin") || id == QStringLiteral("openkylin")) {
        return Family::Kylin;
    }
    if (id == QStringLiteral("uos") || id == QStringLiteral("uniontechos")
        || id == QStringLiteral("uniontech")) {
        return Family::UOS;
    }
    return Family::GenericLinux;
}

namespace detail {

/**
 * @brief 把亮度限制在可用范围内，同时保留原始色相与饱和度。
 *
 * 官方品牌色里不少是接近纯黑（如 AlmaLinux、Slackware）的取值：填满整个图标轮廓后会在
 * 深色主题的卡片背景上失去可见边界。这里只收紧亮度这一个维度，色相/饱和度原样保留，
 * 因此调整后仍是同一个品牌色的可辨识版本，不是换色。
 */
[[nodiscard]] inline QColor clampGlyphLightness(const QColor &color)
{
    float hue = 0.0F;
    float saturation = 0.0F;
    float lightness = 0.0F;
    float alpha = 0.0F;
    color.getHslF(&hue, &saturation, &lightness, &alpha);
    if (hue < 0.0F) {
        hue = 0.0F;
    }
    lightness = std::clamp(lightness, 0.30F, 0.82F);
    QColor clamped;
    clamped.setHslF(hue, saturation, lightness, alpha);
    return clamped;
}

} // namespace detail

/** @brief 返回各发行版家族的图标填充色（取自品牌官方色，深浅收紧后保证对比度）。 */
[[nodiscard]] inline QColor colorForFamily(const Family family)
{
    switch (family) {
    case Family::Ubuntu:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#e95420")));
    case Family::Debian:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#a81d33")));
    case Family::Fedora:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#51a2da")));
    case Family::RedHat:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#ee0000")));
    case Family::CentOS:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#262577")));
    case Family::Rocky:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#10b981")));
    case Family::AlmaLinux:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#000000")));
    case Family::Arch:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#1793d1")));
    case Family::Manjaro:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#35bfa4")));
    case Family::Alpine:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#0d597f")));
    case Family::OpenSuse:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#73ba25")));
    case Family::Suse:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#0c322c")));
    case Family::LinuxMint:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#86be43")));
    case Family::Kali:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#557c94")));
    case Family::Nix:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#5277c3")));
    case Family::Gentoo:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#54487a")));
    case Family::Slackware:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#000000")));
    case Family::Void:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#478061")));
    case Family::PopOS:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#48b9c7")));
    case Family::Elementary:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#64baff")));
    case Family::Zorin:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#15a6f0")));
    case Family::Deepin:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#007cff")));
    case Family::RaspberryPi:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#a22846")));
    case Family::Devuan:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#004489")));
    case Family::Solus:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#5294e2")));
    case Family::MXLinux:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#000000")));
    case Family::EndeavourOS:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#7f7fff")));
    case Family::ArtixLinux:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#10a0cc")));
    case Family::Photon:
        return detail::clampGlyphLightness(QColor(QStringLiteral("#004480")));
    case Family::OpenCloudOS:
        return QColor(QStringLiteral("#00a6e8"));
    case Family::Kylin:
        return QColor(QStringLiteral("#2176d2"));
    case Family::UOS:
        return QColor(QStringLiteral("#2f80ed"));
    case Family::GenericLinux:
        return QColor(QStringLiteral("#667085"));
    }
    return QColor(QStringLiteral("#667085"));
}

/**
 * @brief 返回家族对应的真实官方标志矢量资源路径。
 * @return GenericLinux 没有对应文件，返回空字符串，调用方据此改用手绘企鹅兜底。
 */
[[nodiscard]] inline QString svgResourcePathForFamily(const Family family)
{
    switch (family) {
    case Family::Ubuntu:
        return QStringLiteral(":/resources/icons/distros/ubuntu.svg");
    case Family::Debian:
        return QStringLiteral(":/resources/icons/distros/debian.svg");
    case Family::Fedora:
        return QStringLiteral(":/resources/icons/distros/fedora.svg");
    case Family::RedHat:
        return QStringLiteral(":/resources/icons/distros/redhat.svg");
    case Family::CentOS:
        return QStringLiteral(":/resources/icons/distros/centos.svg");
    case Family::Rocky:
        return QStringLiteral(":/resources/icons/distros/rockylinux.svg");
    case Family::AlmaLinux:
        return QStringLiteral(":/resources/icons/distros/almalinux.svg");
    case Family::Arch:
        return QStringLiteral(":/resources/icons/distros/archlinux.svg");
    case Family::Manjaro:
        return QStringLiteral(":/resources/icons/distros/manjaro.svg");
    case Family::Alpine:
        return QStringLiteral(":/resources/icons/distros/alpinelinux.svg");
    case Family::OpenSuse:
        return QStringLiteral(":/resources/icons/distros/opensuse.svg");
    case Family::Suse:
        return QStringLiteral(":/resources/icons/distros/suse.svg");
    case Family::LinuxMint:
        return QStringLiteral(":/resources/icons/distros/linuxmint.svg");
    case Family::Kali:
        return QStringLiteral(":/resources/icons/distros/kalilinux.svg");
    case Family::Nix:
        return QStringLiteral(":/resources/icons/distros/nixos.svg");
    case Family::Gentoo:
        return QStringLiteral(":/resources/icons/distros/gentoo.svg");
    case Family::Slackware:
        return QStringLiteral(":/resources/icons/distros/slackware.svg");
    case Family::Void:
        return QStringLiteral(":/resources/icons/distros/voidlinux.svg");
    case Family::PopOS:
        return QStringLiteral(":/resources/icons/distros/popos.svg");
    case Family::Elementary:
        return QStringLiteral(":/resources/icons/distros/elementary.svg");
    case Family::Zorin:
        return QStringLiteral(":/resources/icons/distros/zorin.svg");
    case Family::Deepin:
        return QStringLiteral(":/resources/icons/distros/deepin.svg");
    case Family::RaspberryPi:
        return QStringLiteral(":/resources/icons/distros/raspberrypi.svg");
    case Family::Devuan:
        return QStringLiteral(":/resources/icons/distros/devuan.svg");
    case Family::Solus:
        return QStringLiteral(":/resources/icons/distros/solus.svg");
    case Family::MXLinux:
        return QStringLiteral(":/resources/icons/distros/mxlinux.svg");
    case Family::EndeavourOS:
        return QStringLiteral(":/resources/icons/distros/endeavouros.svg");
    case Family::ArtixLinux:
        return QStringLiteral(":/resources/icons/distros/artixlinux.svg");
    case Family::Photon:
        return QStringLiteral(":/resources/icons/distros/photon.svg");
    case Family::OpenCloudOS:
        // OpenCloudOS 当前没有随项目分发的官方 SVG，使用专用云形轮廓而不是误用其它发行版。
        return {};
    case Family::Kylin:
        // 麒麟图标使用专用的低多边形绘制，不回退到通用 Linux 企鹅。
        return {};
    case Family::UOS:
        // UOS 图标使用专用的 U 形几何绘制，不回退到通用 Linux 企鹅。
        return {};
    case Family::GenericLinux:
        return {};
    }
    return {};
}

/**
 * @brief 返回展示在 Tooltip 中的发行版和版本文本。
 * @param distributionId /etc/os-release 的 ID。
 * @param versionId /etc/os-release 的 VERSION_ID，可为空。
 * @return 如"Ubuntu 24.04"的人类可读文本；未知 ID 保留原始 ID，避免丢失诊断信息。
 */
[[nodiscard]] inline QString displayName(const QString &distributionId, const QString &versionId)
{
    const QString id = distributionId.trimmed().toLower();
    const QString name = id == QStringLiteral("ubuntu") ? QStringLiteral("Ubuntu")
        : id == QStringLiteral("pop") ? QStringLiteral("Pop!_OS")
        : id == QStringLiteral("elementary") ? QStringLiteral("elementary OS")
        : id == QStringLiteral("debian") ? QStringLiteral("Debian")
        : id == QStringLiteral("raspbian") ? QStringLiteral("Raspberry Pi OS")
        : id == QStringLiteral("devuan") ? QStringLiteral("Devuan")
        : id == QStringLiteral("fedora") ? QStringLiteral("Fedora")
        : id == QStringLiteral("rhel") ? QStringLiteral("Red Hat Enterprise Linux")
        : id == QStringLiteral("ol") ? QStringLiteral("Oracle Linux")
        : id == QStringLiteral("centos") ? QStringLiteral("CentOS")
        : id == QStringLiteral("rocky") ? QStringLiteral("Rocky Linux")
        : id == QStringLiteral("almalinux") ? QStringLiteral("AlmaLinux")
        : id == QStringLiteral("arch") ? QStringLiteral("Arch Linux")
        : id == QStringLiteral("manjaro") ? QStringLiteral("Manjaro")
        : id == QStringLiteral("alpine") ? QStringLiteral("Alpine Linux")
        : id == QStringLiteral("opensuse") ? QStringLiteral("openSUSE")
        : id == QStringLiteral("sles") ? QStringLiteral("SUSE Linux Enterprise")
        : id == QStringLiteral("linuxmint") ? QStringLiteral("Linux Mint")
        : id == QStringLiteral("kali") ? QStringLiteral("Kali Linux")
        : id == QStringLiteral("nixos") ? QStringLiteral("NixOS")
        : id == QStringLiteral("gentoo") ? QStringLiteral("Gentoo")
        : id == QStringLiteral("slackware") ? QStringLiteral("Slackware")
        : id == QStringLiteral("void") ? QStringLiteral("Void Linux")
        : id == QStringLiteral("zorin") ? QStringLiteral("Zorin OS")
        : id == QStringLiteral("deepin") ? QStringLiteral("deepin")
        : id == QStringLiteral("deepin linux") ? QStringLiteral("deepin")
        : id == QStringLiteral("kylin") || id == QStringLiteral("neokylin")
              || id == QStringLiteral("openkylin") ? QStringLiteral("Kylin Linux")
        : id == QStringLiteral("uos") || id == QStringLiteral("uniontechos")
              || id == QStringLiteral("uniontech") ? QStringLiteral("UnionTech UOS")
        : id == QStringLiteral("solus") ? QStringLiteral("Solus")
        : id == QStringLiteral("mx") ? QStringLiteral("MX Linux")
        : id == QStringLiteral("endeavouros") ? QStringLiteral("EndeavourOS")
        : id == QStringLiteral("artix") ? QStringLiteral("Artix Linux")
        : id == QStringLiteral("photon") ? QStringLiteral("VMware Photon OS")
        : distributionId.trimmed();
    return versionId.trimmed().isEmpty() ? name : QStringLiteral("%1 %2").arg(name, versionId.trimmed());
}

namespace detail {

/**
 * @brief 手绘的通用 Linux 企鹅轮廓，仅用于没有真实矢量来源的家族。
 *
 * 不能假装是某个已知品牌，用一个明确表达"已确认 Linux，具体发行版未收录"的中性图形，
 * 与 UiIcons::Symbol::HostItem（"尚未识别，可能还没登录过"）在视觉上区分开。透明背景下
 * 主体直接用通用色填充，眼睛留白提供内部对比，不再依赖一个额外的圆底来撑场。
 */
inline void paintGenericLinuxGlyph(QPainter &painter, const QColor &color)
{
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(21.0, 14.0, 22.0, 29.0));
    painter.drawEllipse(QRectF(16.0, 35.0, 32.0, 17.0));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(25.0, 23.0, 4.0, 4.0));
    painter.drawEllipse(QRectF(35.0, 23.0, 4.0, 4.0));
}

/**
 * @brief 绘制 OpenCloudOS 的专用云形标识。
 * @param painter 目标画布画笔；调用方已启用抗锯齿并设置透明画布。
 * @param color OpenCloudOS 品牌青蓝色。
 *
 * OpenCloudOS 没有可随项目一起分发的官方矢量资源，因此单独绘制云形轮廓，避免把企鹅或
 * 其它发行版标志误当作 OpenCloudOS。轮廓保持小尺寸下的清晰度，适合主机列表图标容器。
 */
inline void paintOpenCloudOSGlyph(QPainter &painter, const QColor &color)
{
    Q_UNUSED(color)
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 官方标识由青色与蓝色两条相互咬合的弧带组成；保留双色是为了避免它被误认为普通
    // 云朵图标，也与 OpenCloudOS 的实际品牌识别保持一致。颜色固定而非跟随主题反相，
    // 是因为发行版 Logo 本身就是主机系统信息的一部分。
    painter.setBrush(Qt::NoBrush);
    QPainterPath leftRibbon;
    leftRibbon.moveTo(32.0, 10.0);
    leftRibbon.cubicTo(19.0, 13.0, 11.0, 21.0, 10.0, 31.0);
    leftRibbon.cubicTo(9.0, 41.0, 16.0, 50.0, 27.0, 53.0);
    leftRibbon.cubicTo(25.0, 43.0, 28.0, 36.0, 35.0, 31.0);
    leftRibbon.cubicTo(40.0, 27.0, 39.0, 18.0, 32.0, 10.0);
    QPainterPath rightRibbon;
    rightRibbon.moveTo(32.0, 10.0);
    rightRibbon.cubicTo(45.0, 13.0, 53.0, 21.0, 54.0, 31.0);
    rightRibbon.cubicTo(55.0, 41.0, 48.0, 50.0, 37.0, 53.0);
    rightRibbon.cubicTo(39.0, 43.0, 36.0, 36.0, 29.0, 31.0);
    rightRibbon.cubicTo(24.0, 27.0, 25.0, 18.0, 32.0, 10.0);
    painter.setPen(QPen(QColor(QStringLiteral("#16c1df")), 5.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(leftRibbon);
    painter.setPen(QPen(QColor(QStringLiteral("#2674d9")), 5.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(rightRibbon);
}

/**
 * @brief 绘制银河麒麟的专用低多边形图标。
 * @param painter 目标画布画笔；调用方已创建透明高分辨率画布。
 * @param color 麒麟主色；作为基础色保留与其它发行版图标一致的调用约定。
 *
 * 麒麟官方标识是蓝色几何麒麟，项目没有可直接分发的矢量资源，因此用少量多边形复现其
 * 可识别轮廓（角、头、躯干和尾部），并用相邻蓝色区块表达低多边形质感。这样 ID 为 kylin
 * 的主机不会错误显示成企鹅或其它发行版 Logo，同时在 20px 左右的列表尺寸仍保持清晰。
 */
inline void paintKylinGlyph(QPainter &painter, const QColor &color)
{
    Q_UNUSED(color)
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor light(QStringLiteral("#4ca4eb"));
    const QColor main(QStringLiteral("#2176d2"));
    const QColor dark(QStringLiteral("#1855a5"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(main);
    QPolygonF body;
    body << QPointF(31.0, 12.0) << QPointF(43.0, 18.0) << QPointF(47.0, 31.0)
         << QPointF(42.0, 43.0) << QPointF(31.0, 53.0) << QPointF(21.0, 43.0)
         << QPointF(16.0, 30.0) << QPointF(20.0, 18.0);
    painter.drawPolygon(body);
    painter.setBrush(light);
    painter.drawPolygon(QPolygonF{QPointF(31.0, 12.0), QPointF(43.0, 18.0),
                                  QPointF(35.0, 30.0), QPointF(28.0, 24.0)});
    painter.setBrush(dark);
    painter.drawPolygon(QPolygonF{QPointF(28.0, 24.0), QPointF(35.0, 30.0),
                                  QPointF(42.0, 43.0), QPointF(31.0, 53.0),
                                  QPointF(31.0, 33.0)});
    painter.setBrush(light);
    painter.drawPolygon(QPolygonF{QPointF(20.0, 18.0), QPointF(28.0, 24.0),
                                  QPointF(22.0, 38.0), QPointF(16.0, 30.0)});
    // 角和尾部用独立几何块补出麒麟轮廓，避免缩小后只剩普通菱形。
    painter.setBrush(dark);
    painter.drawPolygon(QPolygonF{QPointF(25.0, 17.0), QPointF(28.0, 5.0),
                                  QPointF(32.0, 14.0)});
    painter.setBrush(light);
    painter.drawPolygon(QPolygonF{QPointF(39.0, 42.0), QPointF(53.0, 48.0),
                                  QPointF(43.0, 49.0)});
}

/**
 * @brief 绘制统信 UOS 的专用几何图标。
 * @param painter 目标画布画笔；调用方已创建透明高分辨率画布。
 * @param color UOS 主色；参数保留以统一发行版绘制函数签名。
 *
 * 统信 UOS 没有随项目分发的独立 SVG，使用蓝色圆角 U 形轮廓表达其品牌字母标识。专用
 * 绘制路径可与 Deepin、麒麟和未知 Linux 图标区分，避免同一台国产系统显示错误品牌。
 */
inline void paintUosGlyph(QPainter &painter, const QColor &color)
{
    Q_UNUSED(color)
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(QStringLiteral("#2f80ed")), 7.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath uShape;
    uShape.moveTo(16.0, 16.0);
    uShape.lineTo(16.0, 34.0);
    uShape.cubicTo(16.0, 45.0, 22.0, 51.0, 32.0, 51.0);
    uShape.cubicTo(42.0, 51.0, 48.0, 45.0, 48.0, 34.0);
    uShape.lineTo(48.0, 16.0);
    painter.drawPath(uShape);
    painter.setPen(QPen(QColor(QStringLiteral("#53b7ff")), 3.0,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(16.0, 16.0), QPointF(16.0, 11.0));
    painter.drawLine(QPointF(48.0, 16.0), QPointF(48.0, 11.0));
}

/**
 * @brief 把 simple-icons 的单色轮廓按品牌色着色，居中贴到透明画布上。
 *
 * 原始矢量没有任何 fill 声明（默认黑色），且都在 24x24 视口内。这里先按黑色原样渲染到
 * 独立的透明位图，再用 SourceIn 合成模式把所有非透明像素统一改写成传入的品牌色——不必
 * 解析或修改 SVG 内容，对任意路径数、任意视口比例的图标都成立。图标不再叠加圆底，直接
 * 以官方标志本来的颜色和形状展示，边距因此收窄到贴近真实 Logo 的留白比例。
 */
inline void paintDistributionGlyph(QPainter &painter,
                                   const QString &resourcePath,
                                   const int canvasSize,
                                   const QColor &color)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid()) {
        return;
    }
    QRectF viewBox = renderer.viewBoxF();
    if (viewBox.isEmpty()) {
        viewBox = QRectF(0.0, 0.0, 24.0, 24.0);
    }

    constexpr qreal margin = 4.0;
    const QRectF targetBounds(margin, margin, canvasSize - 2.0 * margin, canvasSize - 2.0 * margin);
    const QSizeF scaledGlyph = viewBox.size().scaled(targetBounds.size(), Qt::KeepAspectRatio);
    const QRectF centeredGlyph(targetBounds.center().x() - scaledGlyph.width() / 2.0,
                               targetBounds.center().y() - scaledGlyph.height() / 2.0,
                               scaledGlyph.width(),
                               scaledGlyph.height());

    QPixmap glyph(canvasSize, canvasSize);
    glyph.fill(Qt::transparent);
    {
        QPainter glyphPainter(&glyph);
        glyphPainter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&glyphPainter, centeredGlyph);
    }
    {
        QPainter recolor(&glyph);
        recolor.setCompositionMode(QPainter::CompositionMode_SourceIn);
        recolor.fillRect(glyph.rect(), color);
    }
    painter.drawPixmap(0, 0, glyph);
}

} // namespace detail

/**
 * @brief 生成能随 QIcon 正常缩放的发行版图标。
 * @param distributionId 已验证的 /etc/os-release ID；空值返回空图标，由调用方显示通用主机。
 * @return 64px 透明位图包装的 QIcon；高于实际展示尺寸以保持 Retina 屏幕轮廓清晰。
 */
[[nodiscard]] inline QIcon iconFor(const QString &distributionId)
{
    if (distributionId.trimmed().isEmpty()) {
        return {};
    }

    const Family family = familyForId(distributionId);
    const QColor color = colorForFamily(family);

    constexpr int canvasSize = 64;
    QPixmap canvas(canvasSize, canvasSize);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QString resourcePath = svgResourcePathForFamily(family);
    if (resourcePath.isEmpty()) {
        // OpenCloudOS 使用自己的云形标识；只有其它未收录发行版才回退通用 Linux 企鹅。
        if (family == Family::OpenCloudOS) {
            detail::paintOpenCloudOSGlyph(painter, color);
        } else if (family == Family::Kylin) {
            detail::paintKylinGlyph(painter, color);
        } else if (family == Family::UOS) {
            detail::paintUosGlyph(painter, color);
        } else {
            detail::paintGenericLinuxGlyph(painter, color);
        }
    } else {
        detail::paintDistributionGlyph(painter, resourcePath, canvasSize, color);
    }
    painter.end();
    return QIcon(canvas);
}

} // namespace HostDistributionIcon
