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
# The install functions below read the name from here, not from the variable,
# which is only set in the scope that included this file.
set_property(GLOBAL PROPERTY PHOSPHOR_SUITE_DIRECTORY_NAME "${PHOSPHOR_SUITE_DIRECTORY_NAME}")

# ── Installing the libraries ──────────────────────────────────────────────────
#
# A library tree installs in one of two ways:
#
# - As a developer install (the default): the shared libraries in the libdir,
#   their headers, export headers and CMake packages, the Qt and QML plugins in
#   Qt's directories, and the licence notices in share/licenses/phosphor-libs.
#   This is what a library repository's own build and its CI install.
#
# - Embedded in an app: the app sets PHOSPHOR_EMBED_APP to its own name before
#   it adds the tree, and names its programs and plugins with
#   phosphor_install_embedded(FOR <target>...). Only the libraries that those
#   targets link, directly or through other libraries, are installed, with the
#   plugins, notices and data of the directories they come from, and only in
#   the app's own directories:
#
#     <libdir>/phosphor-works/<app>/                  the shared libraries
#     <libdir>/phosphor-works/<app>/plugins/<type>/   the Qt plugins
#     share/licenses/<app>/                           the licence notices
#     share/phosphor-works/<app>/                     data for the app's users
#
#   Headers, CMake packages, the development symlinks, the programs for
#   developers and the shared QML modules are not installed. Each app ships its
#   own copy of the libraries at the commit it pins, so two apps never install
#   the same file. A library finds its siblings, and a plugin the libraries,
#   through an $ORIGIN run path. phosphor_embedded_dirs() gives the app the
#   directories, for its own run paths and notices. The install rules are made
#   at the end of the app's top-level directory, when every target exists.

# Embedded, the install rules wait for the end of the app's top-level
# directory: _phosphor_embed_record notes a target or a file with the directory
# that registers it, and _phosphor_embed_finish installs what the app's
# targets use. A plugin, notice or data file goes with the libraries of its
# directory.
function(_phosphor_embed_defer)
    get_property(_deferred GLOBAL PROPERTY _PHOSPHOR_EMBED_DEFERRED)
    if(NOT _deferred)
        set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_DEFERRED TRUE)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _phosphor_embed_finish)
    endif()
endfunction()

# _phosphor_embed_record_target(<LIBRARY|PLUGIN> <target> <destination>)
function(_phosphor_embed_record_target kind target destination)
    _phosphor_embed_defer()
    set_target_properties(${target} PROPERTIES
        _PHOSPHOR_EMBED_DESTINATION "${destination}"
        _PHOSPHOR_EMBED_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(GLOBAL APPEND PROPERTY _PHOSPHOR_EMBED_${kind}S ${target})
endfunction()

# _phosphor_embed_record_file(<file> <destination> [<new name>])
function(_phosphor_embed_record_file file destination)
    _phosphor_embed_defer()
    # Absolute: the rule is made in the top-level directory.
    get_filename_component(file "${file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_property(_count GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_COUNT)
    if(NOT _count)
        set(_count 0)
    endif()
    set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_count} "${file}")
    set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_count}_DESTINATION "${destination}")
    # ARGV2 only when it was passed: unset, it reads a caller's variable.
    set(_rename "")
    if(ARGC GREATER 2)
        set(_rename "${ARGV2}")
    endif()
    set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_count}_RENAME "${_rename}")
    set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_count}_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    math(EXPR _count "${_count} + 1")
    set_property(GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_COUNT ${_count})
endfunction()

# phosphor_install_embedded(FOR <target>...)
#
# For an app that set PHOSPHOR_EMBED_APP: the programs and plugins that it
# installs. The embedded libraries that they link are installed, and no
# others. Call it where the targets are made; it can be called more than once.
function(phosphor_install_embedded)
    cmake_parse_arguments(arg "" "" "FOR" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_FOR)
        message(FATAL_ERROR "phosphor_install_embedded: FOR <target>..., got: ${ARGN}")
    endif()
    if(NOT PHOSPHOR_EMBED_APP)
        message(FATAL_ERROR "phosphor_install_embedded: set PHOSPHOR_EMBED_APP to the app's name before adding the libraries")
    endif()
    foreach(_target IN LISTS arg_FOR)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "phosphor_install_embedded: ${_target} is not a target")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY _PHOSPHOR_EMBED_ROOTS ${arg_FOR})
