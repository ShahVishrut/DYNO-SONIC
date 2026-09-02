import subprocess
import os

try:
    with open('test_out.txt', 'w') as f:
        subprocess.run(['nix', 'develop', '-c', 'bazel', 'run', '-c', 'opt', '--copt=-g', '//src/cmd:test_correctness'], stdout=f, stderr=subprocess.STDOUT)
except Exception as e:
    print(e)
