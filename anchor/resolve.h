/* SPDX-License-Identifier: GPL-3.0-only
 *
 * resolve.h: find the ANLZ analysis pair for the track a deck has loaded.
 *
 * The deck does not expose the loaded track's file path. The guide tries, in order:
 *   1. a USBANLZ file EP147 has open, with PPTH matching this track's title,
 *   2. a matching audio file EP147 has open, whose path locates the analysis by rekordbox's
 *      documented folder hash,
 *   3. the rekordbox library service (id + title + known artist -> unambiguous path), then the hash.
 * Unidentified ANLZ files, mismatched ids/titles/artists and a lone unrelated audio fd are
 * not accepted as evidence. Known PPTH/content paths are compared with directories intact.
 * The track's analysis is read straight from the USB; nothing is written.
 */
#ifndef ANCHOR_RESOLVE_H
#define ANCHOR_RESOLVE_H

#include "anlz.h"
#include "azmod-mod.h"

int an_resolve_analysis(const az_core_v1 *C, const char *mod_id, uint32_t content_id,
                        const char *title, const char *artist, an_anlz_t *out,
                        char *why, int why_n);

/* The rekordbox analysis-folder hash for a content path: "P0xx/xxxxxxxx". Exposed for the
 * host test, which checks it against a row from a real export.pdb. */
void an_anlz_folder_hash(const char *content_path, char *out, size_t out_n);

#endif
