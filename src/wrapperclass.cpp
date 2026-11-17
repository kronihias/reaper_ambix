#include "wrapperclass.h"
#include "metadata.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <wavpack.h>

extern void (*format_timestr)(double tpos, char *buf, int buflen);
extern void (*update_disk_counters)(int read, int write);
extern void (*ShowConsoleMsg)(const char* msg);

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
  m_isreadingblock=false;
  m_ambi_in_channels=m_ambi_out_channels=m_xtrachannels=0;
  m_order=0;
  m_has_matrix=false;
  m_matrix_rows=m_matrix_cols=0;
  m_cuelist=NULL;
  
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
  return m_reader.isOpen();
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
  m_sfinfo=ambixfile::Info();
  m_has_matrix=false;

  ambixfile::ReaderConfig config;
  config.wavpackThreads=AmbixWavPackThreads();

  const ambixfile::Status status = m_filename ? m_reader.open(m_filename, config)
                                              : ambixfile::Status::InvalidArgument;
  if (status != ambixfile::Status::Ok)
  {
    printf("Failed to open ambix file: %s\n", ambixfile::toString(status));
    return;
  } else
  {
#ifdef REAPER_DEBUG_OUTPUT_TRACING
    ShowConsoleMsg("succeeded to open ambix file");
#endif

    /* retrieve file information */
    m_sfinfo=m_reader.info();
    m_ambi_in_channels=(int)m_sfinfo.ambiChannels;   // as stored in the file
    m_ambi_out_channels=(int)m_sfinfo.fullAmbiChannels; // after the adapter matrix
    m_xtrachannels=(int)m_sfinfo.extraChannels;

    /* the adapter matrix: its rows are the ambisonics channels we hand out */
    const ambixfile::Matrix &matrix = m_reader.adaptorMatrix();
    m_has_matrix = !matrix.empty();
    m_matrix_rows = matrix.rows();
    m_matrix_cols = matrix.cols();

    uint32_t order = 0;
    ambixfile::channelsToOrder((uint32_t)m_ambi_out_channels, order);
    m_order = (int)order;

    m_length=(INT64)m_sfinfo.frames;

    // bit per sample counter... for disc activity
    switch (m_sfinfo.sampleFormat)
    {
      case ambixfile::SampleFormat::Int16:   m_bps=16; break;
      case ambixfile::SampleFormat::Int24:   m_bps=24; break;
      case ambixfile::SampleFormat::Int32:
      case ambixfile::SampleFormat::Float32: m_bps=32; break;
      case ambixfile::SampleFormat::Float64: m_bps=64; break;
    }

    m_nch = m_ambi_out_channels+m_xtrachannels;
    m_srate = m_sfinfo.sampleRate;

    /* retrieve markers */
    /* the cues need to be in sequential order to be displayed correctly by reaper */
    if (!m_retrieved_cues)
    {
      const std::vector<ambixfile::Marker> &markers = m_reader.markers();
      for (size_t i = 0; i < markers.size(); ++i)
      {
        if (markers[i].position > m_length)
          break;
        AddCueToList(m_numcues, markers[i].position/m_srate, 0., false, markers[i].name.c_str(), 0);
      }

      const std::vector<ambixfile::Region> &regions = m_reader.regions();
      for (size_t i = 0; i < regions.size(); ++i)
      {
        if (regions[i].start > m_length)
          break;
        AddCueToList(m_numcues, regions[i].start/m_srate, regions[i].end/m_srate, true, regions[i].name.c_str(), 0);
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
  
  m_reader.close();
}

/* The APEv2 text items of a WavPack file (the ambiX reader does not expose
 * them, so the file is opened once more, for its tags only). */
static std::vector<AmbixTagItem> ReadWavPackTags(const char *filename)
{
  std::vector<AmbixTagItem> items;
  char err[80] = {0};
  WavpackContext *wpc = filename ? WavpackOpenFileInput(filename, err, OPEN_TAGS | OPEN_FILE_UTF8, 0) : NULL;
  if (!wpc)
    return items;
  const int count = WavpackGetNumTagItems(wpc);
  for (int i = 0; i < count; ++i)
  {
    const int namelen = WavpackGetTagItemIndexed(wpc, i, NULL, 0);
    if (namelen <= 0) continue;
    std::vector<char> name(namelen + 1, 0);
    WavpackGetTagItemIndexed(wpc, i, &name[0], namelen + 1);
    const int valuelen = WavpackGetTagItem(wpc, &name[0], NULL, 0);
    std::string value;
    if (valuelen > 0)
    {
      std::vector<char> v(valuelen + 1, 0);
      WavpackGetTagItem(wpc, &name[0], &v[0], valuelen + 1);
      value.assign(&v[0], valuelen); // may hold NUL-separated values
    }
    items.push_back(AmbixTagItem(std::string(&name[0]), value));
  }
  WavpackCloseFile(wpc);
  return items;
}

void LSFW_SimpleMediaDecoder::GetInfoString(char *buf, int buflen, char *title, int titlelen)
{
  strncpy(title,"ambix File Properties",titlelen);
  if (IsOpen())
  {
    // todo: add any decoder specific info
    char temp[4096],lengthbuf[128];
    format_timestr((double) m_length / (double)m_srate,lengthbuf,sizeof(lengthbuf));
    
    const char *ambix_format = "AMBIX_NONE";
    switch (m_sfinfo.fileFormat) {
      case ambixfile::FileFormat::None:     ambix_format = "AMBIX_NONE";     break;
      case ambixfile::FileFormat::Basic:    ambix_format = "AMBIX_BASIC";    break;
      case ambixfile::FileFormat::Extended: ambix_format = "AMBIX_EXTENDED"; break;
    }

    char matrix[32];
    if (m_has_matrix)
      snprintf(matrix, sizeof(matrix), "yes [%ux%u]", m_matrix_cols, m_matrix_rows);
    else
      snprintf(matrix, sizeof(matrix), "no");

    /* For WavPack, say whether the stream is lossless or hybrid (lossy), and
     * at what stored rate, so a delivered file can be checked in REAPER's
     * source properties without any other tool. */
    const bool is_wavpack = (m_sfinfo.container == ambixfile::Container::WavPack);
    char container_str[64];
    if (!is_wavpack)
      snprintf(container_str, sizeof(container_str), "CAF (uncompressed)");
    else if (m_sfinfo.lossy)
      snprintf(container_str, sizeof(container_str),
               "WavPack lossy (%.1f bit/sample)", m_sfinfo.lossyBitsPerSample);
    else
      snprintf(container_str, sizeof(container_str), "WavPack lossless");

    snprintf(temp, 4096, "Length: %s:\r\n"
            "Samplerate: %.0f\r\n"
            "Bits/sample: %d\r\n"
            "Container: %s\r\n"
            "\nFormat: %s\r\n"
            "Ambisonic Order: %d\r\n"
            "Ambisonic Channels Stored: %d\r\n"
            "Ambisonic Channels Retrieved: %d\r\n"
            "Adapter Matrix embedded: %s\r\n"
            "Extra Channels: %d\r\n",
            lengthbuf, m_srate, m_bps, container_str, ambix_format, m_order, m_ambi_in_channels, m_ambi_out_channels, matrix, m_xtrachannels);

    const char *credits =
            "\r\nreaper_ambix read support by Matthias Kronlachner\r\nwww.matthiaskronlachner.com\r\n"
            "ambiX format by the Institute of Electronic Music and Acoustics Graz (IEM)\r\nwww.iem.at";

    /* tags (title, recording details, markers...) stored by the recorder */
    std::string meta;
    if (is_wavpack)
      meta = AmbixFormatMetadata(ReadWavPackTags(m_filename));

    /* if it does not all fit, shorten the metadata at a line end */
    const size_t fixed = strlen(temp) + strlen(credits);
    const size_t room = (buflen > 0 && (size_t)buflen - 1 > fixed) ? (size_t)buflen - 1 - fixed : 0;
    if (meta.size() > room)
    {
      const char *more = "...\r\n";
      size_t cut = (room > strlen(more) + 2) ? meta.rfind("\r\n", room - strlen(more) - 2) : std::string::npos;
      meta = (cut == std::string::npos) ? std::string() : meta.substr(0, cut + 2) + more;
      if (meta.size() > room) meta.clear();
    }

    std::string text = std::string(temp) + meta + credits;
    if (buflen > 0)
    {
      strncpy(buf, text.c_str(), buflen);
      buf[buflen - 1] = 0;
    }
  } else
    snprintf(buf, buflen, "Error: File hasn't been opened succesfully");
  
}

void LSFW_SimpleMediaDecoder::SetPosition(INT64 pos)
{
  if (m_isreadingblock)
  {
    // hopefully this won't ever happen: the reader is not safe to seek
    // while a block is being read.
    //ShowConsoleMsg("SetPosition() called while reading block"); // this could be A Bad Thing(tm) if it happened
  }
  if (m_reader.isOpen())
  {
    if (pos!=m_lastpos+m_lastblocklen) // this condition prevents glitches when Reaper plays this media decoder resampled
      m_reader.seek(pos > 0 ? (uint64_t)pos : 0);
    m_lastpos=pos;
    //char buf[200];
    //sprintf(buf,"seeked to %d",pos);
    //ShowConsoleMsg(buf);
  }
}

int LSFW_SimpleMediaDecoder::ReadSamples(double *buf, int length)
{
  if (!m_reader.isOpen() || length <= 0)
    return 0;

  m_isreadingblock=true;

  int rdframes = 0;

  if (m_has_matrix)
  {
    /* expand to the full ambisonics set, then put the extra channels behind
     * it: one interleaved buffer for reaper */
    m_ambibuf.resize((size_t)length*m_ambi_out_channels);
    m_xtrabuf.resize((size_t)length*m_xtrachannels);

    rdframes=(int)m_reader.readAmbisonics(m_ambibuf.empty() ? NULL : &m_ambibuf[0],
                                          m_xtrabuf.empty() ? NULL : &m_xtrabuf[0],
                                          length);

    const double *ambi = m_ambibuf.empty() ? NULL : &m_ambibuf[0];
    const double *xtra = m_xtrabuf.empty() ? NULL : &m_xtrabuf[0];
    double *out = buf;
    for (int f = 0; f < rdframes; ++f)
    {
      for (int c = 0; c < m_ambi_out_channels; ++c) *out++ = *ambi++;
      for (int c = 0; c < m_xtrachannels; ++c)      *out++ = *xtra++;
    }
  }
  else // no adapter matrix: the file's channels are what reaper gets
  {
    rdframes=(int)m_reader.read(buf, length);
  }

  m_isreadingblock=false;
  update_disk_counters(rdframes*(m_ambi_in_channels+m_xtrachannels)*m_bps,0);
  m_lastpos+=rdframes;
  return rdframes;
}


void LSFW_SimpleMediaDecoder::AddCueToList(int id, double time, double endtime, bool isregion, const char* name, int flags)
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
