#include "wrapperclass.h"
//#include "libsndfileImport.h"

extern ambix_t* (*ptr_ambix_open) (const char *path, const ambix_filemode_t mode, ambix_info_t *ambixinfo) ;
extern ambix_err_t (*ptr_ambix_close) (ambix_t *ambix) ;
extern int64_t (*ptr_ambix_seek) (ambix_t *ambix, int64_t frames, int whence) ;
extern int64_t (*ptr_ambix_readf_float64) (ambix_t *ambix, float64_t *ambidata, float64_t *otherdata, int64_t frames) ;
extern int64_t (*ptr_ambix_writef_float32) (ambix_t *ambix, const float32_t *ambidata, const float32_t *otherdata, int64_t frames) ;
extern const ambix_matrix_t* (*ptr_ambix_get_adaptormatrix) (ambix_t *ambix) ;
extern ambix_err_t (*ptr_ambix_set_adaptormatrix) (ambix_t *ambix, const ambix_matrix_t *matrix) ;
extern ambix_matrix_t* (*ptr_ambix_matrix_create) (void) ;
extern void (*ptr_ambix_matrix_destroy) (ambix_matrix_t *mtx) ;
extern ambix_matrix_t* (*ptr_ambix_matrix_init) (uint32_t rows, uint32_t cols, ambix_matrix_t *mtx) ;
extern void (*ptr_ambix_matrix_deinit) (ambix_matrix_t *mtx) ;

extern ambix_marker_t* (*ptr_ambix_get_marker)(ambix_t *ambix, uint32_t id) ;
extern ambix_region_t* (*ptr_ambix_get_region)(ambix_t *ambix, uint32_t id) ;

extern ambix_err_t 	(*ptr_ambix_matrix_multiply_float64) (float64_t *dest, const ambix_matrix_t *mtx, const float64_t *source, int64_t frames);

extern uint32_t (*ptr_ambix_order2channels) (uint32_t order) ;
extern int32_t (*ptr_ambix_channels2order) (uint32_t channels) ;
extern int (*ptr_ambix_is_fullset) (uint32_t channels) ;


extern void (*format_timestr)(double tpos, char *buf, int buflen);
extern void (*update_disk_counters)(int read, int write);
extern void (*ShowConsoleMsg)(const char* msg);


extern void post_matrix(ambix_matrix_t *matrix);


/* merge two buffers of interleaved samples into a single interleaved buffer
 * buf1 holds chan1 samples per frame, buf2 holds chan2 samples per frame
 * the dest buffer holds (want1+want2) samples per frame
 * if chan<want, samples are dropped, if want>chan ZERO samples are filled in
 */
static void merge_samples(float64_t*buf1, uint32_t chan1, uint32_t want1,
                          float64_t*buf2, uint32_t chan2, uint32_t want2,
                          double*dest, uint32_t destsize,
                          uint32_t offset, uint32_t frames) {
  uint32_t left1=(want1>chan1)?want1-chan1:0;
  uint32_t left2=(want2>chan2)?want2-chan2:0;
  uint32_t use1=(chan1<want1)?chan1:want1;
  uint32_t use2=(chan2<want2)?chan2:want2;
  
  const uint32_t framesize=want1+want2;
  
  uint32_t f, c;
  for(f=0; f<frames; f++) {
    float64_t*in=NULL;
    double*out=dest+((offset+f)%destsize)*framesize;
    /* copy samples from buf1 */
    in=buf1+(f*chan1);
    for(c=0; c<use1; c++)
      *out++=*in++;
    /* zero-pad if needed */
    for(c=0; c<left1; c++)
      *out++=0.;
    /* copy samples from buf2 */
    in=buf2+(f*chan2);
    for(c=0; c<use2; c++)
      *out++=*in++;
    /* zero-pad if needed */
    for(c=0; c<left2; c++)
      *out++=0.;
  }
}

/* constructor */
LSFW_SimpleMediaDecoder::LSFW_SimpleMediaDecoder()
{
  // ShowConsoleMsg("constructing LSFW_SimpleMediaDecoder");
  m_filename=NULL;
  
  m_nch=0;
  m_bps=0;
  m_srate=0.0;
  m_length=0;
  m_lastpos=0;
  m_lastblocklen=0;
  m_fh=NULL;
  
  /* CUES */
  m_numcues = 0;
  m_retrieved_cues = false;
}

LSFW_SimpleMediaDecoder::~LSFW_SimpleMediaDecoder()
{
  // ShowConsoleMsg("destroying LSFW_SimpleMediaDecoder");
  Close(true);
  freeCueList();
  if (m_filename)
    free(m_filename);
}

