# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/root/warp/storage/warp/src/fastbit-2.0.3")
  file(MAKE_DIRECTORY "/root/warp/storage/warp/src/fastbit-2.0.3")
endif()
file(MAKE_DIRECTORY
  "/root/warp/storage/warp/src/fastbit-2.0.3-build"
  "/root/warp/storage/warp"
  "/root/warp/storage/warp/tmp"
  "/root/warp/storage/warp/src/fastbit-2.0.3-stamp"
  "/root/warp/storage/warp/src"
  "/root/warp/storage/warp/src/fastbit-2.0.3-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/warp/storage/warp/src/fastbit-2.0.3-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/warp/storage/warp/src/fastbit-2.0.3-stamp${cfgdir}") # cfgdir has leading slash
endif()
