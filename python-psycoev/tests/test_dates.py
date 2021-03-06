#!/usr/bin/env python
import math
import unittest

import psycoev
from psycoev.tz import FixedOffsetTimezone
import tests


class CommonDatetimeTestsMixin:

    def execute(self, *args):
        self.curs.execute(*args)
        return self.curs.fetchone()[0]

    def test_parse_date(self):
        value = self.DATE('2007-01-01', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.year, 2007)
        self.assertEqual(value.month, 1)
        self.assertEqual(value.day, 1)

    def test_parse_null_date(self):
        value = self.DATE(None, self.curs)
        self.assertEqual(value, None)

    def test_parse_incomplete_date(self):
        self.assertRaises(psycoev.DataError, self.DATE, '2007', self.curs)
        self.assertRaises(psycoev.DataError, self.DATE, '2007-01', self.curs)

    def test_parse_time(self):
        value = self.TIME('13:30:29', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.hour, 13)
        self.assertEqual(value.minute, 30)
        self.assertEqual(value.second, 29)

    def test_parse_null_time(self):
        value = self.TIME(None, self.curs)
        self.assertEqual(value, None)

    def test_parse_incomplete_time(self):
        self.assertRaises(psycoev.DataError, self.TIME, '13', self.curs)
        self.assertRaises(psycoev.DataError, self.TIME, '13:30', self.curs)

    def test_parse_datetime(self):
        value = self.DATETIME('2007-01-01 13:30:29', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.year, 2007)
        self.assertEqual(value.month, 1)
        self.assertEqual(value.day, 1)
        self.assertEqual(value.hour, 13)
        self.assertEqual(value.minute, 30)
        self.assertEqual(value.second, 29)

    def test_parse_null_datetime(self):
        value = self.DATETIME(None, self.curs)
        self.assertEqual(value, None)

    def test_parse_incomplete_time(self):
        self.assertRaises(psycoev.DataError,
                          self.DATETIME, '2007', self.curs)
        self.assertRaises(psycoev.DataError,
                          self.DATETIME, '2007-01', self.curs)
        self.assertRaises(psycoev.DataError,
                          self.DATETIME, '2007-01-01 13', self.curs)
        self.assertRaises(psycoev.DataError,
                          self.DATETIME, '2007-01-01 13:30', self.curs)

    def test_parse_null_interval(self):
        value = self.INTERVAL(None, self.curs)
        self.assertEqual(value, None)


