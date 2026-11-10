#include "docxstudio/app/ColorPaletteWidget.h"

#include <QGridLayout>
#include <QLabel>
#include <QKeyEvent>
#include <limits>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>

namespace docxstudio::app {
namespace {
struct NamedColor { const char* name; const char* hex; };
constexpr std::array<NamedColor,10> themeColors{{
    {"Warm white", "#FAFAF7"}, {"Graphite", "#222624"},
    {"Forest", "#173B32"}, {"Slate", "#626A66"},
    {"Green", "#2F6B57"}, {"Brown", "#6F5747"},
    {"Purple", "#66507A"}, {"Blue", "#356078"},
    {"Yellow", "#8F6B1C"}, {"Red", "#994A55"}
}};
constexpr std::array<NamedColor,28> paletteColors{{
    {"Light green", "#69947B"}, {"Light brown", "#9C765C"},
    {"Light purple", "#9A7EAD"}, {"Light blue", "#6F94AC"},
    {"Light yellow", "#B38B30"}, {"Light red", "#C06B73"},
    {"Orange", "#A95F2B"}, {"Light orange", "#C37C40"},
    {"Dark palette white", "#F4F3EF"}, {"Soft slate", "#AEB7B2"},
    {"Pattern green", "#789087"}, {"Dark palette green", "#73A88F"},
    {"Dark palette light green", "#A5C8B2"}, {"Dark palette brown", "#AD846B"},
    {"Dark palette light brown", "#D2B29A"}, {"Dark palette purple", "#9B82B4"},
    {"Dark palette light purple", "#C2ADD2"}, {"Dark palette blue", "#6D9FC0"},
    {"Dark palette light blue", "#A2C3D7"}, {"Dark palette yellow", "#D2AE4D"},
    {"Dark palette light yellow", "#E8D18A"}, {"Dark palette red", "#C46F79"},
    {"Dark palette light red", "#E0A0A5"}, {"Dark palette orange", "#D18A4C"},
    {"Dark palette light orange", "#E6B685"}, {"Black", "#000000"},
    {"White", "#FFFFFF"}, {"Gray", "#808080"}
}};
constexpr std::array<NamedColor,10> standardColors{{
    {"Black", "#000000"}, {"White", "#FFFFFF"}, {"Red", "#FF0000"},
    {"Orange", "#FFC000"}, {"Yellow", "#FFFF00"}, {"Green", "#00B050"},
    {"Cyan", "#00B0F0"}, {"Blue", "#0070C0"},
    {"Purple", "#7030A0"}, {"Gray", "#808080"}
}};

QColor mix(const QColor& base, const QColor& target, int percent) {
    const auto channel=[percent](int from,int to) {
        return (from*(100-percent)+to*percent+50)/100;
    };
    return QColor(channel(base.red(),target.red()),channel(base.green(),target.green()),
                  channel(base.blue(),target.blue()));
}

class PaletteActionButton : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) {
            click();event->accept();return;
        }
        QToolButton::keyPressEvent(event);
    }
};

class Swatch final : public PaletteActionButton {
public:
    Swatch(const QColor& color,const QString& description,QWidget* parent)
        : PaletteActionButton(parent),color_(color) {
        setFixedSize(22,22);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setProperty("paletteColor",color);
        setToolTip(description+QStringLiteral(" (%1)").arg(color.name().toUpper()));
        setAccessibleName(toolTip());
        setAccessibleDescription(tr("Choose this color"));
    }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        const int key=event->key();
        if (key==Qt::Key_Return || key==Qt::Key_Enter) {
            click();event->accept();return;
        }
        if (key==Qt::Key_Left || key==Qt::Key_Right || key==Qt::Key_Up || key==Qt::Key_Down) {
            Swatch* nearest=nullptr;
            int best=std::numeric_limits<int>::max();
            const QPoint origin=geometry().center();
            for (auto* candidate : parentWidget()->findChildren<QToolButton*>(QString{},Qt::FindDirectChildrenOnly)) {
                auto* swatch=dynamic_cast<Swatch*>(candidate);
                if (!swatch || swatch==this || !swatch->isEnabled()) continue;
                const QPoint delta=swatch->geometry().center()-origin;
                const bool horizontal=key==Qt::Key_Left || key==Qt::Key_Right;
                const int forward=horizontal ? delta.x() : delta.y();
                const int side=horizontal ? delta.y() : delta.x();
                if ((key==Qt::Key_Right || key==Qt::Key_Down) ? forward<=0 : forward>=0) continue;
                const int distance=std::abs(forward)+100*std::abs(side);
                if (distance<best) { best=distance;nearest=swatch; }
            }
            if (nearest) nearest->setFocus(Qt::OtherFocusReason);
            event->accept();return;
        }
        QToolButton::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(),palette().color(QPalette::Base));
        const QRect chip=rect().adjusted(2,2,-3,-3);
        painter.fillRect(chip,color_);
        painter.setPen(QPen(palette().color(hasFocus() || underMouse() || isChecked() ? QPalette::Highlight : QPalette::Mid),
                            hasFocus() || underMouse() || isChecked() ? 2 : 1));
        painter.drawRect(chip);
    }
