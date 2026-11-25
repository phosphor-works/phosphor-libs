# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later

# The same directory option as libs/phosphor-control, which these examples
# used to inherit as its subdirectory: qt_add_qml_module's generated include
# directories can be missing when a target compiles (see that file).
set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "-Wno-missing-include-dirs")

add_subdirectory(minimal)
