/* ============================================================================
 *  test_metadata.cpp — the tag summary shown in the source properties.
 *
 *  Feeds AmbixFormatMetadata() the tags a SpaceLevelMeter recording carries
 *  and checks the summary lines, plus the cases that must not break the
 *  dialog: missing keys, wrong types, malformed JSON, long marker lists.
 * ==========================================================================*/

#include "metadata.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void CheckHas(const std::string &text, const std::string &want, const char *what)
{
  if (text.find(want) == std::string::npos)
  {
    printf("FAIL: %s: missing \"%s\" in\n%s\n", what, want.c_str(), text.c_str());
    ++g_failures;
  }
}
static void CheckLacks(const std::string &text, const std::string &unwanted, const char *what)
{
  if (text.find(unwanted) != std::string::npos)
  {
    printf("FAIL: %s: unexpected \"%s\" in\n%s\n", what, unwanted.c_str(), text.c_str());
    ++g_failures;
  }
}

static const char *kFullJson =
  "{\"version\":1,\"start\":\"2026-10-09T14:03:22Z\",\"fs\":48000,"
  "\"telemetry\":[[0,0.0174524,0,1,21.5],[1,\"nan\",\"nan\",\"nan\",\"nan\"],[2,0,0,1,23.3]],"
  "\"markers\":[{\"time\":4.0105,\"text\":\"Marker 1\"},{\"time\":65.24,\"text\":\"line\\nbreak \\u00e9\"}],"
  "\"location\":{\"latitude\":48.20853,\"longitude\":-16.37285,\"accuracy\":5,\"altitude\":172.4,\"place\":\"Stephansplatz\"},"
  "\"heading\":{\"magnetic\":120.04,\"true\":123.4,\"accuracy\":5},"
  "\"orientation\":{\"frontOffset\":-90,\"levelWithGravity\":false,\"frame\":\"mic\"},"
  "\"fullScaleSPL\":130,\"source\":\"spcmic\",\"device\":\"iPad Pro\",\"app\":\"SpaceLevelMeter 1.0 (12)\","
  "\"input\":{\"name\":\"spcmic\",\"uid\":\"x\",\"channels\":16,\"sampleRate\":48000},"
  "\"title\":\"Square\",\"note\":\"windy\"}";

static void TestFull()
{
  std::vector<AmbixTagItem> items;
  items.push_back(AmbixTagItem("Title", "Square"));
  items.push_back(AmbixTagItem("Record Date", "2026-10-09T14:03:22Z"));
  items.push_back(AmbixTagItem("Artist", std::string("one\0two", 7)));
  items.push_back(AmbixTagItem("SpaceLevelMeter", kFullJson));
  const std::string t = AmbixFormatMetadata(items);
  const char *w = "full";
  CheckHas(t, "  Title: Square\r\n", w);
  CheckHas(t, "  Record Date: 2026-10-09T14:03:22Z\r\n", w);
  CheckHas(t, "  Artist: one / two\r\n", w);
  CheckHas(t, "SpaceLevelMeter recording:", w);
  CheckHas(t, "  Recorded: 2026-10-09T14:03:22Z\r\n", w);
  CheckHas(t, "  Place: Stephansplatz\r\n", w);
  CheckHas(t, "48.208530\xC2\xB0 N, 16.372850\xC2\xB0 W (\xC2\xB1" "5 m), altitude 172 m", w);
  CheckHas(t, "source spcmic, device iPad Pro, app SpaceLevelMeter 1.0 (12)", w);
  CheckHas(t, "  Input: spcmic (16 ch, 48000 Hz)\r\n", w);
  CheckHas(t, "full scale = 130.0 dB SPL", w);
  CheckHas(t, "front offset -90.0\xC2\xB0, levelled with gravity: no", w);
  CheckHas(t, "not levelled", w);
  CheckHas(t, "123.4\xC2\xB0 true, 120.0\xC2\xB0 magnetic (\xC2\xB1" "5\xC2\xB0)", w);
  CheckHas(t, "pitch 1.0\xC2\xB0, roll 0.0\xC2\xB0", w);
  CheckHas(t, "21.5 to 23.3 \xC2\xB0" "C", w);
  CheckHas(t, "3 rows", w);
  CheckHas(t, "  Note: windy\r\n", w);
  CheckHas(t, "Markers (2):", w);
  CheckHas(t, "    00:04.0  Marker 1\r\n", w);
  CheckHas(t, "    01:05.2  line break \xC3\xA9\r\n", w);
  CheckLacks(t, "Tags:\r\n  SpaceLevelMeter", w);
}

