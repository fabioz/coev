/* pqpath.c - single path into libpq
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

/* IMPORTANT NOTE: no function in this file do its own connection locking
   except for pg_execute and pq_fetch (that are somehow high-level. This means
   that all the othe functions should be called while holding a lock to the
   connection.
*/

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

/* #include "modcoev.h" */

#define PSYCOPG_MODULE
#include "psycoev/config.h"
#include "psycoev/python.h"
#include "psycoev/psycoev.h"
#include "psycoev/pqpath.h"
#include "psycoev/connection.h"
#include "psycoev/cursor.h"
#include "psycoev/typecast.h"
#include "psycoev/pgtypes.h"
#include "psycoev/pgversion.h"


/* query/response handling interface on top of coev. */
/* old cruft, v2 proto and threading support dumped. */
/* error handling as follows: psyco_set_error is not supplied any 
   internal pg info. Any error that postgres reports is handled here. */
   
#ifndef HAVE_PQPROTOCOL3
#error postgres >=7.4, protocol >=3 are required.
#endif


static int pq_raise_from_conn(PGconn *pgconn);

/**** building blocks ****/

#include "ucoev.h"

/* raises appropriate exception and always returns -1 */ 
static int
pqp_raise_from_coev(void) {
    switch (coev_current()->status) {
        case CSW_TIMEOUT:
            PyErr_SetString(Error, "I/O Timeout");
        default:
            PyErr_SetString(Error, "Unexpected status after coev_wait()");
    }
    return -1;
}

/* connection flusher, raises appropriate exceptions. */
static int
pqp_flush(PGconn *conn, double pg_io_timeout) {
    int fd = PQsocket(conn);
    
    while (1) {
	switch(PQflush(conn)) {
	    case 0:
		/* success */
		return 0;
	    case -1:
		/* fail */
		return pq_raise_from_conn(conn);
	    case 1:
                Py_BEGIN_ALLOW_THREADS
		coev_wait(fd, COEV_WRITE, pg_io_timeout);
                Py_END_ALLOW_THREADS
		break;
	    default:
		/* insanity */
		return -1;
	}
        if (coev_current()->status != CSW_EVENT)
            return pqp_raise_from_coev();
    }
}

/* result reader */
static int
pqp_consume_input(PGconn *conn, double pg_io_timeout) {
    int fd = PQsocket(conn);
    while(PQisBusy(conn) == 1) {
        Py_BEGIN_ALLOW_THREADS
	coev_wait(fd, COEV_READ, pg_io_timeout);
        Py_END_ALLOW_THREADS
        if (coev_current()->status != CSW_EVENT)
            return pqp_raise_from_coev();
	if (PQconsumeInput(conn) != 1)
	    /* some fail. PQconsumeInput()
               returns 0 at fail, 1 otherwise */
	    return pq_raise_from_conn(conn); 
    }
    return 0;
}

PGconn *
pqp_connect(const char *conninfo, double pg_io_timeout) {
    PGconn *conn;
    ConnStatusType status;
    int fd;
    
    conn = PQconnectStart(conninfo);
    if (conn == NULL) {
	/* libpq allocation fail */
	return NULL;
    }
    status = PQstatus(conn);
    if (status == CONNECTION_BAD) {
	/* some other fail */
	PQfinish(conn);
	return NULL;
    }
    fd = PQsocket(conn);
    while(1) {
	switch (PQconnectPoll(conn)) {
	    case PGRES_POLLING_READING:
                Py_BEGIN_ALLOW_THREADS
		coev_wait(fd, COEV_READ, pg_io_timeout);
                Py_END_ALLOW_THREADS
		break;
	    case PGRES_POLLING_WRITING:
                Py_BEGIN_ALLOW_THREADS
		coev_wait(fd, COEV_WRITE, pg_io_timeout);
                Py_END_ALLOW_THREADS
		break;
	    case PGRES_POLLING_OK:
		return conn;
	    case PGRES_POLLING_FAILED:
		/* connect failed */
		PQfinish(conn);
		return NULL;
	    default:
		/* some crazy return status */
		return NULL;
	}
        if (coev_current()->status == CSW_EVENT)
            continue;
        /* timeout or something */
        PQfinish(conn);
        return NULL;
    }
}

static int
pqp_putcopyend(PGconn *conn, const char *errormsg, double pg_io_timeout) {
    int rv, fd;
    
    fd = PQsocket(conn);
    while (1) {
        rv = PQputCopyEnd(conn, errormsg);
        if (rv == -1)
            return -1;
        if (rv == 1)
            return 0;
        
        Py_BEGIN_ALLOW_THREADS
        coev_wait(fd, COEV_WRITE, pg_io_timeout);
        Py_END_ALLOW_THREADS
        if (coev_current()->status != CSW_EVENT)
            return pqp_raise_from_coev();
    }
}

