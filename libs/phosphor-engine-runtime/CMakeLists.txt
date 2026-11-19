# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorEngineRuntime — the placement engine runtime a host embeds
#
# The settings model (the schema and its host extension, the store-backed
# Settings and its interfaces, the defaults, keys and migrations) and the core
# the runtime is built on (its interfaces, types, resolvers, geometry and
# layout helpers, logging and translation catalog), window tracking and the
# drag resolver, the services a host binds to its transport with the host
# interfaces it implements, and the controllers and overlays with the overlay
# QML (shortcuts, workspaces, the layout controller, the OSDs, snap assist, the
# zone selector and the cheatsheet), and the EngineRuntime hub that wires
# them for a host. Built standalone or as a subdirectory of Phosphor.

cmake_minimum_required(VERSION 3.16)

set(PHOSPHORENGINERUNTIME_VERSION "0.1.0")

# The Qt floor, from the top-level QT_MIN_VERSION in-tree so the two cannot
# drift, and its own value standalone, where that variable does not exist.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORENGINERUNTIME_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORENGINERUNTIME_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorEngineRuntime VERSION ${PHOSPHORENGINERUNTIME_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GenerateExportHeader)

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORENGINERUNTIME_QT_MIN} REQUIRED COMPONENTS Core Gui Qml Quick Concurrent)
# The overlay settings warm-bake through PhosphorRendering's ShaderCompiler,
# whose header includes QShaderBaker (rhi/), in this private module.
if(NOT TARGET Qt6::ShaderToolsPrivate)
    find_package(Qt6ShaderToolsPrivate CONFIG REQUIRED)
endif()
set(CMAKE_AUTOMOC ON)

if(NOT TARGET PhosphorAnimation::PhosphorAnimation)
    find_package(PhosphorAnimation CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorCompositor::PhosphorCompositor)
    find_package(PhosphorCompositor CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorConfig::PhosphorConfig)
    find_package(PhosphorConfig CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorEngine::PhosphorEngine)
    find_package(PhosphorEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorIdentity::PhosphorIdentity)
    find_package(PhosphorIdentity CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorLayoutApi::PhosphorLayoutApi)
    find_package(PhosphorLayoutApi CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorProtocol::PhosphorProtocol)
    find_package(PhosphorProtocol CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRegistry::PhosphorRegistry)
    find_package(PhosphorRegistry CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRules::PhosphorRules)
    find_package(PhosphorRules CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorScreens::PhosphorScreens)
    find_package(PhosphorScreens CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorScrollEngine::PhosphorScrollEngine)
    find_package(PhosphorScrollEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorShaders::PhosphorShaders)
    find_package(PhosphorShaders CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSnapEngine::PhosphorSnapEngine)
    find_package(PhosphorSnapEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSurface::PhosphorSurface)
    find_package(PhosphorSurface CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorTileEngine::PhosphorTileEngine)
    find_package(PhosphorTileEngine CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorTiles::PhosphorTiles)
    find_package(PhosphorTiles CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorAudio::PhosphorAudio)
    find_package(PhosphorAudio CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorGeometry::PhosphorGeometry)
    find_package(PhosphorGeometry CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorPlacement::PhosphorPlacement)
    find_package(PhosphorPlacement CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorWorkspaces::PhosphorWorkspaces)
    find_package(PhosphorWorkspaces CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorContextResolver::PhosphorContextResolver)
    find_package(PhosphorContextResolver CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorShortcuts::PhosphorShortcuts)
    find_package(PhosphorShortcuts CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorZones::PhosphorZones)
    find_package(PhosphorZones CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorLayer::PhosphorLayer)
    find_package(PhosphorLayer CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorOverlay::PhosphorOverlay)
    find_package(PhosphorOverlay CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorRendering::PhosphorRendering)
    find_package(PhosphorRendering CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorShellPatterns::PhosphorShellPatterns)
    find_package(PhosphorShellPatterns CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSurfaceQuick::PhosphorSurfaceQuick)
    find_package(PhosphorSurfaceQuick CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorSurfaces::PhosphorSurfaces)
    find_package(PhosphorSurfaces CONFIG REQUIRED)
