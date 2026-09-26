#!/usr/bin/env python3
"""
Generate the ambiX EXTENDED-format adaptor matrices in src/adaptermatrices/.

An adaptor matrix A (L x C, L = (N+1)^2) expands the C stored channels to the
full ACN/SN3D set: full = A . reduced. libambix derives the reduction matrix
as pinv(A) when writing. Both shipped families have orthonormal columns
(A^T A = I), so pinv(A) = A^T, pinv(A) . A = I, and reduce -> expand is an
orthogonal projector. Re-applying it any number of times loses nothing beyond
the first pass (tests/test_adapter_matrices.cpp checks this).

2D circular (C = 2N+1)
    Selects the sectoral harmonics |m| = n, which are all that a horizontal
    source excites. Entries are exactly 0/1, and the output is byte-identical
    to the shipped header.

Upper hemisphere (C = (N+1)(N+2)/2)
    Spherical Slepian basis of the cap from the zenith down to el_min, a little
    below the horizon: truncated eigendecomposition of the Gram matrix
    G = int_cap y(theta) y(theta)^T dtheta of the N3D harmonics, done
    separately per azimuthal degree m, because a zenith-only restriction
    leaves G block-diagonal in m. Block m keeps as many eigenvectors (largest
    eigenvalue first) as it has z-symmetric harmonics (n + |m| even), which
    gives the paper's K = (N+1)(N+2)/2.
      - Nachbar, Zotter, Deleflie, Sontacchi, "ambiX - A Suggested Ambisonics
        Format", Ambisonics Symposium 2011, sec. 4.5
      - Zotter, Pomberger, Frank, "Ambisonic Decoding with and without
        Mode-Matching: A Case Study Using the Hemisphere", Ambisonics
        Symposium 2010, sec. 2.4 / 7.1
      - Zotter, Pomberger, "Spherical Slepian functions for approximation of
        spherical measurement data", DAGA 2012

    The 2016 script that produced the shipped hemi header is lost. EL_MIN_DEG
    below was recovered by fitting that header: every block with |m| >= 2 (and
    all of N=1) matches the shipped subspace to ~1e-6. The m = 0/1 blocks,
    which are nonzero at the zenith, differ by up to ~2e-2, which suggests
    the original used a coarser quadrature near the pole. So the
    regenerated hemi header is equivalent to the shipped one, not identical.
    `--check` reports the per-order difference.

Usage
    scripts/generate_adapter_matrices.py --check        # compare with shipped headers
    scripts/generate_adapter_matrices.py circular       # print header to stdout
    scripts/generate_adapter_matrices.py hemi -o FILE   # write header to FILE
"""

import argparse
import os
import re
import sys

import numpy as np

MAX_ORDER = 7
HERE = os.path.dirname(os.path.abspath(__file__))
HEADER_DIR = os.path.join(HERE, "..", "src", "adaptermatrices")

# lower cap boundary (elevation, degrees) per order, fitted to the shipped header
EL_MIN_DEG = {1: -12.824, 2: -24.81, 3: -23.49, 4: -21.35, 5: -18.27, 6: -16.21, 7: -14.18}


def acn(n, m):
    return n * n + n + m


# ---------------------------------------------------------------------------
# real spherical harmonics, ACN order, N3D (orthonormal to 4*pi)
# ---------------------------------------------------------------------------
def real_sh_n3d(order, azimuth, colatitude):
    """rows: ACN index, columns: directions. No Condon-Shortley phase (ambiX)."""
    x = np.cos(colatitude)
    s = np.sin(colatitude)
    L = (order + 1) ** 2
    Y = np.zeros((L, np.size(x)))
    # unnormalised associated Legendre P_n^m(x) without (-1)^m
    P = {(0, 0): np.ones_like(x)}
    for m in range(1, order + 1):
        P[(m, m)] = (2 * m - 1) * s * P[(m - 1, m - 1)]
    for m in range(0, order):
        P[(m + 1, m)] = (2 * m + 1) * x * P[(m, m)]
    for m in range(0, order + 1):
        for n in range(m + 2, order + 1):
            P[(n, m)] = ((2 * n - 1) * x * P[(n - 1, m)] - (n + m - 1) * P[(n - 2, m)]) / (n - m)
    from math import factorial
    for n in range(order + 1):
        for m in range(-n, n + 1):
            am = abs(m)
            norm = np.sqrt((2 * n + 1) * (2 - (m == 0)) * factorial(n - am) / factorial(n + am))
            trig = np.cos(am * azimuth) if m >= 0 else np.sin(am * azimuth)
            Y[acn(n, m)] = norm * P[(n, am)] * trig
    return Y


def cap_gram(order, el_min_deg, n_quad=256):
    """G = int over {elevation >= el_min} of y y^T, N3D, Gauss-Legendre in cos(colatitude)."""
    t, w = np.polynomial.legendre.leggauss(n_quad)
    cmin = np.sin(np.radians(el_min_deg))
    c = cmin + (1 - cmin) * (t + 1) / 2
    w = w * (1 - cmin) / 2
    n_az = 2 * order + 2  # exact for trig products up to degree 2*order
    az = 2 * np.pi * np.arange(n_az) / n_az
    C, AZ = np.meshgrid(c, az)
    W = np.tile(w, (n_az, 1)) * (2 * np.pi / n_az)
    Y = real_sh_n3d(order, AZ.ravel(), np.arccos(C.ravel()))
    return (Y * W.ravel()) @ Y.T


