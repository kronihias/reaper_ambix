/* ============================================================================
 *  metadata.h — readable summary of the tags stored in an .ambix file.
 *
 *  WavPack .ambix files can carry APEv2 tags at the end of the file. Recorders
 *  like the SpaceLevelMeter iOS app store the usual text items there (Title,
 *  Comment, Record Date, Record Location, Encoder) plus one item,
 *  "SpaceLevelMeter", holding JSON with recording details: date, location,
 *  heading, calibration, orientation, per-second telemetry and the markers.
 *
 *  This part only formats; reading the tags (WavPack) is the caller's job, so
 *  it can be tested without WavPack or REAPER.
 * ==========================================================================*/

#ifndef AMBIX_METADATA_H
#define AMBIX_METADATA_H

#include <string>
#include <utility>
#include <vector>

/* one APEv2 text item: name and UTF-8 value (multiple values NUL separated) */
typedef std::pair<std::string, std::string> AmbixTagItem;

/* The metadata section for the source properties dialog: the plain text
 * items, then a summary of a "SpaceLevelMeter" JSON item if there is one
 * (its raw value, shortened, if it does not parse). Lines end in "\r\n".
 * Empty if there are no items. */
std::string AmbixFormatMetadata(const std::vector<AmbixTagItem> &items);

#endif /* AMBIX_METADATA_H */
