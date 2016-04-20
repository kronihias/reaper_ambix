#ifdef _WIN32
#include <windows.h>
#else
#include "swell/swell.h"
#endif
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

#include "reaper_plugin.h"

#include "wdlstring.h"

// #include "filewrite.h"

#include <ambix/ambix.h>
#include "libambixImport.h"

#include "resource.h"

// short for AMbixSInk
#define SINK_FOURCC REAPER_FOURCC('a','m','s','i')

extern ambix_t* (*ptr_ambix_open) (const char *path, const ambix_filemode_t mode, ambix_info_t *ambixinfo) ;
extern ambix_err_t (*ptr_ambix_close) (ambix_t *ambix) ;
extern int64_t (*ptr_ambix_writef_float64) (ambix_t *ambix, const float64_t *ambidata, const float64_t *otherdata, int64_t frames) ;
extern ambix_err_t (*ptr_ambix_set_adaptormatrix) (ambix_t *ambix, const ambix_matrix_t *matrix) ;
extern const ambix_matrix_t* (*ptr_ambix_get_adaptormatrix) (ambix_t *ambix) ;

extern ambix_matrix_t* (*ptr_ambix_matrix_create) (void) ;
extern void (*ptr_ambix_matrix_destroy) (ambix_matrix_t *mtx) ;
extern ambix_matrix_t* (*ptr_ambix_matrix_init) (uint32_t rows, uint32_t cols, ambix_matrix_t *mtx) ;
extern ambix_matrix_t * 	(*ptr_ambix_matrix_fill) (ambix_matrix_t *matrix, ambix_matrixtype_t type);


extern void (*format_timestr)(double tpos, char *buf, int buflen);
extern REAPER_PeakBuild_Interface *(*PeakBuild_Create)(PCM_source *src, const char *fn, int srate, int nch);
extern void (*update_disk_counters)(int read, int write);
extern const char *(*get_ini_file)();
extern HWND g_main_hwnd;


extern HINSTANCE g_hInst;
#define WIN32_FILE_IO


void post_matrix(ambix_matrix_t *matrix)
{
  /* post matrix */
  float32_t**data=matrix->data;
  uint32_t r, c;
  printf(" [%dx%d] = %p\n", matrix->rows, matrix->cols, matrix->data);
  for(r=0; r<matrix->rows; r++) {
    for(c=0; c<matrix->cols; c++) {
      printf("%08f ", data[r][c]);
    }
    printf("\n");
  }
}

class PCM_sink_ambix : public PCM_sink
{
public:
  static HWND showConfig(void *cfgdata, int cfgdata_l, HWND parent);
  
