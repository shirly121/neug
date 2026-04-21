## ADDED Requirements

### Requirement: GDS algorithms use GDSAlgoFunction

GDS graph algorithm **`CALL`** entries (e.g. **`k_core`**) SHALL be registered as **`function::GDSAlgoFunction`** instances (subclasses or direct construction), not as raw **`NeugCallFunction`** with manually duplicated **`TableFunction::bindFunc`** wiring.

#### Scenario: KCore registration

- **WHEN** the catalog exposes **`k_core`**
- **THEN** the underlying **`TableFunction`** object SHALL be a **`GDSAlgoFunction`** (or subtype) whose **`TableFunction::bindFunc`** is provided by the shared GDS bind path (**`bindGDSFunction`** or equivalent)

### Requirement: Preserve NeugCallFunction binder contract

**`GDSAlgoFunction`** SHALL inherit **`NeugCallFunction`** so that **`Binder::bindTableFunc`** continues to **`constPtrCast<NeugCallFunction>`** without special cases.

#### Scenario: Binder unchanged

- **WHEN** a query contains **`CALL k_core(...)`**
- **THEN** binding uses the existing **`NeugCallFunction`** / **`TableFunction::bindFunc`** pipeline without new **`FunctionCatalogEntry`** branches solely for GDS

### Requirement: algo_exec_func_t hook

**`GDSAlgoFunction`** SHALL expose an **`algo_exec_func_t`** (or project-approved alias) distinct from **`call_exec_func_t`**, and SHALL bridge it to **`NeugCallFunction::execFunc`** via an adapter until the execution runtime consumes **`GDSAlgo`** directly.

#### Scenario: Stub execution

- **WHEN** execution still uses **`call_exec_func_t`**
- **THEN** the bridge MAY return an empty **`execution::Context`** while preserving correct physical **`GDSAlgo`** lowering
