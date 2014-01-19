/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU Public License v2 or v3 licenses,
 * or any future license endorsed by Mnemosyne LLC.
 *
 * $Id$
 */

#ifndef GTR_ICON_H
#define GTR_ICON_H

#include <gtk/gtk.h>
#include "tr-core.h"

gpointer  gtr_icon_new    (TrCore * core);
void      gtr_icon_refresh (gpointer);

#endif /* GTR_ICON_H */
