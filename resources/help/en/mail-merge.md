# Mail merge and catalog merge
<!-- keywords: mail merge, merge, recipients, address list, labels, form letters, csv, xlsx, vcard, merge field, address block, catalog, product list, email merge, preview -->

Mail merge fills one publication with details from a list. You write a letter once, and JeffPub makes a copy for each person, with that person's name and address in place. A catalog merge repeats one layout for each item in a list.

The tools are on the **Mailings** tab. For a guided path, click **Mailings > Start > Mail Merge > Step-by-Step Mail Merge Wizard** to open the **Mail Merge** pane.

## Choose your recipients

Click **Mailings > Start > Select Recipients**:

- **Use an Existing List…**: pick a CSV file, a tab-separated text file, an .xlsx spreadsheet (the first sheet), or a .vcf contacts file. In a CSV, text, or spreadsheet file, the first row must hold the column names.
- **Select from Contacts…**: pick a contacts file (.vcf or .vcard). Most address books and phones can save their contacts as one; each contact becomes a recipient, with name, address, phone, and email columns.
- **Type a New List…**: fill in the **New Address List** window, using **New Entry** for each person. JeffPub then offers to save the list as a CSV file.

To check the list, click **Mailings > Start > Edit Recipient List**. In the **Mail Merge Recipients** window, uncheck a row to skip that person, click a heading to sort, and type in **Filter recipients** to narrow the list. **Find Duplicates** unchecks repeats, and **Validate Addresses** highlights rows with a missing address or an odd ZIP code.

## Add merge fields

A merge field is a placeholder, such as «First Name», that becomes a real value for each person. Click in a text box (JeffPub adds one if you do not), then use **Mailings > Write & Insert Fields**:

- **Address Block**: a mailing address built from columns named like First Name, Last Name, and Address Line 1.
- **Greeting Line**: "Dear" and the person's name, or "Dear Friend," if there is no name.
- **Insert Merge Field**: pick any column.
- **Picture Field**: pick a column of picture file names. JeffPub adds an empty frame and looks for the files next to your list. Each record's picture shows when you preview the results, print, save a PDF, or merge to a new publication or a catalog.

## Preview the results

Click **Mailings > Preview Results > Preview Results** to see real details in place of the fields. Use **First Record**, **Previous Record**, **Next Record**, and **Last Record** to flip through the people, or **Find Recipient** to jump to a match.

## Finish the merge

Click **Mailings > Finish > Finish & Merge**:

- **Merge to Printer…** opens the **Print** page. Check **Print all mail merge records**, then print.
- **Merge to New Publication** makes a new publication with a copy of your pages for each person.
- **Merge to PDF…** saves one PDF with every person's pages.
- **Merge to Email…** makes one .eml email file per person, with the first page as a picture. Choose the email column, type a subject, and pick a folder. JeffPub does not send them. Open the files in your email program.
- **Export Recipient List…** saves the list as a CSV file.

## Catalog merge

Click **Insert > Pages > Catalog Pages** to open the **Catalog Merge** pane.

1. Choose the product list, as above.
2. Click **Insert Catalog Area**. A box tagged "Catalog area" appears. Set its **Rows** and **Columns**, and its position and size. Put the fields for one item in the first cell by double-clicking them in the list. They repeat in the other cells. Pick the "(picture)" version of a field for a picture.
3. Preview with the arrows. Each page shows as many items as the area has cells. Then merge to a new publication, a PDF, or the printer.

**Remove Catalog Area** takes the area away.

## Related

- [Print a publication](printing)
- [Add and format text boxes](text-boxes)
- [The Mailings tab](tab-mailings)
