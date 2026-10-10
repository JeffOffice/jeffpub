# Check your design
<!-- keywords: design checker, check, errors, problems, low resolution, overflow, text does not fit, empty text box, off the page, preflight, final check, proof, linked picture, missing font -->

The **Design Checker** looks through your publication for things that often go wrong. It finds text that does not fit, empty boxes, objects hanging off the page, and pictures that will print badly. Run it before you print or send a file to a print shop.

## Run the Design Checker

Click **Review > Check > Run Design Checker**. The **Design Checker** pane opens on the right. You can also click **File > Properties**, then **Run Design Checker**.

The pane has two check boxes, both on at first:

- **Run general design checks** looks for layout problems.
- **Run final publishing checks** looks for things that matter when you print.

Leave both on to see every kind of problem. The list updates as you work. If there is nothing to fix, it says "No problems found."

## Fix a problem

1. Click an item in the list under **Select an item to fix**.
2. Click **Go to Item**. Or double-click the item.

JeffPub goes to the page and selects the object. Fix it, and the item drops off the list.

## What it finds

| Message | What it means | What to do |
|---|---|---|
| Page has no content | The page is empty. | Add something, or delete the page. |
| Object is partially off the page | Part of an object is outside the page. | Move it, or leave it if it is a bleed. |
| Text box is empty | A text box holds no text. | Type in it, or delete it. |
| Story with text in overflow area | A story is the text that runs through linked boxes. This one has more text than fits. | Enlarge a box, cut text, or [link another box](linked-text). |
| Text is shrunk a great deal to fit | Autofit, which shrinks text to fit its box, made the text less than half its size. | Enlarge the box or use less text. |
| Picture placeholder is empty | A picture frame has no picture. | Click it to add one, or delete it. |
| Picture is not scaled proportionally | The picture is stretched or squeezed. | Resize it to restore its shape. |
| Picture is linked, not embedded | The picture is tied to a file and only a small preview is stored in the publication. A picture inserted with **Insert and Link** is stored whole and is not listed. | Keep the original file with your publication, or click **Embed Picture** in the [Graphics Manager](graphics-manager). |
| Linked picture is missing | The file a linked picture points to is gone, so the page shows only its small preview, and that is what would print. | In the Graphics Manager, click **Change Link…** to find the file, or put it back and click **Update Link**. |
| Linked picture shows only its preview until Update Link | The picture is linked to a file outside the publication's folder, which JeffPub does not open on its own. The page shows the small preview, and that is what would print. | In the Graphics Manager, select the picture and click **Update Link**. |
| Picture has low resolution | The picture has under 150 ppi (pixels per inch) at its printed size. | Use a larger picture, or print it smaller. |
| Object has transparency or effects (may print differently) | See-through areas and effects can look different on paper. | Print a test page. |
| Font "name" is not installed | A font in your text is not on this computer, so another stands in. | Install the font, or pick another. |

Each message ends with the page number, such as "(Page 2)".

## Before a print shop file

When you make a PDF/X file, JeffPub warns you about low-resolution pictures and missing fonts, and offers to open the Design Checker. See [PDF files and print shops](pdf).

## Related

- [PDF files and print shops](pdf)
- [Print a publication](printing)
- [Flow text between text boxes](linked-text)
- [Add and adjust pictures](pictures)
- [Graphics Manager](graphics-manager)
