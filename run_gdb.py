import subprocess

gdb_script = """
run
bt
"""

with open('gdb_script.txt', 'w') as f:
    f.write(gdb_script)

with open('test_out.txt', 'w') as f:
    subprocess.run(['nix', 'develop', '-c', 'gdb', '-batch', '-x', 'gdb_script.txt', './bazel-bin/src/cmd/test_correctness'], stdout=f, stderr=subprocess.STDOUT)
