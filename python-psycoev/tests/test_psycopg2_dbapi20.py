#!/usr/bin/env python
import dbapi20
import unittest
import psycoev
import popen2

import tests

class Psycopg2TestCase(dbapi20.DatabaseAPI20Test):
    driver = psycoev
    connect_args = ()
    connect_kw_args = {'dsn': tests.dsn}

    lower_func = 'lower' # For stored procedure test

    def test_setoutputsize(self):
        # psycoev's setoutputsize() is a no-op
        pass

    def test_nextset(self):
        # psycoev does not implement nextset()
        pass


def test_suite():
    return unittest.TestLoader().loadTestsFromName(__name__)

if __name__ == '__main__':
    unittest.main()
