#!/usr/bin/env python

from setuptools import setup
import evmemcache

setup(name="python-evmemcached",
      version=evmemcache.__version__,
      description="Coev-augmented pure python memcached client",
      long_description=open("README").read(),
      author="Evan Martin",
      author_email="martine@danga.com",
      maintainer="Alexander Sabourenkov",
      maintainer_email="llxxnntt@gmail.com",
      url="http://code.google.com/p/coev/",
      download_url="https://coev.googlecode.com/hg/",
      py_modules=["evmemcache"],
      classifiers=[
        "Development Status :: Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: Python Software Foundation License",
        "Operating System :: OS Independent",
        "Programming Language :: Python",
        "Topic :: Internet",
        "Topic :: Software Development :: Libraries :: Python Modules",
        ])

