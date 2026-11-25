<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-animation

phosphor-animation runs motion and shader transitions for Phosphor surfaces. It has a motion runtime
(`AnimatedValue<T>` driven by curves, springs and easings) and a shader-transition runtime (GPU effect selection for
each event). JSON-backed `Profile` trees configure both runtimes.

## Responsibility

The library supplies a clock for each output, a typed `AnimatedValue` that interpolates toward a target, polymorphic
curves, and profiles for each event. A profile maps an event name such as `window.open` or `editor.snapIn` to a
motion configuration.

The library has two parallel runtimes:

| Runtime | Namespace | Contents |
|---------|-----------|----------|
| Motion | `PhosphorAnimation` | `AnimatedValue<T>`, polymorphic `Curve`s, `IMotionClock`, retarget and snap policy, stagger, and the `Profile` / `ProfileTree` / `PhosphorProfileRegistry` system |
| Shader transitions | `PhosphorAnimationShaders` | `AnimationShaderRegistry` finds transition shader packs on search paths. The parallel `ShaderProfile` / `ShaderProfileTree` maps the same event names to an effect and its parameter values |

Both profile trees use `std::optional` fields. A leaf can inherit a value from its parent (`nullopt`) or override it
(engaged). The two trees share the dot-path event namespace, but each resolves separately. Thus a user can change
the curve and keep the visual effect.

`SurfaceAnimator` implements `ISurfaceAnimator` of [`phosphor-layer`](../phosphor-layer/README.md). For an event, it
asks both registries what to play.

User-edited curves and profiles reload live from the user data directory through
[`phosphor-fsloader`](../phosphor-fsloader/README.md).

## Key types

### Motion runtime

| Type | Purpose |
|------|---------|
| `PhosphorAnimation::AnimatedValue<T>`         | Typed animation in progress (rect, point, color, scalar). It uses a pull model: the consumer reads `value()` during paint |
| `PhosphorAnimation::MotionSpec<T>`            | Runtime call-site bundle of profile, clock, retarget policy and callbacks |
| `PhosphorAnimation::IMotionClock`             | Pull-model clock contract, one for each output, for mixed refresh rates |
| `PhosphorAnimation::QtQuickClock`             | `IMotionClock` bound to a `QQuickWindow` (built only with `PHOSPHOR_ANIMATION_QUICK=ON`) |
| `PhosphorAnimation::Curve`                    | Polymorphic base. Every curve family implements `step()`, which advances a `CurveState` |
| `PhosphorAnimation::Easing`                   | Cubic Bézier curve family (ease-out, ease-in-out and others) |
| `PhosphorAnimation::Spring`                   | Critically damped spring with configurable tension and friction |
| `PhosphorAnimation::CurveRegistry`            | Name-to-curve factory, so that profiles can refer to curves by string |
| `PhosphorAnimation::Profile`                  | Serializable bundle of curve, duration and stagger. Its `optional` fields support inherit and override |
| `PhosphorAnimation::ProfileTree`              | Hierarchical profile lookup with inheritance (`window.open` inherits from `window`) |
| `PhosphorAnimation::PhosphorProfileRegistry`  | Process-wide registry that reloads profiles live and sends live updates |
| `PhosphorAnimation::ProfileLoader`            | Sink for [`phosphor-fsloader`](../phosphor-fsloader/README.md) |
| `PhosphorAnimation::CurveLoader`              | Sink for [`phosphor-fsloader`](../phosphor-fsloader/README.md) |
| `PhosphorAnimation::RetargetPolicy`           | Behavior when the target changes during an animation: PreserveVelocity, ResetVelocity or PreservePosition |
| `PhosphorAnimation::SnapPolicy`               | Free helpers that decide if a transition gets an animation |
| `PhosphorAnimation::StaggerTimer`             | Schedules animation starts across a group of windows |
| `PhosphorAnimationLayer::SurfaceAnimator`     | `ISurfaceAnimator` implementation that connects both runtimes to [`phosphor-layer`](../phosphor-layer/README.md) |

### Shader-transition runtime

| Type | Purpose |
|------|---------|
| `PhosphorAnimationShaders::AnimationShaderEffect`   | Metadata for one transition (id, shader paths, parameter declarations) |
| `PhosphorAnimationShaders::AnimationShaderRegistry` | Finds transition shader packs on search paths. It uses the metadata-pack scan strategy of [`phosphor-fsloader`](../phosphor-fsloader/README.md) |
| `PhosphorAnimationShaders::ShaderProfile`           | Shader effect selection and parameter values for each event |
| `PhosphorAnimationShaders::ShaderProfileTree`       | Hierarchical lookup with the same shape as `ProfileTree` |

