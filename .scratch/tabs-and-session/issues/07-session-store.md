# Session store

Type: grilling
Status: resolved

## Question

What on-disk shape replaces today’s `recovery-{0..99}.json` slots for a full session — windows, tab order, active tab, unsaved bodies vs paths of clean named documents — so hot exit and crash restore the same way, without writing untitled work onto a user-chosen path?

## Answer

One **`session.json`** under `QStandardPaths::AppDataLocation` (same directory as today’s recovery slots). Crash and hot exit write that same file; there are no per-window recovery slots.

The file is a JSON object with `windows[]`. Each window has geometry (replacing the single `window/*` QSettings blob), `activeTab` index, and `tabs[]` in strip order. A tab stores `fileUrl` (empty if untitled), `pathNeverRead` when that guardrail applies, and **`text` only when there is unsaved work** (untitled, or named and dirty). Clean named tabs are paths only — bodies reload from disk. Untitled work never becomes a user-chosen file via this store.

Write atomically (`QSaveFile` / equivalent), on the existing debounce and on quit. Autosave-off still writes the session (today autosave-off still snapshots). One application lock (`session.lock`) so two processes cannot clobber it.

First launch: if `session.json` is missing, import leftover `recovery-*.json` as tabs in one window, then stop claiming slots.
