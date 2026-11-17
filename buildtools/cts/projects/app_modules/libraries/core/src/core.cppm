module;

#include <ctskit_core_resources.h>

export module ctskit.core;

import std;
import huxerui;

export namespace ctskit {

/// Resolves the core library's own string resource in the caller's composition scope.
std::string UseCoreName() {
  return huxerui::UseString(ctskit_core::strings::name);
}

} // namespace ctskit
