#ifdef _WIN32
#include <windows.h>
#else
#include "swell/swell.h"
#endif
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

#include "reaper_plugin.h"

// #include "LineParse.h"
#include "wdlstring.h"

// #include "filewrite.h"

#include <ambix/ambix.h>
#include "libambixImport.h"

extern ambix_t* (*ptr_ambix_open) (const char *path, const ambix_filemode_t mode, ambix_info_t *ambixinfo) ;
extern ambix_err_t (*ptr_ambix_close) (ambix_t *ambix) ;
extern int64_t (*ptr_ambix_writef_float32) (ambix_t *ambix, const float32_t *ambidata, const float32_t *otherdata, int64_t frames) ;

extern void (*format_timestr)(double tpos, char *buf, int buflen);
extern REAPER_PeakBuild_Interface *(*PeakBuild_Create)(PCM_source *src, const char *fn, int srate, int nch);
extern void (*update_disk_counters)(int read, int write);
extern const char *(*get_ini_file)();
extern HWND g_main_hwnd;


extern HINSTANCE g_hInst;
#define WIN32_FILE_IO


class PCM_sink_ambix : public PCM_sink
{
  public:
    static HWND showConfig(void *cfgdata, int cfgdata_l, HWND parent);

    PCM_sink_ambix(const char *fn, void *cfgdata, int cfgdata_l, int nch, int srate, bool buildpeaks)
    {
        printf("creating sink...\n");
        m_peakbuild=0;
        

        if (cfgdata_l >= 32 && *((int *)cfgdata) == REAPER_FOURCC('a','m','b','x'))
        {
          // m_fileformat=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[0]);
            /*
          m_stereomode=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[1]);
          m_quality=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[2]);
          m_vbrmethod=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[3]);
          m_vbrq=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[4]);
          m_vbrmax=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[5]);
          m_abr=REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[6]);
             */
        }

        m_nch=nch;
        m_srate=srate;
        m_lensamples=0;
        m_filesize=0;
        m_fn.Set(fn);
        
        m_ambichannels = ((int)(sqrt(nch)))*((int)(sqrt(nch)));
        m_xtrachannels = nch-m_ambichannels;
        
        if (m_xtrachannels == 0)
        {
            m_fileformat = AMBIX_BASIC;
        } else {
            m_fileformat = AMBIX_EXTENDED;
        }
        
        memset(&m_ainfo, 0, sizeof(m_ainfo));
        
        m_ainfo.fileformat=m_fileformat;
        
        m_ainfo.ambichannels=m_ambichannels;//ambichannels;
        m_ainfo.extrachannels=m_xtrachannels;// xtrachannels;
        
        m_ainfo.samplerate=m_srate;
	    m_ainfo.sampleformat=AMBIX_SAMPLEFORMAT_PCM24;
        
        //if (m_fh)
        //    ptr_ambix_close(m_fh);
        
        m_fh=ptr_ambix_open(m_fn.Get(), AMBIX_WRITE, &m_ainfo);
        
        if (buildpeaks && m_fh)
        {
          m_peakbuild=PeakBuild_Create(NULL,fn,m_srate,m_nch);
        }

    }

    bool IsOpen()
    {
      return m_fh;
    }

    ~PCM_sink_ambix()
    {
      if (IsOpen())
      {

      }
      ptr_ambix_close(m_fh);
      
      m_fh=0;

      delete m_peakbuild;
      m_peakbuild=0;
    }

    const char *GetFileName() { return m_fn.Get(); }
    int GetNumChannels() { return m_nch; } // return number of channels
    double GetLength() { return m_lensamples / (double) m_srate; } // length in seconds, so far
    INT64 GetFileSize()
    {
      return m_filesize;
    }
    int GetLastSecondPeaks(int sz, ReaSample *buf)
    {
      if (m_peakbuild)
        return m_peakbuild->GetLastSecondPeaks(sz,buf);
      return 0;
    }
    void GetPeakInfo(PCM_source_peaktransfer_t *block)
    {
      if (m_peakbuild) m_peakbuild->GetPeakInfo(block);
      else block->peaks_out=0;
    }

