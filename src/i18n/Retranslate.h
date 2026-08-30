#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>
#include <utility>

namespace i18n {

namespace detail {

/**
 * @brief 全局重译分发器：捕获 qApp 的 LanguageChange，逐个宿主重跑已登记的文案闭包。
 *
 * 只重写虚函数 `eventFilter`，不声明信号/槽，因此不需要 Q_OBJECT / moc，可整体内联进头文件，
 * 各测试目标无需单独把某个 .cpp 加进源清单。单例挂在 qApp 上，生命周期与应用一致。
 */
class Retranslator final : public QObject {
public:
    static Retranslator *instance()
    {
        static QPointer<Retranslator> self;
        if (self.isNull() && QCoreApplication::instance() != nullptr) {
            self = new Retranslator(QCoreApplication::instance());
            QCoreApplication::instance()->installEventFilter(self);
        }
        return self.data();
    }

    void add(QObject *owner, std::function<void()> apply)
    {
        m_entries.append({owner, apply});
        QObject::connect(owner, &QObject::destroyed, this, [this](QObject *dead) {
            m_entries.removeIf([dead](const Entry &e) { return e.owner.data() == dead; });
        });
        apply();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == QCoreApplication::instance() && event->type() == QEvent::LanguageChange) {
            const QList<Entry> snapshot = m_entries;
            for (const Entry &entry : snapshot) {
                if (entry.owner) {
                    entry.apply();
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    explicit Retranslator(QObject *parent) : QObject(parent) {}

    struct Entry {
        QPointer<QObject> owner;
        std::function<void()> apply;
    };
    QList<Entry> m_entries;
};

} // namespace detail

/**
 * @brief 注册一个「重设界面文案」的回调，随语言切换自动重跑。
 * @param owner  回调的生命周期宿主；owner 被销毁后回调自动注销。通常传 `this`，
 *               以便回调里的 `tr()` 使用该控件类的翻译上下文。
 * @param apply  重新应用静态文案的闭包：立即执行一次（相当于初始化文案），之后每当
 *               安装/卸载翻译器（`QEvent::LanguageChange`）时再次执行。
 *
 * 用它替代构造函数里一次性 `setText(tr(...))` 的写法，常驻控件即可免重启热切换语言：
 * @code
 * i18n::onRetranslate(this, [this] {
 *     m_newHostButton->setText(tr("New host"));
 *     m_searchEdit->setPlaceholderText(tr("Search name, address, or account"));
 * });
 * @endcode
 * 按需弹出的对话框不必使用它——它们下次打开时本来就以当前语言重新构造。
 */
inline void onRetranslate(QObject *owner, std::function<void()> apply)
{
    if (owner == nullptr || !apply) {
        return;
    }
    if (detail::Retranslator *retranslator = detail::Retranslator::instance()) {
        retranslator->add(owner, std::move(apply));
    } else {
        apply();
    }
}

} // namespace i18n
