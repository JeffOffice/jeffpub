# Rulers, guides, and snapping
<!-- keywords: ruler, zero point, ruler origin, guide, ruler guide, margin guide, grid, columns, baseline, snap, align to guides, align to objects, measurement units, inches, centimeters, show guides, boundaries -->

Guides are lines that help you line things up. They show on screen but never print. Snapping makes objects jump to a guide or to another object when they get close.

## Rulers and units

Turn rulers on or off with **View > Show > Rulers**. A red line on each ruler follows your pointer, and the part of the ruler over your selected object is shaded. While you type in a text box, the top ruler shows indent and tab markers.

To change the units, click **File > Options**, open the **Advanced** tab, and choose **Measurement units**: inches, centimeters, millimeters, points, or picas. In any size or position box, you can also type a unit, such as 2 cm.

## Move the zero point

Each ruler counts from the top-left corner of the page: 0 is the corner, and the numbers grow as you move away from it. To count from somewhere else, such as the corner of an object you want to measure, move the zero point:

- Hold **Shift** and drag with the **right** mouse button along a ruler. That ruler's zero moves to where you let go.
- To move both rulers at once, drag from the small box where the two rulers meet.
- To put both back at the page's corner, double-click that small box.

Only the numbers and tick marks on the two page rulers change. The status bar, the position boxes (such as the ones in the **Measurement** window), and the **Ruler Guides** window still measure from the page's corner, and so do the guides you drag out. The indent and tab markers on the text ruler still measure from the text box. The zero point belongs to the window, not to the file: it is not saved, and every publication you open starts at the corner.

## The four kinds of guides

- **Margin guides** (pink) mark the margins. Set them in **Page Design > Page Setup > Margins**.
- **Grid guides** (blue) split the page into columns and rows. They belong to a master page.
- **Baseline guides** (tan dotted lines) mark lines for text to sit on.
- **Ruler guides** (green) are the ones you place yourself.

Show or hide them all with **View > Show > Guides**. **View > Show > Baselines** (Ctrl+F7) shows or hides the baseline guides.

## Set columns and rows

Click **Page Design > Page Setup > Guides**. Under **Built-In Guides**, choose No Grid, 2 Columns, 3 Columns, 4 Columns, or a 2 × 2, 3 × 3, or 4 × 4 Grid. For more control, choose **Grid and Baseline Guides…**. The **Layout Guides** window then has these tabs:

- **Margin Guides**: the four margins.
- **Grid Guides**: columns, rows, and the space between them. You can also add a center guide.
- **Baseline Guides**: the spacing between lines, and where the first one starts.

For text to sit on the baseline guides, open the **Paragraph** window and check **Align text to baseline guides**.

## Add ruler guides

- Drag from the top ruler to make a horizontal guide. Drag from the left ruler to make a vertical one.
- Or choose **Add Horizontal Ruler Guide** or **Add Vertical Ruler Guide** in the **Guides** menu. Each new guide lands beside the last.
- Drag a guide to move it. Drag it back onto its ruler to delete it.
- **Ruler Guides…** lets you type exact positions. It also adds a series of guides at even spacing.
- **Clear All Ruler Guides** removes the guides from the page you are on.

Guides on a master page show on every page that uses it. To move them, go to the master page.

## Turn snapping on or off

Use **Page Design > Layout**:

- **Align to Guides**: objects snap to margin guides, grid guides, ruler guides, the page edges, and the page center.
- **Align to Objects**: objects snap to the edges and centers of other objects.

Both are on at first. Snapping works when you are within a few pixels. Hold **Alt** while you drag to switch it off for that move.

## Related

- [Move, align, group, and layer objects](arrange)
- [Master pages, headers, and page numbers](master-pages)
- [Page size, margins, and layout](page-setup)
- [The Page Design tab](tab-page-design)
