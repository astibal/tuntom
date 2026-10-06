"""Read-only readiness check run as the adapter's unprivileged identity."""
import os
from pathlib import Path
import stat
import sys
import time


def check(directory, sockets):
    os.chdir(directory)  # Exercises traversal of every ancestor.
    if not os.access('.', os.W_OK | os.X_OK):
        raise PermissionError('Runtime directory is not writable: ' + directory)
    for path in sockets:
        if not stat.S_ISSOCK(Path(path).stat().st_mode):
            raise OSError('Relay path is not a socket: ' + path)
        if not os.access(path, os.W_OK):
            raise PermissionError('Relay socket is not writable: ' + path)


if __name__ == '__main__':
    for attempt in range(50):
        try:
            check(sys.argv[1], sys.argv[2:])
            break
        except OSError as error:
            if attempt == 49:
                sys.exit('Adapter runtime access check failed as tuntom:tuntom: ' + str(error))
            time.sleep(.1)