static void TestSparseAndBroken()
{
  /* only the required keys, and odd types where objects are expected */
  std::vector<AmbixTagItem> items;
  items.push_back(AmbixTagItem("SpaceLevelMeter",
    "{\"version\":1,\"start\":\"2026\",\"fs\":48000,\"location\":5,\"markers\":{},\"telemetry\":[1,[\"nan\"]],"
    "\"heading\":{\"true\":\"north\"}}"));
  std::string t = AmbixFormatMetadata(items);
  CheckHas(t, "  Recorded: 2026\r\n", "sparse");
  CheckLacks(t, "Markers", "sparse");
  CheckLacks(t, "Heading", "sparse");
  CheckLacks(t, "Tilt", "sparse");
  CheckLacks(t, "Tags:", "sparse");

  /* malformed: the raw text, shortened */
  items.clear();
  items.push_back(AmbixTagItem("spacelevelmeter", "{\"version\":1, \"start\": \"x\"" + std::string(1000, ' ')));
  t = AmbixFormatMetadata(items);
  CheckHas(t, "SpaceLevelMeter (unreadable): {\"version\":1, \"start\": \"x\"", "malformed");
  if (t.size() > 400) { printf("FAIL: malformed value not shortened (%u bytes)\n", (unsigned)t.size()); ++g_failures; }

  const char *bad[] = { "", "[1,2]", "{\"a\":}", "{\"a\":1,}", "{\"a\":\"\\x\"}", "{\"a\":1} x", "{{{{{{{{" };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
  {
    items.clear();
    items.push_back(AmbixTagItem("SpaceLevelMeter", bad[i]));
    CheckHas(AmbixFormatMetadata(items), "(unreadable)", bad[i]);
  }

  /* deep nesting is refused, not recursed into */
  items.clear();
  items.push_back(AmbixTagItem("SpaceLevelMeter", std::string(100000, '[')));
  CheckHas(AmbixFormatMetadata(items), "(unreadable)", "deep");

  if (!AmbixFormatMetadata(std::vector<AmbixTagItem>()).empty())
  {
    printf("FAIL: no items should give no text\n");
    ++g_failures;
  }
}

static void TestManyMarkers()
{
  std::string json = "{\"version\":1,\"start\":\"s\",\"fs\":48000,\"markers\":[";
  for (int i = 0; i < 45; ++i)
  {
    char m[64];
    snprintf(m, sizeof(m), "%s{\"time\":%d.5,\"text\":\"m%d\"}", i ? "," : "", 3600 + i, i);
    json += m;
  }
  json += "]}";
  std::vector<AmbixTagItem> items;
  items.push_back(AmbixTagItem("SpaceLevelMeter", json));
  const std::string t = AmbixFormatMetadata(items);
  CheckHas(t, "Markers (45):", "many");
  CheckHas(t, "    1:00:00.5  m0\r\n", "many");
  CheckHas(t, "    1:00:29.5  m29\r\n", "many");
  CheckLacks(t, "m30\r\n", "many");
  CheckHas(t, "... and 15 more", "many");
}

int main()
{
  TestFull();
  TestSparseAndBroken();
  TestManyMarkers();
  if (g_failures)
  {
    printf("%d failure(s)\n", g_failures);
    return 1;
  }
  printf("metadata: all passed\n");
  return 0;
}
