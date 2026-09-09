# AGENTS.md

# GraphXPort

## Mission

GraphXPort is a standalone Unreal Engine Editor plugin for exporting Blueprint
graphs into deterministic, portable representations.

It must work without:

- UnrealMCP
- an MCP client
- an AI service
- Python, Node.js, or an external process
- network access

Users access GraphXPort directly from Blueprint Editor context menus.

## Supported Engine

- Unreal Engine 5.4.4 is the current build and test target.
- Keep engine-specific editor integrations isolated where practical.

## Modules

The plugin ships two editor modules:

- `GraphXPort` - export, formatting, and Beautify. Has no git dependency and works
  entirely offline, as described below.
- `GraphXPortMerge` - the **GraphXPort Merge** window: git status, side-by-side
  Blueprint revision comparison, and conflict resolution. It shells out to `git`,
  which is the one place the plugin invokes an external process.

`GraphXPortMerge` may depend on `GraphXPort`; never the reverse. Nothing in
`GraphXPort` may require git to be installed.

## Current Scope

GraphXPort currently exports selected nodes or the entire active graph as:

- canonical `.graphx.json`
- AI-oriented Markdown
- ASCII flow text
- Mermaid flowchart text

Selection exports must include:

- Blueprint and graph identity
- nodes and pins
- pin defaults
- function, event, variable, member, and macro references when available
- connections inside the selection
- connections crossing the selection boundary

The export path is read-only. Two deliberate, separately reviewed exceptions
mutate state: the manual **Beautify Selection** command, and the
`GraphXPortMerge` conflict resolution flow. Both are described under Safety.

## Architecture

Keep responsibilities separated:

- `GraphXPortMenu` owns Unreal Editor menu integration, active selection, and
  applying Beautify's computed positions inside an undo transaction.
- `GraphXPortSerializer` converts Unreal graph objects into the canonical model.
- `GraphXPortDocument` defines the canonical in-memory representation.
- `GraphXPortFormatter` renders JSON, Markdown, ASCII, and Mermaid.
- `GraphXPortBeautifier` computes a Beautify layout (node -> target position)
  from live `UEdGraphNode`/`UEdGraphPin` data. It only reads graph state and
  returns positions; it never mutates a node itself.
- Tests validate deterministic export behavior, graph boundaries, and Beautify
  layout correctness (linear/branch ordering, cycles, disconnected islands).

`GraphXPortMerge` keeps the same separation:

- `GraphXPortGitProcess` is the only place a git process is launched. It runs
  `git` directly - never through `cmd.exe` - reads stdout as raw bytes so binary
  `.uasset` payloads survive, and launches detached so Windows gives the child no
  console. Launching merely "hidden" is not enough: `bLaunchReallyHidden` only sets
  `SW_HIDE`, which Windows Terminal ignores, popping a visible window per call.
- `GraphXPortGitStatus` parses `git status --porcelain=v2` and stages files.
  Keep parsing in `ParseStatusOutput`, separate from process execution, so it
  stays testable.
- `GraphXPortGitBlob` extracts a revision's bytes for a path.
- `GraphXPortTempPackage` loads a revision as a Blueprint using the engine's own
  `DiffUtils::LoadPackageForDiff`. Use that helper rather than a hand-rolled
  `LoadPackage`: it loads from a loose file with
  `LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks`,
  so an old revision never compiles on load, and it needs no mount point, so
  revisions never appear in the Content Browser.
- `SGraphXPortMergeWindow` owns the file list; `SGraphXPortDiffPanel` owns the
  revision panes, the differences tree, and resolution.

Do not put serialization logic in menu callbacks or UI code.
Do not make formatters inspect live Unreal graph objects.
Do not add a second place that launches git.

## Canonical Format

The canonical document is the source of truth. Other formats are views of that
document, not independent graph parsers.

Canonical output must be:

- deterministic for the same graph state
- structured and machine-readable
- stable enough for source control and structural comparison
- explicit about internal edges and external boundary links
- independent of visual node placement unless placement is intentionally added
  as optional metadata

Sort collections with stable keys before serialization. Never rely on pointer
order, UObject iteration order, selection order, or hash-map order.

Do not silently change canonical field meanings. When compatibility handling is
eventually required, document it and add tests before changing the format.

## Editor Integration

All user actions belong under one `GraphXPort` submenu in Blueprint graph
context menus.

Support graph-node context menus first. Direct My Blueprint tree integration
must use a supported public extension point; do not patch engine source or rely
on private `SMyBlueprint` implementation details.

User-facing operations must report clear failures for missing Blueprint
editors, graphs, selections, clipboard access, or file writes.

## Safety

Export operations must not modify Blueprint assets, compile Blueprints, save
packages, or run PIE.

**Beautify Selection is the one explicit, intentional exception.** It is a
manual, user-initiated command that repositions the nodes the user has
selected (`NodePosX`/`NodePosY` only) to improve readability. It must stay
within these bounds:

- Position-only: never touch pins, connections, node properties, or logic.
  (A fan-out reroute-insertion feature was implemented, live-tested, and
  deliberately rejected/reverted - do not reintroduce node/connection mutation
  in Beautify without this section being updated first and explicit sign-off.)
- Selection-only: never move a node the user did not select.
- Transactional and undoable: wrap changes in `FScopedTransaction`, call
  `Node->Modify()` before each change, and mark the Blueprint package dirty -
  follow normal Unreal editor edit conventions here, not the export path's
  read-only guarantee.
