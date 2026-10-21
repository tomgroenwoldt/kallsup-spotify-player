cmake_minimum_required(VERSION 3.16)

# Phase 0: cspot is present as a submodule but not wired in yet.
# Phase 5 removes this line and adds cspot as a real component.
set(EXCLUDE_COMPONENTS cspot)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kallsup-alarm)