endif()
if(NOT TARGET PhosphorZoneRendering::PhosphorZoneRendering)
    find_package(PhosphorZoneRendering CONFIG REQUIRED)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Library
# ═══════════════════════════════════════════════════════════════════════════════

set(phosphorengineruntime_public_HDRS
    include/PhosphorEngineRuntime/AnimationBootstrap.h
    include/PhosphorEngineRuntime/AnimationShaderSupportedPaths.h
    include/PhosphorEngineRuntime/AppDirectoryName.h
    include/PhosphorEngineRuntime/AutotileService.h
    include/PhosphorEngineRuntime/CavaOptions.h
    include/PhosphorEngineRuntime/ConfigBackends.h
    include/PhosphorEngineRuntime/ConfigDefaults.h
    include/PhosphorEngineRuntime/ConfigDefaultsAppearance.h
    include/PhosphorEngineRuntime/ConfigDefaultsGaps.h
    include/PhosphorEngineRuntime/ConfigDefaultsLimits.h
    include/PhosphorEngineRuntime/ConfigDefaultsScreens.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrolling.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrollingBehavior.h
    include/PhosphorEngineRuntime/ConfigDefaultsScrollingShortcuts.h
    include/PhosphorEngineRuntime/ConfigDefaultsShaders.h
    include/PhosphorEngineRuntime/ConfigDefaultsWorkspaces.h
    include/PhosphorEngineRuntime/ConfigKeyMacro.h
    include/PhosphorEngineRuntime/ConfigKeyMacroUndef.h
    include/PhosphorEngineRuntime/ConfigKeys.h
    include/PhosphorEngineRuntime/ConfigKeysScrolling.h
    include/PhosphorEngineRuntime/ConfigKeysWorkspaces.h
    include/PhosphorEngineRuntime/ConfigLogging.h
    include/PhosphorEngineRuntime/ConfigMigration.h
    include/PhosphorEngineRuntime/ConfigMigrationUtil.h
    include/PhosphorEngineRuntime/Constants.h
    include/PhosphorEngineRuntime/ContextResolverWiring.h
    include/PhosphorEngineRuntime/ControlService.h
    include/PhosphorEngineRuntime/CrossSurfaceResolver.h
    include/PhosphorEngineRuntime/DaemonGeometryResolver.h
    include/PhosphorEngineRuntime/DmabufThumbnail.h
    include/PhosphorEngineRuntime/DragActivation.h
    include/PhosphorEngineRuntime/DragResolver.h
    include/PhosphorEngineRuntime/DropProxy.h
    include/PhosphorEngineRuntime/EngineFactory.h
    include/PhosphorEngineRuntime/EngineRouting.h
    include/PhosphorEngineRuntime/EngineRuntime.h
    include/PhosphorEngineRuntime/EngineRuntimeFwd.h
    include/PhosphorEngineRuntime/Enums.h
    include/PhosphorEngineRuntime/GeometryUtils.h
    include/PhosphorEngineRuntime/IHostApps.h
    include/PhosphorEngineRuntime/IIdleSource.h
    include/PhosphorEngineRuntime/IOverlayService.h
    include/PhosphorEngineRuntime/ISettings.h
    include/PhosphorEngineRuntime/IShellCheatsheet.h
    include/PhosphorEngineRuntime/ISurfacePresentationHost.h
    include/PhosphorEngineRuntime/ISystemOsd.h
    include/PhosphorEngineRuntime/IWorkspaceHost.h
    include/PhosphorEngineRuntime/Interfaces.h
    include/PhosphorEngineRuntime/LayoutBundleBuilder.h
    include/PhosphorEngineRuntime/LayoutPreviewSerialize.h
    include/PhosphorEngineRuntime/LayoutService.h
    include/PhosphorEngineRuntime/Logging.h
    include/PhosphorEngineRuntime/Lookups.h
    include/PhosphorEngineRuntime/OverlayControlService.h
    include/PhosphorEngineRuntime/OverlayHelpers.h
    include/PhosphorEngineRuntime/OverlayInternal.h
    include/PhosphorEngineRuntime/OverlayQmlPropertyNames.h
    include/PhosphorEngineRuntime/OverlayRoles.h
    include/PhosphorEngineRuntime/OverlayService.h
    include/PhosphorEngineRuntime/OverlayServiceTypes.h
    include/PhosphorEngineRuntime/OverlayShaderTree.h
    include/PhosphorEngineRuntime/PerScreenResolver.h
    include/PhosphorEngineRuntime/RuleService.h
    include/PhosphorEngineRuntime/RuntimeI18n.h
    include/PhosphorEngineRuntime/ScreenIdResolver.h
    include/PhosphorEngineRuntime/ScreenModeRouter.h
    include/PhosphorEngineRuntime/ScreenReshapeTracker.h
    include/PhosphorEngineRuntime/ScreenScale.h
    include/PhosphorEngineRuntime/ScrollingService.h
    include/PhosphorEngineRuntime/SeedOrderFilter.h
    include/PhosphorEngineRuntime/Settings.h
    include/PhosphorEngineRuntime/SettingsConfigStore.h
    include/PhosphorEngineRuntime/SettingsInterfaces.h
    include/PhosphorEngineRuntime/SettingsSchema.h
    include/PhosphorEngineRuntime/SettingsSchemaChoices.h
    include/PhosphorEngineRuntime/SettingsService.h
    include/PhosphorEngineRuntime/ShaderRegistry.h
    include/PhosphorEngineRuntime/ShaderService.h
    include/PhosphorEngineRuntime/ShortcutManager.h
    include/PhosphorEngineRuntime/ShortcutManagerIds.h
    include/PhosphorEngineRuntime/SnapAssistThumbnailProvider.h
    include/PhosphorEngineRuntime/SnapGapTracker.h
    include/PhosphorEngineRuntime/SnapService.h
    include/PhosphorEngineRuntime/SpatialAdjacency.h
    include/PhosphorEngineRuntime/StripCardSerialize.h
    include/PhosphorEngineRuntime/StripSelectorHitTest.h
    include/PhosphorEngineRuntime/StripZones.h
    include/PhosphorEngineRuntime/ThumbnailUrl.h
    include/PhosphorEngineRuntime/TilingService.h
    include/PhosphorEngineRuntime/Types.h
    include/PhosphorEngineRuntime/UnifiedLayoutController.h
    include/PhosphorEngineRuntime/UnifiedLayoutList.h
    include/PhosphorEngineRuntime/Utils.h
    include/PhosphorEngineRuntime/WindowLifecycleRelay.h
    include/PhosphorEngineRuntime/WindowTracker.h
    include/PhosphorEngineRuntime/WindowTrackerInternal.h
    include/PhosphorEngineRuntime/WorkspaceController.h
    include/PhosphorEngineRuntime/ZoneQueryService.h
    include/PhosphorEngineRuntime/ZoneSelectorLayout.h
)

