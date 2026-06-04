# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2026 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---

import logging
import os
import signal
import pytest
import threading
import time

from lib389 import pid_from_file, pid_exists
from lib389._constants import DEFAULT_SUFFIX, DIRSRV_STATE_OFFLINE
from lib389.dbgen import dbgen_users, get_index, finalize_ldif_file
from lib389.idm.group import Group
from lib389.plugins import MemberOfPlugin
from lib389.topologies import topology_st as topo
from lib389.utils import get_default_db_lib

log = logging.getLogger(__name__)

NUM_USERS = 5000
NUM_GROUPS = 50
STOP_TIMEOUT = 60


def _generate_ldif(inst, ldif_file, suffix, num_users, num_groups):
    """Generate LDIF with users under ou=People and groups under ou=Groups,
    each group containing all users as members."""
    parent = f"ou=People,{suffix}"
    dbgen_users(inst, num_users, ldif_file, suffix, generic=True, parent=parent)

    member_dns = [f"uid=user{get_index(i, num_users)},{parent}"
                  for i in range(1, num_users + 1)]

    with open(ldif_file, 'a') as f:
        for g in range(1, num_groups + 1):
            f.write(f"dn: cn=group-{g},ou=Groups,{suffix}\n")
            f.write("objectClass: top\n")
            f.write("objectClass: groupOfNames\n")
            f.write(f"cn: group-{g}\n")
            for mdn in member_dns:
                f.write(f"member: {mdn}\n")
            f.write("\n")
    finalize_ldif_file(inst, ldif_file)


@pytest.mark.skipif(get_default_db_lib() == "mdb",
                    reason="Deferred memberof not supported with LMDB")
def test_deferred_memberof_shutdown(topo):
    """Test server shuts down when deferred memberof update is in progress.

    Concurrent group deletes with deferred memberof can cause worker threads
    to hang in the SLAPI_DEFERRED_MEMBEROF polling loop if the deferred
    thread exits during shutdown without clearing the flag (Issue 7152).

    :id: 3e5a7b2c-1d4f-4e8a-9c6b-0f2d8e7a1b3c
    :setup: Standalone Instance
    :steps:
        1. Import LDIF with users and groups via ldif2db
        2. Enable memberOf plugin with deferred update
        3. Delete groups concurrently from 4 threads
        4. Stop the server while deletes are in progress
    :expectedresults:
        1. Success
        2. Success
        3. Success
        4. Server stops within 60s (no hang in polling loop)
    """
    inst = topo.standalone

    ldif_file = os.path.join(inst.get_ldif_dir(), 'deferred_shutdown.ldif')
    _generate_ldif(inst, ldif_file, DEFAULT_SUFFIX, NUM_USERS, NUM_GROUPS)

    inst.stop()
    assert inst.ldif2db(bename=None, suffixes=[DEFAULT_SUFFIX],
                        encrypt=None, excludeSuffixes=None,
                        import_file=ldif_file)
    inst.start()

    memberof = MemberOfPlugin(inst)
    memberof.enable()
    memberof.set_autoaddoc('nsMemberOf')
    memberof.set_memberofdeferredupdate('on')
    inst.restart()

    # Delete groups from 4 concurrent threads
    def _delete_range(start, end):
        try:
            conn = inst.clone()
            conn.open()
            for g in range(start, end + 1):
                Group(conn, f"cn=group-{g},ou=Groups,{DEFAULT_SUFFIX}").delete()
        except Exception:
            pass

    per_thread = NUM_GROUPS // 4
    threads = []
    for i in range(4):
        t = threading.Thread(target=_delete_range,
                             args=(i * per_thread + 1, (i + 1) * per_thread),
                             daemon=True)
        t.start()
        threads.append(t)

    time.sleep(3)
    assert any(t.is_alive() for t in threads), "Deletes should still be running"

    # Stop the server while deletes are in progress.
    # inst.stop() calls systemctl stop which sends SIGTERM.
    # On unfixed code the server hangs in the polling loop.
    # Run in a thread so we can enforce our own timeout with SIGKILL.
    pid = pid_from_file(inst.pid_file())
    stop_thread = threading.Thread(target=inst.stop, daemon=True)
    start_time = time.time()
    stop_thread.start()
    stop_thread.join(timeout=STOP_TIMEOUT)
    elapsed = time.time() - start_time

    if stop_thread.is_alive() and pid and pid_exists(pid):
        os.kill(pid, signal.SIGKILL)
        time.sleep(2)
        inst.state = DIRSRV_STATE_OFFLINE

    for t in threads:
        t.join(timeout=5)

    assert elapsed < STOP_TIMEOUT, \
        f"Server hung for {elapsed:.0f}s, deferred memberof polling loop did not exit"

    inst.start()

if __name__ == '__main__':
    # Run isolated
    # -s for DEBUG mode
    CURRENT_FILE = os.path.realpath(__file__)
    pytest.main(["-s", CURRENT_FILE])
