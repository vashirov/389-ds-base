# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2016 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---
#
import pytest
from lib389.tasks import *

from lib389.utils import *
# Skip on older versions
pytestmark = pytest.mark.skipif(ds_is_older('1.3.5'), reason="Not implemented")
from lib389.topologies import topology_m2

from lib389._constants import DEFAULT_SUFFIX, DN_DM, PASSWORD

logging.getLogger(__name__).setLevel(logging.DEBUG)
log = logging.getLogger(__name__)

CONFIG_DN = 'cn=config'
ENCRYPTION_DN = 'cn=encryption,%s' % CONFIG_DN
RSA = 'RSA'
RSA_DN = 'cn=%s,%s' % (RSA, ENCRYPTION_DN)
ISSUER = 'cn=CAcert'
CACERT = 'CAcertificate'
SERVERCERT = 'Server-Cert'


@pytest.fixture(scope="module")
def add_entry(server, name, rdntmpl, start, num):
    log.info("\n######################### Adding %d entries to %s ######################" % (num, name))

    for i in range(num):
        ii = start + i
        dn = '%s%d,%s' % (rdntmpl, ii, DEFAULT_SUFFIX)
        try:
            server.add_s(Entry((dn, {'objectclass': 'top person extensibleObject'.split(),
                                     'uid': '%s%d' % (rdntmpl, ii),
                                     'cn': '%s user%d' % (name, ii),
                                     'sn': 'user%d' % (ii)})))
        except ldap.LDAPError as e:
            log.error('Failed to add %s ' % dn + e.message['desc'])
            assert False


def enable_ssl(server, ldapsport, copy_serv=False):
    server.stop()
    server.nss_ssl.reinit()
    if copy_serv:
        ca_cert = copy_serv.get_cert_dir() + "/ca.crt"
        os.system('cp %s/*.db %s' % (copy_serv.get_cert_dir(), server.get_cert_dir()))
        os.system('cp %s %s' % (ca_cert, server.get_cert_dir()))
        os.system('cp %s/noise* %s' % (copy_serv.get_cert_dir(), server.get_cert_dir()))
        os.system('cp %s/p* %s' % (copy_serv.get_cert_dir(), server.get_cert_dir()))
    else:
        server.nss_ssl.create_rsa_ca()
        server.nss_ssl.create_rsa_key_and_cert()
    server.start()

    server.modify_s(ENCRYPTION_DN, [(ldap.MOD_REPLACE, 'nsSSL3', 'off'),
                                    (ldap.MOD_REPLACE, 'nsTLS1', 'on'),
                                    (ldap.MOD_REPLACE, 'nsSSLClientAuth', 'allowed'),
                                    (ldap.MOD_REPLACE, 'allowWeakCipher', 'on'),
                                    (ldap.MOD_REPLACE, 'nsSSL3Ciphers', '+all')])

    time.sleep(1)
    server.modify_s(CONFIG_DN, [(ldap.MOD_REPLACE, 'nsslapd-security', 'on'),
                                (ldap.MOD_REPLACE, 'nsslapd-ssl-check-hostname', 'off'),
                                (ldap.MOD_REPLACE, 'nsslapd-secureport', ldapsport)])

    time.sleep(1)
    server.add_s(Entry((RSA_DN, {'objectclass': "top nsEncryptionModule".split(),
                                 'cn': RSA,
                                 'nsSSLPersonalitySSL': SERVERCERT,
                                 'nsSSLToken': 'internal (software)',
                                 'nsSSLActivation': 'on'})))
    time.sleep(1)
    server.restart()


