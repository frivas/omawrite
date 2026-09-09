# Open and OS-opened files

Type: grilling
Status: resolved

## Question

When the user opens a Markdown path with Cmd/Ctrl+O, or the OS asks Omawrite to open a file (Finder, portal, `OmawriteApplication` file-open), where does that document appear: a new tab in the current window, a new window, or replacement of the active document?

## Answer

A new tab in the **current** window. Cmd/Ctrl+O and OS-open add a tab there; they do not replace the active document and they do not spawn a window. If the app is launched by opening a file, that file is the first tab of the new window. A new window stays the explicit new-window action. Whether that path is already open is [Same path already open](04-same-path-already-open.md).
