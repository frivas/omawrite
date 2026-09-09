# Today’s document, window, save, and recovery model

Type: research
Status: resolved

## Question

What is today’s model for a window, a single document, Save, autosave, crash snapshot / `restoreRecovery`, and `newWindow` — citing `src/` and tests — and which of those facts a tabs + session design must treat as constraints or replace?

## Answer

One process is one window, one `Backend`, and one `QTextDocument`. Save and named-file autosave write that document; untitled and guarded paths get a per-window crash JSON restored in `attachDocument`. `newWindow` and Finder open spawn another process. Tabs must keep the autosave/never-read/failed-close guardrails and replace the 1:1:1 identity, crash slots, and process-spawn New Window with a session.

Findings: [`../research/today-document-window-save-recovery.md`](../research/today-document-window-save-recovery.md) (committed on `research/today-document-window-save-recovery`).
