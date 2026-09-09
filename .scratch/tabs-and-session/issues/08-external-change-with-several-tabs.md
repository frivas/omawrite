# External change with several tabs

Type: grilling
Status: resolved

## Question

When a named file changes on disk, does the existing external-change prompt attach to the tab that owns that path (even if it is not active), or to the window as a whole?

## Answer

The prompt belongs to **the tab that owns that path**, not the window. Other tabs stay editable. If that tab is not active, switch to it when the dialog opens so keep-vs-reload is about the buffer you can see. If several watched files change, queue one dialog per tab. Unanswered external change still blocks autosave for **that** tab only (`canAutosaveToFile` guardrail stays per document).

## Comments

Implemented in [`ca901ef`](https://github.com/frivas/omawrite/commit/ca901efaf40e088877a84200316dc8d1f710026c), PR [omacom/omawrite#59](https://github.com/omacom/omawrite/pull/59).

The file watcher watches every named tab path. On a change it `setActiveTab`s to the owner, then emits `externalChangeDetected` as before. Autosave still uses per-document `canAutosaveToFile` (unanswered change and never-read). Existing external-change tests still cover the dialog once that tab is active.
