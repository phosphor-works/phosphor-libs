# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The phosphor-libs package: every library, its headers and its CMake package
# files. `withTests` builds and runs the test suite in the sandbox, which is
# what the flake's `checks.<system>.tests` does.
{
  lib,
  stdenv,
  fetchurl,
  linkFarm,
  cmake,
  ninja,
  pkg-config,
  qt6,
  kdePackages,
  wayland,
  wayland-scanner,
  vulkan-headers,
  dbus,
  src,
  version,
  withTests ? false,
}:

let
  # Luau and valijson, pinned (version, URL, SHA-256) in
  # cmake/PhosphorDependencies.cmake, and with the tests the surface packs the
  # tests load (PHOSPHOR_TEST_DEPS there). The sandbox has no network, so they
  # are fetched here as fixed-output derivations and handed to CMake as one
  # directory (PHOSPHOR_DEPS_ARCHIVES), each under the FILE name its pin gives
  # it. CMake checks each against the pin's hash again before unpacking it.
  # fetchurl, not fetchFromGitHub: the pin hashes the archive file itself.
  depsArchives = linkFarm "phosphor-libs-deps-archives" (
    [
      {
        name = "luau-0.745.tar.gz";
        path = fetchurl {
          url = "https://github.com/luau-lang/luau/archive/refs/tags/0.745.tar.gz";
          # sha256 53c22773bace6cabbabaead576fbc08d11a0317fce11a3dbf9d027dda6849248
          hash = "sha256-U8Inc7rObKu6uurVdvvAjRGgMX/OEaPb+dAn3aaEkkg=";
        };
      }
      {
        name = "valijson-1.1.3.tar.gz";
        path = fetchurl {
          url = "https://github.com/tristanpenman/valijson/archive/refs/tags/v1.1.3.tar.gz";
          # sha256 887b53b1a924f6fe0b35fa3bbc9bbbe5ae8c72097b7f0f7c17b45f4cfa646029
          hash = "sha256-iHtTsakk9v4LNfo7vJu75a6Mcgl7fw98F7RfTPpkYCk=";
        };
      }
    ]
    ++ lib.optionals withTests [
      {
        name = "phosphor-surface-packs-e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz";
        path = fetchurl {
          url = "https://github.com/phosphor-works/phosphor-surface-packs/archive/e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz";
          # sha256 fbbf0b9849d83396029bc4e310c3dcac49c08758e73d560538888e685f0a5e30
          hash = "sha256-+78LmEnYM5YCm8TjEMPcrEnAh1jnPVYFOIiOaF8KXjA=";
        };
      }
    ]
  );
in
stdenv.mkDerivation {
  pname = if withTests then "phosphor-libs-tests" else "phosphor-libs";
  inherit version src;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    kdePackages.extra-cmake-modules
    wayland-scanner
    # Its environment hook collects every Qt input's plugin and QML directories
    # (QtQuick, Controls, Kirigami, each in its own store path) into
    # qtWrapperArgs, which preCheck turns into the test environment.
    qt6.wrapQtAppsHook
  ];

  # Libraries and their QML modules, no applications to wrap.
  dontWrapQtApps = true;

  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    qt6.qtshadertools
    qt6.qtsvg
    qt6.qtwayland
    wayland
    # Qt declares QVulkanInstance only with the Vulkan headers present, and
    # phosphor-surfaces uses it when Qt has Vulkan, as nixpkgs' Qt does.
    vulkan-headers
    # Optional lookups, so the package builds the code they switch on:
    # KGlobalAccel (phosphor-shortcuts) and PlasmaActivities
    # (phosphor-workspaces). Kirigami is used by phosphor-control's tests.
    kdePackages.kglobalaccel
    kdePackages.plasma-activities
    kdePackages.kirigami
  ];

  # dbus-run-session gives each test a private session bus
  # (cmake/PhosphorTestIsolation.cmake).
  nativeCheckInputs = [ dbus ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_TESTING=${if withTests then "ON" else "OFF"}"
    "-DPHOSPHOR_DEPS_ARCHIVES=${depsArchives}"
    "-DKDE_INSTALL_QTPLUGINDIR=${placeholder "out"}/${qt6.qtbase.qtPluginPrefix}"
    "-DKDE_INSTALL_QMLDIR=${placeholder "out"}/${qt6.qtbase.qtQmlPrefix}"
  ];

  doCheck = withTests;
  # The tests run uninstalled, from the build tree, so nothing wraps them.
  # nixpkgs' Qt finds QML modules through NIXPKGS_QT6_QML_IMPORT_PATH and
  # plugins through QT_PLUGIN_PATH, and wrapQtAppsHook puts both only into the
  # wrapper arguments of installed programs. Export the same entries here, so
  # the tests that load QtQuick, Controls or Kirigami find them. Then the
  # offscreen platform for the GUI tests, and a writable HOME for the ones the
  # XDG sandbox does not cover.
  preCheck = ''
    for ((i = 0; i + 3 < ''${#qtWrapperArgs[@]}; i++)); do
      if [[ "''${qtWrapperArgs[i]}" == --prefix ]]; then
        case "''${qtWrapperArgs[i + 1]}" in
          NIXPKGS_QT6_QML_IMPORT_PATH | QT_PLUGIN_PATH)
            addToSearchPath "''${qtWrapperArgs[i + 1]}" "''${qtWrapperArgs[i + 3]}"
            ;;
        esac
      fi
    done
    export NIXPKGS_QT6_QML_IMPORT_PATH QT_PLUGIN_PATH
    export QT_QPA_PLATFORM=offscreen
    export HOME="$(mktemp -d)"
    export CTEST_OUTPUT_ON_FAILURE=1
  '';

  meta = {
    description = "LGPL Qt 6 and Wayland libraries for window placement, rendering and layer-shell surfaces";
    homepage = "https://github.com/phosphor-works/phosphor-libs";
    license = with lib.licenses; [
      lgpl21Plus
      mit
      bsd2
    ];
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
