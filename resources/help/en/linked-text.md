# Flow text between text boxes
<!-- keywords: link text boxes, linked boxes, create link, break link, text flow, continue story, overflow, pitcher, continued on page, continued from page, connect text boxes, chain, next box, previous box, autoflow -->

When a story is too long for one text box, you can link the box to a second one. The rest of the text then flows into the second box. Linked boxes form a chain, and all the text in a chain is one story. Edit in any box, and the words flow through the whole chain again.

## Link two boxes

The second box must be empty and not already linked. Draw it first with **Home > Objects > Draw Text Box**.

1. Click the first box to select it. It must be the last box in its chain.
2. Click **Text Box > Linking > Create Link**. The pointer turns into a pitcher.
3. Click the empty box. The extra text pours into it.

You can also click the red **A…** overflow indicator at the lower-right corner of a full box. That starts the same pitcher.

If you click an empty spot on the page instead, JeffPub draws a new box there, the same size as the first, and links it.

The empty box can be on another page. While the pitcher shows, click that page in the page list, and then click the box.

If the link fails, the status bar says why. Either the box you clicked has text in it, or it is already linked.

## How the text flows

The text fills the first box, then the next box in the order you linked them. The order does not depend on where the boxes sit on the page. If a box has columns, the columns fill first. If the last box is still too small, the red **A…** appears and the leftover text stays hidden.

## Move between linked boxes

Select a linked box. A small arrow box at its lower-right corner jumps to the next box. One at its upper-left corner jumps to the previous box. You can also use **Text Box > Linking > Next** and **Text Box > Linking > Previous**. JeffPub changes pages for you.

## Break a link

1. Select the box just before the break.
2. Click **Text Box > Linking > Break**.

All the text stays with the first part of the chain. The boxes after the break become empty, and you can link them again.

## Show "Continued on" and "Continued from" notices

A notice tells readers where a story goes next.

1. Right-click a linked box and choose **Format Object…**.
2. Open the **Text Box** tab.
3. Check **Include "Continued on page…"** to add a notice at the bottom of the box. Check **Include "Continued from page…"** to add one at the top.

The notice reads "(Continued on page 4)" or "(Continued from page 2)" in small italic type. JeffPub fills in the page number. The first shows only when a box follows, and the second only when a box comes before.

## Let JeffPub add pages

If you use **Insert > Text > Insert File** and the text is too long, JeffPub asks if you want to continue it on new pages. Choose **Yes**. It adds pages with a linked box on each, in the same place and size as the last box.

## Related

- [Add and format text boxes](text-boxes)
- [The Text Box tab](tab-text-box)
- [Add, move, delete, and name pages](pages)
