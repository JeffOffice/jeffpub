// Insert > Barcode: book ISBNs with or without a price add-on, EAN-13, UPC-A,
// EAN-8, Code 128 and Code 39, placed as one vector shape (bars and digits)
// on a white quiet zone, grouped.

#include "app/dialogs.h"
#include "app/editor.h"
#include "app/settings.h"
#include "core/barcode.h"
#include "core/document.h"
#include "core/items.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace jp {

namespace {
// The digits' font: OCR-B (the standard for EAN and ISBN digits) when the
// computer has it, otherwise Arimo.
QString digitFamily()
{
    static const QString f = [] {
        for (const QString &name : {QStringLiteral("OCR-B"), QStringLiteral("OCR B"), QStringLiteral("OCRB"), QStringLiteral("OCR-B 10 BT")})
            if (QFontDatabase::hasFamily(name)) return name;
        return QStringLiteral("Arimo");
    }();
    return f;
}
} // namespace

QPainterPath barcodePath(const barcode::Layout &l)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const QRectF &r : l.bars) path.addRect(r);
    // Digits as letter outlines (the same everywhere, whatever fonts are
    // installed), made at 1000 px and scaled: Qt sizes fonts in whole pixels.
    QFont f(digitFamily());
    f.setPixelSize(1000);
    const QFontMetricsF fm(f);
    for (const barcode::Label &lb : l.labels) {
        QPainterPath t;
        t.addText(QPointF(-fm.horizontalAdvance(lb.text) / 2, 0), f, lb.text);
        QTransform m;
        m.translate(lb.baseline.x(), lb.baseline.y());
        m.scale(lb.size / 1000, lb.size / 1000);
        path.addPath(m.map(t));
    }
    return path;
}

ItemPtr barcodeItem(const barcode::Layout &l, const QPointF &topLeft, bool whiteBackground, const QString &description, const QJsonObject &settings)
{
    auto group = std::make_shared<GroupItem>();
    group->barcode = settings;
    const QRectF frame(topLeft, l.size);
    if (whiteBackground) {
        auto bg = std::make_shared<ShapeItem>();
        bg->rect = frame;
        bg->fill = Fill::solid(ColorRef::rgb(Qt::white));
        bg->stroke = Stroke::none();
        bg->name = QStringLiteral("Quiet zone");
        group->children.push_back(bg);
    }
    auto bars = std::make_shared<ShapeItem>();
    bars->rect = frame;
    bars->customPath = barcodePath(l);
    bars->fill = Fill::solid(ColorRef::rgb(Qt::black));
    bars->stroke = Stroke::none();
    bars->name = QStringLiteral("Bars");
    group->children.push_back(bars);
    group->name = QStringLiteral("Barcode");
    group->altText = description;
    group->syncRect();
    return group;
}