bool LSFW_SimpleMediaDecoder::IsOpen()
{
  return m_fh;
  
  /*
   if (!m_fh && m_bps && m_nch && m_srate>0)
   {
   
   } else
   printf("File not opened correctly...\n");
   
   return m_fh && m_bps && m_nch && m_srate>0; // todo: check to make sure decoder is initialized properly
   */
}

void LSFW_SimpleMediaDecoder::Open(const char *filename, int diskreadmode, int diskreadbs, int diskreadnb)
{
  m_isreadingblock=false;
  Close(filename && strcmp(filename,m_filename?m_filename:""));
  if (filename)
  {
    free(m_filename);
    m_filename=strdup(filename);
  }
  
  
  m_length=0;
  m_bps=0;
  m_nch=0;
  m_srate=0;
  memset(&m_sfinfo, 0, sizeof(m_sfinfo));
  
  /* we could let the library do the conversion back to AMBIX_BASIC format internally */
  /* however, i think it's nice to give the user feedback about the file details (stored channels,...) */
  // m_sfinfo.fileformat = AMBIX_BASIC;
  
  m_fh=ptr_ambix_open(m_filename,AMBIX_READ,&m_sfinfo);
  if (!m_fh)
  {
    printf("Failed to open file with libambix!\n");
    
    ptr_ambix_close(m_fh);
    m_fh=0;
    
    return;
  } else
  {
#ifdef REAPER_DEBUG_OUTPUT_TRACING
    ShowConsoleMsg("succeeded to open file with libambix");
#endif
    
    /* retrieve file information */
    m_ambi_in_channels=m_sfinfo.ambichannels;
    m_ambi_out_channels=m_ambi_in_channels; // in case of basic format there is no adapter matrix..
    
    m_xtrachannels=m_sfinfo.extrachannels;
    
    
    /* try to get the adapter matrix */
    m_matrix = NULL;
    m_matrix=ptr_ambix_get_adaptormatrix(m_fh);
    
    if (m_matrix)
    {
      
      m_ambi_out_channels = m_matrix->rows; // the number of rows will be our ambisonics channels
      
      // printf("Adapter Matrix:\n");
      // post_matrix((ambix_matrix_t*)m_matrix);
    } else {
      // printf("Error: No Adapter Matrix!\n");
    }
    
    
    m_order = ptr_ambix_channels2order(m_ambi_out_channels);
    
    // printf("Ambix Fileformat: %d, Got Ambichannels: %d, Ambi Order: %d, Extrachannels: %d!\n", m_sfinfo.fileformat, m_sfinfo.ambichannels, m_order, m_sfinfo.extrachannels);
    
    
    m_length=m_sfinfo.frames;
    
    // bit per sample counter... for disc activity
    m_bps=16;
    int foo=m_sfinfo.sampleformat & 0x0000FFFF;
    if (foo==AMBIX_SAMPLEFORMAT_PCM16)
      m_bps=16;
    else if (foo==AMBIX_SAMPLEFORMAT_PCM24)
      m_bps=24;
    else if (foo==AMBIX_SAMPLEFORMAT_PCM32 || foo==AMBIX_SAMPLEFORMAT_FLOAT32)
      m_bps=32;
    else if (foo==AMBIX_SAMPLEFORMAT_FLOAT64)
      m_bps=64;
    
    
    
    m_nch = m_ambi_out_channels+m_xtrachannels;
    m_srate = m_sfinfo.samplerate;
    
    /* retrieve markers */
    /* the cues need to be in sequential order to be displayed correctly by reaper */
    if (!m_retrieved_cues)
    {
      uint32_t id = 0;
      while (1) {
        ambix_marker_t* new_marker = NULL;
        new_marker = ptr_ambix_get_marker(m_fh, id);
        if (new_marker == NULL)
          break;
        if (new_marker->position > m_length)
          break;
        // printf("Add Marker: ID: %d, POS: %f, NAME: %s\n", id, new_marker->position/m_srate, new_marker->name);
        AddCueToList(m_numcues, new_marker->position/m_srate, 0., false, new_marker->name, 0);
        // m_numcues++; // this is done in AddCueToList
        id++;
      }

      id = 0;
      while (1) {
        ambix_region_t* new_region = NULL;
        new_region = ptr_ambix_get_region(m_fh, id);
        if (new_region == NULL)
          break;
        if (new_region->start_position > m_length)
          break;
        // printf("Add Region: ID: %d, ST_POS: %f, END_POS: %f, NAME Len: %lu, NAME: %s\n", id, new_region->start_position/m_srate, new_region->end_position/m_srate, strlen(new_region->name), new_region->name);
        AddCueToList(m_numcues, new_region->start_position/m_srate, new_region->end_position/m_srate, true, new_region->name, 0);
        // m_numcues++; this is done in AddCueToList
        id++;
      }

      m_retrieved_cues = true;
    }
    
  }
  
}

