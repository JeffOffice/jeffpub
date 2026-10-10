# Get back unsaved work
<!-- keywords: recover, recovery, autorecover, auto recover, autosave, crash, lost work, unsaved, power failure, restore, frozen, closed by accident, backup -->

If JeffPub closes by surprise, or the power goes out, your latest work may still be safe. While you work, JeffPub keeps a spare copy of any publication that has changes you have not saved. This is called **AutoRecover**.

## How AutoRecover works

- Every few minutes, JeffPub saves a spare copy of each publication that has unsaved changes. The default is every 10 minutes.
- JeffPub keeps the copies in an AutoRecover folder, inside the folder where it keeps its own data. You never have to look there yourself.
- The spare copy never replaces your real file.
- The copy goes away when you save the publication, close it, or quit JeffPub in the normal way.

AutoRecover does not replace saving. If JeffPub stops, you can lose the work from the last few minutes. Press Ctrl+S often.

## Recover your work

The next time you start JeffPub after a crash, the **Recover Unsaved Work** window opens by itself. Each line shows the publication's name, where the file was kept (or "never saved"), and when the copy was made.

1. Leave a check mark next to each publication you want back. Clear the mark on the others.
2. Click **Open**.
3. Check the work, then press Ctrl+S.

The recovered publication shows "(Recovered)" in the title bar until you save it. Ctrl+S asks for a name, and starts in the folder of the original file, with the original name. Pick the original name to replace the file, or choose a new name to keep both.

The other buttons in the window are:

- **Delete** removes the checked copies for good, after it asks you to confirm.
- **Not Now** closes the window and leaves the copies. JeffPub offers them again the next time it starts.

JeffPub only offers copies from a run that ended by surprise. It does not offer copies from a JeffPub window that is still open.

## Find recovered work later

If you closed the window and want your copies back, click **File > Open**, then **Recover Unsaved Work**. If there is nothing to recover, JeffPub says so.

## Change how often it saves

Click **File > Settings**, then the **Save** tab. Change **Save AutoRecover information every** to a number from 1 to 120 minutes. A smaller number means less to lose. The new time takes full effect the next time you start JeffPub.

## Related

- [Open and save publications](open-save)
- [Change JeffPub's settings](options)
- [Get help and report problems](support)