class DatetimeTests(unittest.TestCase, CommonDatetimeTestsMixin):
    """Tests for the datetime based date handling in psycoev."""

    def setUp(self):
        self.conn = psycoev.connect(tests.dsn)
        self.curs = self.conn.cursor()
        self.DATE = psycoev._psycopg.PYDATE
        self.TIME = psycoev._psycopg.PYTIME
        self.DATETIME = psycoev._psycopg.PYDATETIME
        self.INTERVAL = psycoev._psycopg.PYINTERVAL

    def tearDown(self):
        self.conn.close()

    def test_parse_bc_date(self):
        # datetime does not support BC dates
        self.assertRaises(ValueError, self.DATE, '00042-01-01 BC', self.curs)

    def test_parse_bc_datetime(self):
        # datetime does not support BC dates
        self.assertRaises(ValueError, self.DATETIME,
                          '00042-01-01 13:30:29 BC', self.curs)

    def test_parse_time_microseconds(self):
        value = self.TIME('13:30:29.123456', self.curs)
        self.assertEqual(value.second, 29)
        self.assertEqual(value.microsecond, 123456)

    def test_parse_datetime_microseconds(self):
        value = self.DATETIME('2007-01-01 13:30:29.123456', self.curs)
        self.assertEqual(value.second, 29)
        self.assertEqual(value.microsecond, 123456)

    def check_time_tz(self, str_offset, offset):
        from datetime import time, timedelta
        base = time(13, 30, 29)
        base_str = '13:30:29'

        value = self.TIME(base_str + str_offset, self.curs)

        # Value has time zone info and correct UTC offset.
        self.assertNotEqual(value.tzinfo, None),
        self.assertEqual(value.utcoffset(), timedelta(seconds=offset))

        # Time portion is correct.
        self.assertEqual(value.replace(tzinfo=None), base)

    def test_parse_time_timezone(self):
        self.check_time_tz("+01", 3600)
        self.check_time_tz("-01", -3600)
        self.check_time_tz("+01:15", 4500)
        self.check_time_tz("-01:15", -4500)
        # The Python datetime module does not support time zone
        # offsets that are not a whole number of minutes, so we get an
        # error here.  Check that we are generating an understandable
        # error message.
        try:
            self.check_time_tz("+01:15:42", 4542)
        except ValueError, exc:
            self.assertEqual(exc.message, "time zone offset 4542 is not a "
                             "whole number of minutes")
        else:
            self.fail("Expected ValueError")

        try:
            self.check_time_tz("-01:15:42", -4542)
        except ValueError, exc:
            self.assertEqual(exc.message, "time zone offset -4542 is not a "
                             "whole number of minutes")
        else:
            self.fail("Expected ValueError")

    def check_datetime_tz(self, str_offset, offset):
        from datetime import datetime, timedelta
        base = datetime(2007, 1, 1, 13, 30, 29)
        base_str = '2007-01-01 13:30:29'

        value = self.DATETIME(base_str + str_offset, self.curs)

        # Value has time zone info and correct UTC offset.
        self.assertNotEqual(value.tzinfo, None),
        self.assertEqual(value.utcoffset(), timedelta(seconds=offset))

        # Datetime is correct.
        self.assertEqual(value.replace(tzinfo=None), base)

        # Conversion to UTC produces the expected offset.
        UTC = FixedOffsetTimezone(0, "UTC")
        value_utc = value.astimezone(UTC).replace(tzinfo=None)
        self.assertEqual(base - value_utc, timedelta(seconds=offset))

    def test_parse_datetime_timezone(self):
        self.check_datetime_tz("+01", 3600)
        self.check_datetime_tz("-01", -3600)
        self.check_datetime_tz("+01:15", 4500)
        self.check_datetime_tz("-01:15", -4500)
        # The Python datetime module does not support time zone
        # offsets that are not a whole number of minutes, so we get an
        # error here.  Check that we are generating an understandable
        # error message.
        try:
            self.check_datetime_tz("+01:15:42", 4542)
        except ValueError, exc:
            self.assertEqual(exc.message, "time zone offset 4542 is not a "
                             "whole number of minutes")
        else:
            self.fail("Expected ValueError")

        try:
            self.check_datetime_tz("-01:15:42", -4542)
        except ValueError, exc:
            self.assertEqual(exc.message, "time zone offset -4542 is not a "
                             "whole number of minutes")
        else:
            self.fail("Expected ValueError")

    def test_parse_time_no_timezone(self):
        self.assertEqual(self.TIME("13:30:29", self.curs).tzinfo, None)
        self.assertEqual(self.TIME("13:30:29.123456", self.curs).tzinfo, None)

    def test_parse_datetime_no_timezone(self):
        self.assertEqual(
            self.DATETIME("2007-01-01 13:30:29", self.curs).tzinfo, None)
        self.assertEqual(
            self.DATETIME("2007-01-01 13:30:29.123456", self.curs).tzinfo, None)

    def test_parse_interval(self):
        value = self.INTERVAL('42 days 12:34:56.123456', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.days, 42)
        self.assertEqual(value.seconds, 45296)
        self.assertEqual(value.microseconds, 123456)

    def test_parse_negative_interval(self):
        value = self.INTERVAL('-42 days -12:34:56.123456', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.days, -43)
        self.assertEqual(value.seconds, 41103)
        self.assertEqual(value.microseconds, 876544)

    def test_adapt_date(self):
        from datetime import date
        value = self.execute('select (%s)::date::text',
                             [date(2007, 1, 1)])
        self.assertEqual(value, '2007-01-01')

    def test_adapt_time(self):
        from datetime import time
        value = self.execute('select (%s)::time::text',
                             [time(13, 30, 29)])
        self.assertEqual(value, '13:30:29')

    def test_adapt_datetime(self):
        from datetime import datetime
        value = self.execute('select (%s)::timestamp::text',
                             [datetime(2007, 1, 1, 13, 30, 29)])
        self.assertEqual(value, '2007-01-01 13:30:29')

    def test_adapt_timedelta(self):
        from datetime import timedelta
        value = self.execute('select extract(epoch from (%s)::interval)',
                             [timedelta(days=42, seconds=45296,
                                        microseconds=123456)])
        seconds = math.floor(value)
        self.assertEqual(seconds, 3674096)
        self.assertEqual(int(round((value - seconds) * 1000000)), 123456)

    def test_adapt_megative_timedelta(self):
        from datetime import timedelta
        value = self.execute('select extract(epoch from (%s)::interval)',
                             [timedelta(days=-42, seconds=45296,
                                        microseconds=123456)])
        seconds = math.floor(value)
        self.assertEqual(seconds, -3583504)
        self.assertEqual(int(round((value - seconds) * 1000000)), 123456)


# Only run the datetime tests if psycopg was compiled with support.
if not hasattr(psycoev._psycopg, 'PYDATETIME'):
    del DatetimeTests


