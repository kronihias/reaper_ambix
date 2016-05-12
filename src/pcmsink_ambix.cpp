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

/* include adapter matrices */
#include "adaptermatrices/adapter_hemi.h"
#include "adaptermatrices/adapter_circular.h"

/*          */


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
extern ambix_err_t 	(*ptr_ambix_matrix_fill_data) (ambix_matrix_t *mtx, const float32_t *data);
extern ambix_matrix_t* (*ptr_ambix_matrix_pinv)(const ambix_matrix_t*matrix, ambix_matrix_t*pinv);
extern ambix_err_t 	(*ptr_ambix_matrix_multiply_float64) (float64_t *dest, const ambix_matrix_t *mtx, const float64_t *source, int64_t frames);


extern void (*format_timestr)(double tpos, char *buf, int buflen);
extern REAPER_PeakBuild_Interface *(*PeakBuild_Create)(PCM_source *src, const char *fn, int srate, int nch);
extern void (*update_disk_counters)(int read, int write);
extern const char *(*get_ini_file)();
extern HWND g_main_hwnd;


extern HINSTANCE g_hInst;
#define WIN32_FILE_IO


/*
 Settings List:
 0: Format (ambix_fileformat_t) -> 1: Basic, 2: Extended
 1: Order (INT)
 2: SampleFormat (INT): 0: 16bit PCM, 1: 24 bit PCM, 2: 32 bit PCM, 3: 32 bit float, 4: 64 bit float
 3: NumExtraChannels (INT)
 4: Selected Reduction Method
 4: do reduction (bool)
 
 // AdaptorMatrix
 5: rows (uint32_t)
 6: cols (uint32_t)
 7......7+(rows*cols): data (float32_t)
 */
