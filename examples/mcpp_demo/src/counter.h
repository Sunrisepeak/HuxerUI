#pragma once

import huxerui;

// A stateful composable. Its DEFINITION lives in counter.cpp, which hcg
// rewrites; the entry file only calls it.
[[nodiscard]] huxerui::View Counter();