class mxDateTimeTests(unittest.TestCase, CommonDatetimeTestsMixin):
    """Tests for the mx.DateTime based date handling in psycoev."""

    def setUp(self):
        self.conn = psycoev.connect(tests.dsn)
        self.curs = self.conn.cursor()
        self.DATE = psycoev._psycopg.MXDATE
        self.TIME = psycoev._psycopg.MXTIME
        self.DATETIME = psycoev._psycopg.MXDATETIME
        self.INTERVAL = psycoev._psycopg.MXINTERVAL

    def tearDown(self):
        self.conn.close()

    def test_parse_bc_date(self):
        value = self.DATE('00042-01-01 BC', self.curs)
        self.assertNotEqual(value, None)
        # mx.DateTime numbers BC dates from 0 rather than 1.
        self.assertEqual(value.year, -41)
        self.assertEqual(value.month, 1)
        self.assertEqual(value.day, 1)

    def test_parse_bc_datetime(self):
        value = self.DATETIME('00042-01-01 13:30:29 BC', self.curs)
        self.assertNotEqual(value, None)
        # mx.DateTime numbers BC dates from 0 rather than 1.
        self.assertEqual(value.year, -41)
        self.assertEqual(value.month, 1)
        self.assertEqual(value.day, 1)
        self.assertEqual(value.hour, 13)
        self.assertEqual(value.minute, 30)
        self.assertEqual(value.second, 29)

    def test_parse_time_microseconds(self):
        value = self.TIME('13:30:29.123456', self.curs)
        self.assertEqual(math.floor(value.second), 29)
        self.assertEqual(
            int((value.second - math.floor(value.second)) * 1000000), 123456)

    def test_parse_datetime_microseconds(self):
        value = self.DATETIME('2007-01-01 13:30:29.123456', self.curs)
        self.assertEqual(math.floor(value.second), 29)
        self.assertEqual(
            int((value.second - math.floor(value.second)) * 1000000), 123456)

    def test_parse_time_timezone(self):
        # Time zone information is ignored.
        from mx.DateTime import Time
        expected = Time(13, 30, 29)
        self.assertEqual(expected, self.TIME("13:30:29+01", self.curs))
        self.assertEqual(expected, self.TIME("13:30:29-01", self.curs))
        self.assertEqual(expected, self.TIME("13:30:29+01:15", self.curs))
        self.assertEqual(expected, self.TIME("13:30:29-01:15", self.curs))
        self.assertEqual(expected, self.TIME("13:30:29+01:15:42", self.curs))
        self.assertEqual(expected, self.TIME("13:30:29-01:15:42", self.curs))

    def test_parse_datetime_timezone(self):
        # Time zone information is ignored.
        from mx.DateTime import DateTime
        expected = DateTime(2007, 1, 1, 13, 30, 29)
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29+01", self.curs))
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29-01", self.curs))
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29+01:15", self.curs))
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29-01:15", self.curs))
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29+01:15:42", self.curs))
        self.assertEqual(
            expected, self.DATETIME("2007-01-01 13:30:29-01:15:42", self.curs))

    def test_parse_interval(self):
        value = self.INTERVAL('42 days 05:50:05', self.curs)
        self.assertNotEqual(value, None)
        self.assertEqual(value.day, 42)
        self.assertEqual(value.hour, 5)
        self.assertEqual(value.minute, 50)
        self.assertEqual(value.second, 5)

    def test_adapt_time(self):
        from mx.DateTime import Time
        value = self.execute('select (%s)::time::text',
                             [Time(13, 30, 29)])
        self.assertEqual(value, '13:30:29')

    def test_adapt_datetime(self):
        from mx.DateTime import DateTime
        value = self.execute('select (%s)::timestamp::text',
                             [DateTime(2007, 1, 1, 13, 30, 29.123456)])
        self.assertEqual(value, '2007-01-01 13:30:29.123456')

    def test_adapt_bc_datetime(self):
        from mx.DateTime import DateTime
        value = self.execute('select (%s)::timestamp::text',
                             [DateTime(-41, 1, 1, 13, 30, 29.123456)])
        self.assertEqual(value, '0042-01-01 13:30:29.123456 BC')

    def test_adapt_timedelta(self):
        from mx.DateTime import DateTimeDeltaFrom
        value = self.execute('select extract(epoch from (%s)::interval)',
                             [DateTimeDeltaFrom(days=42,
                                                seconds=45296.123456)])
        seconds = math.floor(value)
        self.assertEqual(seconds, 3674096)
        self.assertEqual(int(round((value - seconds) * 1000000)), 123456)

    def test_adapt_megative_timedelta(self):
        from mx.DateTime import DateTimeDeltaFrom
        value = self.execute('select extract(epoch from (%s)::interval)',
                             [DateTimeDeltaFrom(days=-42,
                                                seconds=45296.123456)])
        seconds = math.floor(value)
        self.assertEqual(seconds, -3583504)
        self.assertEqual(int(round((value - seconds) * 1000000)), 123456)


# Only run the mx.DateTime tests if psycopg was compiled with support.
if not hasattr(psycoev._psycopg, 'MXDATETIME'):
    del mxDateTimeTests


def test_suite():
    return unittest.TestLoader().loadTestsFromName(__name__)