typedef struct {
  int	fourCC;
	ambix_fileformat_t format;
  uint32_t order;
  uint32_t sampleformat;
  uint32_t numextrachannels;
  uint32_t reduction_sel;
  int doreduction;
  // adaptor matrix
  uint32_t rows;
  uint32_t cols;
  // float32_t *data is extra
} AMBIXSINK_CONFIG;


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
    
    m_isopen = false;
    
    m_peakbuild=0;
    
    m_fh = 0;
    
    m_in_ch=nch;
    m_srate=srate;
    m_lensamples=0;
    m_filesize=0;
    m_fn.Set(fn);
    
    m_adapter_matrix = NULL; // this one gets passed
    
    
    // retrieve settings
    
    if (cfgdata_l >= 32 && *((int *)cfgdata) == SINK_FOURCC)
    {
      AMBIXSINK_CONFIG *pAmbixConfigData = (AMBIXSINK_CONFIG *) cfgdata;
      
      m_wanted_fileformat = pAmbixConfigData->format; // BASIC MEANS NO REDUCTION, EXTENDED MIGHT MEAN WITH OR WITHOUT REDUCTION
      
      m_order = REAPER_MAKELEINT(pAmbixConfigData->order);
      m_sampleformat = REAPER_MAKELEINT(pAmbixConfigData->sampleformat);
      
      if (!m_sampleformat)
        m_sampleformat = AMBIX_SAMPLEFORMAT_PCM24; // default fallback
      
      m_xtrachannels = REAPER_MAKELEINT(pAmbixConfigData->numextrachannels);
      
      m_doreduction = (pAmbixConfigData->doreduction > 0) ? true : false; // if true L=(N+1)^2 channels are expected, otherwise C
      
      uint32_t rows = pAmbixConfigData->rows; // has to be L=(N+1)^2
      uint32_t cols = pAmbixConfigData->cols; // has to be C (reduced number of channels)
      
      if ((rows > 0) && (cols > 0))
      {
        /* parse the matrix which got passed... */
        m_adapter_matrix = ptr_ambix_matrix_init(rows, cols, NULL);
        
        // mtx data offset in passed configchunk
        int param_offset = sizeof(AMBIXSINK_CONFIG);
        // this is the data pointer
        float* mtx_data_passed = (float32_t *)(((char*)cfgdata)+param_offset);
        
        ambix_err_t err = ptr_ambix_matrix_fill_data(m_adapter_matrix, mtx_data_passed);
        
        if (err != AMBIX_ERR_SUCCESS)
          printf("ERROR: Could not read matrix values....\n");
        
        printf("This matrix was passed:\n");
        post_matrix(m_adapter_matrix);
      
      }
      
    } // end retrieve settings
    
    // number of ambisonic channels
    uint32_t L = (uint32_t)(m_order+1)*(m_order+1);
    
    // AMBIX_EXTENDED ... means the sink is already getting a reduced set
    // AMBIX_BASIC ... either a full set is stored or it is reduced if set_adapter_matrix is called
    
    
    
    // if no reduction we write EXTENDED..
    
    if (m_doreduction && (m_wanted_fileformat == AMBIX_EXTENDED))
    {
      m_fileformat = AMBIX_BASIC; // the library does want ambix_basic in order to do the reduction (but it will write extended....!)
    }
    else if (m_wanted_fileformat == AMBIX_BASIC)
    {
      m_fileformat = AMBIX_BASIC;
    }
    else if (!m_doreduction && (m_wanted_fileformat != AMBIX_BASIC))
    {
      m_fileformat = AMBIX_EXTENDED;
    }
    
    /* create identity adapter matrix in case of extended file format and no adapter provided */
    if (!m_adapter_matrix && (m_wanted_fileformat == AMBIX_EXTENDED))
    {
      printf("Init Matrix with L=%d...\n", L);
      
      /* generate matrix of ones in case we have extended format with (N+1)^2 channels, but we miss an adapter matrix */
      m_adapter_matrix = ptr_ambix_matrix_init(L, L, NULL);
      m_adapter_matrix = ptr_ambix_matrix_fill(m_adapter_matrix, AMBIX_MATRIX_IDENTITY);
    }
    
    // set the number of ambisonic channels for reading/writing
    if (m_wanted_fileformat == AMBIX_BASIC)
    {
      m_ambi_in_channels = L; // (N+1)^2 for basic format
      m_ambi_out_channels = L; // only for display...
    }
    else if ((m_wanted_fileformat == AMBIX_EXTENDED) && m_doreduction)
    {
      m_ambi_in_channels = L; // L=(N+1)^2 for basic format and extended doing reduction
      m_ambi_out_channels = m_adapter_matrix->cols; // only for display...
    } else {
      // we already got the reduced format to write...
      m_ambi_in_channels = m_adapter_matrix->cols;
      m_ambi_out_channels = m_adapter_matrix->cols; // only for display...
    }
    
    m_xtrachannels = (uint32_t)m_xtrachannels;
    
    // throw away additional channels!
    m_dummychannels = m_in_ch - m_ambi_in_channels - m_xtrachannels;
    
    // sanity check number of input channels
    
    if (m_ambi_in_channels + m_xtrachannels > m_in_ch)
    {
      printf("ERROR: not enough input channels! Got: %d, Need: %d\n", m_in_ch, m_ambi_in_channels + m_xtrachannels);
      return;
    }
    
    
    
    /* post matrix */
    // post_matrix(m_adapter_matrix);
    
    printf("Inputchannels: %d, AmbiInChannels: %d, AmbiOutChannels: %d, Extrachannels: %d, Dummychannels: %d\n", m_in_ch, m_ambi_in_channels, m_ambi_out_channels, m_xtrachannels, m_dummychannels);
    
    
    if (m_fileformat == AMBIX_BASIC)
    {
      // basic format only works for (N+1)^2 channels, without extrachannels!
      printf("Write BASIC format\n");
    } else {
      printf("Write EXTENDED format\n");
    }
    
    
    memset(&m_ainfo, 0, sizeof(m_ainfo));
    
    m_ainfo.fileformat=m_fileformat;
    
    // this value must contain the reduced numer of channels (as stored on disk)
    m_ainfo.ambichannels=m_ambi_out_channels; // m_ambi_in_channels;
    
    m_ainfo.extrachannels=m_xtrachannels;
    
    m_ainfo.samplerate=m_srate;
    
    
    // printf("Sampleformat: %d\n", m_sampleformat);
    
    m_ainfo.sampleformat=(ambix_sampleformat_t)m_sampleformat;
    
    m_isopen = false;
    
    m_fh=ptr_ambix_open(m_fn.Get(), AMBIX_WRITE, &m_ainfo);
    
    if (!m_fh) {
      printf("Error: Cant't open file!\n");
      return;
    }
    else
      m_isopen = true;
    
    if (m_adapter_matrix) // is this save to always be called in case the matrix exists?
    {
      printf("setting adapter matrix...\n");
      // set the adaptor matrix
      ambix_err_t err = ptr_ambix_set_adaptormatrix(m_fh, m_adapter_matrix);
      if(err!=AMBIX_ERR_SUCCESS)
        fprintf(stderr, "setting adapator matrix [%dx%d] returned %d\n", m_adapter_matrix->rows, m_adapter_matrix->cols, err);
        
      // free the adaptor matrix
      ptr_ambix_matrix_destroy(m_adapter_matrix);
      
      /*
      // crosscheck the matrix
      const ambix_matrix_t *tempmatrix = NULL;
      
      tempmatrix=ptr_ambix_get_adaptormatrix(m_fh);
      
      
      //post matrix
      printf("This is the stored matrix: \n");
      post_matrix((ambix_matrix_t *)tempmatrix);
      */
    }
    
    
    if (buildpeaks && m_isopen)
    {
      m_peakbuild=PeakBuild_Create(NULL,fn,m_srate,m_in_ch);
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
  
  int GetNumChannels()
  {
    return m_ambi_out_channels+m_xtrachannels;
  } // return number of channels
  
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
    char ambix_format[20];
    
    switch (m_wanted_fileformat) {
      case AMBIX_NONE:
        sprintf(ambix_format, "error");
        break;
        
      case AMBIX_BASIC:
        sprintf(ambix_format, "basic");
        break;
        
      case AMBIX_EXTENDED:
        sprintf(ambix_format, "extended");
        break;
        
    }
    
    char tmp[512];
    sprintf(tmp,"Write ambiX %s file: order %d, ambi channels: %d, xtrachannels: %d",
            ambix_format,
            m_order,
            m_ambi_out_channels,
            m_xtrachannels);
    
    strncpy(buf,tmp,buflen);
  }
  
  void WriteMIDI(MIDI_eventlist *events, int len, double samplerate) { }
  void WriteDoubles(ReaSample **samples, int len, int nch, int offset, int spacing)
  {
    // printf("i am writing %d samples, my samples are %d, spacing %d, offset %d \n", len, sizeof(ReaSample), spacing, offset);
    if (m_peakbuild)
      m_peakbuild->ProcessSamples(samples,len,m_in_ch,offset,spacing);
    
    
    /* temp buffer allocation */
    float64_t* ambirawbuf = NULL;
    float64_t* xtrabuf = NULL;
    
    ambirawbuf = (float64_t*)calloc(len*m_ambi_in_channels, sizeof(float64_t));
    xtrabuf = (float64_t*)calloc(len*m_xtrachannels, sizeof(float64_t));
    
    
    float64_t* ambirawbuf_temp = ambirawbuf;
    float64_t* xtrabuf_temp = xtrabuf;
    
    int len_temp = len;
    
    // assume the buffer is in one row... is this save?!
    float64_t* smpl_temp = samples[0];
    
    while (len_temp-- > 0) {
      
      for (int i=0; i < m_ambi_in_channels; i++)
      {
        *ambirawbuf_temp++ = (float64_t) (*smpl_temp++);
      }
      
      for (int i=0; i < m_xtrachannels; i++)
      {
        *xtrabuf_temp++ = (float64_t) (*smpl_temp++);
      }
      
      // this is just for incrementing the pointer... throw away those channels
      for (int i=0; i < m_dummychannels; i++)
      {
        smpl_temp++;
      }
      
    }
    
    /* in case we don't have to do a reduction we are done now and can write the buffers to the file */
      
    int sysrtn = ptr_ambix_writef_float64(m_fh,
                                          ambirawbuf,
                                          xtrabuf,
                                          len);
    
    /* dealloc temp buffers */
    free(ambirawbuf);free(xtrabuf);
    
    // done writing
  }
  
  int Extended(int call, void *parm1, void *parm2, void *parm3)
  {
    printf("sink extended called: 0x%.8x\n", call);
    
    /* use this to retrieve cues (markers) !*/
    
    if (call == PCM_SINK_EXT_ADDCUE)
    {
      // parm1=(REAPER_cue*)cue
      REAPER_cue* cue = (REAPER_cue*)parm1;
      
      printf("cue %d: start: %f end: %f isregion: %d name: %s\n", cue->m_id, cue->m_time, cue->m_endtime, cue->m_isregion, cue->m_name);
      
      return 1;
    }
    
    return 0;
  }
  
  
