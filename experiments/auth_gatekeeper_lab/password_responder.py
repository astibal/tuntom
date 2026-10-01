#!/usr/bin/env python3
"""Demo client credential source. A real deployment would prompt or use a key store."""
import sys

sys.stdin.buffer.read()
sys.stdout.buffer.write(b"laboratory-secret")
