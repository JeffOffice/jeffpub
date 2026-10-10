# Add and adjust pictures
<!-- keywords: photo, image, insert picture, crop, clip art, PDF page, jpg, png, svg, recolor, alt text, caption, placeholder, graphics manager, online pictures, link to file, linked picture, insert and link, embed, missing picture, relink -->

A picture sits inside a frame. You can crop the picture, and move or resize the frame.

## Insert a picture from a file

1. Click **Insert > Illustrations > Pictures**.
2. In the **Insert Picture** window, choose a file. JeffPub opens PNG, JPEG, GIF, BMP, TIFF, WebP, SVG, WMF, EMF, ICO, and PDF files.
3. Pick **Insert**, **Link to File**, or **Insert and Link** in the **Insert as** box, which sits with the file list and can be reached with the Tab key. **Insert** is chosen each time the window opens. See "Link a picture to its file," below.
4. Click **Open**.

The picture lands in the middle of the page. You can also drag a picture file onto the page, which always uses **Insert**.

If you choose several pictures at once, they line up as thumbnails on the scratch area, the gray space beside the page. Drag the ones you want onto the page.

If a PDF has more than one page, the **Insert PDF Page** window shows each page. Pick one and click **OK**.

## Link a picture to its file

**Insert** puts a copy of the picture inside the publication. That copy goes wherever the publication goes, but a publication with many large photos gets large too. A link keeps the photos in their own files instead.

- **Insert** copies the picture into the publication. This is the usual choice.
- **Link to File** keeps only the file's location and a small preview, no more than 512 pixels on its longest side. The picture on the page, and in a print, a PDF, or an e-book, comes from the file at full quality. If the file is moved or deleted, the page shows the small preview instead.
- **Insert and Link** does both. The publication keeps the whole picture and the link. When you open the publication and the file has changed, JeffPub reads it again and replaces its copy.

JeffPub remembers the file's full location, and also where it is compared with the publication, such as "the Photos folder beside it". If you move the publication and its pictures together, the second one wins, so the pictures are found again. Keep the folders as they were.

The **Graphics Manager** shows whether each picture is **Embedded**, **Linked**, **Missing**, or **Modified**, and has buttons to update a link, point it at another file, or turn it into a copy. See [Graphics Manager](graphics-manager).

When you save a publication as a `.pub` file, make a Pack and Go file, or send it by e-mail, JeffPub puts each linked picture into the file in full, so it does not depend on files that stay behind. If a linked file is missing then, its small preview goes in instead. A `.pub` file you open never has links: its pictures come in as copies.

## Add a picture placeholder

A placeholder is an empty frame that saves a spot for a picture. Click **Insert > Illustrations > Picture Placeholder**, then double-click the frame and choose a file. The picture fills the frame, and the edges that stick out are hidden. An empty placeholder does not print.

## Find free pictures online

Click **Insert > Illustrations > Online Pictures**. The **Online Pictures** pane searches two free libraries of openly licensed pictures: Openverse, which covers many collections, and Wikimedia Commons, the library of Wikipedia's pictures.

1. Choose a library, type what you're looking for, such as "red barn", and press Enter.
2. Click a picture to see who made it and its license. A license is the set of rules for using the picture. Most ask you to name the author.
3. Click **Insert**. JeffPub puts the picture on the page and uses its title as its alt text. With **Add a credit under the picture** checked, a line such as "Red barn" by Pat Lee, CC BY 2.0 goes under it.

Your search words go to the library you chose. Check that each picture's license fits how you'll use it.

## Crop a picture

Cropping hides the edges of a picture.

1. Click the picture, then click **Picture Format > Crop > Crop**.
2. Drag a handle to hide part of the picture. Drag the picture to slide it inside the frame.
3. Click **Crop** again, or press **Esc**.

Also in the **Crop** group: **Fit** shows the whole picture in its frame, **Fill** fills the frame, **Crop to Shape** trims to a shape such as an oval, and **Clear Crop** shows the whole picture again.

## Change how a picture looks

On the **Picture Format** tab, use the **Adjust** group:

- **Corrections**: pick a brightness and contrast pair, from minus 40 to plus 40 percent. **Format Object…**, at the bottom of the menu, has sliders.
- **Recolor**: make the picture Grayscale, Sepia, Washout, Black and White, or an accent tint. **Set Transparent Color** makes the color in the picture's top-left corner see-through.
- **Transparency…**: fade the whole picture.
- **Reset Picture**: undo these changes.
- **Compress Pictures…**: shrink the file by lowering detail to 300, 220, or 96 ppi (pixels per inch). By default it changes every picture and deletes cropped areas.
- **Convert to Shapes**: turns an SVG picture (a drawing made of lines and shapes) into shapes you can edit, cut where the drawing clips them. Text and pictures inside it are left out.

## Caption, alt text, and swapping

- **Picture Format > Picture Styles > Caption** adds a text box under the picture.
- Alt text is a short description for people who cannot see the picture, such as screen reader users. Right-click the picture, choose **Format Object…**, open the **Alt Text** tab, and type in **Alternative text**.
- **Picture Format > Insert > Change Picture** puts another file in the same frame.
- To trade two pictures, drag one onto the other. When the tip says **Release to swap the pictures**, let go: the pictures change frames, and the frame you dragged goes back where it was. Or hold Shift, click both, and click **Swap**.

## See every picture

Click **View > Show > Graphics Manager** to list every picture in the **Graphics Manager** pane. Select one, then click **Go to** to jump to it, **Save As…** to save a copy, or **Replace…** to choose another file. For linked pictures it also has **Update Link**, **Change Link…**, and **Embed Picture**. See [Graphics Manager](graphics-manager).

## Related

- [Graphics Manager](graphics-manager)
- [Draw shapes and lines](shapes)
- [Move, align, group, and layer objects](arrange)
- [The Picture Format tab](tab-picture-format)
