#include "Version.h"

#include <Standard_Version.hxx>

namespace viki {

const char* versionString()
{
    return "0.1.0";
}

const char* occtVersionString()
{
    return OCC_VERSION_COMPLETE;
}

} // namespace viki
