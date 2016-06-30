#include "libambixImport.h"


// load library at runtime and import the functionality

ambix_t* (*ptr_ambix_open) (const char *path, const ambix_filemode_t mode, ambix_info_t *ambixinfo) ;
ambix_err_t (*ptr_ambix_close) (ambix_t *ambix) ;
int64_t (*ptr_ambix_seek) (ambix_t *ambix, int64_t frames, int whence) ;
int64_t (*ptr_ambix_readf_float64) (ambix_t *ambix, float64_t *ambidata, float64_t *otherdata, int64_t frames) ;
int64_t (*ptr_ambix_writef_float32) (ambix_t *ambix, const float32_t *ambidata, const float32_t *otherdata, int64_t frames) ;
int64_t (*ptr_ambix_writef_float64) (ambix_t *ambix, const float64_t *ambidata, const float64_t *otherdata, int64_t frames) ;

const ambix_matrix_t* (*ptr_ambix_get_adaptormatrix) (ambix_t *ambix) ;
ambix_err_t (*ptr_ambix_set_adaptormatrix) (ambix_t *ambix, const ambix_matrix_t *matrix) ;
ambix_matrix_t* (*ptr_ambix_matrix_create) (void) ;
void (*ptr_ambix_matrix_destroy) (ambix_matrix_t *mtx) ;
ambix_matrix_t* (*ptr_ambix_matrix_init) (uint32_t rows, uint32_t cols, ambix_matrix_t *mtx) ;
void (*ptr_ambix_matrix_deinit) (ambix_matrix_t *mtx) ;

ambix_err_t 	(*ptr_ambix_matrix_multiply_float64) (float64_t *dest, const ambix_matrix_t *mtx, const float64_t *source, int64_t frames);

ambix_matrix_t * 	(*ptr_ambix_matrix_fill) (ambix_matrix_t *matrix, ambix_matrixtype_t type);
ambix_err_t 	(*ptr_ambix_matrix_fill_data) (ambix_matrix_t *mtx, const float32_t *data);

ambix_matrix_t* (*ptr_ambix_matrix_pinv)(const ambix_matrix_t*matrix, ambix_matrix_t*pinv);

struct SNDFILE_tag (*ptr_ambix_get_sndfile) (ambix_t *ambix) ;

uint32_t (*ptr_ambix_order2channels) (uint32_t order) ;
int32_t (*ptr_ambix_channels2order) (uint32_t channels) ;
int (*ptr_ambix_is_fullset) (uint32_t channels) ;


#ifdef _WIN32
HINSTANCE g_hLibAmbix=0;
#endif

