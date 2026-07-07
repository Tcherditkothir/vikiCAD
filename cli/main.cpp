#include <cstdio>
#include <cstring>

#include <QCoreApplication>

#include "Version.h"
#include "solid/OcctOps.h"

// Headless CLI for VikiCAD. M0 scope: --version and an OCCT link smoke check.
// The verb surface (open/run/query/export/import/--connect) lands from M1 on.
// Output is JSON on stdout so agents can parse it from day one.

static int printVersion()
{
    std::printf("{\"ok\":true,\"result\":{\"app\":\"vikicad-cli\",\"version\":\"%s\","
                "\"occt\":\"%s\",\"occtSmoke\":%s}}\n",
                viki::versionString(), viki::occtVersionString(),
                viki::occtSmokeTest() ? "true" : "false");
    return 0;
}

static int printUsage(FILE* out)
{
    std::fprintf(out,
                 "usage: vikicad-cli <verb> [options]\n"
                 "verbs:\n"
                 "  --version        print version info as JSON\n"
                 "  --help           this message\n"
                 "(open/run/query/export/import arrive with milestone M1+)\n");
    return out == stdout ? 0 : 2;
}

int main(int argc, char** argv)
{
    // QCoreApplication is enough for M0; commands needing font metrics will
    // switch this to QGuiApplication with the offscreen platform.
    QCoreApplication app(argc, argv);

    if (argc < 2)
        return printUsage(stderr);
    if (std::strcmp(argv[1], "--version") == 0)
        return printVersion();
    if (std::strcmp(argv[1], "--help") == 0)
        return printUsage(stdout);

    std::fprintf(stderr, "{\"ok\":false,\"error\":{\"code\":\"E_UNKNOWN_VERB\","
                         "\"message\":\"unknown verb: %s\"}}\n", argv[1]);
    return 2;
}
