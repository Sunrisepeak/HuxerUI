// The windowless UI testing smoke, as CMake's `huxerui_ui_testing_smoke` runs it
// on the desktop: a real Application on virtual frames, resources read from the
// package HUXERUI_SMOKE_PACKAGE names (build.mcpp), a layer attached, a snapshot
// that settles. Exit 0 is the whole assertion.
#include "../../../../tests/platform/testing/desktop.cpp"