void LSFW_SimpleMediaDecoder::Close(bool fullClose)
{
  if (fullClose)
  {
    // delete any decoder data, but we have nothing dynamically allocated
  }
  // freeCueList();
  
  // printf("freed cue list\n");
  if (m_fh)
    ptr_ambix_close(m_fh);
  
  m_fh=0;
}

void LSFW_SimpleMediaDecoder::GetInfoString(char *buf, int buflen, char *title, int titlelen)
{
  strncpy(title,"ambix File Properties",titlelen);
  if (IsOpen())
  {
    // todo: add any decoder specific info
    char temp[4096],lengthbuf[128];
    format_timestr((double) m_length / (double)m_srate,lengthbuf,sizeof(lengthbuf));
    
    char ambix_format[20];
    
    switch (m_sfinfo.fileformat) {
      case AMBIX_NONE:
        snprintf(ambix_format, 20, "AMBIX_NONE");
        break;
        
      case AMBIX_BASIC:
        snprintf(ambix_format, 20, "AMBIX_BASIC");
        break;
        
      case AMBIX_EXTENDED:
        snprintf(ambix_format, 20,"AMBIX_EXTENDED");
        break;
        
    }
    
    char matrix[20];
    if (m_matrix)
      snprintf(matrix, 20, "yes [%dx%d]", m_matrix->cols, m_matrix->rows);
    else
      snprintf(matrix, 20, "no");
    
    snprintf(temp, 4096, "Length: %s:\r\n"
            "Samplerate: %.0f\r\n"
            "Bits/sample: %d\r\n"
            "\nFormat: %s\r\n"
            "Ambisonic Order: %d\r\n"
            "Ambisonic Channels Stored: %d\r\n"
            "Ambisonic Channels Retrieved: %d\r\n"
            "Adapter Matrix embedded: %s\r\n"
            "Extra Channels: %d\r\n"
            
            "\nreaper_ambix read support by Matthias Kronlachner\nwww.matthiaskronlachner.com\n"
            "based on libambix by IOhannes m zmölnig\nInstitute of Electronic Music and Acoustics Graz (IEM)\nwww.iem.at",
            lengthbuf, m_srate, m_bps, ambix_format, m_order, m_ambi_in_channels, m_ambi_out_channels, matrix, m_xtrachannels);
    
    strncpy(buf,temp,buflen);
  } else
    snprintf(buf, buflen, "Error: File hasn't been opened succesfully");
  
}

void LSFW_SimpleMediaDecoder::SetPosition(INT64 pos)
{
  if (m_isreadingblock)
  {
    // hopefully this won't ever happen. libsndfile does actually explode
    // if seeking while also reading the file. we could solve this with
    // a mutex but hmm...
    //ShowConsoleMsg("SetPosition() called while reading block"); // this could be A Bad Thing(tm) if it happened
  }
  if (m_fh)
  {
    // todo: if decoder, seek decoder (rather than file)
    
    if (pos!=m_lastpos+m_lastblocklen) // this condition prevents glitches when Reaper plays this media decoder resampled
      ptr_ambix_seek(m_fh,pos,SEEK_SET);
    m_lastpos=pos;
    //char buf[200];
    //sprintf(buf,"seeked to %d",pos);
    //ShowConsoleMsg(buf);
  }
}