set(phosphorengineruntime_SRCS
    src/common/layoutbundlebuilder.cpp
    src/common/layoutpreviewserialize.cpp
    src/common/screenidresolver.cpp
    src/common/stripcardserialize.cpp
    src/config/configbackends.cpp
    src/config/configdefaults.cpp
    src/config/configkeys_disablelabels.h
    src/config/configlogging.cpp
    src/config/configmigration.cpp
    src/config/configmigration_v2.cpp
    src/config/configmigration_v3.cpp
    src/config/configmigration_v4.cpp
    src/config/configmigration_v4detail.h
    src/config/configmigration_v4finalize.cpp
    src/config/configmigration_v4rules.cpp
    src/config/configmigration_v5.cpp
    src/config/configmigration_v6.cpp
    src/config/configmigration_v7.cpp
    src/config/configmigration_v8.cpp
    src/config/configmigration_v8overlay.cpp
    src/config/perscreenresolver.cpp
    src/config/settings.cpp
    src/config/settings/animationprofile.cpp
    src/config/settings/disable.cpp
    src/config/settings/loadsave.cpp
    src/config/settings/perscreen.cpp
    src/config/settings/perscreen_detail.h
    src/config/settings/perscreen_selector.cpp
    src/config/settings/profiletrees.cpp
    src/config/settings/scrolling.cpp
    src/config/settings/setters.cpp
    src/config/settings/settings_detail.h
    src/config/settings/shortcuts.cpp
    src/config/settings/storescalars.cpp
    src/config/settings/systemcolors.cpp
    src/config/settings/triggers.cpp
    src/config/settings/uienums.cpp
    src/config/settings/workspaces.cpp
    src/config/settingsconfigstore.cpp
    src/config/settingsschema.cpp
    src/config/settingsschema_overlayshaders.cpp
    src/config/settingsschema_p.h
    src/config/settingsschema_scrolling.cpp
    src/config/settingsschema_shaderbounds.cpp
    src/config/settingsschema_shaderbounds.h
    src/config/settingsschema_shadertrees.cpp
    src/config/settingsschema_tiling.cpp
    src/config/settingsschema_workspaces.cpp
    src/core/interfaces/interfaces.cpp
    src/core/interfaces/isettings.cpp
    src/core/platform/appdirectoryname.cpp
    src/core/platform/logging.cpp
    src/core/resolve/animationbootstrap.cpp
    src/core/resolve/crosssurfaceresolver.cpp
    src/core/resolve/daemongeometryresolver.cpp
    src/core/resolve/screenmoderouter.cpp
    src/core/types/overlayshadertree.cpp
    src/core/utils/geometryutils.cpp
    src/core/utils/unifiedlayoutlist.cpp
    src/core/utils/utils.cpp
    src/runtime/common/lookups.cpp
    src/runtime/common/runtimei18n.cpp
    src/runtime/controllers/contextresolverwiring.cpp
    src/runtime/controllers/enginefactory.cpp
    src/runtime/controllers/shortcutmanager.cpp
    src/runtime/controllers/shortcutmanager_adhoc.cpp
    src/runtime/controllers/shortcutmanager_catalog.cpp
    src/runtime/controllers/shortcutmanager_table.cpp
    src/runtime/controllers/shortcutmanager_table.h
    src/runtime/controllers/unifiedlayoutcontroller.cpp
    src/runtime/controllers/workspacecontroller.cpp
    src/runtime/controllers/workspacecontroller_verbs.cpp
    src/runtime/controllers/zoneselectorcontroller.cpp
    src/runtime/controllers/zoneselectorcontroller.h
    src/runtime/dragresolver/drag.cpp
    src/runtime/dragresolver/drag_protocol.cpp
    src/runtime/dragresolver/dragactivation.cpp
    src/runtime/dragresolver/dragresolver.cpp
    src/runtime/dragresolver/dragtimers.cpp
    src/runtime/dragresolver/dragzonespan.cpp
    src/runtime/dragresolver/drop.cpp
    src/runtime/dragresolver/dropproxy.cpp
    src/runtime/dragresolver/selector_gate.cpp
    src/runtime/engine/animation_profiles.cpp
    src/runtime/engine/autotile.cpp
    src/runtime/engine/autotile_init.cpp
    src/runtime/engine/cheatsheet.cpp
    src/runtime/engine/desktop_renumber.cpp
    src/runtime/engine/engine_release.cpp
    src/runtime/engine/enginerouting.cpp
    src/runtime/engine/engineruntime.cpp
    src/runtime/engine/evacuees.cpp
    src/runtime/engine/helpers.h
    src/runtime/engine/idle.cpp
    src/runtime/engine/init_adaptors.cpp
    src/runtime/engine/init_assignment_apply.cpp
    src/runtime/engine/init_engines.cpp
    src/runtime/engine/init_service_wiring.cpp
    src/runtime/engine/init_services.cpp
    src/runtime/engine/lifecycle.cpp
    src/runtime/engine/macros.h
    src/runtime/engine/navigation.cpp
    src/runtime/engine/osd.cpp
    src/runtime/engine/screenreshapetracker.cpp
    src/runtime/engine/scrolling.cpp
    src/runtime/engine/scrolling_init.cpp
    src/runtime/engine/seedorderfilter.cpp
    src/runtime/engine/shader_warmup.cpp
    src/runtime/engine/shortcuts_wiring.cpp
    src/runtime/engine/signals.cpp
    src/runtime/engine/snapgaptracker.cpp
    src/runtime/engine/start.cpp
    src/runtime/engine/workspaces.cpp
    src/runtime/overlay/animation_config.cpp
    src/runtime/overlay/cheatsheet.cpp
    src/runtime/overlay/decoration.cpp
    src/runtime/overlay/lifecycle.cpp
    src/runtime/overlay/osd.cpp
    src/runtime/overlay/overlay.cpp
    src/runtime/overlay/overlay_data.cpp
    src/runtime/overlay/overlayservice.cpp
    src/runtime/overlay/phosphor_slot_keys.h
    src/runtime/overlay/priming.cpp
    src/runtime/overlay/rekey.cpp
    src/runtime/overlay/screens.cpp
    src/runtime/overlay/scrolldropindicator.cpp
    src/runtime/overlay/selector.cpp
    src/runtime/overlay/selector_strip.cpp
    src/runtime/overlay/selector_update.cpp
    src/runtime/overlay/settings.cpp
    src/runtime/overlay/shader.cpp
    src/runtime/overlay/shellhost_bridge.cpp
    src/runtime/overlay/snapassist.cpp
    src/runtime/rendering/snapassistthumbnailprovider.cpp
    src/runtime/services/autotileservice/autotileservice.cpp
    src/runtime/services/autotileservice/config.cpp
    src/runtime/services/controlservice.cpp
    src/runtime/services/controlservice_shortcuts.cpp
    src/runtime/services/layoutservice/assignment.cpp
    src/runtime/services/layoutservice/assignment_batch.cpp
    src/runtime/services/layoutservice/editor.cpp
    src/runtime/services/layoutservice/layoutservice.cpp
    src/runtime/services/overlaycontrolservice.cpp
    src/runtime/services/ruleservice.cpp
    src/runtime/services/scrollingservice/scrollingservice.cpp
    src/runtime/services/settingsservice/settingsservice.cpp
    src/runtime/services/settingsservice/settingsservice_batch.cpp
    src/runtime/services/settingsservice/settingsservice_registry.cpp
    src/runtime/services/settingsservice/settingsservice_registry_autotile.cpp
    src/runtime/services/settingsservice/settingsservice_registry_scrolling.cpp
    src/runtime/services/settingsservice/settingsservice_registry_snapping.cpp
    src/runtime/services/shaderservice.cpp
    src/runtime/services/snapservice/busgate.cpp
    src/runtime/services/snapservice/commit.cpp
    src/runtime/services/snapservice/navigation.cpp
    src/runtime/services/snapservice/snaprestore.cpp
    src/runtime/services/snapservice/snapservice.cpp
    src/runtime/services/tilingservice/desktopreconcile.cpp
    src/runtime/services/tilingservice/mapmodel.cpp
    src/runtime/services/tilingservice/relay.cpp
    src/runtime/services/tilingservice/tilingservice.cpp
    src/runtime/services/zonequeryservice.cpp
    src/runtime/windowtracker/activation.cpp
    src/runtime/windowtracker/convenience.cpp
    src/runtime/windowtracker/crossmode.cpp
    src/runtime/windowtracker/enginewiring.cpp
    src/runtime/windowtracker/evacueeledger.h
    src/runtime/windowtracker/evacuees.cpp
    src/runtime/windowtracker/float.cpp
    src/runtime/windowtracker/lifecycle.cpp
    src/runtime/windowtracker/navigation.cpp
    src/runtime/windowtracker/persistence.cpp
    src/runtime/windowtracker/persistenceworker.cpp
    src/runtime/windowtracker/persistenceworker.h
    src/runtime/windowtracker/prune.cpp
    src/runtime/windowtracker/queries.cpp
    src/runtime/windowtracker/rules.cpp
    src/runtime/windowtracker/rules_admission.h
    src/runtime/windowtracker/rules_placement.cpp
    src/runtime/windowtracker/saveload.cpp
    src/runtime/windowtracker/screenleave.cpp
    src/runtime/windowtracker/shellsurface.cpp
    src/runtime/windowtracker/shellsurface_desktop.cpp
    src/runtime/windowtracker/windowtracker.cpp
)