void barcodeDialog(QWidget *p, Editor *ed)
{
    using namespace barcode;
    Settings &st = Settings::get();
    // With a barcode selected, the dialog edits it: its own settings, and
    // the new one takes its place.
    GroupItem *editing = nullptr;
    if (const QVector<Item *> sel = ed->selectedItems(); sel.size() == 1)
        if (auto *g = dynamic_cast<GroupItem *>(sel.first()); g && !g->barcode.isEmpty()) editing = g;
    const QJsonObject was = editing ? editing->barcode : QJsonObject();
    auto setting = [&](const char *key, const QVariant &def) {
        return was.isEmpty() ? st.value(QStringLiteral("barcode/") + QLatin1String(key), def) : was.value(QLatin1String(key)).toVariant();
    };
    QDialog dlg(p);
    dlg.setWindowTitle(editing ? QStringLiteral("Edit Barcode") : QStringLiteral("Insert Barcode"));
    auto *v = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout();
    auto *type = new QComboBox(&dlg);
    type->addItems({QStringLiteral("Book (ISBN)"), QStringLiteral("EAN-13"), QStringLiteral("UPC-A"), QStringLiteral("EAN-8"),
                    QStringLiteral("Code 128 (letters and digits)"), QStringLiteral("Code 39 (capitals and digits)")});
    type->setCurrentIndex(std::clamp(setting("type", 0).toInt(), 0, 5));
    form->addRow(QStringLiteral("Type:"), type);
    auto *data = new QLineEdit(&dlg);
    data->setText(setting("data", QString()).toString());
    data->setMinimumWidth(260);
    form->addRow(QStringLiteral("ISBN:"), data);
    auto *addOn = new QLineEdit(&dlg);
    addOn->setPlaceholderText(QStringLiteral("none, or 2 or 5 digits"));
    form->addRow(QStringLiteral("Add-on:"), addOn);
    v->addLayout(form);

    // A book's price add-on.
    auto *price = new QGroupBox(QStringLiteral("Price add-on"), &dlg);
    auto *pv = new QVBoxLayout(price);
    auto *noAddOn = new QRadioButton(QStringLiteral("No add-on"), price);
    auto *withPrice = new QRadioButton(QStringLiteral("Price:"), price);
    auto *noPrice = new QRadioButton(QStringLiteral("No suggested price (90000)"), price);
    auto *group = new QButtonGroup(price);
    group->addButton(noAddOn, 0);
    group->addButton(withPrice, 1);
    group->addButton(noPrice, 2);
    auto *currency = new QComboBox(price);
    currency->addItems({QStringLiteral("US dollars"), QStringLiteral("Canadian dollars"), QStringLiteral("British pounds"), QStringLiteral("Australian dollars"),
                        QStringLiteral("New Zealand dollars")});
    currency->setCurrentIndex(std::clamp(setting("currency", 0).toInt(), 0, 4));
    auto *amount = new QDoubleSpinBox(price);
    amount->setRange(0, 99.99);
    amount->setDecimals(2);
    amount->setValue(setting("price", 19.95).toDouble());
    auto *pr = new QHBoxLayout();
    pr->addWidget(withPrice);
    pr->addWidget(amount);
    pr->addWidget(currency);
    pr->addStretch(1);
    pv->addWidget(noAddOn);
    pv->addLayout(pr);
    pv->addWidget(noPrice);
    group->button(std::clamp(setting("priceMode", 1).toInt(), 0, 2))->setChecked(true);
    v->addWidget(price);

    auto *size = new QGroupBox(QStringLiteral("Size"), &dlg);
    auto *sf = new QFormLayout(size);
    auto *mag = new QSpinBox(size);
    mag->setRange(50, 300);
    mag->setSuffix(QStringLiteral("%"));
    mag->setValue(setting("magnification", 100).toInt());
    mag->setToolTip(QStringLiteral("100% is the standard size; books usually print at 80% to 100%."));
    mag->setFixedWidth(110);
    auto *height = new QSpinBox(size);
    height->setRange(40, 100);
    height->setSuffix(QStringLiteral("%"));
    height->setValue(setting("height", 100).toInt());
    height->setToolTip(QStringLiteral("Shorter bars save room; scanners need most of the standard height."));
    height->setFixedWidth(110);
    sf->addRow(QStringLiteral("Magnification:"), mag);
    sf->addRow(QStringLiteral("Bar height:"), height);
    v->addWidget(size);
    auto *showText = new QCheckBox(QStringLiteral("Print the digits"), &dlg);
    showText->setChecked(setting("text", true).toBool());
    auto *white = new QCheckBox(QStringLiteral("White background (the clear space scanners need)"), &dlg);
    white->setChecked(setting("white", true).toBool());
    auto *check39 = new QCheckBox(QStringLiteral("Add a check character"), &dlg);
    check39->setChecked(setting("check39", false).toBool());
    addOn->setText(setting("addOn", QString()).toString());
    v->addWidget(showText);
    v->addWidget(white);
    v->addWidget(check39);

    auto *preview = new QLabel(&dlg);
    preview->setMinimumSize(380, 190);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("QLabel{background:#ffffff; border:1px solid #d0d4da;}"));
    preview->setObjectName(QStringLiteral("barcodePreview"));
    v->addWidget(preview);
    auto *problem = new QLabel(&dlg);
    problem->setStyleSheet(QStringLiteral("color:#c0392b;"));
    problem->setWordWrap(true);
    v->addWidget(problem);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    QPushButton *insert = bb->addButton(editing ? QStringLiteral("Update") : QStringLiteral("Insert"), QDialogButtonBox::AcceptRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);

    Layout current;
    auto options = [&] {
        Options o;
        o.type = Type(type->currentIndex());
        o.data = data->text();
        o.magnification = mag->value() / 100.0;
        o.barHeight = height->value() / 100.0;
        o.text = showText->isChecked();
        o.code39Check = check39->isChecked();
        if (o.type == Type::Isbn) {
            const int mode = group->checkedId();
            QString err;
            if (mode == 1) o.addOn = priceAddOn(Currency(currency->currentIndex() + 1), amount->value(), &err);
            else if (mode == 2) o.addOn = priceAddOn(Currency::None, 0, &err);
        } else if (o.type == Type::Ean13 || o.type == Type::UpcA || o.type == Type::Ean8) {
            o.addOn = addOn->text();
        }
        return o;
    };
    auto update = [&] {
        const Type t = Type(type->currentIndex());
        const bool book = t == Type::Isbn, ean = t == Type::Ean13 || t == Type::UpcA || t == Type::Ean8;
        static const char *const kWhat[] = {"ISBN:", "Digits:", "Digits:", "Digits:", "Text:", "Text:"};
        if (auto *lab = qobject_cast<QLabel *>(form->labelForField(data))) lab->setText(QString::fromLatin1(kWhat[int(t)]));
        data->setPlaceholderText(book ? QStringLiteral("978-1-23456-789-7 or a 10-digit ISBN") : ean ? QStringLiteral("with or without the check digit") : QString());
        price->setVisible(book);
        addOn->setVisible(ean);
        form->labelForField(addOn)->setVisible(ean);
        check39->setVisible(t == Type::Code39);
        amount->setEnabled(group->checkedId() == 1);
        currency->setEnabled(group->checkedId() == 1);
        current = data->text().trimmed().isEmpty() ? Layout() : make(options());
        if (book && group->checkedId() == 1 && current.error.isEmpty()) {
            QString err;
            priceAddOn(Currency(currency->currentIndex() + 1), amount->value(), &err);
            if (!err.isEmpty()) {
                current = Layout();
                current.error = err;
            }
        }
        problem->setText(data->text().trimmed().isEmpty() ? QString() : current.error);
        insert->setEnabled(current.error.isEmpty() && !current.bars.isEmpty());
        // The preview, scaled to fit.
        QPixmap pm(preview->size() * preview->devicePixelRatioF());
        pm.setDevicePixelRatio(preview->devicePixelRatioF());
        pm.fill(Qt::white);
        if (current.error.isEmpty() && !current.bars.isEmpty()) {
            QPainter pp(&pm);
            pp.setRenderHint(QPainter::Antialiasing);
            const QSizeF room = QSizeF(preview->size()) - QSizeF(20, 20);
            const double k = std::min(room.width() / current.size.width(), room.height() / current.size.height());
            pp.translate((preview->width() - current.size.width() * k) / 2, (preview->height() - current.size.height() * k) / 2);
            pp.scale(k, k);
            pp.fillPath(barcodePath(current), Qt::black);
        }
        preview->setPixmap(pm);
    };
    for (QLineEdit *e : {data, addOn}) QObject::connect(e, &QLineEdit::textChanged, &dlg, update);
    // A whole ISBN-13 typed or pasted without hyphens gets the standard ones.
    QObject::connect(data, &QLineEdit::textChanged, &dlg, [&] {
        if (type->currentIndex() != int(Type::Isbn)) return;
        const QString t = data->text().trimmed();
        if (t.size() != 13 || t.contains(QLatin1Char('-'))) return;
        QString err;
        const QString h = hyphenateIsbn(isbn13(t, &err));
        if (!h.isEmpty()) data->setText(h);
    });
    for (QComboBox *c : {type, currency}) QObject::connect(c, &QComboBox::currentIndexChanged, &dlg, update);
    for (QSpinBox *s : {mag, height}) QObject::connect(s, &QSpinBox::valueChanged, &dlg, update);
    QObject::connect(amount, &QDoubleSpinBox::valueChanged, &dlg, update);
    QObject::connect(group, &QButtonGroup::idClicked, &dlg, update);
    for (QCheckBox *c : {showText, check39}) QObject::connect(c, &QCheckBox::toggled, &dlg, update);
    update();
    if (dlg.exec() != QDialog::Accepted || !current.error.isEmpty() || current.bars.isEmpty()) return;

    // The settings, remembered for the next barcode and kept with this one.
    const QJsonObject settings{{"type", type->currentIndex()}, {"data", data->text()}, {"currency", currency->currentIndex()},
                               {"price", amount->value()}, {"priceMode", group->checkedId()}, {"addOn", addOn->text()},
                               {"magnification", mag->value()}, {"height", height->value()}, {"text", showText->isChecked()},
                               {"white", white->isChecked()}, {"check39", check39->isChecked()}};
    for (auto it = settings.begin(); it != settings.end(); ++it) st.setValue(QStringLiteral("barcode/") + it.key(), it.value().toVariant());

    const Type t = Type(type->currentIndex());
    const QString what = t == Type::Isbn ? isbnCaption(data->text()) : type->currentText().section(QLatin1Char(' '), 0, 0) + QLatin1Char(' ') + current.encoded;
    const QString description = QStringLiteral("Barcode: %1").arg(what);
    if (editing) {
        // In place: the same spot and identity, and one undo step.
        ItemPtr made = barcodeItem(current, editing->rect.topLeft(), white->isChecked(), description, settings);
        made->id = editing->id;
        made->name = editing->name;
        ItemList &items = ed->surfaceItems();
        for (auto &it : items)
            if (it.get() == editing) {
                ed->change(QStringLiteral("Edit Barcode"), [&] { it = made; });
                ed->select(made->id);
                return;
            }
        return;
    }
    // In the middle of the page, or a quarter inch on from a barcode
    // already there, so a second one doesn't land on the first.
    const QSizeF page = ed->doc()->pageSize();
    QPointF at((page.width() - current.size.width()) / 2, (page.height() - current.size.height()) / 2);
    for (bool moved = true; moved;) {
        moved = false;
        for (const ItemPtr &it : ed->surfaceItems())
            if (auto *g = dynamic_cast<const GroupItem *>(it.get()); g && !g->barcode.isEmpty() && (g->rect.topLeft() - at).manhattanLength() < 2) {
                at += QPointF(18, 18);
                moved = true;
            }
    }
    ed->addItem(barcodeItem(current, at, white->isChecked(), description, settings));
}

} // namespace jp