  PCM_sink_ambix(const char *fn, void *cfgdata, int cfgdata_l, int nch, int srate, bool buildpeaks)
  {
    printf("creating sink...\n");
    
    m_peakbuild=0;
    
    m_fh = 0;
    
    
    m_nch=nch;
    m_srate=srate;
    m_lensamples=0;
    m_filesize=0;
    m_fn.Set(fn);
    
    // retrieve settings
    /*
     Settings List:
     0: Format (INT) -> 0: Basic, 1: Extended
     1: Order (INT)
     2: SampleFormat (INT): 0: 16bit PCM, 1: 24 bit PCM, 2: 32 bit PCM, 3: 32 bit float, 4: 64 bit float
     3: NumExtraChannels (INT)
     4: AdaptorMatrix Filename (char*)
     */
    
    if (cfgdata_l >= 32 && *((int *)cfgdata) == SINK_FOURCC)
    {
      m_format = REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[0]);
      m_order = REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[1]);
      m_sampleformat = REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[2]);
      m_xtrachannels = REAPER_MAKELEINT(((int *)(((unsigned char *)cfgdata)+4))[3]);
      
    }
    
    m_ambichannels = (uint32_t)(m_order+1)*(m_order+1); // (N+1)^2 for basic format
    m_xtrachannels = (uint32_t)m_xtrachannels;
    
    m_dummychannels = m_nch - m_ambichannels - m_xtrachannels; // throw away additional channels!
    
    // sanity check number of input channels
    if (m_ambichannels + m_xtrachannels > m_nch)
    {
      printf("ERROR: not enough input channels! Got: %d, Need: %d\n", m_nch, m_ambichannels + m_xtrachannels);
      return;
    }
    
    /* generate matrix of ones in case we have extended format with (N+1)^2 channels */
    m_ambix_matrix = ptr_ambix_matrix_init(m_ambichannels, m_ambichannels, NULL);
    
    m_ambix_matrix = ptr_ambix_matrix_fill(m_ambix_matrix, AMBIX_MATRIX_IDENTITY);
    
    
    /* post matrix */
    // post_matrix(m_ambix_matrix);
    
    printf("Inputchannels: %d, Ambichannels: %d, Extrachannels: %d, Dummychannels: %d\n", m_nch, m_ambichannels, m_xtrachannels, m_dummychannels);
    
    if ((m_format == 0) && (m_xtrachannels == 0))
    {
      // basic format only works for (N+1)^2 channels, without extrachannels!
      m_fileformat = AMBIX_BASIC;
      printf("Write BASIC format\n");
    } else {
      m_fileformat = AMBIX_EXTENDED;
      printf("Write EXTENDED format\n");
    }
    
    
    memset(&m_ainfo, 0, sizeof(m_ainfo));
    
    m_ainfo.fileformat=m_fileformat;
    
    m_ainfo.ambichannels=m_ambichannels;//ambichannels;
    m_ainfo.extrachannels=m_xtrachannels;// xtrachannels;
    
    m_ainfo.samplerate=m_srate;
    
    printf("Sampleformat: %d\n", m_sampleformat);
    
    m_ainfo.sampleformat=(ambix_sampleformat_t)m_sampleformat;
    
    m_isopen = false;
    
    m_fh=ptr_ambix_open(m_fn.Get(), AMBIX_WRITE, &m_ainfo);
    
    if (!m_fh) {
      printf("Error: Cant't open file!\n");
      return;
    }
    else
      m_isopen = true;
    
    if (m_fileformat == AMBIX_EXTENDED)
    {
      // set the adaptor matrix
      ambix_err_t err = ptr_ambix_set_adaptormatrix(m_fh, m_ambix_matrix);
      if(err!=AMBIX_ERR_SUCCESS)
        fprintf(stderr, "setting adapator matrix [%dx%d] returned %d\n", m_ambix_matrix->rows, m_ambix_matrix->cols, err);
        
      // free the adaptor matrix
      ptr_ambix_matrix_destroy(m_ambix_matrix);
      
      // crosscheck the matrix
      const ambix_matrix_t *tempmatrix = NULL;
      
      tempmatrix=ptr_ambix_get_adaptormatrix(m_fh);
      
      
      /* post matrix */
      printf("This is the stored matrix: \n");
      post_matrix((ambix_matrix_t *)tempmatrix);
    }
    
    
    if (buildpeaks && m_isopen)
    {
      m_peakbuild=PeakBuild_Create(NULL,fn,m_srate,m_nch);
    }
    
  }
  
  bool IsOpen()
  {
    return m_isopen;
  }
  
  ~PCM_sink_ambix()
  {
    if (IsOpen())
    {
      ambix_err_t err = ptr_ambix_close(m_fh);
      if(err!=AMBIX_ERR_SUCCESS)
        fprintf(stderr, "Error closing file %d\n", err);
      

      printf("close the file...\n");
    }
    
    
    
    delete m_peakbuild;
    m_peakbuild=0;
    
    printf("destructor done...\n");
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
    
    float64_t* ambibuf = NULL;
    float64_t* xtrabuf = NULL;
    
    ambibuf = (float64_t*)calloc(len*m_ambichannels, sizeof(float64_t));
    xtrabuf = (float64_t*)calloc(len*m_xtrachannels, sizeof(float64_t));
    
    // printf("m_ambichannels %d, m_xtrachannels %d\n", m_ambichannels, m_xtrachannels);
    
    float64_t* ambibuf_temp = ambibuf;
    float64_t* xtrabuf_temp = xtrabuf;
    
    int len_temp = len;
    
    // assume the buffer is in one row... is this save?!
    float64_t* smpl_temp = samples[0];
    
    while (len_temp-- > 0) {
      
      for (int i=0; i < m_ambichannels; i++)
      {
        *ambibuf_temp++ = (float64_t) (*smpl_temp++);
      }
      
      for (int i=0; i < m_xtrachannels; i++)
      {
        *xtrabuf_temp++ = (float64_t) (*smpl_temp++);
      }
      
      // this is just for incrementing the pointer... throw away those channels
      for (int i=0; i < m_dummychannels; i++)
      {
        (float64_t) (*smpl_temp++);
      }
      
    }
    
    int sysrtn = ptr_ambix_writef_float64(m_fh,
                                          ambibuf,
                                          xtrabuf,
                                          len);
    // printf("written return: %d", sysrtn);
    free(ambibuf);free(xtrabuf);
    
  }
  
  int Extended(int call, void *parm1, void *parm2, void *parm3)
  {
    return 0;
  }
  
  
