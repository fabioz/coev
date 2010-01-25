#
# Copyright 2009 Apache Software Foundation 
# 
# Licensed under the Apache License, Version 2.0 (the "License"); you
# may not use this file except in compliance with the License.  You
# may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied.  See the License for the specific language governing
# permissions and limitations under the License.
#
# depends on libapr1-dev
#

from distutils.core import setup, Extension
import subprocess

def apr_config(*options):
    print repr(options)
    options = ["apr-config"] + list(options)
    print repr(options)
    p = subprocess.Popen(options, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p.stdin.close()
    r = p.stdout.readline().strip()
    print r
    if not r:
        raise Warning(p.stderr.readline())
    return r


module = Extension('cqsl', 
    sources = ['cqsl.c'],
    include_dirs=[apr_config('--includedir')],
    extra_link_args=apr_config('--link-ld').split(' '),
    extra_compile_args=apr_config('--cflags','--cppflags').split(' ') )


setup (name = 'cqsl',
       version = '1.0',
       description = 'parse_qs and parse_qsl implementations lifted from mod_python',
       ext_modules = [module] )
