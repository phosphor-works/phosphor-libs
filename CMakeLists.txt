# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Phosphor libraries: LGPL Qt6 and Wayland libraries for window placement,
# rendering, layer-shell surfaces and desktop shells.
#
# Two ways in:
#
#   * On its own. cmake --workflow --preset dev configures, builds and tests
#     every library here, with nothing else checked out.
#   * As a subproject of an app. PlasmaZones and Afterglow pin one commit of
#     this repository and add it with CPM (CPMAddPackage), so the app's own
#     find_package and target_link_libraries calls resolve to the targets
#     declared here. CPM's -DCPM_phosphor-libs_SOURCE=<dir> builds a local
#     checkout instead of the pinned one.
#
# As a subproject the app's top level has already set up ECM, the KDE
# settings, Qt and the compile definitions below. Each of those steps here is
# either idempotent or runs only at the top level, so the app's settings are
# the ones in force.
#
# Each library keeps its own CMakeLists, project() metadata and find_package
# config, so it still configures alone from its own directory. Add new
# libraries under libs/ following the phosphor-<name> convention.

cmake_minimum_required(VERSION 3.16)

project(PhosphorLibs VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PHOSPHOR_LIBS_TOP_LEVEL ON)
else()
    set(PHOSPHOR_LIBS_TOP_LEVEL OFF)
endif()