def doAndPrintIt(cmdline, filename):
    proc = subprocess.Popen(cmdline, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if filename is None:
        log.info("      OUT:")
    else:
        log.info("      OUT: %s" % filename)
        fd = open(filename, "w")
    while True:
        l = proc.stdout.readline()
        if l == "":
            break
        if filename is None:
            log.info("      %s" % l)
        else:
            fd.write(l)
    log.info("      ERR:")
    while True:
        l = proc.stderr.readline()
        if l == "" or l == "\n":
            break
        log.info("      <%s>" % l)
        assert False

    if filename is not None:
        fd.close()
    time.sleep(1)


def config_tls_agreements(topology_m2):
    log.info("######################### Configure SSL/TLS agreements ######################")
    log.info("######################## master1 <-- startTLS -> master2 #####################")

    log.info("##### Update the agreement of master1")
    m1 = topology_m2.ms["master1"]
    m1_m2_agmt = m1.agreement.list(suffix=DEFAULT_SUFFIX)[0].dn
    topology_m2.ms["master1"].modify_s(m1_m2_agmt, [(ldap.MOD_REPLACE, 'nsDS5ReplicaTransportInfo', 'TLS')])

    log.info("##### Update the agreement of master2")
    m2 = topology_m2.ms["master2"]
    m2_m1_agmt = m2.agreement.list(suffix=DEFAULT_SUFFIX)[0].dn
    topology_m2.ms["master2"].modify_s(m2_m1_agmt, [(ldap.MOD_REPLACE, 'nsDS5ReplicaTransportInfo', 'TLS')])

    time.sleep(1)

    topology_m2.ms["master1"].restart(10)
    topology_m2.ms["master2"].restart(10)

    log.info("\n######################### Configure SSL/TLS agreements Done ######################\n")


def set_ssl_Version(server, name, version):
    log.info("\n######################### Set %s on %s ######################\n" %
             (version, name))
    server.simple_bind_s(DN_DM, PASSWORD)
    server.modify_s(ENCRYPTION_DN, [(ldap.MOD_REPLACE, 'nsSSL3', 'off'),
                                    (ldap.MOD_REPLACE, 'nsTLS1', 'on'),
                                    (ldap.MOD_REPLACE, 'sslVersionMin', version),
                                    (ldap.MOD_REPLACE, 'sslVersionMax', version)])


def test_ticket48784(topology_m2):
    """
    Set up 2way MMR:
        master_1 <----- startTLS -----> master_2

    Make sure the replication is working.
    Then, stop the servers and set only TLS1.0 on master_1 while TLS1.2 on master_2
    Replication is supposed to fail.
    """
    log.info("Ticket 48784 - Allow usage of OpenLDAP libraries that don't use NSS for crypto")

    #create_keys_certs(topology_m2)
    enable_ssl(topology_m2.ms["master1"], '636')
    enable_ssl(topology_m2.ms["master2"], '637', topology_m2.ms["master1"])
    config_tls_agreements(topology_m2)

    add_entry(topology_m2.ms["master1"], 'master1', 'uid=m1user', 0, 5)
    add_entry(topology_m2.ms["master2"], 'master2', 'uid=m2user', 0, 5)

    time.sleep(10)

    log.info('##### Searching for entries on master1...')
    entries = topology_m2.ms["master1"].search_s(DEFAULT_SUFFIX, ldap.SCOPE_SUBTREE, '(uid=*)')
    assert 10 == len(entries)

    log.info('##### Searching for entries on master2...')
    entries = topology_m2.ms["master2"].search_s(DEFAULT_SUFFIX, ldap.SCOPE_SUBTREE, '(uid=*)')
    assert 10 == len(entries)

    log.info("##### openldap client just accepts sslVersionMin not Max.")
    set_ssl_Version(topology_m2.ms["master1"], 'master1', 'TLS1.0')
    set_ssl_Version(topology_m2.ms["master2"], 'master2', 'TLS1.2')

    log.info("##### restart master[12]")
    topology_m2.ms["master1"].restart(timeout=10)
    topology_m2.ms["master2"].restart(timeout=10)

    log.info("##### replication from master_1 to master_2 should be ok.")
    add_entry(topology_m2.ms["master1"], 'master1', 'uid=m1user', 10, 1)
    log.info("##### replication from master_2 to master_1 should fail.")
    add_entry(topology_m2.ms["master2"], 'master2', 'uid=m2user', 10, 1)

    time.sleep(10)

    log.info('##### Searching for entries on master1...')
    entries = topology_m2.ms["master1"].search_s(DEFAULT_SUFFIX, ldap.SCOPE_SUBTREE, '(uid=*)')
    assert 11 == len(entries)  # This is supposed to be "1" less than master 2's entry count

    log.info('##### Searching for entries on master2...')
    entries = topology_m2.ms["master2"].search_s(DEFAULT_SUFFIX, ldap.SCOPE_SUBTREE, '(uid=*)')
    assert 12 == len(entries)

    log.info("Ticket 48784 - PASSED")


if __name__ == '__main__':
    # Run isolated
    # -s for DEBUG mode

    CURRENT_FILE = os.path.realpath(__file__)
    pytest.main("-s %s" % CURRENT_FILE)
