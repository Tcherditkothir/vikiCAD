#pragma once

namespace viki {

// M0 link smoke test: builds a unit box through BRepPrimAPI and reports
// success. Proves the OCCT modeling toolkits actually link. The real solid
// operations (extrude/revolve/booleans) land in M7.
bool occtSmokeTest();

} // namespace viki