# The overlay QML, loaded from qrc:/phosphor-engine-runtime/overlay/. The
# files reach each other as types of that one directory, so they need no QML
# module of their own. They import org.phosphor.zonerendering, org.phosphor.
# animation, org.phosphor.surface, Phosphor.Components and Kirigami at run
# time, from the plugins the host process links.
set(phosphorengineruntime_overlay_QML
    qml/overlay/CheatsheetContent.qml
    qml/overlay/CheatsheetGroup.qml
    qml/overlay/CheatsheetRow.qml
    qml/overlay/CheatsheetSearchField.qml
    qml/overlay/KeyChip.qml
    qml/overlay/LayoutOsdContent.qml
    qml/overlay/LayoutPickerContent.qml
    qml/overlay/NavigationOsdContent.qml
    qml/overlay/OsdDismissable.qml
    qml/overlay/PassiveOverlayModalSlots.qml
    qml/overlay/PassiveOverlayShell.qml
    qml/overlay/PopupCardTitle.qml
    qml/overlay/RenderNodeOverlayContent.qml
    qml/overlay/ScrollDropIndicatorContent.qml
    qml/overlay/SnapAssistContent.qml
    qml/overlay/ZoneItem.qml
    qml/overlay/ZoneOverlayContent.qml
    qml/overlay/ZoneSelectorContent.qml
    qml/overlay/ZoneSelectorStripCard.qml
)

