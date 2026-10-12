#pragma once

// POSIX strings.h для MSVC.

#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
