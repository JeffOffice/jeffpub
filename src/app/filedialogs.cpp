// One save dialog for every Save As / Export command.
//
// On Windows the system Save dialog is called directly. Qt's own wrapper
// leaves on the dialog's "test file" check: before accepting a new name the
// dialog creates and deletes an empty file there, and when Windows Security's
// Controlled folder access or a sync tool blocks that, the dialog refuses
// every new name with "File not found". Here the check is off; if Windows
// blocks the real save, the save reports it with a way out.

#include "app/appfuncs.h"
#include "app/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

namespace jp {

#ifdef Q_OS_WIN
// Returns false only when the system dialog couldn't be created at all.
static bool windowsSaveDialog(QWidget *parent, const QString &caption, const QString &dir, const QString &name, const QString &filter,
                              const QString &suffix, QString *selectedFilter, QString *out)
{
    IFileSaveDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, reinterpret_cast<void **>(&dlg))) || !dlg)
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOTESTFILECREATE;
    opts &= ~DWORD(FOS_FILEMUSTEXIST | FOS_NOREADONLYRETURN);
    dlg->SetOptions(opts);
    dlg->SetTitle(reinterpret_cast<LPCWSTR>(caption.utf16()));
    // "Name (*.a *.b);;Other (*.c)" -> {Name, *.a;*.b}, {Other, *.c}
    const QStringList filters = filter.split(QStringLiteral(";;"), Qt::SkipEmptyParts);
    QVector<std::wstring> names, specs;
    for (const QString &f : filters) {
        const auto m = QRegularExpression(QStringLiteral("^(.*?)\\s*\\(([^)]*)\\)\\s*$")).match(f);
        names << f.toStdWString();
        specs << (m.hasMatch() ? m.captured(2).split(QLatin1Char(' '), Qt::SkipEmptyParts).join(QLatin1Char(';')) : f).toStdWString();
    }
    QVector<COMDLG_FILTERSPEC> fs;
    for (int i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
    if (!fs.isEmpty()) {
        dlg->SetFileTypes(UINT(fs.size()), fs.constData());
        int idx = selectedFilter ? filters.indexOf(*selectedFilter) : -1;
        dlg->SetFileTypeIndex(UINT(std::max(0, idx) + 1));
    }
    if (!suffix.isEmpty()) dlg->SetDefaultExtension(reinterpret_cast<LPCWSTR>(suffix.utf16()));
    if (!name.isEmpty()) dlg->SetFileName(reinterpret_cast<LPCWSTR>(name.utf16()));
    IShellItem *folder = nullptr;
    const QString nativeDir = QDir::toNativeSeparators(dir);
    if (SUCCEEDED(SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(nativeDir.utf16()), nullptr, IID_IShellItem, reinterpret_cast<void **>(&folder))) && folder) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    HWND owner = parent ? reinterpret_cast<HWND>(parent->window()->winId()) : nullptr;
    const HRESULT hr = dlg->Show(owner);
    if (SUCCEEDED(hr)) {
        UINT ti = 0;
        if (selectedFilter && SUCCEEDED(dlg->GetFileTypeIndex(&ti)) && ti >= 1 && int(ti) <= filters.size()) *selectedFilter = filters[int(ti) - 1];
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                *out = QDir::fromNativeSeparators(QString::fromWCharArray(p));
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return true;
}
#endif

QString askSavePath(QWidget *parent, const QString &caption, const QString &suggested, const QString &filter, QString *selectedFilter)
{
    auto exists = [](const QString &d) { return !d.isEmpty() && QDir(d).exists(); };
    const QFileInfo fi(suggested);
    QString dir = suggested.contains('/') || suggested.contains('\\') ? fi.absolutePath() : QString();
    const QString name = fi.fileName();
    if (!exists(dir)) {
        const QString last = Settings::get().value(QStringLiteral("ui/lastSaveDir")).toString();
        dir = exists(last) ? last : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (!exists(dir)) dir = QDir::homePath();
    }
    QString suffix = fi.suffix();
    if (suffix.isEmpty()) {
        const auto m = QRegularExpression(QStringLiteral("\\*\\.([A-Za-z0-9]+)")).match(filter);
        if (m.hasMatch()) suffix = m.captured(1);
    }
#ifdef Q_OS_WIN
    {
        QString out;
        if (windowsSaveDialog(parent, caption, dir, name, filter, suffix, selectedFilter, &out)) {
            if (!out.isEmpty()) Settings::get().setValue(QStringLiteral("ui/lastSaveDir"), QFileInfo(out).absolutePath());
            return out;
        }
    }
#endif
    QFileDialog dlg(parent, caption, dir, filter);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    if (selectedFilter && !selectedFilter->isEmpty()) dlg.selectNameFilter(*selectedFilter);
    if (!name.isEmpty()) dlg.selectFile(name);
    if (!suffix.isEmpty()) dlg.setDefaultSuffix(suffix);
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty()) return {};
    if (selectedFilter) *selectedFilter = dlg.selectedNameFilter();
    const QString out = dlg.selectedFiles().first();
    Settings::get().setValue(QStringLiteral("ui/lastSaveDir"), QFileInfo(out).absolutePath());
    return out;
}

} // namespace jp

namespace jp {

QString saveFailureHint(const QString &error)
{
#ifdef Q_OS_WIN
    // Windows Security's Controlled folder access blocks programs it doesn't
    // know from Documents, Pictures, Desktop and similar folders. The blocked
    // program usually sees "file not found" rather than "access denied".
    const bool blocked = error.contains(QLatin1String("cannot find"), Qt::CaseInsensitive) || error.contains(QLatin1String("not found"), Qt::CaseInsensitive)
                         || error.contains(QLatin1String("denied"), Qt::CaseInsensitive) || error.contains(QLatin1String("permission"), Qt::CaseInsensitive);
    if (blocked)
        return QStringLiteral("\n\nWindows is probably blocking JeffPub 79 from saving in this folder. Windows Security's "
                              "\"Controlled folder access\" protects folders such as Documents and Desktop from programs it doesn't recognize yet.\n\n"
                              "To allow JeffPub 79: open Windows Security, choose Virus & threat protection, then Manage ransomware protection, "
                              "then Allow an app through Controlled folder access. Choose Add an allowed app, then Browse all apps, and select:\n%1\n\n"
                              "Or save to a folder that isn't protected.")
            .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
#endif
    Q_UNUSED(error);
    return QString();
}

} // namespace jp