private:
  
  bool m_isopen;
  
  ambix_t *m_fh;
  ambix_info_t m_ainfo;
  ambix_fileformat_t m_fileformat;
  
  ambix_matrix_t* m_ambix_matrix;
  
  
  int m_format;
  int m_order;
  int m_sampleformat;
  
  uint32_t m_ambichannels, m_xtrachannels, m_dummychannels;
  
  
  int m_nch,m_srate;
  INT64 m_filesize;
  INT64 m_lensamples;
  WDL_String m_fn;
  
  REAPER_PeakBuild_Interface *m_peakbuild;
};

static unsigned int GetFmt(char **desc)
{
  if (desc) *desc="ambiX (Ambisonics eXchangeable)";
  return SINK_FOURCC;
}

static const char *GetExtension(const void *cfg, int cfg_l)
{
  if (cfg_l >= 4 && *((int *)cfg) == SINK_FOURCC) return "ambix";
  return NULL;
}

// config stuff

static int LoadDefaultConfig(void **data, const char *desc)
{
  // printf("LoadDefaultConfig\n");
  
  static WDL_HeapBuf m_hb;
  const char *fn=get_ini_file();
  int l=GetPrivateProfileInt(desc,"default_size",0,fn);
  if (l<1) return 0;
  
  if (GetPrivateProfileStruct(desc,"default",m_hb.Resize(l),l,fn))
  {
    *data = m_hb.Get();
    return l;
  }
  return 0;
}

int SinkGetConfigSize() {
  // printf("SinkGetConfigSize\n");
  
  return 8;
}


void SinkInitDialog(HWND hwndDlg, void *cfgdata, int cfgdata_l)
{
  
  // printf("SinkInitDialog\n");
  
  
  if (cfgdata_l < 8 || *((int *)cfgdata) != SINK_FOURCC)
    cfgdata_l=LoadDefaultConfig(&cfgdata,"ambix sink defaults");
  
  if (cfgdata_l>=8 && ((int*)cfgdata)[0] == SINK_FOURCC)
  {
    
    
  }
  
  // todo: show conifguration
  
}



void SinkSaveState(HWND hwndDlg, void *_data)
{
  printf("SinkSaveState\n");
  
}



void SaveDefaultConfig(HWND hwndDlg)
{
  printf("SaveDefaultConfig\n");
  
  char data[1024];
  SinkSaveState(hwndDlg,data);
  int l=SinkGetConfigSize();
  char *desc="ambix sink defaults";
  const char *fn=get_ini_file();
  char buf[64];
  sprintf(buf,"%d",l);
  WritePrivateProfileString(desc,"default_size",buf,fn);
  WritePrivateProfileStruct(desc,"default",data,l,fn);
  
}

/* GUI ELEMENTS HANDLING STUFF */

/* add entry to ambix format selection */
static void SetAmbixFormatStr(HWND hwndDlg, const char* txt, int idx)
{
  int n = SendDlgItemMessage(hwndDlg, IDC_AMBIX_FORMAT, CB_GETCOUNT, 0, 0);
  SendDlgItemMessage(hwndDlg, IDC_AMBIX_FORMAT, CB_ADDSTRING, n, (LPARAM)txt);
  SendDlgItemMessage(hwndDlg, IDC_AMBIX_FORMAT, CB_SETITEMDATA, n, idx);
}

/* add entry to order selection */
static void SetOrderStr(HWND hwndDlg, const char* txt, int idx)
{
  int n = SendDlgItemMessage(hwndDlg, IDC_AMBI_ORDER, CB_GETCOUNT, 0, 0);
  SendDlgItemMessage(hwndDlg, IDC_AMBI_ORDER, CB_ADDSTRING, n, (LPARAM)txt);
  SendDlgItemMessage(hwndDlg, IDC_AMBI_ORDER, CB_SETITEMDATA, n, idx);
}