private:
  
  bool m_isopen;
  
  ambix_t *m_fh;
  ambix_info_t m_ainfo;
  ambix_fileformat_t m_wanted_fileformat;
  ambix_fileformat_t m_fileformat;
  
  ambix_matrix_t* m_adapter_matrix; // this matrix gets stored in the file!
  
  int m_order;
  int m_sampleformat;
        
  uint32_t m_ambi_in_channels, m_ambi_out_channels, m_xtrachannels, m_dummychannels;
  
  bool m_doreduction;
  
  int m_in_ch, m_srate;
  INT64 m_filesize;
  INT64 m_lensamples;
  WDL_String m_fn;
  
  REAPER_PeakBuild_Interface *m_peakbuild;
};

static unsigned int GetFmt(const char **desc)
{
  if (desc) *desc="ambiX (Ambisonics eXchangeable)";
  return SINK_FOURCC;
}

static const char *GetExtension(const void *cfg, int cfg_l)
{
  if (cfg_l >= 4 && *((int *)cfg) == SINK_FOURCC) return "ambix";
  return NULL;
}

/* extended sink */
static int ExtendedSinkInfo(int call, void* parm1, void* parm2, void* parm3)
{
  printf("callback extended called: 0x%.8x\n", call);
  if (call == PCM_SINK_EXT_ADDCUE)
  {
    /* we support cues... */
    return 1;
  }
  
  return 0;
}

