#!/usr/bin/env python
import psycoev
import time
import unittest

class DateTimeAllocationBugTestCase(unittest.TestCase):
    def test_date_time_allocation_bug(self):
        d1 = psycoev.Date(2002,12,25)
        d2 = psycoev.DateFromTicks(time.mktime((2002,12,25,0,0,0,0,0,0)))
        t1 = psycoev.Time(13,45,30)
        t2 = psycoev.TimeFromTicks(time.mktime((2001,1,1,13,45,30,0,0,0)))
        t1 = psycoev.Timestamp(2002,12,25,13,45,30)
        t2 = psycoev.TimestampFromTicks(
            time.mktime((2002,12,25,13,45,30,0,0,0)))


def test_suite():
    return unittest.TestLoader().loadTestsFromName(__name__)

if __name__ == "__main__":
    unittest.main()
