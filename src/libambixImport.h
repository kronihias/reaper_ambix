#ifndef LIBAMBIXIMPORT_H
#define LIBAMBIXIMPORT_H

#include <ambix/ambix.h>

#ifdef _WIN32
    #include <windows.h>
#endif
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <Carbon/Carbon.h>
#include <dlfcn.h>
#endif

int ImportLibAmbixFunctions();

#endif // LIBAMBIXIMPORT_H