# ---------------------------------------------------------------------------
# generators. Column k corresponds to the k-th kept harmonic in ACN order.
# ---------------------------------------------------------------------------
def circular(order):
    L = (order + 1) ** 2
    cols = [0] + [acn(n, m) for n in range(1, order + 1) for m in (-n, n)]
    A = np.zeros((L, len(cols)))
    for k, row in enumerate(cols):
        A[row, k] = 1.0
    return A


def hemi(order, el_min_deg=None):
    el_min = EL_MIN_DEG[order] if el_min_deg is None else el_min_deg
    G = cap_gram(order, el_min)
    L = (order + 1) ** 2
    kept = [(n, m) for n in range(order + 1) for m in range(-n, n + 1) if (n + m) % 2 == 0]
    A = np.zeros((L, len(kept)))
    for m in range(-order, order + 1):
        rows = [acn(n, m) for n in range(abs(m), order + 1)]
        slots = [k for k, (n, mm) in enumerate(kept) if mm == m]
        ev, V = np.linalg.eigh(G[np.ix_(rows, rows)])
        V = V[:, np.argsort(ev)[::-1][: len(slots)]]
        for j, k in enumerate(slots):
            v = V[:, j]
            v = v * np.sign(v[np.argmax(np.abs(v))])  # largest entry positive
            A[rows, k] = v
    return A


# ---------------------------------------------------------------------------
# header I/O, same layout as the shipped files
# ---------------------------------------------------------------------------
def fmt(v):
    return "%.10f" % (float(np.float32(v)) + 0.0)


def write_header(name, mats, with_dims):
    out = []
    for order, A in enumerate(mats, 1):
        L, C = A.shape
        out.append("/* adapter matrix %dx%d */\n" % (L, C))
        out.append("static const float %s_order%d[] = { \n" % (name, order))
        rows = ["    " + ", ".join(fmt(v) for v in A[r]) for r in range(L)]
        out.append(", \n".join(rows) + "\n};\n\n")
        if with_dims:
            out.append("static const ambix_matrix_t mtx_%s_order%d = { %d, %d, NULL };\n\n\n" % (name, order, L, C))
        else:
            out.append("\n\n")
    out.append("\n\n" if with_dims else "")
    out.append("static const float* %s[] = {\n" % name)
    out.append(",\n".join("    %s_order%d" % (name, o) for o in range(1, len(mats) + 1)) + "\n};\n\n")
    if with_dims:
        out.append("static const ambix_matrix_t mtx_%s[] = {\n" % name)
        out.append(",\n".join("    mtx_%s_order%d" % (name, o) for o in range(1, len(mats) + 1)) + "\n};\n\n")
    return "".join(out)


def read_header(path, name):
    src = open(path).read()
    mats = {}
    pat = r"adapter matrix (\d+)x(\d+) \*/\s*static const float %s_order(\d+)\[\] = \{(.*?)\};" % name
    for m in re.finditer(pat, src, re.S):
        L, C, order = int(m[1]), int(m[2]), int(m[3])
        vals = [float(v) for v in m[4].replace("\n", " ").split(",") if v.strip()]
        mats[order] = np.array(vals).reshape(L, C)
    return mats


FAMILIES = {
    "circular": ("adapter_circular", circular, False),
    "hemi": ("adapter_hemi", hemi, True),
}


def generate(family):
    name, fn, dims = FAMILIES[family]
    return write_header(name, [fn(o) for o in range(1, MAX_ORDER + 1)], dims)


def check():
    ok = True

    # the harmonics themselves: orthonormal over the full sphere
    G = cap_gram(MAX_ORDER, -90.0)
    err = np.abs(G / (4 * np.pi) - np.eye(len(G))).max()
    print("N3D harmonics orthonormal on the sphere: %.1e" % err)
    ok &= err < 1e-10

    for family, (name, fn, _) in FAMILIES.items():
        path = os.path.join(HEADER_DIR, name + ".h")
        shipped = read_header(path, name)
        if family == "circular":
            same = open(path).read() == generate(family)
            print("%-8s regenerated header byte-identical: %s" % (family, same))
            ok &= same
        for order in range(1, MAX_ORDER + 1):
            A, S = fn(order), shipped[order]
            ortho = np.abs(A.T @ A - np.eye(A.shape[1])).max()
            sub = np.abs(A @ A.T - S @ np.linalg.pinv(S)).max()  # compare subspaces, not bases
            print("%-8s N=%d  %2dx%-2d  |AtA-I|=%.1e  subspace diff vs shipped=%.1e" % (family, order, *A.shape, ortho, sub))
            ok &= A.shape == S.shape and ortho < 1e-9 and sub < (1e-6 if family == "circular" else 5e-2)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("family", nargs="?", choices=sorted(FAMILIES))
    ap.add_argument("-o", "--output", help="write the header here instead of stdout")
    ap.add_argument("--check", action="store_true", help="verify the generators against the shipped headers")
    args = ap.parse_args()
    if args.check:
        return check()
    if not args.family:
        ap.error("give a family (circular, hemi) or --check")
    text = generate(args.family)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
