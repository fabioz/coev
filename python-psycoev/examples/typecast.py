# typecast.py - example of per-cursor and per-connection typecasters.
#
# Copyright (C) 2001-2007 Federico Di Gregorio  <fog@debian.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTIBILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

## put in DSN your DSN string

DSN = 'dbname=test'

## don't modify anything below this line (except for experimenting)

class SimpleQuoter(object):
    def sqlquote(x=None):
        return "'bar'"

import sys
import psycoev
import psycoev.extensions

if len(sys.argv) > 1:
    DSN = sys.argv[1]

print "Opening connection using dns:", DSN
conn = psycoev.connect(DSN)
print "Encoding for this connection is", conn.encoding

curs = conn.cursor()
curs.execute("SELECT 'text'::text AS foo")
textoid = curs.description[0][1]
print "Oid for the text datatype is", textoid

def castA(s, curs):
    if s is not None: return "(A) " + s
TYPEA = psycoev.extensions.new_type((textoid,), "TYPEA", castA)

def castB(s, curs):
    if s is not None: return "(B) " + s
TYPEB = psycoev.extensions.new_type((textoid,), "TYPEB", castB)

curs = conn.cursor()
curs.execute("SELECT 'some text.'::text AS foo")
print "Some text from plain connection:", curs.fetchone()[0]

psycoev.extensions.register_type(TYPEA, conn)
curs = conn.cursor()
curs.execute("SELECT 'some text.'::text AS foo")
print "Some text from connection with typecaster:", curs.fetchone()[0]

curs = conn.cursor()
psycoev.extensions.register_type(TYPEB, curs)
curs.execute("SELECT 'some text.'::text AS foo")
print "Some text from cursor with typecaster:", curs.fetchone()[0]

curs = conn.cursor()
curs.execute("SELECT 'some text.'::text AS foo")
print "Some text from connection with typecaster again:", curs.fetchone()[0]