endfunction()

# The targets that <roots> link, directly or through other targets, the roots
# included. Imported targets (Qt, the system's) end the walk.
function(_phosphor_link_closure out)
    set(_queue ${ARGN})
    set(_seen)
    while(_queue)
        list(POP_FRONT _queue _target)
        if(NOT TARGET "${_target}")
            continue()
        endif()
        get_target_property(_aliased "${_target}" ALIASED_TARGET)
        if(_aliased)
            set(_target "${_aliased}")
        endif()
        if(_target IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_target}")
        get_target_property(_imported "${_target}" IMPORTED)
        if(_imported)
            continue()
        endif()
        get_target_property(_type "${_target}" TYPE)
        set(_items)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_links "${_target}" LINK_LIBRARIES)
            if(_links)
                list(APPEND _items ${_links})
            endif()
        endif()
        get_target_property(_links "${_target}" INTERFACE_LINK_LIBRARIES)
        if(_links)
            list(APPEND _items ${_links})
        endif()
        foreach(_item IN LISTS _items)
            # ::@(dir) marks a link made from another directory. A generator
            # expression is searched for target names: a link it might add
            # counts.
            if(_item MATCHES "^::@")
                continue()
            endif()
            if(_item MATCHES "\\$<")
                string(REGEX MATCHALL "[A-Za-z0-9_.+:-]+" _tokens "${_item}")
                foreach(_token IN LISTS _tokens)
                    if(TARGET "${_token}")
                        list(APPEND _queue "${_token}")
                    endif()
                endforeach()
            elseif(TARGET "${_item}")
                list(APPEND _queue "${_item}")
            endif()
        endforeach()
    endwhile()
    set(${out} ${_seen} PARENT_SCOPE)
endfunction()

function(_phosphor_embed_finish)
    get_property(_roots GLOBAL PROPERTY _PHOSPHOR_EMBED_ROOTS)
    if(NOT _roots)
        message(FATAL_ERROR "PHOSPHOR_EMBED_APP is \"${PHOSPHOR_EMBED_APP}\", but no phosphor_install_embedded(FOR ...) "
                            "names the programs and plugins of the app, so no Phosphor library would be installed.")
    endif()
    _phosphor_link_closure(_used ${_roots})
    get_property(_libraries GLOBAL PROPERTY _PHOSPHOR_EMBED_LIBRARYS)
    set(_directories)
    foreach(_target IN LISTS _libraries)
        if(_target IN_LIST _used)
            get_target_property(_destination ${_target} _PHOSPHOR_EMBED_DESTINATION)
            get_target_property(_directory ${_target} _PHOSPHOR_EMBED_DIRECTORY)
            install(TARGETS ${_target} LIBRARY DESTINATION ${_destination} NAMELINK_SKIP)
            list(APPEND _directories "${_directory}")
        endif()
    endforeach()
    get_property(_plugins GLOBAL PROPERTY _PHOSPHOR_EMBED_PLUGINS)
    foreach(_target IN LISTS _plugins)
        get_target_property(_directory ${_target} _PHOSPHOR_EMBED_DIRECTORY)
        if(_directory IN_LIST _directories)
            get_target_property(_destination ${_target} _PHOSPHOR_EMBED_DESTINATION)
            install(TARGETS ${_target} LIBRARY DESTINATION ${_destination})
        endif()
    endforeach()
    get_property(_count GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_COUNT)
    if(_count)
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE ${_last})
            get_property(_directory GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_i}_DIRECTORY)
            if(_directory IN_LIST _directories)
                get_property(_file GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_i})
                get_property(_destination GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_i}_DESTINATION)
                get_property(_rename GLOBAL PROPERTY _PHOSPHOR_EMBED_FILE_${_i}_RENAME)
                set(_rename_args)
                if(_rename)
                    set(_rename_args RENAME "${_rename}")
                endif()
                install(FILES "${_file}" DESTINATION "${_destination}" ${_rename_args})
            endif()
        endforeach()
    endif()
endfunction()

