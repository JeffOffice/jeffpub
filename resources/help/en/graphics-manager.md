# Graphics Manager
<!-- keywords: graphics manager, pictures, linked picture, link to file, update link, change link, embed picture, embedded, missing, modified, relink, find picture file, picture status, resolution -->

The **Graphics Manager** lists every picture in the publication and tells you where each picture comes from. It matters most for pictures you linked to their files, because a linked picture depends on a file outside the publication. For how to link a picture, see [Add and adjust pictures](pictures).

## Open it

Click **View > Show > Graphics Manager**. The pane opens on the right. Click the same button again, or the **X** at the top of the pane, to close it.

## Read the list

Each row shows a small picture, the file name, the page, the type of file, its size in KB, and its **status**. The box above the list sorts the rows by page, name, size, or type. Under the list, the details of the selected picture give its status, its file, whether the publication keeps the whole picture, its size in pixels, and its **effective resolution**: how many pixels of the picture fall in each inch of the page. About 300 is good for print.

| Status | What it means |
|---|---|
| Embedded | The picture is stored inside the publication. Nothing outside it is needed. |
| Linked | The picture is tied to a file, and the file is where JeffPub expects it, unchanged. |
| Missing | The picture is tied to a file that is no longer there. If the publication keeps no copy, the page shows the small preview, which looks blurry, and that is what prints. |
| Modified | The file has changed since the link was made or last updated. Click **Update Link** to bring the changes in. |

## What the buttons do

- **Go to** selects the picture on its page.
- **Save As…** saves a copy of the picture to a file.
- **Replace…** puts a different file in the same frame.
- **Update Link** reads the file again. Use it for a **Modified** picture, or after you put a **Missing** file back. Every picture that shares the link is updated, and the picture keeps its place and width.
- **Change Link…** points the picture at another file, for example when you moved the files to another folder.
- **Embed Picture** turns the link into a copy stored in the publication. The picture then shows as **Embedded**. If the file is missing, only the small preview can be embedded, and the status bar tells you so.

The three link buttons are gray unless the selected picture is linked. Each one is a single step, so **Ctrl+Z** undoes it.

## Fix a missing picture

1. Select the **Missing** picture in the list.
2. Click **Change Link…** and choose the file. Or move the file back where it was, and click **Update Link**.
3. If the file is gone for good, click **Embed Picture** to keep the preview, or **Replace…** to use another picture.

The [Design Checker](design-checker) also lists linked pictures that are missing.

## Related

- [Add and adjust pictures](pictures)
- [Check your design](design-checker)
- [Save as a picture, web page, or e-book](export)
