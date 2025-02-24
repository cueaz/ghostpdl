/* Copyright (C) 2001-2023 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  39 Mesa Street, Suite 108A, San Francisco,
   CA 94129, USA, for further information.
*/


/* ASCII85Decode filter interface */
/* Requires scommon.h; strimpl.h if any templates are referenced */

#ifndef sa85d_INCLUDED
#  define sa85d_INCLUDED

#include "scommon.h"

/* ASCII85Decode */
typedef struct stream_A85D_state_s {
    stream_state_common;
    int odd;			/* # of odd digits */
    ulong word;			/* word being accumulated */
    bool pdf_rules;             /* hacks and tweaks for PDF */
    bool require_eod;		/* ~> is required for tokens */
} stream_A85D_state;

#define private_st_A85D_state()	/* in sfilter2.c */\
  gs_private_st_simple(st_A85D_state, stream_A85D_state,\
    "ASCII85Decode state")
/* We define the initialization procedure here, so that the scanner */
/* can avoid a procedure call. */
#define s_A85D_init_inline(ss)\
  BEGIN \
  (ss)->min_left = 1; \
  (ss)->word = 0; \
  (ss)->odd = 0; \
  /* pdf_rules should not be initialized here. This flag is initialised in\
   * zA85D to either true or false, and this inline function is called *after*\
   * that in s_A85D_init to do the remaining initialisation. However, this\
   * inline function is also called from gs_scan_token to handle inline\
   * ASCII85 strings. These are not to be interpreted using PDF rules\
   * and so we must not set the flag here, but in the relevant calling\
   * functions.\
   */ \
  (ss)->require_eod=false; \
  END
extern const stream_template s_A85D_template;

#endif /* sa85d_INCLUDED */
