# GraphXPort

GraphXPort is a standalone Unreal Editor plugin for exporting Blueprint graph flows without MCP, an AI service, or an external process.

## Current workflow

1. Open a Blueprint graph.
2. Select one or more nodes.
3. Right-click a selected node and open **GraphXPort**.
4. Copy the selection as AI text, compact ASCII, or Mermaid, export the entire graph as a `.graphx.json` snapshot, or beautify the selected nodes' layout.

The submenu can also copy or export the entire current function, event, macro, or graph.

**Beautify Selection** rearranges nodes left-to-right by execution flow, from the same context menu entry for two workflows:

- **Select several nodes** and beautify just that selection. Nothing outside it is touched, and the layout never follows a connection past the last selected node.
- **Select a single node** and beautify its entire downstream chain: GraphXPort expands the selection to every node reachable forward through that node's own execution paths (every branch, not just one), plus each visited node's own pure/data inputs - without crossing into an unrelated part of the graph a data output happens to also feed.

The node you right-click anchors the whole layout: it keeps its own current position exactly, and everything else is arranged relative to it - later execution steps to its right, earlier ones (and its own pure/data inputs) to its left - rather than the layout snapping to an arbitrary bounding box.

Nodes with an exec pin form the main spine, laid out in topological columns/rows, with straight 1-out/1-in execution links sharing a row so the connecting wire renders horizontal instead of arcing. Each pure/data-only node (variable gets, math, etc.) is placed in its own small column immediately to the left of - and top-aligned with - the specific spine node that actually consumes it, rather than sharing one distant column with every other pure node in the selection; a pure node with no in-selection consumer stacks in a dedicated column to the left of everything else. Column widths, row heights, and every node's footprint are estimated from its own pin count and label length rather than a fixed grid cell, and a final collision pass slides any node straight down if it would still overlap something already placed - both together prevent the overlap and cross-graph wires a fixed-size grid produced for real, larger nodes.

Beautify is the plugin's one exception to "GraphXPort never modifies your Blueprint": it only changes node position (`NodePosX`/`NodePosY`), never pins, connections, or logic, and it is a normal undoable editor transaction like any other graph edit (Ctrl+Z reverts it). It requires at least two nodes to arrange (after single-node expansion, if that applies) and does nothing to nodes outside that set.

Selection exports preserve internal connections and record connections crossing the selection boundary as external links.

ASCII flow output is a compact execution tree. It uses collision-safe short node IDs, preserves the Blueprint node's pin order for Branch, loop, Sequence, and Switch paths, folds reroute nodes, distinguishes connected data (`←`) from literal defaults (`=`), and marks converging or cyclic execution with `↩` references instead of repeating nodes indefinitely.

The tree covers multiple events, disconnected execution sections, rootless cycles,
and data-only selections. Control-flow labels are node-aware: a Branch's `then`
means `TRUE`, but an ordinary `then` or a Switch case named `Then` does not.
Unconnected control outputs are shown explicitly. Loop macro instances show their
interface paths (`LOOP BODY`, `COMPLETED`), not a simulation of loop iterations or
an expansion of the macro's internal graph.

Pure calls use named arguments in node pin order and identify the selected result,
for example `[12345678] Compute(First=1, Second=2).Result`. Impure call outputs are
references such as `[abcdef12].Result`, not additional invocations. External IDs
participate in short-ID collision detection, including links through reroutes.

## Verification

The `GraphXPort.` automation group contains the selection/whole-graph checks,
four focused ASCII tests (`Golden`, `CoverageAndReferences`,
`ReroutesAndBoundaries`, and `EngineControlNodes`), and two Beautify layout
tests (`LinearChainAndBranch`, `CyclesAndIslands`). The engine-node fixture
uses real Branch, Sequence, Switch and macro-instance classes with transient
loop interface definitions; real project assets still need a manual export
check.

For DSM, build `AR_GIS_MAPEditor Win64 Development`. If Unreal Editor must not be
run or hot-reloaded, use compile-only flags `-NoLink -NoHotReload
-NoHotReloadFromIDE`. This does not build a new loadable DLL or execute automation;
a normal build and an explicit test run remain necessary before using the changes.

Canonical format version 1 includes `nodePinOrder` on pins so renderers and future structural comparisons can preserve semantic execution-output order without depending on selection or UObject iteration order.

## Scope

Export is read-only. Beautify is the sole exception, and only ever changes node
position within the user's own selection. Snapshot comparison, structural
conflict reporting, and transactional import from a `.graphx.json` snapshot are
planned on top of the canonical JSON model.
