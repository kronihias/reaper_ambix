#ifndef WRAPPERCLASS_H
#define WRAPPERCLASS_H
#include "libambixImport.h"
#include "reaper_plugin.h"
#include <ambix/ambix.h>

class LSFW_SimpleMediaDecoder : public ISimpleMediaDecoder
{
public:
    LSFW_SimpleMediaDecoder();
    ~LSFW_SimpleMediaDecoder();
    ISimpleMediaDecoder *Duplicate();
    void Open(const char *filename, int diskreadmode, int diskreadbs, int diskreadnb);
    void Close(bool fullClose);
    const char *GetFileName() { return m_filename?m_filename:""; }
    const char *GetType() { return "AMBIX"; }
    void GetInfoString(char *buf, int buflen, char *title, int titlelen);
    bool IsOpen();
    int GetNumChannels() { return m_nch; }
    int GetBitsPerSample() { return m_bps; }
    double GetSampleRate() { return m_srate; }
    INT64 GetLength() { return m_length; }
    INT64 GetPosition() { return m_lastpos; }
    void SetPosition(INT64 pos);
    int ReadSamples(double *buf, int length);
    int Extended(int call, void *parm1, void *parm2, void *parm3);
    
    void AddCueToList(int id, double time, double endtime, bool isregion, char* name, int flags);
    
    void freeCueList();
    
private:
    ambix_t *m_fh;
    ambix_info_t m_sfinfo;
    char *m_filename;

    double *m_audiordbuf;
    int m_nch, m_bps;
    int m_ambi_in_channels, m_ambi_out_channels, m_xtrachannels;
  
    int m_order; // 3d ambisonic order
  
    const ambix_matrix_t* m_matrix;
    double m_srate;
    INT64 m_lastpos;
    int m_lastblocklen;
    INT64 m_length; // length in sample-frames
    bool m_isreadingblock;
    
    REAPER_cue *m_cuelist; // store the cue list
    int m_numcues; // num of stored cues
    
};

#endif // WRAPPERCLASS_H