// config stuff


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


void getAdapterMatrix(HWND hwndDlg, uint32_t& mtx_rows, uint32_t &mtx_cols, const float* &mtx_data)
{
  uint32_t reduction_sel = getCurrentItemData(hwndDlg, IDC_ADAPTORMATRIX);
  
  uint32_t order = getCurrentItemData(hwndDlg, IDC_AMBI_ORDER);
  
  // the adapter matrix is LxC
  // L... (N+1)^2
  // C... reduced number of channels
  
  /* Get the circular reduction matrix */
  if (reduction_sel == 1)
  {
    mtx_rows = (order+1)*(order+1);
    mtx_cols = 2*order+1; // 2d...
    
    mtx_data = (const float*)adapter_circular[order-1];
  }
  /* Get the hemi reduction matrix */
  else if (reduction_sel == 2)
  {
    mtx_rows = mtx_adapter_hemi[order-1].rows;
    mtx_cols = mtx_adapter_hemi[order-1].cols;
    
    mtx_data = (const float*)adapter_hemi[order-1];
    
    // printf("set matrix to rows: %u cols: %u\n", mtx_rows, mtx_cols);
    
  }
  
}

static void calcNumChannels(HWND hwndDlg)
{
  int order = getCurrentItemData(hwndDlg, IDC_AMBI_ORDER);
  int xtrachannels = getCurrentItemData(hwndDlg, IDC_EXTRACHANNELS);
  
  /*
  uint32_t mtx_rows = 0;
  uint32_t mtx_cols = 0; // this is the number of needed input channels...
  const float* data;
  getAdapterMatrix(hwndDlg, mtx_rows, mtx_cols, data);
  */
  
  
  setNumChannelsLabel(hwndDlg, (order+1)*(order+1)+xtrachannels);
}