private:
    QColor color_;
};
}

struct ColorPaletteWidget::Private {
    Usage usage;
    QVBoxLayout* layout{};
    QWidget* recentHolder{};
    QGridLayout* recentGrid{};
    QToolButton* defaultButton{};
    QColor current;
    QString settingsKey;
};

ColorPaletteWidget::ColorPaletteWidget(Usage usage,QWidget* parent)
    : QWidget(parent),d_(std::make_unique<Private>()) {
    d_->usage=usage;
    d_->settingsKey=usage==Usage::Text ? QStringLiteral("palette/text/recentColors") : QStringLiteral("palette/highlight/recentColors");
    setObjectName(QStringLiteral("color.palette"));
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(usage==Usage::Text ? tr("Text color palette") : tr("Highlight color palette"));
    d_->layout=new QVBoxLayout(this);
    d_->layout->setContentsMargins(8,8,8,8);
    d_->layout->setSpacing(6);
    d_->layout->setSizeConstraint(QLayout::SetFixedSize);
    d_->defaultButton=new PaletteActionButton(this);
    d_->defaultButton->setObjectName(QStringLiteral("palette.default"));
    d_->defaultButton->setText(usage==Usage::Text ? tr("Automatic") : tr("No Color"));
    d_->defaultButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    d_->defaultButton->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    d_->defaultButton->setCheckable(true);
    d_->defaultButton->setFocusPolicy(Qt::StrongFocus);
    d_->defaultButton->setAccessibleName(d_->defaultButton->text());
    d_->layout->addWidget(d_->defaultButton);
    connect(d_->defaultButton,&QToolButton::clicked,this,[this] {
        setCurrentColor({});emit defaultRequested();
    });
    const auto section=[this](const QString& title) {
        auto* label=new QLabel(title,this);
        auto font=label->font();font.setBold(true);label->setFont(font);
        d_->layout->addWidget(label);
        auto* holder=new QWidget(this);
        auto* grid=new QGridLayout(holder);
        grid->setContentsMargins(0,0,0,0);grid->setHorizontalSpacing(2);grid->setVerticalSpacing(2);
        d_->layout->addWidget(holder);
        return grid;
    };
    int serial=0;
    const auto add=[this,&serial](QGridLayout* grid,int row,int column,const QColor& color,const QString& name) {
        auto* button=new Swatch(color,name,grid->parentWidget());
        button->setObjectName(QStringLiteral("palette.swatch.%1").arg(serial++));
        grid->addWidget(button,row,column);
        connect(button,&QToolButton::clicked,this,[this,color] {
            setCurrentColor(color);rememberColor(color);emit colorSelected(color);
        });
    };
    auto* themes=section(tr("Theme Colors"));
    constexpr std::array<int,5> amounts{80,60,40,25,50};
    constexpr std::array<int,5> whiteAmounts{5,15,25,35,50};
    for (std::size_t column=0;column<themeColors.size();++column) {
        const auto& entry=themeColors[column];
        const QColor base(QString::fromLatin1(entry.hex));
        const QString name=QString::fromLatin1(entry.name);
        add(themes,0,static_cast<int>(column),base,name);
        for (std::size_t shade=0;shade<amounts.size();++shade) {
            const bool nearWhite=base.lightness()>230;
            const bool lighter=!nearWhite && shade<3U;
            const int amount=nearWhite ? whiteAmounts[shade] : amounts[shade];
            const QColor color=mix(base,lighter ? QColor(Qt::white) : QColor(Qt::black),amount);
            add(themes,static_cast<int>(shade+1U),static_cast<int>(column),color,
                tr("%1, %2 %3%").arg(name,lighter ? tr("lighter") : tr("darker")).arg(amount));
        }
    }
    auto* palette=section(tr("Palette Colors"));
    for (std::size_t index=0;index<paletteColors.size();++index)
        add(palette,static_cast<int>(index/10U),static_cast<int>(index%10U),
            QColor(QString::fromLatin1(paletteColors[index].hex)),QString::fromLatin1(paletteColors[index].name));
    auto* standards=section(tr("Standard Colors"));
    for (std::size_t index=0;index<standardColors.size();++index)
        add(standards,0,static_cast<int>(index),QColor(QString::fromLatin1(standardColors[index].hex)),
            QString::fromLatin1(standardColors[index].name));
    if (usage==Usage::Highlight) {
        constexpr std::array<NamedColor,8> highlighters{{
            {"Yellow", "#FFF200"}, {"Bright green", "#00FF00"},
            {"Cyan", "#00FFFF"}, {"Pink", "#FF00FF"},
            {"Blue", "#00A2FF"}, {"Red", "#FF0000"},
            {"Orange", "#FFB347"}, {"Gray", "#D9D9D9"}}};
        for (std::size_t index=0;index<highlighters.size();++index)
            add(standards,1,static_cast<int>(index),QColor(QString::fromLatin1(highlighters[index].hex)),
                QString::fromLatin1(highlighters[index].name));
    }
    d_->recentHolder=new QWidget(this);
    d_->recentHolder->setObjectName(QStringLiteral("palette.recents"));
    auto* recentLayout=new QVBoxLayout(d_->recentHolder);
    recentLayout->setContentsMargins(0,0,0,0);recentLayout->setSpacing(6);
    auto* recentTitle=new QLabel(tr("Recent Colors"),d_->recentHolder);
    auto recentFont=recentTitle->font();recentFont.setBold(true);recentTitle->setFont(recentFont);
    recentLayout->addWidget(recentTitle);
    auto* recentButtons=new QWidget(d_->recentHolder);
    d_->recentGrid=new QGridLayout(recentButtons);
    d_->recentGrid->setContentsMargins(0,0,0,0);d_->recentGrid->setHorizontalSpacing(2);
    recentLayout->addWidget(recentButtons);d_->layout->addWidget(d_->recentHolder);
    auto* custom=new PaletteActionButton(this);
    custom->setObjectName(QStringLiteral("palette.custom"));custom->setText(tr("More Colors…"));
    custom->setAccessibleName(custom->text());custom->setToolButtonStyle(Qt::ToolButtonTextOnly);
    custom->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);custom->setFocusPolicy(Qt::StrongFocus);
    d_->layout->addWidget(custom);
    connect(custom,&QToolButton::clicked,this,&ColorPaletteWidget::customColorRequested);
    refreshRecentColors();setCurrentColor({});
}

