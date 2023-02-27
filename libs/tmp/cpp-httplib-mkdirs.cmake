# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/stole/simian/libs/src/cpp-httplib"
  "/home/stole/simian/libs/src/cpp-httplib-build"
  "/home/stole/simian/libs"
  "/home/stole/simian/libs/tmp"
  "/home/stole/simian/libs/src/cpp-httplib-stamp"
  "/home/stole/simian/libs/src"
  "/home/stole/simian/libs/src/cpp-httplib-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/stole/simian/libs/src/cpp-httplib-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/stole/simian/libs/src/cpp-httplib-stamp${cfgdir}") # cfgdir has leading slash
endif()