/* depending on the ambix format enable/disable some control elements */
static void enableDisableElements(HWND hwndDlg)
{
  int format = (ambix_fileformat_t)getCurrentItemData(hwndDlg, IDC_AMBIX_FORMAT);
  
  if (format == AMBIX_BASIC)
  {
    /* Reset and disable Adaptor Matrix and Extra Channels */
    
    SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_SETCURSEL, 0, 0);
    SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_SETCURSEL, 0, 0);
    
    EnableWindow( GetDlgItem( hwndDlg, IDC_EXTRACHANNELS ), false );
    EnableWindow( GetDlgItem( hwndDlg, IDC_ADAPTORMATRIX ), false );
    EnableWindow( GetDlgItem( hwndDlg, IDC_REVIEWMATRIX ), false );
    
    /* recalc the needed channels */
    calcNumChannels(hwndDlg);
  }
  else
  {
    /* Enable Adapter Matrix and Extra Channels */
    
    EnableWindow( GetDlgItem( hwndDlg, IDC_EXTRACHANNELS ), true );
    EnableWindow( GetDlgItem( hwndDlg, IDC_ADAPTORMATRIX ), true );
    EnableWindow( GetDlgItem( hwndDlg, IDC_REVIEWMATRIX ), true );
  }
}

int SinkGetConfigSize(HWND hwndDlg) {
  
  uint32_t mtx_rows = 0;
  uint32_t mtx_cols = 0;
  const float* data;
  
  getAdapterMatrix(hwndDlg, mtx_rows, mtx_cols, data);
  
  // printf("retrieved config size: %lu, rows: %d cols: %d\n", sizeof(AMBIXSINK_CONFIG) + mtx_rows*mtx_cols*sizeof(float32_t), mtx_rows, mtx_cols);
  
  return sizeof(AMBIXSINK_CONFIG) + mtx_rows*mtx_cols*sizeof(float32_t);
}


/* use this to store the settings for the sink */
void SinkSaveState(HWND hwndDlg, void *pSize, void *pConfigData)
{
  printf("SinkSaveState\n");
  
  /* first the size is asked to reserve the memory!! */
  if (pSize) *((int *)pSize) = SinkGetConfigSize(hwndDlg);
  
  
  /* second callback will fetch the actual data */
  if (pConfigData)
  {
    AMBIXSINK_CONFIG *pAmbixConfigData = (AMBIXSINK_CONFIG *) pConfigData;
    memset(pAmbixConfigData, 0, sizeof(AMBIXSINK_CONFIG));
    
    // identifier
    pAmbixConfigData->fourCC = SINK_FOURCC;
    
    
    pAmbixConfigData->format = (ambix_fileformat_t)getCurrentItemData(hwndDlg, IDC_AMBIX_FORMAT);
    pAmbixConfigData->order = getCurrentItemData(hwndDlg, IDC_AMBI_ORDER);
    pAmbixConfigData->sampleformat = getCurrentItemData(hwndDlg, IDC_SAMPLEFORMAT);
    pAmbixConfigData->numextrachannels = getCurrentItemData(hwndDlg, IDC_EXTRACHANNELS);
    
    pAmbixConfigData->reduction_sel = getCurrentItemData(hwndDlg, IDC_ADAPTORMATRIX);
    
    pAmbixConfigData->doreduction = 1; // set this static for now, might be altered afterwards
    
    /* AdaptorMatrix */
    
    uint32_t mtx_rows = 0;
    uint32_t mtx_cols = 0;
    
    const float* mtx_data_src = NULL;
    
    getAdapterMatrix(hwndDlg, mtx_rows, mtx_cols, mtx_data_src);
    
    /* store the matrix data in our settings */
    
    pAmbixConfigData->rows = mtx_rows;
    pAmbixConfigData->cols = mtx_cols;
    
    int param_offset = sizeof(AMBIXSINK_CONFIG);
    float* mtx_data_dst = (float32_t *)(((char*)pConfigData)+param_offset); // write to this destination
    
    memcpy(mtx_data_dst, mtx_data_src, mtx_rows*mtx_cols*sizeof(float32_t));
    
    /*
    for (int i=0; i < mtx_rows*mtx_cols; i++)
    {
      printf("value: %f\n", mtx_data_src[0]);
      
      (*mtx_data_dst++) = (*mtx_data_src++);
    }
    */
    
    /*
    float* mtx_data = (float32_t *)(((char*)pConfigData)+param_offset);
    
    
    for (int i=0; i < rows; i++)
    {
      for (int j=0; j < cols; j++)
      {
        float val = 0.f;
        if (i == j) {
          val = (float32_t)i+1;
        }
        mtx_data[i*cols + j] = val;
      }
    }
    */
    
  }
  
}