    void GetOutputInfoString(char *buf, int buflen)
    {
      char tmp[512];
      sprintf(tmp,"ambiX %dHz %dch",m_srate,
          m_nch);
      strncpy(buf,tmp,buflen);
    }

    void WriteMIDI(MIDI_eventlist *events, int len, double samplerate) { }
    void WriteDoubles(ReaSample **samples, int len, int nch, int offset, int spacing)
    {
        // printf("i am writing %d samples, my samples are %d, spacing %d, offset %d \n", len, sizeof(ReaSample), spacing, offset);
      if (m_peakbuild)
        m_peakbuild->ProcessSamples(samples,len,nch,offset,spacing);
        
        float32_t* ambibuf = NULL;
        float32_t* xtrabuf = NULL;
        
        ambibuf = (float32_t*)calloc(len*m_ambichannels, sizeof(float32_t));
        xtrabuf = (float32_t*)calloc(len*m_xtrachannels, sizeof(float32_t));
        
        // printf("m_ambichannels %d, m_xtrachannels %d\n", m_ambichannels, m_xtrachannels);
        
        float32_t* ambibuf_temp = ambibuf;
        float32_t* xtrabuf_temp = xtrabuf;
        
        int len_temp = len;
        
        // assume the buffer is in one row... is this save?!
        ReaSample* smpl_temp = samples[0];
        
        while (len_temp-- > 0) {
            
            for (int i=0; i < m_ambichannels; i++)
            {
                *ambibuf_temp++ = (float32_t) (*smpl_temp++);
            }
            
            for (int i=0; i < m_xtrachannels; i++)
            {
                *xtrabuf_temp++ = (float32_t) (*smpl_temp++);
            }
            
        }
        
        int sysrtn = ptr_ambix_writef_float32(m_fh,
                                      ambibuf,
                                      xtrabuf,
                                      len);
        
        free(ambibuf);free(xtrabuf);
      
    }

    int Extended(int call, void *parm1, void *parm2, void *parm3) 
    {
      return 0;
    }


 private:
    ambix_t *m_fh;
    ambix_info_t m_ainfo;
    ambix_fileformat_t m_fileformat;
    
    int m_ambichannels, m_xtrachannels;
    
    int m_bitrate;
    int m_vbrq, m_abr, m_vbrmax, m_quality, m_stereomode, m_vbrmethod;
    
    
    WDL_TypedBuf<float> m_inbuf;
    int m_nch,m_srate;
    INT64 m_filesize;
    INT64 m_lensamples;
    WDL_String m_fn;
    
    REAPER_PeakBuild_Interface *m_peakbuild;
};

static unsigned int GetFmt(char **desc) 
{
  if (desc) *desc="ambiX (Ambisonics eXchangeable)";
  return REAPER_FOURCC('a','m','b','x');
}

static const char *GetExtension(const void *cfg, int cfg_l)
{
  if (cfg_l >= 4 && *((int *)cfg) == REAPER_FOURCC('a','m','b','x')) return "ambix";
  return NULL;
}


static HWND ShowConfig(const void *cfg, int cfg_l, HWND parent)
{
    printf("3\n");
    return 0;
}

static PCM_sink *CreateSink(const char *filename, void *cfg, int cfg_l, int nch, int srate, bool buildpeaks)
{
  if (cfg_l >= 4 && *((int *)cfg) == REAPER_FOURCC('a','m','b','x')) 
  {
    if (ImportLibAmbixFunctions())
    {
        printf("could not load libambix\n");
        // loading libamibx/resolving functions failed
        return 0;
    }
    PCM_sink_ambix *v=new PCM_sink_ambix(filename,cfg,cfg_l,nch,srate,buildpeaks);
    if (v->IsOpen()) return v;
    delete v;
  }
  return 0;
}



pcmsink_register_t mySinkRegStruct={GetFmt,GetExtension,ShowConfig,CreateSink};