add_library(PhosphorEngineRuntime SHARED
    ${phosphorengineruntime_public_HDRS}
    ${phosphorengineruntime_SRCS}
)
add_library(PhosphorEngineRuntime::PhosphorEngineRuntime ALIAS PhosphorEngineRuntime)

qt_add_resources(PhosphorEngineRuntime "phosphorengineruntime_overlay_qml"
    PREFIX "/phosphor-engine-runtime/overlay"
    BASE qml/overlay
    FILES ${phosphorengineruntime_overlay_QML}
)

# Out of the unity build, as they were in PlasmaZones: the drag resolver's
# translation units each declare file-local helpers under the same names,
# which collide once unity merges them.
set_source_files_properties(
    src/runtime/dragresolver/drag.cpp
    src/runtime/dragresolver/drag_protocol.cpp
    src/runtime/dragresolver/drop.cpp
    src/runtime/dragresolver/dragtimers.cpp
    src/runtime/dragresolver/dragzonespan.cpp
    src/runtime/dragresolver/selector_gate.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)

generate_export_header(PhosphorEngineRuntime
    EXPORT_FILE_NAME phosphorengineruntime_export.h
    EXPORT_MACRO_NAME PHOSPHORENGINERUNTIME_EXPORT
)

if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

target_include_directories(PhosphorEngineRuntime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_compile_features(PhosphorEngineRuntime PUBLIC cxx_std_20)

# PUBLIC: the public headers expose each of these libraries' types (the
# settings interfaces return their profiles, trees, rule stores and engine
# settings by value or reference).
target_link_libraries(PhosphorEngineRuntime
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        PhosphorAnimation::PhosphorAnimation
        PhosphorCompositor::PhosphorCompositor
        PhosphorConfig::PhosphorConfig
        PhosphorEngine::PhosphorEngine
        PhosphorIdentity::PhosphorIdentity
        PhosphorLayoutApi::PhosphorLayoutApi
        PhosphorProtocol::PhosphorProtocol
        PhosphorRegistry::PhosphorRegistry
        PhosphorRules::PhosphorRules
        PhosphorScreens::PhosphorScreens
        PhosphorScrollEngine::PhosphorScrollEngine
        PhosphorShaders::PhosphorShaders
        PhosphorSnapEngine::PhosphorSnapEngine
        PhosphorSurface::PhosphorSurface
        PhosphorTileEngine::PhosphorTileEngine
        PhosphorTiles::PhosphorTiles
        PhosphorAudio::PhosphorAudio
        PhosphorGeometry::PhosphorGeometry
        PhosphorPlacement::PhosphorPlacement
        PhosphorWorkspaces::PhosphorWorkspaces
        PhosphorZones::PhosphorZones
        # WindowTracker and DragResolver hold the context resolver their hosts give them.
        PhosphorContextResolver::PhosphorContextResolver
        # OverlayService.h and the overlay roles hand out PhosphorLayer roles
        # and PhosphorOverlay shell state; ShortcutManager is a
        # PhosphorShortcuts ad-hoc registrar.
        PhosphorLayer::PhosphorLayer
        PhosphorOverlay::PhosphorOverlay
        PhosphorShellPatterns::PhosphorShellPatterns
        PhosphorShortcuts::PhosphorShortcuts
    PRIVATE
        # The overlays' surface manager, and the zone labels they build with
        # ZoneLabelTextureBuilder into a PhosphorRendering texture. The overlay
        # QML imports org.phosphor.zonerendering at run time from the host's
        # link.
        PhosphorRendering::PhosphorRendering
        PhosphorSurfaces::PhosphorSurfaces
        PhosphorZoneRendering::PhosphorZoneRendering
        # The hub's shader warm-up bakes the surface packs through
        # SurfaceShaderItem's own bake path.
        PhosphorSurfaceQuick::PhosphorSurfaceQuick
        # The hub runs the warm bake and the startup layout scan off the GUI
        # thread with QtConcurrent.
        Qt6::Concurrent
        # overlay/settings.cpp reaches PhosphorRendering's ShaderCompiler.h.
        Qt6::ShaderToolsPrivate
)

set_target_properties(PhosphorEngineRuntime PROPERTIES
    VERSION ${PHOSPHORENGINERUNTIME_VERSION}
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

if(NOT DEFINED KDE_INSTALL_LIBDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()

if(NOT DEFINED KDE_INSTALL_BINDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS PhosphorEngineRuntime
    EXPORT PhosphorEngineRuntimeTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(EXPORT PhosphorEngineRuntimeTargets
    FILE PhosphorEngineRuntimeTargets.cmake
    NAMESPACE PhosphorEngineRuntime::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorEngineRuntimeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfigVersion.cmake"
    VERSION ${PHOSPHORENGINERUNTIME_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorEngineRuntimeConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorEngineRuntime
)

install(DIRECTORY include/PhosphorEngineRuntime
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/phosphorengineruntime_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorEngineRuntime
)

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

if(CMAKE_PROJECT_NAME STREQUAL "PhosphorEngineRuntime" OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
