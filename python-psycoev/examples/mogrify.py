# mogrify.py - test all possible simple type mogrifications
# -*- encoding: latin1 -*-
#
# Copyright (C) 2004 Federico Di Gregorio  <fog@debian.org>
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

import sys, psycoev

if len(sys.argv) > 1:
    DSN = sys.argv[1]

print "Opening connection using dns:", DSN

conn = psycoev.connect(DSN)
print "Encoding for this connection is", conn.encoding

curs = conn.cursor()
curs.execute("SELECT %(foo)s AS foo", {'foo':'bar'})
curs.execute("SELECT %(foo)s AS foo", {'foo':None})
curs.execute("SELECT %(foo)s AS foo", {'foo':True})
curs.execute("SELECT %(foo)s AS foo", {'foo':42})
curs.execute("SELECT %(foo)s AS foo", {'foo':u'yatt�!'})
curs.execute("SELECT %(foo)s AS foo", {'foo':u'bar'})

print curs.mogrify("SELECT %(foo)s AS foo", {'foo':'bar'})
print curs.mogrify("SELECT %(foo)s AS foo", {'foo':None})
print curs.mogrify("SELECT %(foo)s AS foo", {'foo':True})
print curs.mogrify("SELECT %(foo)s AS foo", {'foo':42})
print curs.mogrify("SELECT %(foo)s AS foo", {'foo':u'yatt�!'})
print curs.mogrify("SELECT %(foo)s AS foo", {'foo':u'bar'})

conn.rollback()
