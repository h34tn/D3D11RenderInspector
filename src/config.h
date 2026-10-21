#pragma once
#include <string>

// Shared by the proxy and standalone tests; no game-specific paths.
const std::wstring &InspectorSessionDirectory();
std::wstring InspectorMode();
