#pragma once
// Page Navigation pane: page thumbnails (paired as spreads), drag to reorder,
// right-click for page commands.

#include <QListWidget>
#include <QTimer>

namespace jp {

class Editor;
class MainWindow;

class PagesPane : public QListWidget {
    Q_OBJECT
public:
    PagesPane(Editor *ed, MainWindow *win);
    void refresh(bool thumbnailsOnly = false);

protected:
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    void renderNext();
    Editor *m_ed;
    MainWindow *m_win;
    QTimer m_timer;
    QVector<int> m_pending;
    bool m_updating = false;
};

} // namespace jp