WDL_DLGRET wavecfgDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
      
      // this is called when the dialog is initialized
    case WM_INITDIALOG:
    {
      
      /* SETUP THE GUI */
      
      SetAmbixFormatStr(hwndDlg, "BASIC", 1);
      SetAmbixFormatStr(hwndDlg, "EXTENDED", 2);
      
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
      SetAdaptormatrixStr(hwndDlg, "Full 3D (No Reduction)", 0);
      SetAdaptormatrixStr(hwndDlg, "Reduce to 2D (Circular)", 1);
      SetAdaptormatrixStr(hwndDlg, "Reduce to Upper Hemisphere", 2);
      // SetAdaptormatrixStr(hwndDlg, "...Load from .txt file", 3); // loading from txt file is not implemented yet
      
      // maximum 64 extrachannels
      for (int i=0; i <= 64; i++)
      {
        char buf[2];
        wsprintf(buf,"%d", i);
        
        SetExtrachannelsStr(hwndDlg, buf, i);
      }
      
      /* END SETUP GUI */
      
      ///////////////////////////////
      /* Get the saved parameters */
      
      void *cfgdata=((void **)lParam)[0];
      int configLen = (int) (INT_PTR) ((void **) lParam)[1];
      
      if (configLen < sizeof(AMBIXSINK_CONFIG) || *((int *)cfgdata) != SINK_FOURCC)
      {
        SendDlgItemMessage(hwndDlg, IDC_AMBIX_FORMAT, CB_SETCURSEL, 0, 0);
        SendDlgItemMessage(hwndDlg, IDC_AMBI_ORDER, CB_SETCURSEL, 0, 0);
        SendDlgItemMessage(hwndDlg, IDC_SAMPLEFORMAT, CB_SETCURSEL, 1, 0);
        SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_SETCURSEL, 0, 0);
        SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_SETCURSEL, 0, 0);
      }
      
      // parameters have been sent by reaper -> parse them and set gui accordingly
      if (configLen >= sizeof(AMBIXSINK_CONFIG) && *((int *)cfgdata) == SINK_FOURCC)
      {
        AMBIXSINK_CONFIG *pAmbixConfigData = (AMBIXSINK_CONFIG *) cfgdata;
        
        
        SendDlgItemMessage(hwndDlg, IDC_AMBIX_FORMAT, CB_SETCURSEL, pAmbixConfigData->format-1, 0);
        SendDlgItemMessage(hwndDlg, IDC_AMBI_ORDER, CB_SETCURSEL, pAmbixConfigData->order-1, 0);
        SendDlgItemMessage(hwndDlg, IDC_SAMPLEFORMAT, CB_SETCURSEL, pAmbixConfigData->sampleformat-1, 0);
        SendDlgItemMessage(hwndDlg, IDC_EXTRACHANNELS, CB_SETCURSEL, pAmbixConfigData->numextrachannels, 0);
        SendDlgItemMessage(hwndDlg, IDC_ADAPTORMATRIX, CB_SETCURSEL, pAmbixConfigData->reduction_sel, 0);
        
      }
      
      enableDisableElements(hwndDlg);
      
      /* recalc the needed channels*/
      calcNumChannels(hwndDlg);

      
      return 0;
    }
      
      // this gets called in case something changed
    case WM_COMMAND:
    {
      
      
      /* ONLY FOR TESTING */
      /* try getting hemi adapter and invert it */
      /*
      int order = 1;
      
      int mtx_rows = mtx_adapter_hemi[order-1].rows;
      int mtx_cols = mtx_adapter_hemi[order-1].cols;
      
      ambix_matrix_t* mtx = ptr_ambix_matrix_init(mtx_rows, mtx_cols, NULL);
      ambix_matrix_t* inverse = ptr_ambix_matrix_init(mtx_cols, mtx_rows, NULL);
      
      ptr_ambix_matrix_fill_data(mtx, adapter_hemi[order-1]);
      
      printf("original:\n");
      post_matrix(mtx);
      
      inverse = ptr_ambix_matrix_pinv(mtx, inverse);
      if (inverse)
      {
        printf("pseudo inverse:\n");
        post_matrix(inverse);
      }
      */
//      int mtx_rows = 4;
//      int mtx_cols = 3;
//      ambix_matrix_t* mtx = ptr_ambix_matrix_init(mtx_rows, mtx_cols, NULL);
//      ambix_matrix_t* inverse = ptr_ambix_matrix_init(mtx_cols, mtx_rows, NULL);
//      
//      // ptr_ambix_matrix_fill(mtx, AMBIX_MATRIX_FUMA);
//      // ptr_ambix_matrix_fill(mtx, AMBIX_MATRIX_IDENTITY);
//      
//      
//      
//      // float32_t data[9] = {1.f, 2.f, 4.f, 2.f, 3.f, 4.f, 3.f, 4.f, 5.f};
//      float32_t data[12] = {0.750702260800974, 0.0, 0.0, 0.0, 1.0, 0.0, 0.660640685719784, 0.0, 0.0, 0.0, 0.0, 1.0};
//      ptr_ambix_matrix_fill_data(mtx, data);
//      
//      printf("original:\n");
//      post_matrix(mtx);
//      
//      inverse = ptr_ambix_matrix_pinv(mtx, inverse);
//      if (inverse)
//      {
//        printf("pseudo inverse:\n");
//        post_matrix(inverse);
//      }
      
      
      
      /* Ambix Format changed */
      if ((LOWORD(wParam) == IDC_AMBIX_FORMAT) && (HIWORD(wParam) == CBN_SELCHANGE))
      {
        enableDisableElements(hwndDlg);
      }
      
      
      /* Ambi Order changed */
      if ((LOWORD(wParam) == IDC_AMBI_ORDER || LOWORD(wParam) == IDC_EXTRACHANNELS) && HIWORD(wParam) == CBN_SELCHANGE)
      {
        calcNumChannels(hwndDlg);
      }
      
      break;
    }
      
      
      // this gets called to save the settings!!
    case WM_USER+1024:
    {
      
      SinkSaveState(hwndDlg, (void *)wParam, (void *)lParam);
      
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
//pcmsink_register_ext_t mySinkRegStruct={{GetFmt,GetExtension,ShowConfig,CreateSink}, ExtendedSinkInfo};

// import the resources. Note: if you do not have these files, run "php ../WDL/swell/mac_resgen.php res.rc" from this directory
#ifndef _WIN32 // MAC resources
#include "swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "swell/swell-menugen.h"
#include "res.rc_mac_menu"
#endif