/* add entry to Sampleformat selection */
static void SetSampleformatStr(HWND hwndDlg, const char* txt, int idx)
{
  int n = SendDlgItemMessage(hwndDlg, IDC_SAMPLEFORMAT, CB_GETCOUNT, 0, 0);
  SendDlgItemMessage(hwndDlg, IDC_SAMPLEFORMAT, CB_ADDSTRING, n, (LPARAM)txt);
  SendDlgItemMessage(hwndDlg, IDC_SAMPLEFORMAT, CB_SETITEMDATA, n, idx);
}

/* add entry to adaptormatrix selection */
static void SetAdaptormatrixStr(HWND hwndDlg, const char* txt, int idx)
{
  int n = SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_GETCOUNT, 0, 0);
  SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_ADDSTRING, n, (LPARAM)txt);
  SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_SETITEMDATA, n, idx);
}

/* add entry to extrachannel selection */
static void SetExtrachannelsStr(HWND hwndDlg, const char* txt, int idx)
{
  int n = SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_GETCOUNT, 0, 0);
  SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_ADDSTRING, n, (LPARAM)txt);
  SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_SETITEMDATA, n, idx);
}

/* add entry to extrachannel selection */
static void setNumChannelsLabel(HWND hwndDlg, int numchannels)
{
  char q_text[4];
  
  sprintf(q_text, "%d", numchannels);
  
  SetDlgItemText(hwndDlg, IDC_REQINCH_TXT, q_text);
}

/* get item data of current selection of dropdown box */
static int getCurrentItemData(HWND hwndDlg, int nIDDlgItem)
{
  int id = SendDlgItemMessage(hwndDlg, nIDDlgItem, CB_GETCURSEL, 0, 0);
  int itemdata = SendDlgItemMessage(hwndDlg, nIDDlgItem, CB_GETITEMDATA, id, 0);
  
  return itemdata;
}

static void calcNumChannels(HWND hwndDlg)
{
  int order = getCurrentItemData(hwndDlg, IDC_AMBI_ORDER);
  int xtrachannels = getCurrentItemData(hwndDlg, IDC_EXTRACHANNELS);
  
  setNumChannelsLabel(hwndDlg, (order+1)*(order+1)+xtrachannels);
}

