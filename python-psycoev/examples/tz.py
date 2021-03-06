# tz.py - example of datetime objects with time zones
# -*- encoding: utf8 -*-
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

import sys
import psycoev
import datetime

from psycoev.tz import ZERO, LOCAL, FixedOffsetTimezone

if len(sys.argv) > 1:
    DSN = sys.argv[1]

print "Opening connection using dns:", DSN
conn = psycoev.connect(DSN)
curs = conn.cursor()

try:
    curs.execute("CREATE TABLE test_tz (t timestamp with time zone)")
except:
    conn.rollback()
    curs.execute("DROP TABLE test_tz")
    curs.execute("CREATE TABLE test_tz (t timestamp with time zone)")
conn.commit()

d = datetime.datetime(1971, 10, 19, 22, 30, 0, tzinfo=LOCAL)
curs.execute("INSERT INTO test_tz VALUES (%s)", (d,))
print "Inserted timestamp with timezone:", d
print "Time zone:", d.tzinfo.tzname(d), "offset:", d.tzinfo.utcoffset(d)

tz = FixedOffsetTimezone(-5*60, "EST")
d = datetime.datetime(1971, 10, 19, 22, 30, 0, tzinfo=tz)
curs.execute("INSERT INTO test_tz VALUES (%s)", (d,))
print "Inserted timestamp with timezone:", d
print "Time zone:", d.tzinfo.tzname(d), "offset:", d.tzinfo.utcoffset(d)

curs.execute("SELECT * FROM test_tz")
d = curs.fetchone()[0]
curs.execute("INSERT INTO test_tz VALUES (%s)", (d,))
print "Inserted SELECTed timestamp:", d
print "Time zone:", d.tzinfo.tzname(d), "offset:", d.tzinfo.utcoffset(d)

curs.execute("SELECT * FROM test_tz")
for d in curs:
    u = d[0].utcoffset() or ZERO
    print "UTC time:  ", d[0] - u 
    print "Local time:", d[0]
    print "Time zone:", d[0].tzinfo.tzname(d[0]), d[0].tzinfo.utcoffset(d[0])
    

curs.execute("DROP TABLE test_tz")
conn.commit()