static int
pqp_putcopydata(PGconn *conn, const char *data, int len, double pg_io_timeout) {
    int rv, fd;
    
    fd = PQsocket(conn);
    while (1) {
        rv = PQputCopyData(conn, data, len);
        if (rv == -1)
            return pq_raise_from_conn(conn);
        if (rv == 1)
            return 0;
        
        Py_BEGIN_ALLOW_THREADS
        coev_wait(fd, COEV_WRITE, pg_io_timeout);
        Py_END_ALLOW_THREADS
        if (coev_current()->status != CSW_EVENT)
            return pqp_raise_from_coev();
    }
}


/* if **buffer is null, data is dropped mercilessly. */
static int
pqp_getcopydata(PGconn *conn, char **buffer, double pg_io_timeout) {
    char *buf;
    int rv; 
    
    while (1) {
        rv = PQgetCopyData(conn, &buf, 1);
        if (rv > 0) { /* got some, drop&continue */
            if (buffer) {
                *buffer = buf;
                return 0;
            }
            PQfreemem(buf);
            continue;
        }
        if (rv == -1) /* copy is done, result is handled in caller. */
            return 0;
        
        if (rv == -2) /* PQ error */
            return pq_raise_from_conn(conn);
        
        rv = pqp_consume_input(conn, pg_io_timeout);
        if (rv == -1)
            return -1;
    }
}

static int
pqp_discard_results(PGconn *conn, double pg_io_timeout) {
    ExecStatusType status;
    PGresult *result;
    int rv;
    
    while(1) {
        if (pqp_consume_input(conn, pg_io_timeout))
            return -1;
        result  = PQgetResult(conn);
        if (result == NULL)
            return 0;
        status = PQresultStatus(result);
        PQclear(result);
        rv = 0;
        if (status == PGRES_COPY_IN)
            rv = pqp_putcopyend(conn, "pqp_discard_results()", pg_io_timeout);
        else if (status == PGRES_COPY_OUT)
            rv = pqp_getcopydata(conn, NULL, pg_io_timeout);
        if (rv < 0)
            return -1;
    }
}

/** does not clear pgres. returns it, or NULL if dispatch failed 
    (or zero results from command ?). 
    subsequent results must be fetched as follows:
	pqp_consume_input(conn);
	if (pq_check(conn, NULL))
	    return WHATEVER_ERROR_CONDITION;
	res = PQgetResult(conn);
*/
PGresult *
pqp_exec(PGconn *conn, const char *command, double pg_io_timeout) {
    /* drop incoming */
    if (pqp_discard_results(conn, pg_io_timeout)) 
        return NULL;
    
    if (!PQsendQuery(conn, command)) {
	/* dispatch fail */
        pq_raise_from_conn(conn);
	return NULL;
    }
    
    /* flush data to server */
    if (pqp_flush(conn, pg_io_timeout)) 
	return NULL;
    
    /*  read result */
    if (pqp_consume_input(conn, pg_io_timeout))
	return NULL;
    
    /*  return it */
    return PQgetResult(conn);
}

/*** building blocks end ***/

/* Strip off the severity from a Postgres error message. */
static const char *
strip_severity(const char *msg)
{
    if (!msg)
        return NULL;

    if (strlen(msg) > 8 && (!strncmp(msg, "ERROR:  ", 8) ||
                            !strncmp(msg, "FATAL:  ", 8) ||
                            !strncmp(msg, "PANIC:  ", 8)))
        return &msg[8];
    else
        return msg;
}

/* Returns the Python exception corresponding to an SQLSTATE error
   code.  A list of error codes can be found at:

   http://www.postgresql.org/docs/current/static/errcodes-appendix.html */
