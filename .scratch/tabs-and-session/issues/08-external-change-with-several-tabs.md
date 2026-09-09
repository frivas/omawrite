# External change with several tabs

Type: grilling
Status: resolved

## Question

When a named file changes on disk, does the existing external-change prompt attach to the tab that owns that path (even if it is not active), or to the window as a whole?

## Answer

The prompt belongs to **the tab that owns that path**, not the window. Other tabs stay editable. If that tab is not active, switch to it when the dialog opens so keep-vs-reload is about the buffer you can see. If several watched files change, queue one dialog per tab. Unanswered external change still blocks autosave for **that** tab only (`canAutosaveToFile` guardrail stays per document).
