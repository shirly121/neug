## ADDED Requirements

### Requirement: List literals use ToList in IR when elements are homogeneous

The compiler SHALL encode a bound list-creation expression whose logical type is `LIST` as protobuf `ToList` (not `ToTuple`) in `common.Expression` operators embedded in physical plans.

#### Scenario: Homogeneous literals in RETURN

- **WHEN** a query contains a list literal whose elements all have the same logical type (e.g. `RETURN ['1','2']`)
- **THEN** the generated expression operators for that list SHALL use `to_list` with `node_type` describing a list type

### Requirement: List literals use ToTuple when elements are heterogeneous

The compiler SHALL encode a bound list-creation expression whose logical type is `STRUCT` as protobuf `ToTuple`.

#### Scenario: Mixed types in RETURN

- **WHEN** a query contains a list literal with differing element logical types (e.g. `RETURN [a.name, a.age]`)
- **THEN** the generated expression SHALL use `to_tuple` as today

### Requirement: Runtime evaluates ToList to list values

The execution layer SHALL evaluate `ExprOpr` with `to_list` to a list `Value` whose element type matches the IR list type.

#### Scenario: Evaluation matches binder semantics

- **WHEN** a physical operator carries a `ToList` with homogeneous field expressions
- **THEN** evaluation SHALL produce a list value (not a struct/tuple value) consistent with `LIST` typing
