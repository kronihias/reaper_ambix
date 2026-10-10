/* ============================================================================
 *  metadata.cpp — readable summary of the tags stored in an .ambix file.
 *  See metadata.h.
 *
 *  The JSON reader is deliberately small: it parses into a tree, refuses
 *  anything malformed (the caller then shows the raw text) and never trusts
 *  a key to be present or of the expected type. Numbers are parsed by hand
 *  so the result does not depend on the C locale's decimal separator.
 * ==========================================================================*/

#include "metadata.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

/* ---------- JSON ---------- */

struct JValue
{
  enum Type { Null, Bool, Number, String, Array, Object };
  Type type;
  bool b;
  double n;
  std::string s;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue> > obj;

  JValue() : type(Null), b(false), n(0.) {}

  const JValue *Get(const char *key) const
  {
    if (type != Object) return NULL;
    for (size_t i = 0; i < obj.size(); ++i)
      if (obj[i].first == key) return &obj[i].second;
    return NULL;
  }
  /* a number; the strings "nan"/"inf" (how the recorder encodes them) count */
  bool Num(double &out) const
  {
    if (type == Number) { out = n; return true; }
    if (type == String)
    {
      if (s == "nan" || s == "NaN") { out = NAN; return true; }
      if (s == "inf" || s == "+inf") { out = INFINITY; return true; }
      if (s == "-inf") { out = -INFINITY; return true; }
    }
    return false;
  }
};

class JParser
{
public:
  JParser(const std::string &text) : m_p(text.c_str()), m_end(text.c_str() + text.size()), m_depth(0) {}

  bool ParseDocument(JValue &out)
  {
    if (!ParseValue(out)) return false;
    SkipSpace();
    return m_p == m_end;
  }

private:
  const char *m_p, *m_end;
  int m_depth;

  void SkipSpace()
  {
    while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r')) ++m_p;
  }
  bool Literal(const char *word)
  {
    const size_t len = strlen(word);
    if ((size_t)(m_end - m_p) < len || strncmp(m_p, word, len) != 0) return false;
    m_p += len;
    return true;
  }

  bool ParseValue(JValue &v)
  {
    SkipSpace();
    if (m_p >= m_end) return false;
    switch (*m_p)
    {
      case '{': return ParseObject(v);
      case '[': return ParseArray(v);
      case '"': v.type = JValue::String; return ParseString(v.s);
      case 't': v.type = JValue::Bool; v.b = true;  return Literal("true");
      case 'f': v.type = JValue::Bool; v.b = false; return Literal("false");
      case 'n': v.type = JValue::Null; return Literal("null");
      default:  v.type = JValue::Number; return ParseNumber(v.n);
    }
  }

  bool ParseObject(JValue &v)
  {
    if (++m_depth > 32) return false;
    v.type = JValue::Object;
    ++m_p; // '{'
    SkipSpace();
    if (m_p < m_end && *m_p == '}') { ++m_p; --m_depth; return true; }
    for (;;)
    {
      std::pair<std::string, JValue> member;
      SkipSpace();
      if (m_p >= m_end || *m_p != '"' || !ParseString(member.first)) return false;
      SkipSpace();
      if (m_p >= m_end || *m_p != ':') return false;
      ++m_p;
      if (!ParseValue(member.second)) return false;
      v.obj.push_back(member);
      SkipSpace();
      if (m_p >= m_end) return false;
      if (*m_p == ',') { ++m_p; continue; }
      if (*m_p == '}') { ++m_p; --m_depth; return true; }
      return false;
    }
  }

  bool ParseArray(JValue &v)
  {
    if (++m_depth > 32) return false;
    v.type = JValue::Array;
    ++m_p; // '['
    SkipSpace();
    if (m_p < m_end && *m_p == ']') { ++m_p; --m_depth; return true; }
    for (;;)
    {
      v.arr.push_back(JValue());
      if (!ParseValue(v.arr.back())) return false;
      SkipSpace();
      if (m_p >= m_end) return false;
      if (*m_p == ',') { ++m_p; continue; }
      if (*m_p == ']') { ++m_p; --m_depth; return true; }
      return false;
    }
  }