# Lets an app reach this tree's helper modules (PhosphorTestIsolation,
# PhosphorTestPch) by name once it has added the tree, rather than keeping
# copies of its own that drift.
set(PHOSPHOR_LIBS_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake" CACHE INTERNAL "")

# ═══════════════════════════════════════════════════════════════════════════════
# Top-level setup. Skipped as a subproject, where the app has done it.
# ═══════════════════════════════════════════════════════════════════════════════

if(PHOSPHOR_LIBS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTOMOC_PARALLEL_ENABLED ON)

    option(CMAKE_UNITY_BUILD "Enable unity builds for faster compilation" ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16 CACHE STRING "Number of sources per unity batch")
    option(ENABLE_PCH "Share one precompiled header across the test targets" ON)

    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()

    set(QT_MIN_VERSION "6.10.0")
    set(KF_MIN_VERSION "6.26.0")

    find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH "${PHOSPHOR_LIBS_CMAKE_DIR}" ${ECM_MODULE_PATH})

    # Must come before KDECMakeSettings, which includes CTest and locks
    # BUILD_TESTING into the cache as ON.
    option(BUILD_TESTING "Build the test suite" OFF)

    include(KDEInstallDirs)
    include(KDECMakeSettings)
    include(KDECompilerSettings NO_POLICY_SCOPE)
    include(FeatureSummary)

    # Makes the Qt6 string-literal rule a compile error rather than a
    # convention: no implicit const char* to QString.
    add_compile_definitions(QT_NO_CAST_FROM_ASCII)

    include(KDEClangFormat)
    file(GLOB_RECURSE PHOSPHOR_LIBS_CLANG_FORMAT_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/*.hpp
    )
    kde_clang_format(${PHOSPHOR_LIBS_CLANG_FORMAT_FILES})
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Shared lookups. find_package is idempotent, so these also run as a subproject.
# ═══════════════════════════════════════════════════════════════════════════════

if(NOT DEFINED QT_MIN_VERSION)
    set(QT_MIN_VERSION "6.10.0")
endif()

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Concurrent
    Core
    DBus
    Gui
    Qml
    Quick
    QuickControls2
    ShaderTools
    Svg
    WaylandClient
    Widgets
)

# CMake's if(POLICY) knows only CMP policies. Qt's existence test for its own
# QTP policies is the QT_KNOWN_POLICY_<policy> variable.
if(QT_KNOWN_POLICY_QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# The private Qt modules the libraries use (QRhi, QShaderBaker, the QML
# animation module's QuickPrivate, the Wayland QPA plugin). Looked up here,
# once, so each IMPORTED target is visible to every library and consumer.
# Some distros (Fedora) create these targets inside the public module's
# Config file instead of shipping standalone ones, hence the TARGET checks.
foreach(_phosphor_private GuiPrivate ShaderToolsPrivate QuickPrivate WaylandClientPrivate)
    if(NOT TARGET Qt6::${_phosphor_private})
        find_package(Qt6${_phosphor_private} CONFIG REQUIRED)
    endif()
endforeach()

# The pinned archives (Luau, valijson, the test suite's surface packs) and the
# CPM.cmake that downloads them. Included here, at this tree's top scope, and
# not from the libraries that use them: the pins and CPM's state are ordinary
# variables, and a module first included in one library's directory would leave
# them out of its siblings.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PhosphorDependencies.cmake")

# The surface decoration packs, for phosphor-surface's tests that load them.
# Test data only, so fetched only when the tests are built. An app that has
# already fetched the packs and included their PhosphorSurfacePacks.cmake has
# set PHOSPHOR_SURFACE_PACKS_DIR, and its copy is the one tested.
if(BUILD_TESTING AND NOT PHOSPHOR_SURFACE_PACKS_DIR)
    phosphor_fetch_dependency(phosphor-surface-packs)
    include("${phosphor-surface-packs_SOURCE_DIR}/cmake/PhosphorSurfacePacks.cmake")
endif()

# Where the app's bundled animation and pointer packs live, for the library
# tests that check a library constant against them (phosphor-animation's
# built-in effects, phosphor-pointer's shared helper). Empty when this tree is
# built on its own: those tests then skip and say why. PlasmaZones sets it to
# its data/ tree, so they run in its build.
set(PHOSPHOR_LIBS_BUNDLED_PACKS_DIR "" CACHE PATH
    "Directory holding bundled animations/ and pointer/ packs, for the tests that check them. Empty: those tests skip")

# ═══════════════════════════════════════════════════════════════════════════════
# Libraries
# ═══════════════════════════════════════════════════════════════════════════════
#
# Declaration order matters where a library find_package()s a sibling: the
# in-tree target must exist before the consumer configures, or the lookup
# resolves a stale installed copy.

add_subdirectory(libs/phosphor-channel)     # ordered byte frames between two processes with backpressure (shared-memory ring, multiplexed stream) - no Qt
add_subdirectory(libs/phosphor-identity)    # stable cross-process window identity (composite ids, app-id matching)
add_subdirectory(libs/phosphor-dbus)        # generic, service-agnostic D-Bus client utilities (Client, HasDBusStreaming)
add_subdirectory(libs/phosphor-protocol)    # D-Bus wire types and service constants, needed by phosphor-screens (Resolver endpoint defaults)
add_subdirectory(libs/phosphor-fsloader)    # filesystem-backed loader scaffolding (WatchedDirectorySet + DirectoryLoader) - Core-only, needed by phosphor-rules's store watcher
# phosphor-registry: generic Registry<T> + 5 factory interfaces + plugin/metadata
# loaders. Unconditional (third-party hosts + demos consume it without the shell).
# Moved ahead of phosphor-shaders: ShaderRegistry now composes Registry<T> +
# MetadataPackLoader<T>, so the in-tree target must exist before shaders configures
# (else find_package resolves a stale installed copy). Depends on phosphor-fsloader.
add_subdirectory(libs/phosphor-registry)    # generic Registry<T> + 5 factory interfaces + plugin loader
add_subdirectory(libs/phosphor-rules)  # unified window/context rule engine (MatchExpression, RuleEvaluator) - depends on phosphor-identity + phosphor-protocol + phosphor-fsloader
add_subdirectory(libs/phosphor-layout-api)  # shared layout-preview contract (LayoutPreview, ILayoutSource)
add_subdirectory(libs/phosphor-config)      # schema-driven configuration + migration runner - needed by phosphor-zones
add_subdirectory(libs/phosphor-shaders)     # shader-domain primitives (BaseUniforms, IUniformExtension, ShaderRegistry)
add_subdirectory(libs/phosphor-surface)     # surface-shader pack subsystem (SurfaceShaderContract/Effect/Registry) - depends on phosphor-shaders/registry/fsloader
add_subdirectory(libs/phosphor-pointer)     # pointer-shader pack subsystem (PointerShaderContract/Effect/Registry, PointerHistory) - depends on phosphor-shaders/registry/fsloader
add_subdirectory(libs/phosphor-wayland)       # Wayland layer-shell QPA plugin + LayerSurface - needed by phosphor-screens
# ext-idle-notify-v1. Built unconditionally (it used to sit behind
# BUILD_PHOSPHOR_SHELL) because the DAEMON now depends on it: the KWin effect
# pauses decoration animation while the session is idle, and idleness is a Wayland
# CLIENT concern the compositor serves rather than consumes, so the daemon has to
# watch for it and push the result to the effect. Only needs phosphor-wayland, which
# is already unconditional.
add_subdirectory(libs/phosphor-service-idle)  # Wayland idle monitoring + inhibition
add_subdirectory(libs/phosphor-geometry)    # pure geometry math (coordinate transforms, overlap, min-size) - needed by phosphor-screens (swapper directional selector)
add_subdirectory(libs/phosphor-screens)     # screen-topology + virtual-screen subdivision - needed by phosphor-zones
add_subdirectory(libs/phosphor-zones)       # zone primitives + LayoutManager (depends on phosphor-config IBackend, phosphor-screens ScreenIdentity)
add_subdirectory(libs/phosphor-context-resolver) # frozen-snapshot per-screen mode + disable/lock cascade façade (depends on phosphor-zones for AssignmentEntry::Mode)
add_subdirectory(libs/phosphor-scripting)   # generic embedded Luau host (engine, sandbox, watchdog, marshalling) — vendored extern/luau
add_subdirectory(libs/phosphor-tiles)       # auto-tiling algorithm primitives (TilingAlgorithm, AlgorithmRegistry, SplitTree)
add_subdirectory(libs/phosphor-rendering)   # GPU shader rendering via Qt RHI
add_subdirectory(libs/phosphor-surface-quick) # Qt Quick consumer of surface packs (SurfaceShaderItem + the org.phosphor.surface module) - depends on phosphor-rendering/surface/shaders
add_subdirectory(libs/phosphor-layer)       # wlr-layer-shell surface lifecycle management
add_subdirectory(libs/phosphor-shell-patterns) # UI-pattern recipes (axis 2) built on PhosphorLayer::Role
add_subdirectory(libs/phosphor-shortcuts)   # global shortcut backends + domain-free Registry
add_subdirectory(libs/phosphor-ipc)         # JSON-over-Unix-socket IpcRouter + IpcTarget QML type
# The org.phosphor.animation QML module is built with the rest of this tree,
# and its plugin's QtQuickClockManager constructs QtQuickClock directly.
# QtQuickClock is gated behind PHOSPHOR_ANIMATION_QUICK in phosphor-animation's
# CMakeLists, so without it every binary linking the plugin (PlasmaZones'
# daemon and editor, the shell) fails with an unresolved symbol.
#
# FORCE so a stale cached OFF (from a prior standalone configure of
# libs/phosphor-animation in this build directory, or an explicit
# -DPHOSPHOR_ANIMATION_QUICK=OFF on the parent build) cannot silently
# turn into a broken build. Surface the override loudly so a user who
# DID pass -DPHOSPHOR_ANIMATION_QUICK=OFF understands their request
# was rejected — and why.
if(DEFINED CACHE{PHOSPHOR_ANIMATION_QUICK} AND NOT PHOSPHOR_ANIMATION_QUICK)
    message(WARNING
        "PHOSPHOR_ANIMATION_QUICK=OFF was requested but the org.phosphor.animation "
        "QML plugin, which this tree always builds, requires QtQuickClock. "
        "Forcing PHOSPHOR_ANIMATION_QUICK=ON for this configure. "
        "To build without QtQuickClock, configure libs/phosphor-animation standalone.")
endif()
set(PHOSPHOR_ANIMATION_QUICK ON CACHE BOOL "Build QtQuickClock (required by the org.phosphor.animation QML plugin)" FORCE)
add_subdirectory(libs/phosphor-animation)   # unified animation library: motion runtime, QML module, shaders, layer
add_subdirectory(libs/phosphor-audio)       # audio spectrum provider (CAVA backend)
add_subdirectory(libs/phosphor-surfaces)    # managed surface lifecycle (engine, keep-alive, scope gen)
add_subdirectory(libs/phosphor-overlay)     # per-screen shell-host + slot mechanism (rides on layer/shell-patterns/surfaces/animation/screens)
# Order still matters below. The shell libraries (in PlasmaZones' tree until
# they move here) link PhosphorWorkspaces, and phosphor-workspaces links
# PhosphorEngine, so a consumer that adds this tree before its own targets
# finds both already declared. Their own dependencies (identity, protocol,
# layout-api, geometry) are all declared further up.
add_subdirectory(libs/phosphor-engine)  # unified placement engine contracts + window registry (SHARED lib)
add_subdirectory(libs/phosphor-workspaces)  # virtual desktop / workspace management
add_subdirectory(libs/phosphor-tile-engine) # autotile placement engine (IAutotileSettings + engine code)
add_subdirectory(libs/phosphor-snap-engine) # zone-based snap placement engine
add_subdirectory(libs/phosphor-scroll-engine) # niri-style scrolling placement engine (strip model + engine)

add_subdirectory(libs/phosphor-placement)   # window placement state tracking
add_subdirectory(libs/phosphor-compositor)  # compositor-plugin SDK
add_subdirectory(libs/phosphor-control) # reusable Qt6/QML/Kirigami settings-app framework (page controllers, registry, chrome)
add_subdirectory(libs/phosphor-engine-runtime) # the placement engine runtime a host embeds (settings model first) - depends on the engines, zones, screens, config, animation
add_subdirectory(libs/phosphor-zone-rendering) # zone overlays drawn with shader packs (ZoneShaderItem) - depends on phosphor-engine-runtime/rendering/shaders

# ═══════════════════════════════════════════════════════════════════════════════
# Engine QML firewall
# ═══════════════════════════════════════════════════════════════════════════════

# Canonical list of engine library target names. Consumed by the
# engine QML firewall below. Kept in sync with the engine
# add_subdirectory calls above by hand; the firewall FATAL_ERRORs naming any
# entry that matches no target, so a single renamed engine cannot quietly
# stop being guarded.
set(PHOSPHOR_ENGINE_TARGETS
    PhosphorZones
    PhosphorSnapEngine
    PhosphorTileEngine
    PhosphorScrollEngine
    PhosphorEngine
    PhosphorWorkspaces
    PhosphorWorkspacesPlasma
    PhosphorPlacement
)

# ─── Engine QML firewall ─────────────────────────────────────────────────────
#
# Engine libraries hold authoritative placement/snap/zone state and must
# stay free of the QML/JS engine. The moment one of them links Qt6::Qml or
# Qt6::Quick, the cost model changes: every QObject in the engine gets a
# JS scope, bindings cross language boundaries, and ownership becomes
# implicit through QQmlEngine. That's the trajectory Noctalia couldn't
# escape, their engines and config were both QML, so multi-day uptime
# leaks (FD exhaustion, GC pauses, JSValue retention across reloads) had
# nowhere to be fixed without a full rewrite.
#
# Refuse to configure if any engine target reaches Qml/Quick anywhere in its
# link closure: its own direct links (PUBLIC and PRIVATE), then the
# INTERFACE_LINK_LIBRARIES of every target those name, recursively, through
# in-tree libraries and imported Qt targets alike. A check over the direct
# lists alone let PhosphorRegistry's PUBLIC Qt6::Qml reach every engine
# through PhosphorEngine -> PhosphorLayoutApi -> PhosphorRegistry, so the
# engines compiled with QT_QML_LIB while this guard reported them clean. The
# failure names the whole chain, so the offending edge is visible.
#
# Every entry must also match a real target, so the firewall can't
# silently degrade if PHOSPHOR_ENGINE_TARGETS goes stale (e.g. an engine
# lib gets renamed but the list doesn't follow). An unmatched entry would
# let a future Qml link into that engine through unnoticed, which is
# exactly the failure mode the rationale above is designed to prevent.

# Strip the link-list wrappers that carry a target name through to the link
# step, and answer the bare item. Directory-scope markers (`::@...`, recorded
# when target_link_libraries runs in a directory other than the target's) and
# install-only entries answer empty, since neither names a build-tree target.
function(_phosphor_engine_link_item _raw _out)
    set(_item "${_raw}")
    if(_item MATCHES "^::@" OR _item MATCHES "^\\$<INSTALL_INTERFACE:")
        set(${_out} "" PARENT_SCOPE)
        return()
    endif()
    while(_item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):(.*)>$")
        set(_item "${CMAKE_MATCH_2}")
    endwhile()
    if(TARGET "${_item}")
        get_target_property(_aliased "${_item}" ALIASED_TARGET)
        if(_aliased)
            set(_item "${_aliased}")
        endif()
    endif()
    set(${_out} "${_item}" PARENT_SCOPE)
endfunction()

# Breadth-first walk of <engine>'s link closure. Sets <out> to the chain
# "engine -> ... -> Qt6::Qml" for the first Qml/Quick target reached, or to
# empty when the closure is clean. Each target is visited once, so a diamond in
# the graph costs nothing extra and a cycle cannot loop.
function(_phosphor_engine_qml_chain _engine _out)
    set(_queue "")
    set(_seen "${_engine}")
    get_target_property(_direct ${_engine} LINK_LIBRARIES)
    if(NOT _direct)
        set(_direct "")
    endif()
    get_target_property(_iface ${_engine} INTERFACE_LINK_LIBRARIES)
    if(_iface)
        list(APPEND _direct ${_iface})
    endif()
    foreach(_raw IN LISTS _direct)
        _phosphor_engine_link_item("${_raw}" _item)
        if(_item STREQUAL "" OR _item IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_item}")
        list(APPEND _queue "${_item}")
        string(MAKE_C_IDENTIFIER "${_item}" _key)
        set(_p_via_${_key} "${_engine}")
    endforeach()
    while(_queue)
        list(POP_FRONT _queue _node)
        if(_node MATCHES "^Qt6?::(Qml|Quick)(Private)?$")
            set(_chain "${_node}")
            set(_cursor "${_node}")
            while(NOT _cursor STREQUAL _engine)
                string(MAKE_C_IDENTIFIER "${_cursor}" _key)
                set(_cursor "${_p_via_${_key}}")
                set(_chain "${_cursor} -> ${_chain}")
            endwhile()
            set(${_out} "${_chain}" PARENT_SCOPE)
            return()
        endif()
        if(NOT TARGET "${_node}")
            continue()
        endif()
        get_target_property(_next "${_node}" INTERFACE_LINK_LIBRARIES)
        if(NOT _next)
            continue()
        endif()
        foreach(_raw IN LISTS _next)
            _phosphor_engine_link_item("${_raw}" _item)
            if(_item STREQUAL "" OR _item IN_LIST _seen)
                continue()
            endif()
            list(APPEND _seen "${_item}")
            list(APPEND _queue "${_item}")
            string(MAKE_C_IDENTIFIER "${_item}" _key)
            set(_p_via_${_key} "${_node}")
        endforeach()
    endwhile()
    set(${_out} "" PARENT_SCOPE)
endfunction()

set(_p_engine_unmatched "")
foreach(_p_engine_lib IN LISTS PHOSPHOR_ENGINE_TARGETS)
    if(NOT TARGET ${_p_engine_lib})
        list(APPEND _p_engine_unmatched ${_p_engine_lib})
        continue()
    endif()
    _phosphor_engine_qml_chain(${_p_engine_lib} _p_engine_chain)
    if(_p_engine_chain)
        message(FATAL_ERROR
            "Engine library '${_p_engine_lib}' reaches the QML engine through "
            "its link closure: ${_p_engine_chain}. Engine libs must stay "
            "QML-free, see the comment above this check in phosphor-libs' CMakeLists.txt "
            "for the rationale. Drop the Qml/Quick edge from the library that "
            "carries it, or move the engine's dependency off that library.")
    endif()
endforeach()
if(_p_engine_unmatched)
    # PER-ENTRY, not "did anything match". Every name in the list is an
    # unconditional target by this point in the file, so a name that matches
    # nothing means that ONE engine silently stopped being guarded while the
    # others still are — the realistic staleness mode, and one a
    # matched-anything check sails straight past.
    message(FATAL_ERROR
        "Engine QML firewall matched no target for: ${_p_engine_unmatched}. "
        "Those entries of PHOSPHOR_ENGINE_TARGETS guard nothing, so a Qml "
        "link into them would go unnoticed. Either the library was renamed "
        "without updating the list, the add_subdirectory order changed so it "
        "has not been declared yet at this point, or it got gated behind an "
        "option that is currently OFF. Update PHOSPHOR_ENGINE_TARGETS to "
        "match the real engine target names, or move this firewall block "
        "below the add_subdirectory calls that declare the engines.")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Top-level finish
# ═══════════════════════════════════════════════════════════════════════════════

if(PHOSPHOR_LIBS_TOP_LEVEL)
    # One precompiled header shared by every test target. An app that adds
    # this tree makes the same call itself, after its own targets.
    if(BUILD_TESTING AND ENABLE_PCH)
        include(PhosphorTestPch)
        phosphor_share_test_pch()
    endif()

    feature_summary(WHAT REQUIRED_PACKAGES_FOUND REQUIRED_PACKAGES_NOT_FOUND
                         OPTIONAL_PACKAGES_FOUND OPTIONAL_PACKAGES_NOT_FOUND
                    FATAL_ON_MISSING_REQUIRED_PACKAGES)
endif()