int LSFW_SimpleMediaDecoder::ReadSamples(double *buf, int length)
{
  if (m_fh)
  {
    m_isreadingblock=true;
    
    int rdframes = 0;
    
    if (m_matrix)
    {
      /* allocate temporary buffers */
      float64_t*ambi_in_buf = NULL;
      float64_t*ambi_out_buf = NULL;
      float64_t*xtrabuf = NULL;
      
      ambi_in_buf = (float64_t*)calloc(length*m_ambi_in_channels, sizeof(float64_t));
      ambi_out_buf = (float64_t*)calloc(length*m_ambi_out_channels, sizeof(float64_t));
      xtrabuf = (float64_t*)calloc(length*m_xtrachannels, sizeof(float64_t));
      
      /* read raw data from file */
      rdframes=ptr_ambix_readf_float64(m_fh, ambi_in_buf, xtrabuf, length);
      
      /* convert to full ambisonic set in case there is an adapter matrix... */
      ptr_ambix_matrix_multiply_float64(ambi_out_buf, m_matrix, ambi_in_buf, rdframes);
      
      /* merge samples to one continuous buffer for reaper */
      merge_samples(ambi_out_buf, m_ambi_out_channels, m_ambi_out_channels,
                    xtrabuf, m_xtrachannels, m_xtrachannels,
                    buf, length,
                    0, rdframes);
      
      /* free the buffers */
      free(ambi_in_buf);
      free(ambi_out_buf);
      free(xtrabuf);
      
    }
    else // no adapter matrix provided, use ambi channels as they are...
    {
      /* allocate temporary buffers */
      float64_t*ambi_out_buf = NULL;
      float64_t*xtrabuf = NULL;
      
      ambi_out_buf = (float64_t*)calloc(length*m_ambi_out_channels, sizeof(float64_t));
      xtrabuf = (float64_t*)calloc(length*m_xtrachannels, sizeof(float64_t));
      
      /* read raw data from file */
      rdframes=ptr_ambix_readf_float64(m_fh, ambi_out_buf, xtrabuf, length);
      
      /* no need to apply adaper matrix, merge directly for reaper... */
      merge_samples(ambi_out_buf, m_ambi_out_channels, m_ambi_out_channels,
                    xtrabuf, m_xtrachannels, m_xtrachannels,
                    buf, length,
                    0, rdframes);
      
      /* free the buffers */
      free(ambi_out_buf);
      free(xtrabuf);
      
    }
    
    
    
    m_isreadingblock=false;
    update_disk_counters(rdframes*(m_ambi_in_channels+m_xtrachannels)*m_bps,0);
    m_lastpos+=rdframes;
    return rdframes;
  }
return 0;

}


void LSFW_SimpleMediaDecoder::AddCueToList(int id, double time, double endtime, bool isregion, char* name, int flags)
{
  if (m_numcues == 0)
    m_cuelist = (REAPER_cue *) malloc((m_numcues + 1) * sizeof(REAPER_cue));
  else
    m_cuelist = (REAPER_cue *) realloc(m_cuelist, (m_numcues + 1) * sizeof(REAPER_cue));
  
  if (m_cuelist == NULL) return;
  
  memset(m_cuelist + m_numcues, 0, sizeof(REAPER_cue));
  
  m_cuelist[m_numcues].m_id = id;
	m_cuelist[m_numcues].m_time = time;
	m_cuelist[m_numcues].m_endtime = endtime;
	m_cuelist[m_numcues].m_isregion = isregion;
  if (name)
    m_cuelist[m_numcues].m_name = strdup(name);
	m_cuelist[m_numcues].m_flags= flags;
  
  m_numcues += 1;
  
}

void LSFW_SimpleMediaDecoder::freeCueList()
{
  for (int i=0; i<m_numcues; i++) {
    if (m_cuelist[i].m_name)
      free(m_cuelist[i].m_name);
  }
  if (m_cuelist)
    free(m_cuelist);
  m_cuelist = NULL;
  m_numcues = 0;
}

int LSFW_SimpleMediaDecoder::Extended(int call, void *parm1, void *parm2, void *parm3)
{
  // printf("Extended Called with call: 0x%05x...\n", call);
  
  /* this will be called multiple times to retrieve cue information */
  if (call == PCM_SOURCE_EXT_ENUMCUES_EX)
  {
    int cueIndex = (int) (INT_PTR) parm1;
    
    if (cueIndex < m_numcues)
    {
      
      /* pass the cue to reaper */
      
      *(REAPER_cue*) parm2 = *(m_cuelist+cueIndex);
      
      // parm1=(int) index of cue (source must provide persistent backing store for cue->m_name),
      // parm2=(REAPER_cue*) optional. Returns 0 when out of cues, otherwise returns how much to advance index (1 or 2 usually).
      
      return 1;
    }
    else
      return 0; // no more cues
    
    
  }

  return 0;
  
}

ISimpleMediaDecoder* LSFW_SimpleMediaDecoder::Duplicate()
{
  LSFW_SimpleMediaDecoder *r=new LSFW_SimpleMediaDecoder;
  if (r->m_filename)
    free(r->m_filename);
  r->m_filename = m_filename ? strdup(m_filename) : NULL;
  return r;
}