  static void AppendUtf8(std::string &out, unsigned long cp)
  {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)
    {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }
  bool Hex4(unsigned long &out)
  {
    if (m_end - m_p < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i)
    {
      const char c = *m_p++;
      out <<= 4;
      if (c >= '0' && c <= '9') out |= (unsigned long)(c - '0');
      else if (c >= 'a' && c <= 'f') out |= (unsigned long)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= (unsigned long)(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool ParseString(std::string &out)
  {
    ++m_p; // '"'
    out.clear();
    while (m_p < m_end)
    {
      const char c = *m_p++;
      if (c == '"') return true;
      if ((unsigned char)c < 0x20) return false;
      if (c != '\\') { out += c; continue; }
      if (m_p >= m_end) return false;
      const char e = *m_p++;
      switch (e)
      {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
        {
          unsigned long cp;
          if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && m_end - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u')
          {
            const char *save = m_p;
            unsigned long lo;
            m_p += 2;
            if (Hex4(lo) && lo >= 0xDC00 && lo < 0xE000)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else
              m_p = save;
          }
          if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD; // lone surrogate
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseNumber(double &out)
  {
    bool neg = false, digits = false;
    double value = 0.;
    if (m_p < m_end && (*m_p == '-' || *m_p == '+')) { neg = (*m_p == '-'); ++m_p; }
    while (m_p < m_end && *m_p >= '0' && *m_p <= '9') { value = value * 10. + (*m_p++ - '0'); digits = true; }
    if (m_p < m_end && *m_p == '.')
    {
      double scale = 0.1;
      ++m_p;
      while (m_p < m_end && *m_p >= '0' && *m_p <= '9') { value += (*m_p++ - '0') * scale; scale *= 0.1; digits = true; }
    }
    if (!digits) return false;
    if (m_p < m_end && (*m_p == 'e' || *m_p == 'E'))
    {
      bool eneg = false, edigits = false;
      int exponent = 0;
      ++m_p;
      if (m_p < m_end && (*m_p == '-' || *m_p == '+')) { eneg = (*m_p == '-'); ++m_p; }
      while (m_p < m_end && *m_p >= '0' && *m_p <= '9')
      {
        if (exponent < 10000) exponent = exponent * 10 + (*m_p - '0');
        ++m_p;
        edigits = true;
      }
      if (!edigits) return false;
      value *= pow(10., eneg ? -exponent : exponent);
    }
    out = neg ? -value : value;
    return true;
  }
};

/* ---------- text helpers ---------- */

const char *const kDeg = "\xC2\xB0"; // degree sign, UTF-8
const double kRadToDeg = 57.29577951308232;

std::string Format(const char *fmt, double v)
{
  char buf[64];
  snprintf(buf, sizeof(buf), fmt, v);
  /* no "-0.0" for something that rounds to zero */
  if (buf[0] == '-' && strspn(buf + 1, "0.") == strlen(buf + 1)) return buf + 1;
  return buf;
}

/* one line of text: control characters (newlines too) become spaces, and at
 * most maxlen bytes are kept, cut at a UTF-8 character boundary */
std::string OneLine(const std::string &in, size_t maxlen)
{
  std::string out;
  out.reserve(in.size() < maxlen ? in.size() : maxlen);
  for (size_t i = 0; i < in.size(); ++i)
  {
    const unsigned char c = (unsigned char)in[i];
    out += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
  }
  if (out.size() > maxlen)
  {
    size_t cut = maxlen;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;
    out.resize(cut);
    out += "...";
  }
  return out;
}

bool EqualsNoCase(const std::string &a, const char *b)
{
  const size_t len = strlen(b);
  if (a.size() != len) return false;
  for (size_t i = 0; i < len; ++i)
  {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

const std::string *Str(const JValue *obj, const char *key)
{
  const JValue *v = obj ? obj->Get(key) : NULL;
  return (v && v->type == JValue::String && !v->s.empty()) ? &v->s : NULL;
}
bool Num(const JValue *obj, const char *key, double &out)
{
  const JValue *v = obj ? obj->Get(key) : NULL;
  return v && v->Num(out) && std::isfinite(out);
}

/* "mm:ss.s" (or "h:mm:ss.s" from an hour on) */
std::string TimeString(double seconds)
{
  char buf[48];
  if (!std::isfinite(seconds)) return "--:--.-";
  const bool neg = seconds < 0.;
  long long tenths = (long long)floor(fabs(seconds) * 10. + 0.5);
  const long long h = tenths / 36000;
  const long long m = (tenths / 600) % 60;
  const long long s = (tenths / 10) % 60;
  const long long t = tenths % 10;
  if (h > 0)
    snprintf(buf, sizeof(buf), "%s%lld:%02lld:%02lld.%lld", neg ? "-" : "", h, m, s, t);
  else
    snprintf(buf, sizeof(buf), "%s%02lld:%02lld.%lld", neg ? "-" : "", m, s, t);
  return buf;
}

std::string Coordinate(double value, char pos, char negc, int decimals)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f%s %c", decimals, fabs(value), kDeg, value < 0. ? negc : pos);
  return buf;
}

const size_t kMaxValue = 300;   // bytes shown of one tag value
const size_t kMaxMarkers = 30;  // marker lines listed

/* ---------- the SpaceLevelMeter summary ---------- */

void Line(std::string &out, const std::string &label, const std::string &value)
{
  out += "  " + label + ": " + value + "\r\n";
}

void SpaceLevelSummary(const JValue &root, std::string &out)
{
  double v = 0.;
  const std::string *s;

  if ((s = Str(&root, "title"))) Line(out, "Title", OneLine(*s, kMaxValue));
  if ((s = Str(&root, "start"))) Line(out, "Recorded", OneLine(*s, 64));

  /* where */
  const JValue *loc = root.Get("location");
  if (loc && loc->type == JValue::Object)
  {
    if ((s = Str(loc, "place"))) Line(out, "Place", OneLine(*s, kMaxValue));
    double lat, lon;
    if (Num(loc, "latitude", lat) && Num(loc, "longitude", lon))
    {
      std::string c = Coordinate(lat, 'N', 'S', 6) + ", " + Coordinate(lon, 'E', 'W', 6);
      if (Num(loc, "accuracy", v)) c += " (\xC2\xB1" + Format("%.0f", v) + " m)";
      if (Num(loc, "altitude", v)) c += ", altitude " + Format("%.0f", v) + " m";
      Line(out, "Coordinates", c);
    }
  }

  /* with what */
  {
    std::string what;
    if ((s = Str(&root, "source"))) what += "source " + OneLine(*s, 64);
    if ((s = Str(&root, "device"))) what += (what.empty() ? "" : ", ") + std::string("device ") + OneLine(*s, 100);
    if ((s = Str(&root, "app")))    what += (what.empty() ? "" : ", ") + std::string("app ") + OneLine(*s, 100);
    if (!what.empty()) Line(out, "Recorded with", what);
  }
  const JValue *input = root.Get("input");
  if (input && input->type == JValue::Object)
  {
    std::string in;
    if ((s = Str(input, "name"))) in = OneLine(*s, 100);
    std::string detail;
    if (Num(input, "channels", v)) detail += Format("%.0f ch", v);
    if (Num(input, "sampleRate", v)) detail += (detail.empty() ? "" : ", ") + Format("%.0f Hz", v);
    if (!detail.empty()) in += (in.empty() ? "" : " ") + ("(" + detail + ")");
    if (!in.empty()) Line(out, "Input", in);
  }

  if (Num(&root, "fullScaleSPL", v))
    Line(out, "Calibration", "full scale = " + Format("%.1f", v) + " dB SPL");

  /* orientation */
  const JValue *orient = root.Get("orientation");
  if (orient && orient->type == JValue::Object)
  {
    std::string o;
    if (Num(orient, "frontOffset", v)) o += "front offset " + Format("%.1f", v) + kDeg;
    const JValue *level = orient->Get("levelWithGravity");
    if (level && level->type == JValue::Bool)
      o += (o.empty() ? "" : ", ") + std::string("levelled with gravity: ") + (level->b ? "yes" : "no");
    if (!o.empty()) Line(out, "Orientation", o);
    if ((s = Str(orient, "frame")))
    {
      if (*s == "mic")
        Line(out, "Frame", "mic (audio is in the microphone's own frame, not levelled)");
      else
        Line(out, "Frame", OneLine(*s, 64));
    }
  }

  const JValue *heading = root.Get("heading");
  if (heading && heading->type == JValue::Object)
  {
    std::string h;
    if (Num(heading, "true", v)) h += Format("%.1f", v) + kDeg + " true";
    if (Num(heading, "magnetic", v)) h += (h.empty() ? "" : ", ") + Format("%.1f", v) + kDeg + " magnetic";
    if (Num(heading, "accuracy", v)) h += " (\xC2\xB1" + Format("%.0f", v) + kDeg + ")";
    if (!h.empty()) Line(out, "Heading at start (top edge)", h);
  }

  /* telemetry: [seconds, upX, upY, upZ, temperatureC] per second */
  const JValue *tele = root.Get("telemetry");
  if (tele && tele->type == JValue::Array && !tele->arr.empty())
  {
    bool have_tilt = false, have_temp = false;
    double pitch = 0., roll = 0., tmin = 0., tmax = 0.;
    for (size_t i = 0; i < tele->arr.size(); ++i)
    {
      const JValue &row = tele->arr[i];
      if (row.type != JValue::Array) continue;
      double x, y, z, t;
      if (!have_tilt && row.arr.size() >= 4 && row.arr[1].Num(x) && row.arr[2].Num(y) && row.arr[3].Num(z)
          && std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
      {
        if (x > 1.) x = 1.;
        if (x < -1.) x = -1.;
        pitch = asin(x) * kRadToDeg;
        roll = -atan2(y, z) * kRadToDeg;
        have_tilt = true;
      }
      if (row.arr.size() >= 5 && row.arr[4].Num(t) && std::isfinite(t))
      {
        if (!have_temp || t < tmin) tmin = t;
        if (!have_temp || t > tmax) tmax = t;
        have_temp = true;
      }
    }
    if (have_tilt)
      Line(out, "Tilt at start", "pitch " + Format("%.1f", pitch) + kDeg + ", roll " + Format("%.1f", roll) + kDeg);
    if (have_temp)
    {
      if (tmax - tmin < 0.05)
        Line(out, "Temperature", Format("%.1f", tmin) + " " + kDeg + "C");
      else
        Line(out, "Temperature", Format("%.1f", tmin) + " to " + Format("%.1f", tmax) + " " + kDeg + "C");
    }
    Line(out, "Telemetry", Format("%.0f", (double)tele->arr.size()) + " rows (one per second)");
  }

  if ((s = Str(&root, "note"))) Line(out, "Note", OneLine(*s, kMaxValue));

  /* markers */
  const JValue *markers = root.Get("markers");
  if (markers && markers->type == JValue::Array && !markers->arr.empty())
  {
    out += "  Markers (" + Format("%.0f", (double)markers->arr.size()) + "):\r\n";
    for (size_t i = 0; i < markers->arr.size() && i < kMaxMarkers; ++i)
    {
      const JValue &m = markers->arr[i];
      double t = NAN;
      const JValue *tv = m.Get("time");
      if (tv) tv->Num(t);
      s = Str(&m, "text");
      out += "    " + TimeString(t) + "  " + (s ? OneLine(*s, 120) : std::string()) + "\r\n";
    }
    if (markers->arr.size() > kMaxMarkers)
      out += "    ... and " + Format("%.0f", (double)(markers->arr.size() - kMaxMarkers)) + " more\r\n";
  }
}

} // namespace

std::string AmbixFormatMetadata(const std::vector<AmbixTagItem> &items)
{
  std::string tags, spacelevel;
  for (size_t i = 0; i < items.size(); ++i)
  {
    const std::string &name = items[i].first;
    std::string value = items[i].second;
    /* APEv2 separates multiple values with NULs */
    for (size_t j = 0; j < value.size(); ++j)
      if (value[j] == '\0') value.replace(j, 1, " / ");

    if (EqualsNoCase(name, "SpaceLevelMeter"))
    {
      JValue root;
      JParser parser(value);
      if (parser.ParseDocument(root) && root.type == JValue::Object)
      {
        spacelevel += "\r\nSpaceLevelMeter recording:\r\n";
        SpaceLevelSummary(root, spacelevel);
      }
      else
      {
        spacelevel += "\r\nSpaceLevelMeter (unreadable): " + OneLine(value, kMaxValue) + "\r\n";
      }
      continue;
    }
    if (tags.empty()) tags = "\r\nTags:\r\n";
    Line(tags, OneLine(name, 64), OneLine(value, kMaxValue));
  }
  return tags + spacelevel;
}