static PyObject *
exception_from_sqlstate(const char *sqlstate)
{
    switch (sqlstate[0]) {
    case '0':
        switch (sqlstate[1]) {
        case 'A': /* Class 0A - Feature Not Supported */
            return NotSupportedError;
        }
        break;
    case '2':
        switch (sqlstate[1]) {
        case '1': /* Class 21 - Cardinality Violation */
            return ProgrammingError;
        case '2': /* Class 22 - Data Exception */
            return DataError;
        case '3': /* Class 23 - Integrity Constraint Violation */
            return IntegrityError;
        case '4': /* Class 24 - Invalid Cursor State */
        case '5': /* Class 25 - Invalid Transaction State */
            return InternalError;
        case '6': /* Class 26 - Invalid SQL Statement Name */
        case '7': /* Class 27 - Triggered Data Change Violation */
        case '8': /* Class 28 - Invalid Authorization Specification */
            return OperationalError;
        case 'B': /* Class 2B - Dependent Privilege Descriptors Still Exist */
        case 'D': /* Class 2D - Invalid Transaction Termination */
        case 'F': /* Class 2F - SQL Routine Exception */
            return InternalError;
        }
        break;
    case '3':
        switch (sqlstate[1]) {
        case '4': /* Class 34 - Invalid Cursor Name */
            return OperationalError;
        case '8': /* Class 38 - External Routine Exception */
        case '9': /* Class 39 - External Routine Invocation Exception */
        case 'B': /* Class 3B - Savepoint Exception */
            return InternalError;
        case 'D': /* Class 3D - Invalid Catalog Name */
        case 'F': /* Class 3F - Invalid Schema Name */
            return ProgrammingError;
        }
        break;
    case '4':
        switch (sqlstate[1]) {
        case '0': /* Class 40 - Transaction Rollback */
#ifdef PSYCOPG_EXTENSIONS
            return TransactionRollbackError;
#else
            return OperationalError;
#endif
        case '2': /* Class 42 - Syntax Error or Access Rule Violation */
        case '4': /* Class 44 - WITH CHECK OPTION Violation */
            return ProgrammingError;
        }
        break;
    case '5':
        /* Class 53 - Insufficient Resources
           Class 54 - Program Limit Exceeded
           Class 55 - Object Not In Prerequisite State
           Class 57 - Operator Intervention
           Class 58 - System Error (errors external to PostgreSQL itself) */
#ifdef PSYCOPG_EXTENSIONS
        if (!strcmp(sqlstate, "57014"))
            return QueryCanceledError;
        else
#endif
            return OperationalError;
    case 'F': /* Class F0 - Configuration File Error */
        return InternalError;
    case 'P': /* Class P0 - PL/pgSQL Error */
        return InternalError;
    case 'X': /* Class XX - Internal Error */
        return InternalError;
    }
    /* return DatabaseError as a fallback */
    return DatabaseError;
}

/* pq_raise - raise a python exception of the right kind

   This function should be called while holding the GIL. */

static void
pq_raise(connectionObject *conn, cursorObject *curs, PGresult *pgres)
{
    PyObject *pgc = (PyObject*)curs;
    PyObject *exc = NULL;
    const char *err = NULL;
    const char *err2 = NULL;
    const char *code = NULL;

    if (conn == NULL) {
        PyErr_SetString(Error, "psycoev went psycotic and raised a null error");
        return;
    }
    
    /* if the connection has somehow beed broken, we mark the connection
       object as closed but requiring cleanup */
    if (conn->pgconn != NULL && PQstatus(conn->pgconn) == CONNECTION_BAD)
        conn->closed = 2;

    if (pgres == NULL && curs != NULL)
        pgres = curs->pgres;

    if (pgres) {
        err = PQresultErrorMessage(pgres);
        if (err != NULL) {
            code = PQresultErrorField(pgres, PG_DIAG_SQLSTATE);
        }
    }
    if (err == NULL)
        err = PQerrorMessage(conn->pgconn);

    /* if the is no error message we probably called pq_raise without reason:
       we need to set an exception anyway because the caller will probably
       raise and a meaningful message is better than an empty one */
    if (err == NULL) {
        PyErr_SetString(Error, "psycoev went psycotic without error set");
        return;
    }

    /* Analyze the message and try to deduce the right exception kind
       (only if we got the SQLSTATE from the pgres, obviously) */
    if (code != NULL) {
        exc = exception_from_sqlstate(code);
    }

    /* if exc is still NULL psycoev was not built with HAVE_PQPROTOCOL3 or the
       connection is using protocol 2: in both cases we default to comparing
       error messages */
    if (exc == NULL) {
        if (!strncmp(err, "ERROR:  Cannot insert a duplicate key", 37)
            || !strncmp(err, "ERROR:  ExecAppend: Fail to add null", 36)
            || strstr(err, "referential integrity violation"))
            exc = IntegrityError;
        else if (strstr(err, "could not serialize") ||
                 strstr(err, "deadlock detected"))
#ifdef PSYCOPG_EXTENSIONS
            exc = TransactionRollbackError;
#else
            exc = OperationalError;
#endif
        else
            exc = ProgrammingError;
    }

    /* try to remove the initial "ERROR: " part from the postgresql error */
    err2 = strip_severity(err);

    psyco_set_error(exc, pgc, err2, err, code);
}

