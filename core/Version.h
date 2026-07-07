#pragma once

namespace viki {

// Single source of truth for the application version string.
const char* versionString();      // "0.1.0"
const char* occtVersionString();  // OCCT version we were built against

} // namespace viki
