#pragma once
// Built-in publication templates and building blocks.

#include "core/document.h"

#include <QJsonObject>
#include <functional>

namespace jp {

struct TemplateOptions {
    QString colorScheme;
    QString fontScheme;
    BusinessInfo business;
    QJsonObject options;
};

struct TemplateInfo {
    QString id;
    QString category;
    QString name;
    QString description;
    std::function<std::unique_ptr<Document>(const TemplateOptions &)> build;
    QStringList optionKeys;   // template-specific options shown in the New page
};

const QVector<TemplateInfo> &templates();
QStringList templateCategories();
const TemplateInfo *findTemplate(const QString &id);

struct BlankSize {
    QString name, group;
    QSizeF size;   // points
};
const QVector<BlankSize> &blankSizes();

// Building blocks: return items to insert on the current page (page coordinates).
struct BuildingBlock {
    QString id, category, name;
    std::function<ItemList(Document &, const QRectF &pageContent)> build;
};
const QVector<BuildingBlock> &buildingBlocks();
const BuildingBlock *findBlock(const QString &id);
ItemList makeCalendar(Document &doc, const QRectF &area, int year, int month, int style);

} // namespace jp
