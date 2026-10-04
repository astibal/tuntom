"""Bounded text matching in an expendable process, outside the poller threads."""
import json
import re
import subprocess
import sys
from pathlib import Path

MAX_PAYLOAD = 256 * 1024
MAX_PATTERN = 1024


def validate_pattern(value):
    if not isinstance(value, str) or len(value) > MAX_PATTERN:
        raise ValueError('payload_regex must be a string of at most 1024 characters')
    try:
        re.compile(value)
    except (re.error, RecursionError, OverflowError) as error:
        raise ValueError('invalid payload regex') from error
    return value


def match_payload(pattern, text, timeout=0.5):
    validate_pattern(pattern)
    try:
        result = subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), '--worker'],
            input=json.dumps([pattern, text]), text=True, capture_output=True, timeout=timeout, check=True)
        return {'matched': result.stdout.strip() == 'true', 'status': 'matched' if result.stdout.strip() == 'true' else 'mismatch'}
    except subprocess.TimeoutExpired:
        return {'matched': False, 'status': 'timeout'}
    except (subprocess.CalledProcessError, OSError):
        return {'matched': False, 'status': 'error'}


if __name__ == '__main__':
    import resource
    resource.setrlimit(resource.RLIMIT_AS, (128 * 1024 * 1024, 128 * 1024 * 1024))
    resource.setrlimit(resource.RLIMIT_CPU, (1, 1))
    pattern, text = json.loads(sys.stdin.read(MAX_PAYLOAD * 8))
    print('true' if re.search(pattern, text) else 'false')
