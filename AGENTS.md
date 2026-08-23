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

The plugin is read-only until import and conflict resolution are explicitly
implemented.

## Architecture

Keep responsibilities separated:

- `GraphXPortMenu` owns Unreal Editor menu integration and active selection.
- `GraphXPortSerializer` converts Unreal graph objects into the canonical model.
- `GraphXPortDocument` defines the canonical in-memory representation.
- `GraphXPortFormatter` renders JSON, Markdown, ASCII, and Mermaid.
- Tests validate deterministic export behavior and graph boundaries.

Do not put serialization logic in menu callbacks or UI code.
Do not make formatters inspect live Unreal graph objects.

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

Future import or merge operations must be:

- previewable before mutation
- transactional where Unreal APIs permit it
- undoable through editor transactions
- explicit about unsupported nodes and unresolved references
- non-destructive by default
- blocked from silently overwriting user graph changes

Never deserialize arbitrary UObject classes from an export file without a
strict allowlist and validation.

## Planned Conflict Workflow

Build conflict support in this order:

1. Canonical graph snapshots.
2. Structural comparison between two snapshots.
3. Three-way comparison using base, local, and incoming snapshots.
4. Human-readable conflict reports.
5. Preview-only import planning.
6. Transactional application of explicitly accepted changes.

Diffs should compare semantic graph identity, node properties, pin defaults,
and edges rather than raw JSON text or node screen positions.

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

1. Build `UE_544_MCPEditor` for Win64 Development.
2. Run `GraphXPort.Export.SelectionAndWholeGraph` automation tests.
3. Verify export output remains deterministic.
4. For menu changes, manually verify the Blueprint node context submenu in the
   Unreal Editor when practical.

Add focused automation coverage for serializer, formatter, boundary-link, and
future diff/import changes.

## Repository Hygiene

Track source, tests, documentation, and the `.uplugin` manifest.

Do not commit plugin-local generated output such as:

- `Binaries/`
- `Intermediate/`
- `Saved/`
- IDE-generated files