WDL_DLGRET wavecfgDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
      
      // this is called when the dialog is initialized
    case WM_INITDIALOG:
    {
      
      
      /* Get the saved parameters */
      void *cfgdata=((void **)lParam)[0];
      int configLen = (int) (INT_PTR) ((void **) lParam)[1];
      
      
      if (configLen < 32 || *((int *)cfgdata) != SINK_FOURCC)
        configLen=LoadDefaultConfig(&cfgdata,"ambix sink defaults");
      
      // parameters have been sent by reaper -> parse them and set gui accordingly
      if (configLen >= 32 && *((int *)cfgdata) == SINK_FOURCC)
      {
        
      }
      
      
      /* setup the gui */
      
      SetAmbixFormatStr(hwndDlg, "BASIC", 0);
      SetAmbixFormatStr(hwndDlg, "EXTENDED", 1);
      
      // order 7 is maximum for 64 channel limit
      for (int i=1; i <= 7; i++)
      {
        char buf[2];
        wsprintf(buf,"%d", i);
        
        SetOrderStr(hwndDlg, buf, i);
      }
      
      // Sample Formats
      SetSampleformatStr(hwndDlg, "Signed 16 bit PCM", 1);
      SetSampleformatStr(hwndDlg, "Signed 24 bit PCM", 2);
      SetSampleformatStr(hwndDlg, "Signed 32 bit PCM", 3);
      SetSampleformatStr(hwndDlg, "32 bit float", 4);
      SetSampleformatStr(hwndDlg, "64 bit float", 5);
      
      // Adaptor Matrix
      SetAdaptormatrixStr(hwndDlg, "Reduce to 2D (Circular)", 0);
      SetAdaptormatrixStr(hwndDlg, "Reduce to upper Hemisphere", 1);
      SetAdaptormatrixStr(hwndDlg, "...Load from .txt file", 2);
      
      // maximum 64 extrachannels
      for (int i=0; i <= 64; i++)
      {
        char buf[2];
        wsprintf(buf,"%d", i);
        
        SetExtrachannelsStr(hwndDlg, buf, i);
      }
      
      return 0;
    }
      
      // this gets called in case something changed
    case WM_COMMAND:
    {
      
      /* Ambi Order changed */
      if ((LOWORD(wParam) == IDC_AMBI_ORDER || LOWORD(wParam) == IDC_EXTRACHANNELS) && HIWORD(wParam) == CBN_SELCHANGE)
      {
        calcNumChannels(hwndDlg);
      }
      
      /*
       if (LOWORD(wParam) == IDC_FORMAT && HIWORD(wParam) == CBN_SELCHANGE)
       {
       int id = SendDlgItemMessage(hwndDlg, IDC_FORMAT, CB_GETCURSEL, 0, 0);
       int sel_format = SendDlgItemMessage(hwndDlg, IDC_FORMAT, CB_GETITEMDATA, id, 0);
       
       updateSubFormats(hwndDlg, -1);
       
       }
       else if (LOWORD(wParam) == IDC_ENCODING && HIWORD(wParam) == CBN_SELCHANGE)
       {
       int id = SendDlgItemMessage(hwndDlg, IDC_ENCODING, CB_GETCURSEL, 0, 0);
       int sel_format = SendDlgItemMessage(hwndDlg, IDC_ENCODING, CB_GETITEMDATA, id, 0);
       
       updateByteOrder(hwndDlg, -1);
       }
       */
      break;
    }
      
      
      // this gets called to retrieve the settings!
    case WM_USER+1024:
    {
      if (wParam) *((int *)wParam)=32;
      if (lParam)
      {
        /*
         Settings List:
         0: Format (INT) -> 0: Basic, 1: Extended
         1: Order (INT)
         2: SampleFormat (INT): 0: 16bit PCM, 1: 24 bit PCM, 2: 32 bit PCM, 3: 32 bit float, 4: 64 bit float
         3: NumExtraChannels (INT)
         4: AdaptorMatrix Filename (char*)
        */
        
        int format = getCurrentItemData(hwndDlg, IDC_AMBIX_FORMAT);
        int order = getCurrentItemData(hwndDlg, IDC_AMBI_ORDER);
        int sampleformat = getCurrentItemData(hwndDlg, IDC_SAMPLEFORMAT);
        int numextrachannels = getCurrentItemData(hwndDlg, IDC_EXTRACHANNELS);
        
        
        // identifier
        ((int *)lParam)[0] = SINK_FOURCC;
        
        // parameters
        ((int *)(((unsigned char *)lParam)+4))[0]=REAPER_MAKELEINT(format);
        ((int *)(((unsigned char *)lParam)+4))[1]=REAPER_MAKELEINT(order);
        ((int *)(((unsigned char *)lParam)+4))[2]=REAPER_MAKELEINT(sampleformat);
        ((int *)(((unsigned char *)lParam)+4))[3]=REAPER_MAKELEINT(numextrachannels);
        
        /*
         // get the format selection
         int id = SendDlgItemMessage(hwndDlg, IDC_BYTEORDER, CB_GETCURSEL, 0, 0);
         int format = SendDlgItemMessage(hwndDlg, IDC_BYTEORDER, CB_GETITEMDATA, id, 0);
         
         // get the vbr quality
         int pos = SendDlgItemMessage(hwndDlg, IDC_VBR_SLIDER, TBM_GETPOS, 0, 0);
         
         
         ((int *)lParam)[0] = SINK_FOURCC;
         ((int *)(((unsigned char *)lParam)+4))[0]=REAPER_MAKELEINT(format);
         ((float *)(((unsigned char *)lParam)+4))[1]=(float)pos/100.f;
         */
      }
      
      return 0;
    }
    case WM_DESTROY:
    {
      
      return 0;
    }
      
  }
  return 0;
}

static HWND ShowConfig(const void *cfg, int cfg_l, HWND parent)
{
  
  if (cfg_l >= 4 && *((int *)cfg) == SINK_FOURCC)
  {
    const void *x[2]={cfg,(void *)cfg_l};
    return CreateDialogParam(g_hInst,MAKEINTRESOURCE(IDD_AMBIXSINK_CFG),parent,wavecfgDlgProc,(LPARAM)x);
  }
  
  return 0;
  
}

static PCM_sink *CreateSink(const char *filename, void *cfg, int cfg_l, int nch, int srate, bool buildpeaks)
{
  if (cfg_l >= 4 && *((int *)cfg) == SINK_FOURCC)
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


// import the resources. Note: if you do not have these files, run "php ../WDL/swell/mac_resgen.php res.rc" from this directory
#ifndef _WIN32 // MAC resources
#include "swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "swell/swell-menugen.h"
#include "res.rc_mac_menu"
#endif

