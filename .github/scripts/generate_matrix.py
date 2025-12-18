import argparse
import os
import glob
import json

# TODO: Remove this limit after testing
TEST_SUITES_LIMIT = ['automember_plugin', 'basic', 'config', 'schema']


def get_suites(test_args):
    """Get list of test suites to run."""
    if test_args:
        valid_suites = []
        for suite in test_args:
            test_path = os.path.join("dirsrvtests/tests/suites/", suite)
            if os.path.exists(test_path) and not os.path.islink(test_path):
                if os.path.isfile(test_path) and test_path.endswith(".py"):
                    valid_suites.append(suite)
                elif os.path.isdir(test_path):
                    valid_suites.append(suite)
        return valid_suites

    # Use tests from the source
    suites = next(os.walk('dirsrvtests/tests/suites/'))[1]

    # Run each replication test module separately to speed things up
    suites.remove('replication')
    repl_tests = glob.glob('dirsrvtests/tests/suites/replication/*_test.py')
    suites += [repl_test.replace('dirsrvtests/tests/suites/', '') for repl_test in repl_tests]
    suites.sort()

    # TODO: Remove this filter after testing
    if TEST_SUITES_LIMIT:
        suites = [s for s in suites if s in TEST_SUITES_LIMIT]

    return suites


def main():
    parser = argparse.ArgumentParser(description='Generate GitHub Actions test matrix')
    parser.add_argument('--with-db-libs', action='store_true',
                        help='Include db_lib (bdb, mdb) in the matrix')
    parser.add_argument('tests', nargs='*', default=[],
                        help='Specific test suites or modules to run')

    args = parser.parse_args()

    # Filter out 'false' which comes from workflow_dispatch default
    test_args = [t for t in args.tests if t and t != 'false']

    suites = get_suites(test_args)

    if args.with_db_libs:
        # Generate combined matrix with both db_lib and suite
        db_libs = ['bdb', 'mdb']
        suites_list = [
            {"db_lib": db_lib, "suite": suite}
            for suite in suites
            for db_lib in db_libs
        ]
    else:
        # Original behavior: just suites
        suites_list = [{"suite": suite} for suite in suites]

    matrix = {"include": suites_list}
    print(json.dumps(matrix))


if __name__ == '__main__':
    main()
