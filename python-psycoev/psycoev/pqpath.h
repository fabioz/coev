/* pqpath.h - definitions for pqpath.c
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

#ifndef PSYCOPG_PQPATH_H
#define PSYCOPG_PQPATH_H 1

#include "psycoev/config.h"
#include "psycoev/cursor.h"
#include "psycoev/connection.h"

/* macros to clean the pg result */
#define IFCLEARPGRES(pgres)  if (pgres) {PQclear(pgres); pgres = NULL;}
#define CLEARPGRES(pgres)    PQclear(pgres); pgres = NULL

/* exported functions */

HIDDEN PGconn *pqp_connect(const char *conninfo, double pg_io_timeout);
HIDDEN PGresult *pqp_exec(PGconn *conn, const char *command, double pg_io_timeout);
HIDDEN void pq_close(connectionObject *conn);

HIDDEN int pq_fetch(cursorObject *curs);
HIDDEN int pq_execute(cursorObject *curs, const char *query);
HIDDEN int pq_commit(connectionObject *conn);

HIDDEN int pq_abort(connectionObject *conn);
HIDDEN int pq_is_busy(connectionObject *conn);

HIDDEN int pq_check_result(PGconn *pgconn, PGresult *pgres);
#endif /* !defined(PSYCOPG_PQPATH_H) */
