<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-layout-api

phosphor-layout-api defines the interfaces and value types that describe a layout across library boundaries. It
holds the `ILayoutSource` seam, the provider contracts, and the value types for algorithm metadata, edge gaps,
aspect-ratio classes, layout IDs and previews.

## Responsibility

The zones library and the autotiling library both supply layouts. Zones come from a JSON description that the user
draws. Autotiling comes from an algorithm that runs over the current window set. Consumers such as the snap engine,
the overlay and the settings UI preview must not depend on the source of a layout.

`phosphor-layout-api` is the shared vocabulary of the two sides:

- Every layout producer implements `ILayoutSource`. A source lists the layouts that it can show with
  `availableLayouts()`. It gives a `LayoutPreview` for one layout ID at a window count with `previewAt()`.
- The provider contracts let a registry (zones, tiles) supply layout sources by ID. The caller does not know the
  concrete type.
- The composition-root glue lets a process declare its provider kinds one time. Consumers then ask the bundle, not
  the individual libraries.
- `CompositeLayoutSource` joins several sources (for example, zones and autotile) into one logical source.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorLayout::ILayoutSource`              | Abstract layout producer. Emits `contentsChanged` when its layouts change. |
| `PhosphorLayout::ILayoutSourceFactory`       | Builds an `ILayoutSource` for a given layout-ID |
| `PhosphorLayout::ILayoutSourceRegistry`      | Provider-side enumeration that emits `contentsChanged` |
| `PhosphorLayout::LayoutSourceProviderRegistrar` | Registers a provider at static-init time, so a process declares "which providers exist" one time |
| `PhosphorLayout::FactoryContext`             | The registries that a composition root gives to each provider builder |
| `PhosphorLayout::LayoutSourceBundle`         | Per-process bundle that builds every registered provider into one `CompositeLayoutSource` |
| `PhosphorLayout::LayoutId`                   | Namespace of helpers that build, parse and classify layout IDs (`"autotile:"`, `"scrolling:"`) |
| `PhosphorLayout::AlgorithmMetadata`          | Capability and display metadata of one autotile algorithm, for a picker UI |
| `PhosphorLayout::AspectRatioClass`           | Screen aspect class (`Any`, `Standard`, `Ultrawide`, `SuperUltrawide`, `Portrait`) |
| `PhosphorLayout::EdgeGaps`                   | Per-side outer gap value type (top, bottom, left, right) |
| `PhosphorLayout::GapKeys`                    | JSON key constants for roundtrip |
| `PhosphorLayout::CompositeLayoutSource`      | Multiplexes several `ILayoutSource`s |
| `PhosphorLayout::LayoutPreview`              | Plain data for one layout entry. Its zone rectangles are in 0.0 to 1.0 relative space, ready for a picker to scale. |

The library headers are in `include/PhosphorLayoutApi/`. `LayoutSourceProviderRegistry.h` declares the registrar,
the factory context and the list of pending providers.

## Use

Implement a custom layout source:

```cpp
#include <PhosphorLayoutApi/ILayoutSource.h>

class MyLayoutSource : public PhosphorLayout::ILayoutSource {
public:
    QVector<PhosphorLayout::LayoutPreview> availableLayouts() const override
    {
        QVector<PhosphorLayout::LayoutPreview> previews;
        // Populate one LayoutPreview per layout this source can render…
        return previews;
    }

    PhosphorLayout::LayoutPreview previewAt(
        const QString &id, int windowCount, const QSize &canvas) override
    {
        PhosphorLayout::LayoutPreview preview;
        // Populate zones for the requested entry…
        return preview;
    }
};
```

Chain two sources, so that consumers hold one pointer for both:

```cpp
CompositeLayoutSource composite;
composite.addSource(zonesSource);                // user-drawn
composite.addSource(autotileSource);

ILayoutSource *combined = &composite;            // consumers don't know the difference
```

`availableLayouts()` on the composite returns the lists of its children, in the order that you added them.
`previewAt()` returns the first non-empty preview of a child. The composite borrows its children. Keep each child
alive while the composite exists.

## Design notes

- **No Qt GUI dependency.** The library links only `QtCore` and phosphor-registry. A headless test or a
  command-line tool can link it without `QtGui`, QML or Quick.
- **One set of `LayoutId` helpers.** IDs are `QString` values in three forms: a UUID for a manual layout,
  `"autotile:<algorithmId>"` for an algorithm, and `"scrolling:"` for scrolling. Every consumer builds and
  classifies IDs through `LayoutId`, so no other code writes these prefixes.
- **`CompositeLayoutSource`** lets a daemon handle mixed-source setups. The snap and autotile engines do not need
  special logic for each source.
- **Provider registry pattern.** Both `phosphor-zones` and `phosphor-tiles` implement `ILayoutSourceFactory` and
  register through `LayoutSourceProviderRegistrar`. The composition root of the daemon binds the providers one
  time and does not know them by name.

## Dependencies

- `QtCore`
- [`phosphor-registry`](../phosphor-registry/README.md): the `Registry` and `IFactoryBase` that the factories use

## See also

- [`phosphor-zones`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-zones/README.md):
  the primary `ILayoutSource` implementation (`ZonesLayoutSource`)
- [`phosphor-tiles`](https://github.com/phosphor-works/phosphor-engine-libs/blob/main/libs/phosphor-tiles/README.md):
  the alternative implementation (`AutotileLayoutSource`)