## Use

Animate a window rect with a profile:

```cpp
#include <PhosphorAnimation/AnimatedValue.h>
#include <PhosphorAnimation/QtQuickClock.h>
#include <PhosphorAnimation/PhosphorProfileRegistry.h>
#include <PhosphorAnimation/ProfileLoader.h>

using namespace PhosphorAnimation;

QtQuickClock clock(window);

// CurveLoader populates the curve registry first, then ProfileLoader
// scans profile files into the profile registry.
PhosphorProfileRegistry registry;
ProfileLoader loader(registry, curveRegistry);
loader.loadFromDirectory(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));

AnimatedValue<QRectF> geometry(initialRect);

MotionSpec<QRectF> spec{
    .profile        = registry.resolveWithInheritance(QStringLiteral("editor.snapIn")),
    .clock          = &clock,
    .retargetPolicy = RetargetPolicy::PreserveVelocity,
};
geometry.start(initialRect, targetRect, spec);

// In paint:
QRectF current = geometry.value();
```

Choose a transition with the shader runtime:

```cpp
#include <PhosphorAnimation/AnimationShaderRegistry.h>
#include <PhosphorAnimation/ShaderProfileTree.h>

using namespace PhosphorAnimationShaders;

AnimationShaderRegistry shaders;
shaders.addSearchPath(systemDir);
shaders.addSearchPath(userDir);
shaders.refresh();

ShaderProfile sp = shaderProfileTree.resolve(QStringLiteral("window.open"));
QString effectId    = sp.effectId.value_or(QStringLiteral("dissolve"));
QVariantMap params  = sp.parameters.value_or(QVariantMap{});
```

## Design notes

- **Pull-model clock.** `AnimatedValue<T>::value()` reads `IMotionClock::now()` in the paint cycle of the consumer.
  There are no timers and no Qt signals for each frame. One clock for each output keeps mixed refresh rates correct.
- **Polymorphic step contract.** Every `Curve` returns the state that the consumer needs: the current value, the
  velocity and a settled flag. The `AnimatedValue` does not know if the curve is a spring, an easing or a
  user-defined Bézier curve.
- **Profile and ShaderProfile share an event namespace, not a tree.** `window.open` selects a motion profile and a
  transition effect, but through two trees that resolve separately. Thus a user can change the curve and keep the
  visual effect, or change the effect and keep the curve.
- **`optional` means inherit.** `ProfileTree` and `ShaderProfileTree` both read a `nullopt` field from the parent.
  Engaged fields win. Thus an override can change one parameter and keep the rest of the bundle.
- **`PHOSPHOR_ANIMATION_QUICK` controls the Qt Quick dependencies.** Headless tools and the KWin compositor adapter
  do not build `QtQuickClock`, so they do not link `Qt6::Quick`. The core motion code needs only `QtCore` and
  `QtGui`.

## Dependencies

- `QtCore`, `QtGui`, `QtQuick`, `QtQuickPrivate`, `QtQml`
- [`phosphor-registry`](../phosphor-registry/README.md): `Registry<T>` storage and `MetadataPackLoader<T>`, which back `AnimationShaderRegistry` and `CurveRegistry`
- [`phosphor-fsloader`](../phosphor-fsloader/README.md): directory loaders for curves, profiles and shader packs
- [`phosphor-shaders`](../phosphor-shaders/README.md): header-only use of `CustomParamsKey.h`
- [`phosphor-layer`](../phosphor-layer/README.md): `ISurfaceAnimator`, which `SurfaceAnimator` implements
- [`phosphor-rendering`](../phosphor-rendering/README.md)

## See also

- [`phosphor-fsloader`](../phosphor-fsloader/README.md): its `MetadataPackScanStrategy` drives the pack discovery of `AnimationShaderRegistry`. The `MetadataPackLoader<T>` of [`phosphor-registry`](../phosphor-registry/README.md) wraps it into a `Registry<T>`. Profile and curve files come through its `DirectoryLoader`.
- [`phosphor-rendering`](../phosphor-rendering/README.md): host items that use the compiled shader of the chosen effect.
- [`phosphor-shaders`](../phosphor-shaders/README.md): the overlay shader registry. The animation registry of this library uses the same metadata-pack scan strategy.
