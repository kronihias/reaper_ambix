#include "reaper_plugin.h"

#include "libambixImport.h"
#ifdef _WIN32
    #include <windows.h>
#endif
#include "wrapperclass.h"

#define IMPAPI(x) if (!((*((void **)&(x)) = (void *)rec->GetFunc(#x)))) impapierrcnt++;

REAPER_PLUGIN_HINSTANCE g_hInst=0;

PCM_source *(*PCM_Source_CreateFromSimple)(ISimpleMediaDecoder *dec, const char *fn);
void (*format_timestr)(double tpos, char *buf, int buflen);
void (*update_disk_counters)(int read, int write);
void (*ShowConsoleMsg)(const char* msg);

REAPER_PeakBuild_Interface *(*PeakBuild_Create)(PCM_source *src, const char *fn, int srate, int nch);
void (*GetPreferredDiskWriteMode)(int *mode, int nb[2], int *bs);
const char *(*get_ini_file)();

// output diagnostics messages using Reaper's currently available console
#define REAPER_DEBUG_OUTPUT_TRACING

PCM_source *CreateFromType(const char *type, int priority)
{
    // printf("create from type \n");
    if (priority > 4) // let other plug-ins override "RAW" if they want. or whatever in this case...
    {
        if (!strcmp(type,"AMBIX"))
            return PCM_Source_CreateFromSimple(new LSFW_SimpleMediaDecoder,NULL);
    }

    return NULL;
}

PCM_source *CreateFromFile(const char *filename, int priority)
{
    // printf("create from file \n");
    int lfn=strlen(filename);
    
    // is it .ambix? -> take it
    if (priority > 4 && lfn>4 && !stricmp(filename+lfn-6,".ambix"))
    {
        PCM_source *w=PCM_Source_CreateFromSimple(new LSFW_SimpleMediaDecoder,filename);
        if (w->IsAvailable() && priority >= 6) return w;
            delete w;
    }
    
    return NULL;
}

// this is used for UI only, not so muc
const char *EnumFileExtensions(int i, const char **descptr) // call increasing i until returns a string, if descptr's output is NULL, use last description
{
    if (i == 0)
    {
        if (descptr) *descptr = "ambiX Audio Files";
        return "AMBIX";
    }
    if (descptr) *descptr=NULL;
    return NULL;
}


pcmsrc_register_t myRegStruct={CreateFromType,CreateFromFile,EnumFileExtensions};


extern pcmsink_register_t mySinkRegStruct; // from pcmsink_ambix.cpp
// extern pcmsink_register_ext_t mySinkRegStruct; // from pcmsink_ambix.cpp

const char *(*GetExePath)();

extern "C"
{

REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t *rec)
{
    // ShowConsoleMsg("Entering entrypoint function in libsndfile wrapper extension");
    
    g_hInst=hInstance;
    //g_EntrypointCallCount++;
    if (rec)
    {
        if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc)
            return 0;
        int impapierrcnt=0;
        IMPAPI(GetExePath);
        IMPAPI(PCM_Source_CreateFromSimple);
        IMPAPI(format_timestr);
        IMPAPI(update_disk_counters);
        IMPAPI(ShowConsoleMsg);
        IMPAPI(get_ini_file);
        IMPAPI(GetPreferredDiskWriteMode);
        IMPAPI(PeakBuild_Create);
        if (impapierrcnt)
        {
            // ShowConsoleMsg("Errors importing Reaper API functions, aborting loading");
            return 0;
        }
        
#ifdef REAPER_DEBUG_OUTPUT_TRACING
        //ShowConsoleMsg(lsfpath);
#endif
        if (ImportLibAmbixFunctions())
        {
            // loading libamibx/resolving functions failed
            printf("Failed to load libambix!\n");
            return 0;
        }
        rec->Register("pcmsrc",&myRegStruct);
        rec->Register("pcmsink",&mySinkRegStruct);
        /*
        if (!rec->Register("pcmsink_ext",&mySinkRegStruct))
        {
          printf("Failed to register extended Sink\n");
          
          if (rec->Register("pcmsink",&mySinkRegStruct))
            printf("Registered normal Sink!\n");
          
        }
        */
      
      
        return 1;
    }
    return 0;
}
}
