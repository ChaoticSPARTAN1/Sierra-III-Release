#!/usr/bin/env python3
"""ci_run.py - runs a workflow step's command and, when it fails, puts the
end of its output in a GitHub annotation, which the run's page (and the API)
shows without opening the job's log.

    python ci_run.py "<what it is>" <command> [arguments ...]
    python ci_run.py "<what it is>" --log <file> <exit code>

The output still streams to the log as it comes; the exit code passes
through. The second form annotates a log the step wrote itself: MSYS2's bash
must be started from its own shell, not from a Windows program such as this
Python, or it takes Windows' paths for its own."""
import collections
import shutil
import subprocess
import sys

title, command = sys.argv[1], sys.argv[2:]
tail = collections.deque(maxlen=60)
if command[0] == '--log':
    with open(command[1], encoding='utf-8', errors='replace') as f:
        tail.extend(line.rstrip('\r\n') for line in f)
    code = int(command[2])
else:
    # by PATH, as a shell would: Windows itself looks in System32 first
    command[0] = shutil.which(command[0]) or command[0]
    p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for raw in p.stdout:
        line = raw.decode('utf-8', 'replace')
        sys.stdout.write(line)
        sys.stdout.flush()
        tail.append(line.rstrip('\r\n'))
    code = p.wait()
if code:
    # an annotation is one line: GitHub's escapes keep its line breaks
    text = '\n'.join(tail).replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
    print('::error title=%s failed (%d)::%s' % (title.replace(',', ' ').replace(':', ' '), code, text), flush=True)
sys.exit(code)
