# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The folder every Phosphor app's files live under in each XDG root, defined
# here once. It has no dependency, so a build without Qt (aglow-mux's) can
# name its install destinations too. phosphor-paths includes it, compiles its
# C++ API from it and exports it from PhosphorPathsConfig.cmake.
#
# Include it at the top scope of the project that uses the name:
#   include("${PHOSPHOR_LIBS_CMAKE_DIR}/PhosphorSuite.cmake")
#   install(... DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/<tree>)

set(PHOSPHOR_SUITE_DIRECTORY_NAME "phosphor-works")
