from setuptools import setup

VERSION = '0.3'
DESCRIPTION = 'coewsgi - coev-based HTTP WSGI gateway'
LONG_DESCRIPTION = """
    A port of HTTP WSGI gateway from Paste to the coev
    IO event-scheduled corourine framework.
"""

CLASSIFIERS = filter(None, map(str.strip,
"""
Development Status :: 3 - Alpha
Intended Audience :: Developers
License :: OSI Approved :: MIT License
Natural Language :: English
Programming Language :: Python
Operating System :: POSIX
Topic :: Software Development :: Libraries :: Python Modules
""".splitlines()))

REPOSITORY="http://coev.lxnt.info/"


setup(name='coewsgi',
      version=VERSION,
      description=DESCRIPTION,
      long_description=LONG_DESCRIPTION,
      classifiers=CLASSIFIERS,
      author='Alexander Sabourenkov',
      author_email='screwdriver@lxnt.info',
      url='http://coev.lxnt.info/',
      license='MIT License',
      packages=['coewsgi'],
      include_package_data=True,
      zip_safe=False,
      install_requires=[ 'coev >= 0.5' ],
      entry_points= {
        'paste.server_runner' : [ 'http = coewsgi.httpserver:server_runner' ]
        }
      )