int ImportLibAmbixFunctions()
{
    int errcnt=0;
    
#ifdef _WIN32
    if (g_hLibAmbix)
        return 0;
    g_hLibAmbix=LoadLibraryA("libambix.dll");

    if (!g_hLibAmbix)
        errcnt++;
    if (g_hLibAmbix)
    {
        //OutputDebugStringA("libambix dll loaded! now loading functions...");
        *((void **)&ptr_ambix_open)=(void*)GetProcAddress(g_hLibAmbix,"ambix_open");
        *((void **)&ptr_ambix_close)=(void*)GetProcAddress(g_hLibAmbix,"ambix_close");
        *((void **)&ptr_ambix_seek)=(void*)GetProcAddress(g_hLibAmbix,"ambix_seek");
        *((void **)&ptr_ambix_readf_float64)=(void*)GetProcAddress(g_hLibAmbix,"ambix_readf_float64");
        *((void **)&ptr_ambix_writef_float32)=(void*)GetProcAddress(g_hLibAmbix,"ambix_writef_float32");
        *((void **)&ptr_ambix_writef_float64)=(void*)GetProcAddress(g_hLibAmbix,"ambix_writef_float64");
        *((void **)&ptr_ambix_get_adaptormatrix)=(void*)GetProcAddress(g_hLibAmbix,"ambix_get_adaptormatrix");
        *((void **)&ptr_ambix_set_adaptormatrix)=(void*)GetProcAddress(g_hLibAmbix,"ambix_set_adaptormatrix");
        *((void **)&ptr_ambix_matrix_create)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_create");
        *((void **)&ptr_ambix_matrix_destroy)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_destroy");
        *((void **)&ptr_ambix_matrix_init)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_init");
        *((void **)&ptr_ambix_matrix_deinit)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_deinit");
        *((void **)&ptr_ambix_order2channels)=(void*)GetProcAddress(g_hLibAmbix,"ambix_order2channels");
        *((void **)&ptr_ambix_channels2order)=(void*)GetProcAddress(g_hLibAmbix,"ambix_channels2order");
        *((void **)&ptr_ambix_is_fullset)=(void*)GetProcAddress(g_hLibAmbix,"ambix_is_fullset");
        *((void **)&ptr_ambix_matrix_fill)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_fill");
        *((void **)&ptr_ambix_matrix_multiply_float64)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_multiply_float64");
        *((void **)&ptr_ambix_matrix_fill_data)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_fill_data");
        *((void **)&ptr_ambix_matrix_pinv)=(void*)GetProcAddress(g_hLibAmbix,"ambix_matrix_pinv");
        *((void **)&ptr_ambix_get_sndfile)=(void*)GetProcAddress(g_hLibAmbix,"ambix_get_sndfile");
      
        // *((void **)&ptr_sf_version_string)=(void*)GetProcAddress(g_hLibAmbix,"sf_version_string");
        // if (!ptr_sf_version_string) errcnt++;
        //OutputDebugStringA("libsndfile functions loaded!");
    } //else OutputDebugStringA("libsndfile DLL not loaded!");
    
#elif defined(__APPLE__)
    static int a;
    static void *dll;
    if (!dll&&!a)
    {
        a=1;
        if (!dll) dll=dlopen("libambix.dylib",RTLD_LAZY);
        if (!dll) dll=dlopen("/usr/local/lib/libambix.dylib",RTLD_LAZY);
        if (!dll) dll=dlopen("/usr/lib/libambix.dylib",RTLD_LAZY);
        
        if (!dll)
        {
            CFBundleRef bund=CFBundleGetMainBundle();
            if (bund)
            {
                CFURLRef url=CFBundleCopyBundleURL(bund);
                if (url)
                {
                    char buf[8192];
                    if (CFURLGetFileSystemRepresentation(url,true,(UInt8*)buf,sizeof(buf)-128))
                    {
                        char *p=buf;
                        while (*p) p++;
                        while (p>=buf && *p != '/') p--;
                        if (p>=buf)
                        {
                            strcat(buf,"/Contents/Plugins/libambix.dylib");
                            if (!dll) dll=dlopen(buf,RTLD_LAZY);
                            
                            if (!dll)
                            {
                                strcpy(p,"/libambix.dylib");
                                dll=dlopen(buf,RTLD_LAZY);
                            }
                            if (!dll)
                            {
                                strcpy(p,"/Plugins/libambix.dylib");
                                if (!dll) dll=dlopen(buf,RTLD_LAZY);
                            }
                        }          
                    }
                    CFRelease(url);
                }
            }
        }
        
        if (dll)
        {
            *(void **)(&ptr_ambix_open) = dlsym(dll, "ambix_open");
            *(void **)(&ptr_ambix_close) = dlsym(dll, "ambix_close");
            *(void **)(&ptr_ambix_seek) = dlsym(dll, "ambix_seek");
            *(void **)(&ptr_ambix_readf_float64) = dlsym(dll, "ambix_readf_float64");
            *(void **)(&ptr_ambix_writef_float32) = dlsym(dll, "ambix_writef_float32");
            *(void **)(&ptr_ambix_writef_float64) = dlsym(dll, "ambix_writef_float64");
            *(void **)(&ptr_ambix_get_adaptormatrix) = dlsym(dll, "ambix_get_adaptormatrix");
            *(void **)(&ptr_ambix_set_adaptormatrix) = dlsym(dll, "ambix_set_adaptormatrix");
            *(void **)(&ptr_ambix_matrix_create) = dlsym(dll, "ambix_matrix_create");
            *(void **)(&ptr_ambix_matrix_destroy) = dlsym(dll, "ambix_matrix_destroy");
            *(void **)(&ptr_ambix_matrix_init) = dlsym(dll, "ambix_matrix_init");
            *(void **)(&ptr_ambix_matrix_deinit) = dlsym(dll, "ambix_matrix_deinit");
            *(void **)(&ptr_ambix_order2channels) = dlsym(dll, "ambix_order2channels");
            *(void **)(&ptr_ambix_channels2order) = dlsym(dll, "ambix_channels2order");
            *(void **)(&ptr_ambix_is_fullset) = dlsym(dll, "ambix_is_fullset");
            *(void **)(&ptr_ambix_matrix_fill) = dlsym(dll, "ambix_matrix_fill");
            *(void **)(&ptr_ambix_matrix_multiply_float64) = dlsym(dll, "ambix_matrix_multiply_float64");
            *(void **)(&ptr_ambix_matrix_fill_data) = dlsym(dll, "ambix_matrix_fill_data");
            *(void **)(&ptr_ambix_matrix_pinv) = dlsym(dll, "ambix_matrix_pinv");
            *(void **)(&ptr_ambix_get_sndfile) = dlsym(dll, "ambix_get_sndfile");
          
        }
        if (!dll)
			errcnt++;
    }
#endif
    return errcnt;
}