static int
pq_raise_from_result(PGresult *pgres) {
    const char *err, *err2, *code;
    PyObject *exc = NULL;
    
    err = PQresultErrorMessage(pgres);
    if (err != NULL) {
        code = PQresultErrorField(pgres, PG_DIAG_SQLSTATE);
        if (code != NULL) {
            exc = exception_from_sqlstate(code);
        }
    } else {
        Dprintf("pq_raise_from_result: no error message for result structute with error status");
        PyErr_SetString(Error, "no error message for result structute with error status");
        return -1;
    }
    
    /* if exc is still NULL psycoev was not built with HAVE_PQPROTOCOL3 or the
       connection is using protocol 2: in both cases we default to comparing
       error messages */
    if (exc == NULL) {
        if (!strncmp(err, "ERROR:  Cannot insert a duplicate key", 37)
            || !strncmp(err, "ERROR:  ExecAppend: Fail to add null", 36)
            || strstr(err, "referential integrity violation"))
            exc = IntegrityError;
        else if (strstr(err, "could not serialize") ||
                 strstr(err, "deadlock detected"))
#ifdef PSYCOPG_EXTENSIONS
            exc = TransactionRollbackError;
#else
            exc = OperationalError;
#endif
        else
            exc = ProgrammingError;
    }
    Dprintf("pq_raise_from_result: %s", err);

    /* try to remove the initial "ERROR: " part from the postgresql error */
    err2 = strip_severity(err);
    psyco_set_error(exc, NULL, err2, err, code);
    return -1;
}

/* sets an exception and always returns -1 */
static int
pq_raise_from_conn(PGconn *pgconn) {
    const char *err, *err2;
    PyObject *exc;
    
    err = PQerrorMessage(pgconn);
    if (!err) {
        PyErr_SetString(Error, "no error message for CONNECTION_BAD connection");
        return -1;
    }
    
    if (!strncmp(err, "ERROR:  Cannot insert a duplicate key", 37)
        || !strncmp(err, "ERROR:  ExecAppend: Fail to add null", 36)
        || strstr(err, "referential integrity violation"))
        exc = IntegrityError;
    else if (strstr(err, "could not serialize") ||
             strstr(err, "deadlock detected"))
#ifdef PSYCOPG_EXTENSIONS
            exc = TransactionRollbackError;
#else
            exc = OperationalError;
#endif
    else
        exc = ProgrammingError;
    
    Dprintf("pq_raise_from_conn: %s", err);
    /* try to remove the initial "ERROR: " part from the postgresql error */
    err2 = strip_severity(err);
    psyco_set_error(exc, NULL, err2, err, NULL);
    return -1;
}

/** pq_check_result - check supplied connection and result.
    clear the result and raise appropriate exception 
    if something's fishy.

    returns:
            0 if all is ok.
           -1 iff an exception was raised.

*/

int
pq_check_result(PGconn *pgconn, PGresult *pgres) {
    if (pgres) {
        switch(PQresultStatus(pgres)) {
            case PGRES_BAD_RESPONSE:
            case PGRES_FATAL_ERROR:
                pq_raise_from_result(pgres);
                PQclear(pgres);
                return -1;
            case PGRES_NONFATAL_ERROR:
                /* maybe warn or something */
            default:
                break;
        }
    }
    if (pgconn) {
        /* check connection just in case */
        if (PQstatus(pgconn) != CONNECTION_OK)
            return pq_raise_from_conn(pgconn);

        Dprintf("pq_check_result: OK.");
        return 0;
    }
    
    /* epic fail */
    PyErr_SetString(ProgrammingError, "pq_check_result called with nulls");
    return -1;
}

