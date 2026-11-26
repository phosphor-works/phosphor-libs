<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-rules

phosphor-rules is the window and context rule engine for Phosphor. It has one composable match-expression language,
one pluggable action set, one serialization format, and one evaluation pipeline with a match cache.

## Responsibility

The GPL KWin effect and the GPL daemon both link this LGPL-2.1+ library. Thus there is exactly **one**
implementation of the match code. (The license lets GPL code link an LGPL library.)

The library matches a window or a context against a set of rules, and resolves the actions that apply. It has no
QObjects, no QML and no D-Bus. Those belong to the higher-level GPL targets.

## Key types

| Header | Purpose |
|---|---|
| `MatchTypes.h` | `Field` / `Operator` enums and strict string conversion |
| `WindowQuery.h` | The attribute bag that an expression is evaluated against |
| `MatchExpression.h` | Composable leaf/composite predicate tree, JSON, cached regex |
| `RuleAction.h` | Pluggable slot-based action descriptors and `ActionRegistry` |
| `ActionTypes.h` | The built-in action type ids (wire strings), included by `RuleAction.h` |
| `ActionParams.h` | Param keys, shared validation bounds, closed token vocabularies |
| `ActionSlots.h` | The built-in slot ids |
| `Rule.h` | `{ id, name, enabled, priority, match, actions, managed }` |
| `RuleSet.h` | Ordered collection, monotonic revision, `rules.json` I/O |
| `RuleEvaluator.h` | Descending-priority resolution and the `(windowId, revision)` match cache |

## Use

```cpp
#include <PhosphorRules/PhosphorRules.h>

using namespace PhosphorRules;

RuleAction snapping;
snapping.type = QString(ActionType::SetEngineMode);
snapping.params.insert(ActionParam::Mode, QStringLiteral("snapping"));

Rule rule;
rule.id = QUuid::createUuid();
rule.name = QStringLiteral("konsole snaps");
rule.priority = 100;
rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::Equals, QStringLiteral("org.kde.konsole"));
rule.actions = {snapping};

RuleSet set;
set.addRule(rule);             // false if the rule is invalid or its id is taken
RuleEvaluator evaluator(set);  // the caller keeps the set alive

WindowQuery query;
query.appId = QStringLiteral("org.kde.konsole");
const ResolvedActions result = evaluator.resolve(query);
if (const std::optional<RuleAction> mode = result.slot(QString(ActionSlot::EngineMode))) {
    // apply mode->params
}
```

## Evaluation model

`RuleEvaluator::resolve()` walks the rule set in **descending priority**. A stable sort breaks ties by list order.
For each matching enabled rule, it keeps the **first action that fills each slot**. Actions in different slots
stack. The evaluator ignores a second action for a slot that is already filled. A matching rule with a terminal
`Exclude` action stops the walk.

`ResolvedActions` makes a difference between a **slot-unfilled** result (`std::nullopt`) and a
**slot-filled-with-empty-params** result. The animation engaged-empty `effectId` sentinel depends on exactly this
difference.

An empty `All{}` match expression is the **always-true catch-all**. It is the migrated provider default.

## Serialization

`RuleSet` reads and writes `rules.json` at `"_version": 4`. `fromJson` **refuses** all other versions. Schema
migration is the job of the config layer, never of the library. Loaders use strict validation. A malformed rule or
action is dropped with a logged diagnostic, and the set still loads.

## Dependencies

- `Qt6::Core` (no `Qt6::Gui`)
- `PhosphorProtocol::Types`: PUBLIC, for `WindowType`
- `PhosphorIdentity`: PRIVATE, supplies the `AppIdMatches` operator