# The install directories: ECM's when it set them, else GNUInstallDirs', so the
# functions also work in a library configured on its own. With PHOSPHOR_EMBED_APP
# also _phosphor_embed_libdir and _phosphor_embed_licensedir.
macro(_phosphor_install_dirs)
    if(NOT DEFINED KDE_INSTALL_BINDIR OR NOT DEFINED KDE_INSTALL_LIBDIR OR NOT DEFINED KDE_INSTALL_DATADIR)
        include(GNUInstallDirs)
    endif()
    set(_phosphor_bindir "${KDE_INSTALL_BINDIR}")
    set(_phosphor_libdir "${KDE_INSTALL_LIBDIR}")
    set(_phosphor_datadir "${KDE_INSTALL_DATADIR}")
    if(NOT _phosphor_bindir)
        set(_phosphor_bindir "${CMAKE_INSTALL_BINDIR}")
    endif()
    if(NOT _phosphor_libdir)
        set(_phosphor_libdir "${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(NOT _phosphor_datadir)
        set(_phosphor_datadir "${CMAKE_INSTALL_DATADIR}")
    endif()
    set(_phosphor_embed_libdir "")
    set(_phosphor_embed_licensedir "")
    set(_phosphor_embed_datadir "")
    if(PHOSPHOR_EMBED_APP)
        get_property(_phosphor_suite GLOBAL PROPERTY PHOSPHOR_SUITE_DIRECTORY_NAME)
        set(_phosphor_embed_libdir "${_phosphor_libdir}/${_phosphor_suite}/${PHOSPHOR_EMBED_APP}")
        set(_phosphor_embed_licensedir "${_phosphor_datadir}/licenses/${PHOSPHOR_EMBED_APP}")
        set(_phosphor_embed_datadir "${_phosphor_datadir}/${_phosphor_suite}/${PHOSPHOR_EMBED_APP}")
    endif()
endmacro()

# phosphor_embedded_dirs([LIBDIR <var>] [LICENSEDIR <var>])
#
# For an app that set PHOSPHOR_EMBED_APP: the directory of the embedded
# libraries (its plugins are in plugins/ below it) and of their licence
# notices, relative to the install prefix.
function(phosphor_embedded_dirs)
    cmake_parse_arguments(arg "" "LIBDIR;LICENSEDIR" "" ${ARGN})
    if(NOT PHOSPHOR_EMBED_APP)
        message(FATAL_ERROR "phosphor_embedded_dirs: set PHOSPHOR_EMBED_APP to the app's name before adding the libraries")
    endif()
    _phosphor_install_dirs()
    if(arg_LIBDIR)
        set(${arg_LIBDIR} "${_phosphor_embed_libdir}" PARENT_SCOPE)
    endif()
    if(arg_LICENSEDIR)
        set(${arg_LICENSEDIR} "${_phosphor_embed_licensedir}" PARENT_SCOPE)
    endif()
endfunction()

# phosphor_install_targets(TARGETS <target>... [EXPORT <set>])
#
# The libraries and programs of a library. Developer install: the usual
# RUNTIME, LIBRARY and ARCHIVE destinations. Embedded: the shared libraries in
# the app's library directory with no development symlink, and no program or
# archive.
function(phosphor_install_targets)
    cmake_parse_arguments(arg "" "EXPORT" "TARGETS" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_TARGETS)
        message(FATAL_ERROR "phosphor_install_targets: TARGETS <target>... [EXPORT <set>], got: ${ARGN}")
    endif()
    _phosphor_install_dirs()
    set(_export)
    if(arg_EXPORT)
        set(_export EXPORT ${arg_EXPORT})
    endif()
    if(PHOSPHOR_EMBED_APP)
        # Only the shared libraries, once the app's targets show which it uses.
        # The export set still gets the targets, for the developer files that
        # phosphor_install_devel leaves out of the default install.
        foreach(_target IN LISTS arg_TARGETS)
            get_target_property(_type ${_target} TYPE)
            if(_type STREQUAL "SHARED_LIBRARY")
                set_target_properties(${_target} PROPERTIES INSTALL_RPATH "$ORIGIN")
                _phosphor_embed_record_target(LIBRARY ${_target} "${_phosphor_embed_libdir}")
            endif()
        endforeach()
        if(arg_EXPORT)
            install(TARGETS ${arg_TARGETS} ${_export}
                LIBRARY DESTINATION ${_phosphor_embed_libdir} EXCLUDE_FROM_ALL
                RUNTIME DESTINATION ${_phosphor_bindir} EXCLUDE_FROM_ALL
                ARCHIVE DESTINATION ${_phosphor_libdir} EXCLUDE_FROM_ALL)
        endif()
    else()
        install(TARGETS ${arg_TARGETS} ${_export}
            RUNTIME DESTINATION ${_phosphor_bindir}
            LIBRARY DESTINATION ${_phosphor_libdir}
            ARCHIVE DESTINATION ${_phosphor_libdir})
    endif()
endfunction()

# phosphor_install_devel(<install() arguments>)
#
# A file only a developer of another program needs: headers, export headers,
# CMake packages and export sets, shared QML modules, protocol XML, developer
# programs. The arguments are install()'s own. Embedded, the rule is left out of
# the default install.
function(phosphor_install_devel)
    if(NOT PHOSPHOR_EMBED_APP)
        install(${ARGN})
        return()
    endif()
    # EXCLUDE_FROM_ALL right after each DESTINATION <dir>: a TARGETS rule takes
    # it per artifact kind, and a DIRECTORY rule refuses it after a PATTERN.
    set(_args)
    set(_after_destination FALSE)
    foreach(_arg IN LISTS ARGN)
        list(APPEND _args "${_arg}")
        if(_after_destination)
            list(APPEND _args EXCLUDE_FROM_ALL)
            set(_after_destination FALSE)
        elseif(_arg STREQUAL "DESTINATION")
            set(_after_destination TRUE)
        endif()
    endforeach()
    install(${_args})
endfunction()

# phosphor_install_qt_plugin(<target> <plugin type>)
#
# A Qt plugin a library ships, such as a Wayland shell integration. Developer
# install: in Qt's plugin directory. Embedded: in plugins/<plugin type> below
# the app's library directory, where it finds the libraries two levels up and
# PhosphorWayland::bundledPluginDirectory() finds it.
function(phosphor_install_qt_plugin target type)
    _phosphor_install_dirs()
    if(PHOSPHOR_EMBED_APP)
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN/../..")
        _phosphor_embed_record_target(PLUGIN ${target} "${_phosphor_embed_libdir}/plugins/${type}")
    else()
        install(TARGETS ${target} LIBRARY DESTINATION ${QT6_INSTALL_PLUGINS}/${type})
    endif()
endfunction()

# phosphor_install_notice(<file> [RENAME <name>])
#
# A licence notice of code compiled into a library. Developer install: in
# share/licenses/phosphor-libs. Embedded: in share/licenses/<app>.
function(phosphor_install_notice file)
    cmake_parse_arguments(arg "" "RENAME" "" ${ARGN})
    _phosphor_install_dirs()
    set(_rename)
    if(arg_RENAME)
        set(_rename RENAME ${arg_RENAME})
    endif()
    if(PHOSPHOR_EMBED_APP)
        _phosphor_embed_record_file("${file}" "${_phosphor_embed_licensedir}" ${arg_RENAME})
    else()
        install(FILES "${file}" DESTINATION ${_phosphor_datadir}/licenses/phosphor-libs ${_rename})
    endif()
endfunction()

# phosphor_install_app_data(<file>... SUBDIR <dir>)
#
# A file that the app's users read, such as the type definitions for scripts
# they write. Developer install: in share/<dir>. Embedded: in
# share/phosphor-works/<app>, with the libraries of its directory.
function(phosphor_install_app_data)
    cmake_parse_arguments(arg "" "SUBDIR" "" ${ARGN})
    if(NOT arg_SUBDIR OR NOT arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "phosphor_install_app_data: <file>... SUBDIR <dir>, got: ${ARGN}")
    endif()
    _phosphor_install_dirs()
    if(PHOSPHOR_EMBED_APP)
        foreach(_file IN LISTS arg_UNPARSED_ARGUMENTS)
            _phosphor_embed_record_file("${_file}" "${_phosphor_embed_datadir}")
        endforeach()
    else()
        install(FILES ${arg_UNPARSED_ARGUMENTS} DESTINATION ${_phosphor_datadir}/${arg_SUBDIR})
    endif()
endfunction()
