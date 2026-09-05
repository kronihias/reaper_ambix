/* ============================================================================
 *  fuma.h — Furse-Malham (B-format) to ambiX channel conversion.
 *
 *  Builds the matrix that turns a FuMa signal set into the ambiX convention
 *  (ACN ordering, SN3D normalization), per section 4.2.1 of Nachbar, Zotter,
 *  Deleflie & Sontacchi, "ambiX - A Suggested Ambisonics Format" (Ambisonics
 *  Symposium 2011) — the paper this plug-in implements.
 *
 *  Why not libambix's AMBIX_MATRIX_FUMA: it is wrong. It emits the INVERSE of
 *  its own (correct) ordering table, so first-order FuMa X lands in the ambiX
 *  Z slot, and it applies a Condon-Shortley (-1)^m sign that the ambiX paper
 *  explicitly rejects. Its TO_FUMA counterpart is inverted the same way, so
 *  the round trip is a clean identity and libambix's own tests pass. See
 *  tests/test_fuma.cpp for the values this header is checked against.
 * ==========================================================================*/

#ifndef REAPER_AMBIX_FUMA_H
#define REAPER_AMBIX_FUMA_H

#include <vector>

/* FuMa is defined to third order, so 16 channels is the ceiling. */
#define AMBIX_FUMA_MAX_CHANNELS 16

/* Number of ambiX channels a `fumaChannels`-wide FuMa set expands to, or 0 if
 * that is not a FuMa channel count.
 *
 * FuMa allows reduced sets, so the channel count alone does not give the
 * order: 5 channels is WXYUV (horizontal second order), which still expands to
 * a full 9-channel second-order ambiX set with the missing components zero. */
int AmbixFuMaTargetChannels(int fumaChannels);

/* Build the [target x fumaChannels] conversion matrix, row-major, so that
 *
 *     ambix[r] = sum over c of matrixOut[r * fumaChannels + c] * fuma[c]
 *
 * Returns the number of ambiX channels (matrix rows), or 0 if `fumaChannels`
 * is not a FuMa channel count, in which case matrixOut is left empty. */
int AmbixFuMaToAmbixMatrix(int fumaChannels, std::vector<double> &matrixOut);

#endif /* REAPER_AMBIX_FUMA_H */
