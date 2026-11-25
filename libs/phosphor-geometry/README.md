<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-geometry

phosphor-geometry holds pure geometry functions for zones and windows. They clamp zones, resolve overlaps, enforce
minimum window sizes, project into overlay coordinates, find directional neighbors and encode rects as JSON.

## Responsibility

The snap engine and the autotile engine share these pure-function corrections:

- Clamp zones to the screen.
- Remove overlaps.
- Grow zones to respect window minimum sizes.
- Project geometry into the local coordinate system of an overlay window.

The functions take rects and change the zone vector in place. They use no Qt objects and no signals. Headless
geometry tests link the library without GUI infrastructure.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorGeometry::availableAreaToOverlayCoordinates` | Projects an "available-area" rect into the coordinates of an overlay window |
| `PhosphorGeometry::snapToRect`                        | `QRectF` to `QRect` with consistent rounding |
| `PhosphorGeometry::enforceMinSizes`                   | Grows zones to fit per-window minimum sizes. It takes surplus from neighbors, then resolves overlap |
| `PhosphorGeometry::clampZonesToScreen`                | Position-only clamp. It moves zones so that the effective rect of each window stays on screen, and keeps the sizes |
| `PhosphorGeometry::removeRectOverlaps`                | Resolves the overlap that remains between zones (used after min-size growth) |
| `PhosphorGeometry::insetRect`                         | Shrinks a rect by an inset on every edge (a tile or zone border inset) |
| `PhosphorGeometry::rectToJson`                        | Canonical rect-string format for D-Bus and the JSON round trip |
| `PhosphorGeometry::JsonKeys`                          | Key constants for the rect-JSON encoder |
| `PhosphorGeometry::Direction`, `directionFromString`  | Cardinal direction and its parser for the lower-case tokens `left`, `right`, `up`, `down` |
| `PhosphorGeometry::directionalNeighbor`, `edgeMostRect` | Finds the rect in a direction from a focus rect, or the rect at an edge |
| `PhosphorGeometry::neighborDesktopInDirection`        | The virtual desktop one step away on a desktop grid, or 0 when there is none |

## Use

Run `enforceMinSizes` first, then `clampZonesToScreen`.

```cpp
#include <PhosphorGeometry/DirectionalNeighbor.h>
#include <PhosphorGeometry/GeometryUtils.h>

using namespace PhosphorGeometry;

QVector<QRect> zones = layoutZones();          // one rect for each window
const QVector<QSize> minSizes = windowMinSizes();
enforceMinSizes(zones, minSizes, gapThreshold, innerGap);
clampZonesToScreen(zones, minSizes, screenRect);

const QString json = rectToJson(zones.first()); // compact JSON: x, y, width, height

// The neighbor to the right of the focused rect, or -1 when there is none
const int index = directionalNeighbor(focusRect, candidateRects, Direction::Right);
```

## Design notes

- **Position-only compared to growth.** `clampZonesToScreen` only moves zones and never resizes them.
  `enforceMinSizes` is the one path that can grow or shrink zones. They run in that order: first grow to fit the
  minimums, then clamp the position of all zones that are still off-screen. Overlap-tolerant algorithms (Deck,
  Stair, Cascade, Monocle, Paper, Spread, horizontal-deck) skip `enforceMinSizes`, because neighbor stealing would
  destroy the intentional overlap. For them, the position clamp is the only safe correction.
- **Vector-tolerant.** `enforceMinSizes` and `clampZonesToScreen` both accept a `minSizes` vector that is shorter
  than `zones` (or empty for `enforceMinSizes`). A missing entry means no minimum, and extra entries are ignored.
- **Pure functions.** The functions use no Qt objects and no signals. Engines call them directly in their
  layout-emit hot path.

## Dependencies

- `QtCore`

## See also

- [`phosphor-snap-engine`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-snap-engine/README.md)
  and [`phosphor-tile-engine`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-tile-engine/README.md)
  in phosphor-engine-libs. Both send their zone output through these helpers before they publish it.
