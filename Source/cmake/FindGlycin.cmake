# Copyright (C) 2026 Igalia S.L.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.

#[=======================================================================[.rst:
FindGlycin
----------

Find libglycin headers and libraries.

Imported Targets
^^^^^^^^^^^^^^^^

``Glycin::Glycin``
  The libglycin library, if found.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables in your project:

``Glycin_FOUND``
  true if (the requested version of) libglycin is available.
``Glycin_VERSION``
  the version of libglycin.
``Glycin_LIBRARIES``
  the libraries to link against to use libglycin.
``Glycin_INCLUDE_DIRS``
  where to find the libglycin headers.
``Glycin_COMPILE_OPTIONS``
  this should be passed to target_compile_options(), if the
  target is not used for linking

#]=======================================================================]

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(PC_GLYCIN QUIET glycin-2)
    set(Glycin_COMPILE_OPTIONS ${PC_GLYCIN_CFLAGS_OTHER})
    set(Glycin_VERSION ${PC_GLYCIN_VERSION})
endif ()

find_path(Glycin_INCLUDE_DIR
    NAMES glycin.h
    HINTS ${PC_GLYCIN_INCLUDEDIR} ${PC_GLYCIN_INCLUDE_DIRS}
    PATH_SUFFIXES glycin-2
)

find_library(Glycin_LIBRARY
    NAMES ${Glycin_NAMES} glycin-2
    HINTS ${PC_GLYCIN_LIBDIR} ${PC_GLYCIN_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Glycin
    FOUND_VAR Glycin_FOUND
    REQUIRED_VARS Glycin_LIBRARY Glycin_INCLUDE_DIR
    VERSION_VAR Glycin_VERSION
)

if (Glycin_LIBRARY AND NOT TARGET Glycin::Glycin)
    add_library(Glycin::Glycin UNKNOWN IMPORTED GLOBAL)
    set_target_properties(Glycin::Glycin PROPERTIES
        IMPORTED_LOCATION "${Glycin_LIBRARY}"
        INTERFACE_COMPILE_OPTIONS "${Glycin_COMPILE_OPTIONS}"
        INTERFACE_INCLUDE_DIRECTORIES "${Glycin_INCLUDE_DIR}"
    )
endif ()

mark_as_advanced(Glycin_INCLUDE_DIR Glycin_LIBRARY)

if (Glycin_FOUND)
    set(Glycin_LIBRARIES ${Glycin_LIBRARY})
    set(Glycin_INCLUDE_DIRS ${Glycin_INCLUDE_DIR})
endif ()
