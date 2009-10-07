from setuptools import setup, find_packages
import sys, os

version = '0.2'

setup(name='coewsgi',
      version=version,
      description="coev-compatible http wsgi gateway",
      long_description="""\
""",
      classifiers=[], # Get strings from http://pypi.python.org/pypi?%3Aaction=list_classifiers
      keywords='',
      author='Alexander Sabourenkov',
      author_email='screwdriver@lxnt.info',
      url='http://coev.lxnt.info/',
      license='',
      packages=find_packages(exclude=['ez_setup', 'examples', 'tests']),
      include_package_data=True,
      zip_safe=True,
      install_requires=[ 'coev >= 0.3' ],
      entry_points="""
      # -*- Entry points: -*-
        [paste.server_runner]
        http = coewsgi.httpserver:server_runner      
      """,
      )