/*  pq_begin  - begin a transaction.
    sets up an exception and  returns -1 on error
    transaction is in progress, is not an error 
    no cleanup necessary.
*/
int
pq_begin(connectionObject *conn) {
    PGresult *pgres;
    
    const char *query[] = {
        NULL,
        "BEGIN; SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
        "BEGIN; SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};

    Dprintf("pq_begin: pgconn = %p, isolevel = %ld, status = %d",
            conn->pgconn, conn->isolation_level, conn->status);

    if (    (conn->isolation_level == 0)
         || (conn->status != CONN_STATUS_READY)) {
        Dprintf("pq_begin: transaction in progress");
        return 0;
    }

    pgres = pqp_exec(conn->pgconn, query[conn->isolation_level], conn->pg_io_timeout);
    if (pq_check_result(conn->pgconn, pgres) == -1)
        return -1;

    PQclear(pgres);
    conn->status = CONN_STATUS_BEGIN;
    return 0;
}

/* pq_commit - send an END, if necessary */

int
pq_commit(connectionObject *conn)
{
    PGresult *pgres;

    Dprintf("pq_commit: pgconn = %p, isolevel = %ld, status = %d",
            conn->pgconn, conn->isolation_level, conn->status);

    if (    (conn->isolation_level == 0) 
         || (conn->status != CONN_STATUS_BEGIN)) {
        Dprintf("pq_commit: no transaction to commit");
        return 0;
    }

    conn->mark += 1;

    pgres = pqp_exec(conn->pgconn, "COMMIT", conn->pg_io_timeout);
        
    /* Even if an error occurred, the connection will be rolled back,
       so we unconditionally set the connection status here. */
    conn->status = CONN_STATUS_READY;

    if (pq_check_result(conn->pgconn, pgres) == -1)
        return -1;
    
    PQclear(pgres);
    return 0;
}

int
pq_abort(connectionObject *conn)
{
    PGresult *pgres;

    Dprintf("pq_abort: pgconn = %p, isolevel = %ld, status = %d",
            conn->pgconn, conn->isolation_level, conn->status);

    if (    (conn->isolation_level == 0) 
         || (conn->status != CONN_STATUS_BEGIN)) {
        Dprintf("pq_abort_locked: no transaction to abort");
        return 0;
    }

    conn->mark += 1;
    
    pgres = pqp_exec(conn->pgconn, "ROLLBACK", conn->pg_io_timeout);
    if (pq_check_result(conn->pgconn, pgres))
        return -1;
    
    conn->status = CONN_STATUS_READY;

    PQclear(pgres);
    return 0;
}


static void
pq_close_runner(coev_t *c) {
    connectionObject *conn = (connectionObject *)(c->A);
    
    /* execute a forced rollback on the connection (but don't check the
       result, we're going to close the pq connection anyway */
    if (conn->pgconn) {
	if (conn->closed == 1)
	    pq_abort(conn);
    
        PQfinish(conn->pgconn);
        Dprintf("pq_close_runner: PQfinish called");
        conn->pgconn = NULL;
    }
    Dprintf("pq_close_runner: finished.");
}

/* create a coroutine to perform the cleanup on connection */
/* problem is, if there is no scheduler, we must not do 
   Py_BEGIN_ALLOW_THREADS/Py_END_ALLOW_THREADS because pq_abort's pqp_exec
   will do this for us. */
void 
pq_close(connectionObject *conn) {
    coev_t *closer = PyMem_Malloc(sizeof(coev_t));
    coev_t *sched;
    
    if (closer == NULL)
        Py_FatalError("pq_close(): no memory (psycoev)");
    
    closer = coev_new(&pq_close_runner, 8*4096);
    closer->A = conn;
    coev_schedule(closer);
    
    sched = coev_loop();
    
    if (sched != NULL) {
        Py_BEGIN_ALLOW_THREADS        
        if (coev_stall() == CSCHED_NOSCHEDULER) {
            Dprintf("pq_close: CSCHED_NOSCHEDULER from coev_stall(), but coev_loop() returned [%s]", sched->treepos);
            Py_FatalError("pq_close: unpossible contradiction in scheduler existence.");
        }
        Dprintf("pq_close: control is back from coev_stall(), doing Py_END_ALLOW_THREADS.");
        Py_END_ALLOW_THREADS
    }
    
    if ((closer->state != CSTATE_DEAD) && (closer->state != CSTATE_ZERO) )
        /* this is suprising */
        Py_FatalError("pq_close(): closer coroutine not dead: now what?");
    
}


/* pq_is_busy - consume input and return connection status

   a status of 1 means that a call to pq_fetch will block, while a status of 0
   means that there is data available to be collected. -1 means an error, the
   exception will be set accordingly.

   this fucntion locks the connection object
   this function call Py_*_ALLOW_THREADS macros */

/* used here and in cursor_type.c */
int
pq_is_busy(connectionObject *conn)
{
    int res;
    PGnotify *pgn;

    Dprintf("pq_is_busy: consuming input");

    if (PQconsumeInput(conn->pgconn) == 0) {
        Dprintf("pq_is_busy: PQconsumeInput() failed");
        PyErr_SetString(OperationalError, PQerrorMessage(conn->pgconn));
        return -1;
    }

    /* now check for notifies */
    while ((pgn = PQnotifies(conn->pgconn)) != NULL) {
        PyObject *notify;

        Dprintf("pq_is_busy: got NOTIFY from pid %d, msg = %s",
                (int) pgn->be_pid, pgn->relname);

        notify = PyTuple_New(2);
        PyTuple_SET_ITEM(notify, 0, PyInt_FromLong((long)pgn->be_pid));
        PyTuple_SET_ITEM(notify, 1, PyString_FromString(pgn->relname));
        PyList_Append(conn->notifies, notify);
        free(pgn);
    }

    res = PQisBusy(conn->pgconn);
    return res;
}

/** pq_execute - execute a query, possibly asyncronously

    0 on success
    nonzero on failure.

    used only in cursor_type.c */
int
pq_execute(cursorObject *curs, const char *query)
{
    /* check status of connection, raise error if not OK */
    if (PQstatus(curs->conn->pgconn) != CONNECTION_OK) {
        Dprintf("pq_execute: connection NOT OK");
        PyErr_SetString(OperationalError, PQerrorMessage(curs->conn->pgconn));
        return -1;
    }
    
    Dprintf("pq_execute: pg connection at %p status %d (presuming OK)", 
        curs->conn->pgconn,
        PQstatus(curs->conn->pgconn)
    );

    if (pq_begin(curs->conn) == -1)
        return -1;

    curs->pgres = pqp_exec(curs->conn->pgconn, query, curs->conn->pg_io_timeout);
    if (pq_check_result(curs->conn->pgconn, curs->pgres))
        return -1;
    
    if (pq_fetch(curs) == -1) 
        return -1;

    return 0;
}


static void
_pq_fetch_tuples(cursorObject *curs)
{
    int i, *dsize = NULL;
    int pgnfields;
    int pgbintuples;

    pgnfields = PQnfields(curs->pgres);
    pgbintuples = PQbinaryTuples(curs->pgres);

    curs->notuples = 0;

    /* create the tuple for description and typecasting */
    Py_XDECREF(curs->description);
    Py_XDECREF(curs->casts);    
    curs->description = PyTuple_New(pgnfields);
    curs->casts = PyTuple_New(pgnfields);
    curs->columns = pgnfields;

    /* calculate the display size for each column (cpu intensive, can be
       switched off at configuration time) */
#ifdef PSYCOPG_DISPLAY_SIZE
    dsize = (int *)PyMem_Malloc(pgnfields * sizeof(int));
    
    if (dsize != NULL) {
        int j, len;
        for (i=0; i < pgnfields; i++) {
            dsize[i] = -1;
        }
        for (j = 0; j < curs->rowcount; j++) {
            for (i = 0; i < pgnfields; i++) {
                len = PQgetlength(curs->pgres, j, i);
                if (len > dsize[i]) dsize[i] = len;
            }
        }
    }
#endif

    /* calculate various parameters and typecasters */
    for (i = 0; i < pgnfields; i++) {
        Oid ftype = PQftype(curs->pgres, i);
        int fsize = PQfsize(curs->pgres, i);
        int fmod =  PQfmod(curs->pgres, i);

        PyObject *dtitem;
        PyObject *type;
        PyObject *cast = NULL;

        dtitem = PyTuple_New(7);
        PyTuple_SET_ITEM(curs->description, i, dtitem);

        /* fill the right cast function by accessing three different dictionaries:
           - the per-cursor dictionary, if available (can be NULL or None)
           - the per-connection dictionary (always exists but can be null)
           - the global dictionary (at module level)
           if we get no defined cast use the default one */

        type = PyInt_FromLong(ftype);
        Dprintf("_pq_fetch_tuples: looking for cast %d:", ftype);
        if (curs->string_types != NULL && curs->string_types != Py_None) {
            cast = PyDict_GetItem(curs->string_types, type);
            Dprintf("_pq_fetch_tuples:     per-cursor dict: %p", cast);
        }
        if (cast == NULL) {
            cast = PyDict_GetItem(curs->conn->string_types, type);
            Dprintf("_pq_fetch_tuples:     per-connection dict: %p", cast);
        }
        if (cast == NULL) {
            cast = PyDict_GetItem(psyco_types, type);
            Dprintf("_pq_fetch_tuples:     global dict: %p", cast);
        }
        if (cast == NULL) cast = psyco_default_cast;

        /* else if we got binary tuples and if we got a field that
           is binary use the default cast
           FIXME: what the hell am I trying to do here? This just can't work..
        */
        if (pgbintuples && cast == psyco_default_binary_cast) {
            Dprintf("_pq_fetch_tuples: Binary cursor and "
                    "binary field: %i using default cast",
                    PQftype(curs->pgres,i));
            cast = psyco_default_cast;
        }

        Dprintf("_pq_fetch_tuples: using cast at %p (%s) for type %d",
                cast, PyString_AS_STRING(((typecastObject*)cast)->name),
                PQftype(curs->pgres,i));
        Py_INCREF(cast);
        PyTuple_SET_ITEM(curs->casts, i, cast);

        /* 1/ fill the other fields */
        PyTuple_SET_ITEM(dtitem, 0,
                         PyString_FromString(PQfname(curs->pgres, i)));
        PyTuple_SET_ITEM(dtitem, 1, type);

        /* 2/ display size is the maximum size of this field result tuples. */
        if (dsize && dsize[i] >= 0) {
            PyTuple_SET_ITEM(dtitem, 2, PyInt_FromLong(dsize[i]));
        }
        else {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(dtitem, 2, Py_None);
        }

        /* 3/ size on the backend */
        if (fmod > 0) fmod = fmod - sizeof(int);
        if (fsize == -1) {
            if (ftype == NUMERICOID) {
                PyTuple_SET_ITEM(dtitem, 3,
                                 PyInt_FromLong((fmod >> 16) & 0xFFFF));
            }
            else { /* If variable length record, return maximum size */
                PyTuple_SET_ITEM(dtitem, 3, PyInt_FromLong(fmod));
            }
        }
        else {
            PyTuple_SET_ITEM(dtitem, 3, PyInt_FromLong(fsize));
        }

        /* 4,5/ scale and precision */
        if (ftype == NUMERICOID) {
            PyTuple_SET_ITEM(dtitem, 4, PyInt_FromLong((fmod >> 16) & 0xFFFF));
            PyTuple_SET_ITEM(dtitem, 5, PyInt_FromLong(fmod & 0xFFFF));
        }
        else {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(dtitem, 4, Py_None);
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(dtitem, 5, Py_None);
        }

        /* 6/ FIXME: null_ok??? */
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(dtitem, 6, Py_None);
    
    }

    if (dsize) {
        PyMem_Free(dsize);
   }
}

static int
_pq_copy_in_v3(cursorObject *curs)
{
    /* COPY FROM implementation when protocol 3 is available: this function
       uses the new PQputCopyData() and can detect errors and set the correct
       exception */
    PyObject *o;
    Py_ssize_t length = 0;
    int res, error = 0;

    while (1) {
        o = PyObject_CallMethod(curs->copyfile, "read",
            CONV_CODE_PY_SSIZE_T, curs->copysize);
        if (!o || !PyString_Check(o) || (length = PyString_Size(o)) == -1) {
            error = 1;
        }
        if (length == 0 || length > INT_MAX || error == 1) {
            /* .read() error */
            break;
        }

        /* FIXME: ugly Py_ssize_t->int cast */
        res = pqp_putcopydata(curs->conn->pgconn, 
            PyString_AS_STRING(o), (int) length, curs->conn->pg_io_timeout);
        Dprintf("_pq_copy_in_v3: sent %d bytes of data; res = %d",
            (int) length, res);

        Py_DECREF(o);
        
        if (res == -1) {
            /* backend error */
            error = 2;
            break;
        }
    }

    Dprintf("_pq_copy_in_v3: error = %d", error);

    /* 0 means that the copy went well, 2 that there was an error on the
       backend: in both cases we'll get the error message from the PQresult */
    Dprintf("_pq_copy_in_v3: copy ended; res = %d", res);
    IFCLEARPGRES(curs->pgres);
    switch (error) {
        case 0:
            res = pqp_putcopyend(curs->conn->pgconn, NULL, curs->conn->pg_io_timeout);
            break;
        case 1:
            res = pqp_putcopyend(curs->conn->pgconn, "error in .read() call", curs->conn->pg_io_timeout);
            break;
        case 2:
            /* do not try to do anything with connection if the backend already reported an error */
            curs->conn->closed = 2;
            return -1;
    }
    
    /* if the result is -1 exception has already been raised. */
    if (res == -1) {
        curs->conn->closed = 2;
        return -1;
    }
    
    
    /* and finally we grab the operation result from the backend.
       this actually kills any result returned, but that's how it was 
       in the original code. No idea why. */
    
    while (1) {
        res = pqp_consume_input(curs->conn->pgconn, curs->conn->pg_io_timeout);
        if (res == -1)
            return -1;
        
        curs->pgres = PQgetResult(curs->conn->pgconn);
        if (curs->pgres == NULL)
            break;
        
        if (pq_check_result(curs->conn->pgconn, curs->pgres) == -1)
            return -1;
        
        IFCLEARPGRES(curs->pgres);
    }
   
    return error == 0 ? 1 : -1;
}

static int
_pq_copy_out_v3(cursorObject *curs)
{
    PyObject *tmp = NULL;

    char *buffer;
    Py_ssize_t len;

    while (1) {
        Py_BEGIN_ALLOW_THREADS;
        len = PQgetCopyData(curs->conn->pgconn, &buffer, 0);
        Py_END_ALLOW_THREADS;

        if (len > 0 && buffer) {
            tmp = PyObject_CallMethod(curs->copyfile,
                            "write", "s#", buffer, len);
            PQfreemem(buffer);
            if (tmp == NULL)
                return -1;
            else
                Py_DECREF(tmp);
        }
        /* we break on len == 0 but note that that should *not* happen,
           because we are not doing an async call (if it happens blame
           postgresql authors :/) */
        else if (len <= 0) break;
    }

    if (len == -2) {
        pq_raise(curs->conn, curs, NULL);
        return -1;
    }

    /* and finally we grab the operation result from the backend */
    IFCLEARPGRES(curs->pgres);
    while ((curs->pgres = PQgetResult(curs->conn->pgconn)) != NULL) {
        if (PQresultStatus(curs->pgres) == PGRES_FATAL_ERROR)
            pq_raise(curs->conn, curs, NULL);
        IFCLEARPGRES(curs->pgres);
    }
    return 1;
}



/** pq_fetch - fetch data after a query

    this fucntion locks the connection object
    this function call Py_*_ALLOW_THREADS macros

    return value:
     -1 - some error occurred while calling libpq
      0 - no result from the backend but no libpq errors
      1 - result from backend (possibly data is ready)

    used here and in cursor_type.c */
int
pq_fetch(cursorObject *curs)
{
    int pgstatus, ex = -1;
    const char *rowcount;

    /* even if we fail, we remove any information about the previous query */
    curs_reset(curs);

    /* we check the result from the previous execute; if the result is not
       already there, we need to consume some input and go to sleep until we
       get something edible to eat */
    if (!curs->pgres) {
        Dprintf("pq_fetch: no data: this cannot be.");
        return 0;
    }

    pgstatus = PQresultStatus(curs->pgres);
    Dprintf("pq_fetch: pgstatus = %s", PQresStatus(pgstatus));

    /* backend status message */
    Py_XDECREF(curs->pgstatus);
    curs->pgstatus = PyString_FromString(PQcmdStatus(curs->pgres));

    switch(pgstatus) {
        case PGRES_COMMAND_OK:
            Dprintf("pq_fetch: command returned OK (no tuples)");
            rowcount = PQcmdTuples(curs->pgres);
            if (!rowcount || !rowcount[0])
              curs->rowcount = -1;
            else
              curs->rowcount = atoi(rowcount);
            curs->lastoid = PQoidValue(curs->pgres);
            CLEARPGRES(curs->pgres);
            ex = 1;
            break;

        case PGRES_COPY_OUT:
            Dprintf("pq_fetch: data from a COPY TO (no tuples)");
            ex = _pq_copy_out_v3(curs);
            curs->rowcount = -1;
            /* error caught by out glorious notice handler */
            if (PyErr_Occurred()) ex = -1;
            IFCLEARPGRES(curs->pgres);
            break;

        case PGRES_COPY_IN:
            Dprintf("pq_fetch: data from a COPY FROM (no tuples)");
            ex = _pq_copy_in_v3(curs);
            curs->rowcount = -1;
            /* error caught by out glorious notice handler */
            if (PyErr_Occurred()) ex = -1;
            IFCLEARPGRES(curs->pgres);
            break;

        case PGRES_TUPLES_OK:
            Dprintf("pq_fetch: data from a SELECT (got tuples)");
            curs->rowcount = PQntuples(curs->pgres);
            _pq_fetch_tuples(curs); ex = 0;
            /* don't clear curs->pgres, because it contains the results! */
            break;

        default:
            /* impossible: should be caught by pq_check_result() */
            Dprintf("pq_fetch: uh-oh, something FAILED");
            pq_raise(curs->conn, curs, NULL);
            IFCLEARPGRES(curs->pgres);
            ex = -1;
            break;
    }

    Dprintf("pq_fetch: fetching done.");

    return ex;
}
