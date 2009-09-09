/* cursor_int.c - code used by the cursor object
 *
 * Copyright (C) 2003 Federico Di Gregorio <fog@debian.org>
 *
 * This file is part of psycoev.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

#define PSYCOPG_MODULE
#include "psycoev/config.h"
#include "psycoev/psycoev.h"
#include "psycoev/cursor.h"
#include "psycoev/pqpath.h"

/* curs_reset - reset the cursor to a clean state */

void
curs_reset(cursorObject *self)
{
    PyObject *tmp;

    /* initialize some variables to default values */
    self->notuples = 1;
    self->rowcount = -1;
    self->row = 0;

    tmp = self->description;
    Py_INCREF(Py_None);
    self->description = Py_None;
    Py_XDECREF(tmp);

    tmp = self->casts;
    self->casts = NULL;
    Py_XDECREF(tmp);
}
