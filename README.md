# GraphXPort

GraphXPort is a standalone Unreal Editor plugin for exporting Blueprint graph flows without MCP, an AI service, or an external process.

## Current workflow

1. Open a Blueprint graph.
2. Select one or more nodes.
3. Right-click a selected node and open **GraphXPort**.
4. Copy the selection as AI text, compact ASCII, or Mermaid, or export the entire graph as a `.graphx.json` snapshot.

The submenu can also copy or export the entire current function, event, macro, or graph.

Selection exports preserve internal connections and record connections crossing the selection boundary as external links.

ASCII flow output is a compact execution tree. It uses collision-safe short node IDs, preserves the Blueprint node's pin order for Branch, loop, Sequence, and Switch paths, folds reroute nodes, distinguishes connected data (`←`) from literal defaults (`=`), and marks converging or cyclic execution with `↩` references instead of repeating nodes indefinitely.

Canonical format version 1 includes `nodePinOrder` on pins so renderers and future structural comparisons can preserve semantic execution-output order without depending on selection or UObject iteration order.

## Scope

The first release is read-only. Snapshot comparison, structural conflict reporting, and transactional import are planned on top of the canonical JSON model.
