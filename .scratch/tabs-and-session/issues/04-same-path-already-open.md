# Same path already open

Type: grilling
Status: resolved
Blocked by: 02

## Question

If a path is already open as a tab (this window or another), does Open / OS-open focus that tab, or may the same file appear in two tabs?

## Answer

**Focus the existing tab.** Raise its window if it is in another window. Do not open a second tab on the same path, so two buffers cannot diverge. Untitled documents have no path, so they never match this rule.