void ColorPaletteWidget::setCurrentColor(const QColor& color) {
    d_->current=color;
    d_->defaultButton->setChecked(!color.isValid());
    QWidget* focus=d_->defaultButton;
    bool found=false;
    for (auto* button:findChildren<QToolButton*>()) {
        const auto value=button->property("paletteColor").value<QColor>();
        if (value.isValid()) {
            button->setChecked(value==color);button->update();
            if (!found && value==color) { focus=button;found=true; }
        }
    }
    setFocusProxy(focus);
}

void ColorPaletteWidget::rememberColor(const QColor& color) {
    if (!color.isValid()) return;
    QSettings settings;
    auto recent=settings.value(d_->settingsKey).toStringList();
    const auto value=color.name(QColor::HexArgb);
    recent.removeAll(value);recent.prepend(value);
    while (recent.size()>10) recent.removeLast();
    settings.setValue(d_->settingsKey,recent);
    refreshRecentColors();setCurrentColor(d_->current);
}

void ColorPaletteWidget::refreshRecentColors() {
    while (auto* item=d_->recentGrid->takeAt(0)) {
        if (auto* widget=item->widget()) { widget->hide();widget->setParent(nullptr);widget->deleteLater(); }
        delete item;
    }
    const auto recent=QSettings().value(d_->settingsKey).toStringList();
    int column=0;
    QSet<QRgb> used;
    for (const auto& value:recent) {
        const QColor color(value);
        if (!color.isValid() || used.contains(color.rgba()) || column>=10) continue;
        used.insert(color.rgba());
        auto* swatch=new Swatch(color,tr("Recent color"),d_->recentGrid->parentWidget());
        swatch->setObjectName(QStringLiteral("palette.recent.%1").arg(column));
        d_->recentGrid->addWidget(swatch,0,column++);
        connect(swatch,&QToolButton::clicked,this,[this,color] {
            setCurrentColor(color);rememberColor(color);emit colorSelected(color);
        });
    }
    d_->recentHolder->setVisible(column>0);
}

ColorPaletteWidget::~ColorPaletteWidget()=default;
} // namespace docxstudio::app
