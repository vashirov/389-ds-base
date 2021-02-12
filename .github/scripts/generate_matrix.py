import os
import glob
import json
from itertools import product

suites = next(os.walk('dirsrvtests/tests/suites/'))[1]

# Filter out snmp as it is an empty directory:
suites.remove('snmp')

# Run each replication test module separately to speed things up
suites.remove('replication')
suites.sort()
repl_tests = glob.glob('dirsrvtests/tests/suites/replication/*_test.py')
# Put replication tests first as they are the longest
suites = [repl_test.replace('dirsrvtests/tests/suites/', '') for repl_test in repl_tests] + suites

variants = ['gcc', 'gcc-asan']
# Disable clang matrix for now, otherwise we generate more than 256 jobs...
# variants += ['clang', 'clang-asan']


jobs = []
for suite, variant in product(suites, variants):
    jobs.append({"suite": suite, "variant": variant})

matrix = {"include": jobs}

print(json.dumps(matrix))
