# - Try to locate SDL2
# This module defines:
#
#  SDL2_INCLUDE_DIR
#  SDL2_LIBRARY
#  SDL2_FOUND
#

find_path(SDL2_INCLUDE_DIR NAMES SDL2/SDL.h)
find_library(SDL2_LIBRARY NAMES SDL2)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2 REQUIRED_VARS SDL2_INCLUDE_DIR SDL2_LIBRARY)

mark_as_advanced(SDL2_INCLUDE_DIR SDL2_LIBRARY)
# Provide an imported target (vcpkg-style) so SDL2::SDL2 links propagate
# both include/ and include/SDL2/ (the game includes SDL headers by bare name).
if(SDL2_FOUND AND NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 UNKNOWN IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${SDL2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR};${SDL2_INCLUDE_DIR}/SDL2")
endif()