- Deterministic and safe on malformed input: cyclic or disconnected selections
  must still terminate and produce a valid layout (see `GraphXPortBeautifier`'s
  visited-set and depth-cap guards, mirroring the ASCII formatter's).
- No new mutation surface without updating this section first. If a future
  change needs to touch anything beyond node position, treat it as a new,
  separately reviewed exception - do not fold it into Beautify silently.

**GraphXPortMerge conflict resolution is the second explicit exception.** The
merge window edits and saves the real working-tree asset and stages it with git.
It must stay within these bounds:

- Only the "Mine" pane is editable, and it is always the real, live `/Game`
  asset. Base and Theirs are diff-only revisions and must stay read-only.
  Read-only is enforced three ways, and all three are required: `IsEditable(false)`,
  `DisplayAsReadOnly(true)`, and clearing the graph's own `UEdGraph::bEditable`.
  The widget attribute alone does not gate pin and connection editing.
- Clearing `bEditable` is only ever done on diff-only revisions, which are never
  saved. Never clear it on the real asset's graph.
- Diff-only revisions are never force-unloaded. Hold them alive from an
  `FGCObject` while displayed and let GC reclaim them; unloading a package whose
  graph a live `SGraphEditor` still points at crashes the editor. Reference them
  as `TObjectPtr`, not raw pointers - the raw-pointer `AddReferencedObject`
  overload is documented to crash under incremental GC.
- The merge window never overwrites the working-tree file with a revision's
  bytes. "Accept current/incoming" was implemented and deliberately removed:
  wholesale side-taking belongs in the user's git tool, and overwriting a loaded
  `.uasset` on disk fails anyway while the editor holds the file open. Do not
  reintroduce it without this section being updated first and explicit sign-off.
- Resolution is: the user edits Mine with ordinary graph editing, then
  **Save & Mark Resolved** compiles, saves, and runs `git add`. A failed compile
  must abort the whole action - never stage a Blueprint that does not compile,
  because a conflict marked resolved stops being looked at.
- Staging is the only git write. Never commit, push, checkout, reset, or
  otherwise move the user's branch state from inside the editor.
- Only unstaged worktree entries are listed, deliberately. Staged files are the
  user's git tool's business.

Future import or merge operations must be:

- previewable before mutation
- transactional where Unreal APIs permit it
- undoable through editor transactions
- explicit about unsupported nodes and unresolved references
- non-destructive by default
- blocked from silently overwriting user graph changes

Never deserialize arbitrary UObject classes from an export file without a
strict allowlist and validation.

## Conflict Workflow

This shipped differently from the original canonical-snapshot plan, and
deliberately so: rather than diffing `.graphx.json` and replaying a merge plan,
the merge window loads the actual Blueprint revisions and shows them in real
`SGraphEditor` panes, so comparison and editing use the engine's own graph
rendering and editing rather than a reimplementation.

Current behaviour:

- Conflicts (`U`) show Base / Mine / Theirs; unstaged edits (`M`, `R`) show
  HEAD / Working.
- Node-level differences come from `FGraphDiffControl::DiffGraphs`, and each side
  is diffed against the common ancestor so a pane shows what *that* side changed.
  Results are handed to `SGraphEditor`'s own `DiffResults` argument, so
  highlighting is engine-rendered.
- Non-graph differences are surfaced too, because a conflict often lives outside
  the graphs: variables (added/removed/retyped/default changed), class defaults
  and component templates (via `DiffUtils::CompareUnrelatedObjects`), components
  (added/removed/class changed), and graphs present on only one side.
- Selecting a non-graph difference swaps the panes to a member list plus a real
  `IDetailsView` of the relevant component or the class defaults, so changed
  values are visible rather than merely named.
- Merging is done by hand: copy nodes from any pane, paste into Mine only.

Diffs compare semantic graph identity, node properties, pin defaults, and edges
rather than raw JSON text or node screen positions.

The canonical `.graphx.json` model is still the export format, and is not used by
the merge path.

## Coding Standards

- Follow Unreal naming and ownership conventions.
- Prefer Unreal containers and value types.
- Avoid raw `new` and `delete`.
- Keep files focused; split source files that become difficult to review.
- Add comments only where editor API behavior or graph semantics are not clear.
- Keep user-visible strings concise and actionable.
- Do not add dependencies on UnrealMCP modules.

## Verification

After C++ changes:

1. Build the host project's editor target for Win64 Development:
   `AR_GIS_MAPEditor` in DSM, or `UE_544_MCPEditor` in the dedicated test project.
2. Run the `GraphXPort.` automation group, including
   `GraphXPort.Export.SelectionAndWholeGraph`, `GraphXPort.Format.Ascii.*`,
   `GraphXPort.Beautify.*`, and `GraphXPort.Merge.GitStatus.*`.
3. Verify export output remains deterministic.
4. For menu changes, manually verify the Blueprint node context submenu in the
   Unreal Editor when practical. For Beautify changes, manually verify in a
   live Blueprint graph that the layout is sane and that Ctrl+Z fully reverts
   both position and package dirty state.
5. For merge-window changes, manually verify against a real conflicted asset:
   Base/Theirs render and stay non-editable (including pin connections), copy
   from a read-only pane pastes into Mine, and no console window appears for any
   git call. Much of this module cannot be covered by automation because it needs
   a real repository in a conflicted state.

Add focused automation coverage for serializer, formatter, boundary-link, and
future diff/import changes.

When the user prohibits running Unreal Editor, do not launch it (including
headless automation). Compile-only verification may use `-NoLink -NoHotReload
-NoHotReloadFromIDE`; report automation tests and the linked DLL as unverified
until a normal build/test run is permitted.

## Repository Hygiene

Track source, tests, documentation, and the `.uplugin` manifest.

Do not commit plugin-local generated output such as:

- `Binaries/`
- `Intermediate/`
- `Saved/`
- IDE-generated files
