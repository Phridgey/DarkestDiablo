"""Imported only so PyInstaller puts these modules inside the launcher .exe.

The .exe also runs the repo's build scripts (tools/build.py, src/patches/*.py; see
run_as_python in darkestdiablo_launcher.py), and PyInstaller only bundles what the launcher
itself imports. If a build script starts importing a standard module not listed here, the
launcher's build fails with "No module named ..."; add it here.
"""
import argparse, array, base64, binascii, bisect, collections, colorsys, copy, csv, dataclasses, datetime  # noqa: F401
import decimal, difflib, enum, fnmatch, fractions, functools, glob, hashlib, heapq, importlib.util, io  # noqa: F401
import itertools, json, math, operator, os, pathlib, pprint, random, re, shutil, statistics, string  # noqa: F401
import struct, subprocess, sys, tempfile, textwrap, time, typing, unicodedata, uuid, xml.etree.ElementTree  # noqa: F401
import zipfile, zlib  # noqa: F401
