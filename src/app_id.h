#pragma once

// CMake supplies this definition for CI development builds. Keep the fallback
// aligned with packaging/appinfo.json for direct release builds.
#ifndef CHIAKI_APP_ID
#define CHIAKI_APP_ID "org.homebrew.chiaki.fork"
#endif

#define CHIAKI_APP_DIR \
    "/media/developer/apps/usr/palm/applications/" CHIAKI_APP_ID
