/** BEGIN COPYRIGHT BLOCK
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2021 Red Hat, Inc.
 * All rights reserved.
 *
 * License: GPL (version 3 or any later version).
 * See LICENSE for details.
 * END COPYRIGHT BLOCK **/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* repl5_agmt.c */
/*

 Support for 5.0-style replication agreements.

 Directory Server 5.0 replication agreements contain information about
 replication consumers that we are supplying.

 This module encapsulates the methods available for adding, deleting,
 modifying, and firing replication agreements.

 Methods:

 agmt_new - Create a new replication agreement, in response to a new
            replication agreement being added over LDAP.
 agmt_delete - Destroy an agreement. It is an error to destroy an
                agreement that has not been stopped.
 agmt_getstatus - get the status of this replication agreement.
 agmt_replicate_now - initiate a replication session asap, even if the
                      schedule says we shouldn't.
 agmt_start - start replicating, according to schedule. Starts a new
              thread to handle replication.
 agmt_stop - stop replicating asap and end replication thread.
 agmt_notify_change - notify the replication agreement about a change that
                      has been logged. The replication agreement will
                      decide if it needs to take some action, e.g. start a
                      replication session.
 agmt_initialize_replica - start a complete replica refresh.
 agmt_set_schedule_from_entry - (re)set the schedule associated with this
            replication agreement based on a RA entry's contents.
 agmt_set_credentials_from_entry - (re)set the credentials used to bind
            to the remote replica.
 agmt_set_binddn_from_entry - (re)set the DN used to bind
            to the remote replica.
 agmt_set_bind_method_from_entry - (re)set the bind method used to bind
            to the remote replica (SIMPLE or SSLCLIENTAUTH).
 agmt_set_transportinfo_from_entry - (re)set the transport used to bind
            to the remote replica (SSL or not)

*/

#include "repl5.h"
#include "repl5_prot_private.h"
#include "cl5_api.h"
#include "slapi-plugin.h"
#include "slap.h"
#include "../../slapd/back-ldbm/dbimpl.h"          /* for dblayer_is_lmdb */
#include <pk11func.h>
#include <pk11pqg.h>
#include <plbase64.h>

#define DEFAULT_TIMEOUT 120                  /* (seconds) default outbound LDAP connection */
#define DEFAULT_FLOWCONTROL_WINDOW      1000 /* #entries sent without acknowledgment (bdb) */
#define DEFAULT_FLOWCONTROL_PAUSE       2000 /* msec of pause when #entries sent witout acknowledgment (bdb) */
#define LMDB_DEFAULT_FLOWCONTROL_WINDOW 50   /* #entries sent without acknowledgment (lmdb) */
#define LMDB_DEFAULT_FLOWCONTROL_PAUSE  200  /* msec of pause when #entries sent witout acknowledgment (lmdb) */

#define STATUS_LEN 2048
#define STATUS_GOOD "green"
#define STATUS_WARNING "amber"
#define STATUS_BAD "red"


struct changecounter
{
    ReplicaId rid;
    uint32_t num_replayed;
    uint32_t num_skipped;
};

typedef struct repl5agmt
{
    char *hostname;                        /* remote hostname */
    int64_t port;                          /* port of remote server */
    uint32_t transport_flags;              /* LDAPS, StartTLS, etc. */
    char *binddn;                          /* DN to bind as */
    struct berval *creds;                  /* Password, or certificate */
    int64_t bindmethod;                    /* Bind method - simple, SSL */
    Slapi_DN *replarea;                    /* DN of replicated area */
    char **frac_attrs;                     /* list of fractional attributes to be replicated */
    char **frac_attrs_total;               /* list of fractional attributes to be replicated for total update protocol */
    PRBool frac_attr_total_defined;        /* TRUE if frac_attrs_total is defined */
    Schedule *schedule;                    /* Scheduling information */
    int64_t auto_initialize;               /* 1 = automatically re-initialize replica */
    const Slapi_DN *dn;                    /* DN of replication agreement entry */
    const Slapi_RDN *rdn;                  /* RDN of replication agreement entry */
    char *long_name;                       /* Long name (rdn + host, port) of entry, for logging */
    int session_id_cnt;                    /* use to differentiate sessions */
    char *session_id_prefix;               /* use for debugging purpose on server/client sides */
#define SESSION_ID_STR_SZ 15
    char session_id[SESSION_ID_STR_SZ + 49];
    Repl_Protocol *protocol;               /* Protocol object - manages protocol */
    struct changecounter **changecounters; /* changes sent/skipped since server start up */
    int64_t num_changecounters;
    int64_t max_changecounters;
    time_t last_update_start_time;         /* Local start time of last update session */
    time_t last_update_end_time;           /* Local end time of last update session */
    char last_update_status[STATUS_LEN];   /* Status of last update. Format = numeric code <space> textual description */
    char last_update_status_json[STATUS_LEN];
    PRBool update_in_progress;
    PRBool is_enabled;
    time_t last_init_start_time;       /* Local start time of last total init */
    time_t last_init_end_time;         /* Local end time of last total init */
    char last_init_status[STATUS_LEN]; /* Status of last total init. Format = numeric code <space> textual description */
    char last_init_status_json[STATUS_LEN];
    PRLock *lock;
    Object *consumerRUV;     /* last RUV received from the consumer - used for changelog purging */
    CSN *consumerSchemaCSN;  /* last schema CSN received from the consumer */
    ReplicaId consumerRID;   /* indicates if the consumer is the originator of a CSN */
    int64_t tmpConsumerRID;  /* Indicates the consumer rid was set from the agmt maxcsn - it should be refreshed */
    int64_t timeout;         /* timeout (in seconds) for outbound LDAP connections to remote server */
    PRBool stop_in_progress; /* set by agmt_stop when shutting down */
    int64_t busywaittime;    /* time in seconds to wait after getting a REPLICA BUSY from the consumer -
                              * to allow another supplier to finish sending its updates -
                              * if set to 0, this means to use the default value if we get a busy
                              * signal from the consumer
                              */
    int64_t pausetime;       /* time in seconds to pause after sending updates -
                              * to allow another supplier to send its updates -
                              * should be greater than busywaittime -
                              * if set to 0, this means do not pause
                              */
    void *priv;              /* private data, used for windows-specific agreement data
                              * for sync agreements or for replication session plug-in
                              * private data for normal replication agreements
                              */
    char **attrs_to_strip;   /* for fractional replication, if a "mod" is empty, strip out these attributes:
                              * modifiersname, modifytimestamp, internalModifiersname, internalModifyTimestamp, etc */
    int64_t agreement_type;
    Slapi_Counter *protocol_timeout;
    char *maxcsn;                      /* agmt max csn */
    int64_t flowControlWindow;         /* This is the maximum number of entries sent without acknowledgment */
    int64_t flowControlPause;          /* When nb of not acknowledged entries overpass totalUpdateWindow
                                        * This is the duration (in msec) that the RA will pause before sending the next entry */
    int64_t ignoreMissingChange;       /* if set replication will try to continue even if change cannot be found in changelog */
    Slapi_RWLock *attr_lock;           /* RW lock for all the stripped attrs */
    int64_t WaitForAsyncResults;       /* Pass to DS_Sleep(PR_MillisecondsToInterval(WaitForAsyncResults))
                                        * in repl5_inc_waitfor_async_results */
    char *bootstrapBindDN;             /* Bootstrap bind dn */
    struct berval *bootstrapCreds;     /* Bootstrap credentials */
    int64_t bootstrapBindmethod;       /* Bootstrap Bind Method: simple, TLS, client auth, etc */
    uint32_t bootstrapTransportFlags;  /* Bootstrap Transport Info: LDAPS, StartTLS, etc. */

} repl5agmt;

/* Forward declarations */
void agmt_delete(void **rap);
static void update_window_state_change_callback(void *arg, PRBool opened);
static int get_agmt_status(Slapi_PBlock *pb, Slapi_Entry *e, Slapi_Entry *entryAfter, int *returncode, char *returntext, void *arg);
static int agmt_set_bind_method_no_lock(Repl_Agmt *ra, const Slapi_Entry *e);
static int32_t agmt_set_bootstrap_bind_method_no_lock(Repl_Agmt *ra, const Slapi_Entry *e);
static int agmt_set_transportinfo_no_lock(Repl_Agmt *ra, const Slapi_Entry *e);
static int32_t agmt_set_bootstrap_transportinfo_no_lock(Repl_Agmt *ra, const Slapi_Entry *e);
static ReplicaId agmt_maxcsn_get_rid(char *maxcsn);
static void agmt_replica_reset_ignoremissing(const Repl_Agmt *agmt);

/*
Schema for replication agreement:

cn
nsds5ReplicaHost - hostname
nsds5ReplicaPort - port number
nsds5ReplicaTransportInfo - "LDAPS", "StartTLS", or may be absent ("SSL" and "TLS" values will be deprecated later)
nsds5ReplicaBindDN
nsds5ReplicaCredentials
nsds5ReplicaBindMethod - "SIMPLE" or "SSLCLIENTAUTH".
nsds5ReplicaRoot - Replicated suffix
nsds5ReplicatedAttributeList - Fractional attrs for incremental update protocol (and total if not separately defined)
nsds5ReplicatedAttributeListTotal - Fractional attrs for total update protocol
nsds5ReplicaUpdateSchedule
nsds5ReplicaTimeout - Outbound repl operations timeout
nsds50ruv - consumer's RUV
nsds5ReplicaBusyWaitTime - time to wait after getting a REPLICA BUSY from the consumer
nsds5ReplicaSessionPauseTime - time to pause after sending updates to allow another supplier to send
*/

/* It sets various fields related to client side (repl_agmt)
 * of the session tracking
 * - session_id_cnt (counting the the outbound connection)
 * - session_tracking_supported (a flag stating if the server side support SID)
 * - session_id_prefix (fixed part of the session identifier for this repl_agmt)
 */
static void
agmt_init_session_id(Repl_Agmt *ra)
{
    char hash_out[HASH_LENGTH_MAX] = {0};
    char *enc = NULL;
    char *root = NULL; /* e.g. dc=example,dc=com */
    char *host = NULL; /* e.g. localhost.domain */
    char port[10];   /* e.g. 389 */
    char sport[10];  /* e.g. 636 */
    char *hash_in;
    int32_t max_str_sid = SESSION_ID_STR_SZ - 4;

    if (ra == NULL) {
        goto fail;
    }
    ra->session_id_cnt = 1;
    root = slapi_ch_strdup(slapi_sdn_get_dn(ra->replarea));
    if (root == NULL) {
        root = slapi_ch_strdup("unknown suffix");
    }
    host = get_localhost_DNS();
    if (host == NULL) {
        host = slapi_ch_strdup("unknown host");
    }
    snprintf(port,  sizeof(port), "%d", config_get_port());
    snprintf(sport, sizeof(sport),"%d", config_get_secureport());
    hash_in = slapi_ch_calloc(1, strlen(root) + strlen(host) + strlen(port) + strlen(sport) + 1);
    if (hash_in == NULL) {
        goto fail;
    }
    sprintf(hash_in, "%s%s%s%s", root, host, port, sport);
    PK11_HashBuf(SEC_OID_SHA1, (unsigned char *)hash_out, (unsigned char *)hash_in, strlen(hash_in));

    if ((enc = slapi_ch_calloc(LDIF_BASE64_LEN(SHA1_LENGTH) + 1, sizeof(char))) == NULL) {
        goto fail;
    }
    (void)PL_Base64Encode(hash_out, SHA1_LENGTH, enc);
    goto done;

fail:
    enc = slapi_ch_strdup("dummyID");

done:
    if (hash_in) {
        slapi_ch_free_string(&hash_in);
    }
    if (root) {
        slapi_ch_free_string(&root);
    }
    if (host) {
        slapi_ch_free_string(&host);
    }
    if (strlen(enc) > max_str_sid) {
        enc[max_str_sid] = '\0';
    }
    ra->session_id_prefix = enc;
    sprintf(ra->session_id, "%s ---", ra->session_id_prefix);
}

void
agmt_set_session_id(Repl_Agmt *ra)
{
    if (ra->session_id_cnt == 999) {
        ra->session_id_cnt = 1;
    } else {
        ra->session_id_cnt++;
    }
    sprintf(ra->session_id, "%s %3d", ra->session_id_prefix, ra->session_id_cnt);
}

char *
agmt_get_session_id(Repl_Agmt *ra)
{
    return(ra->session_id);
}
/*
 * Validate an agreement, making sure that it's valid.
 * Return 1 if the agreement is valid, 0 otherwise.
 */
static int
agmt_is_valid(Repl_Agmt *ra)
{
    int return_value = 1; /* assume valid, initially */
    PR_ASSERT(NULL != ra);
    PR_ASSERT(NULL != ra->dn);

    if (NULL == ra->hostname) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       "is malformed: missing host name.\n",
                      slapi_sdn_get_dn(ra->dn));
        return_value = 0;
    }
    if (ra->port <= 0) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       "is malformed: invalid port number %" PRId64 ".\n",
                      slapi_sdn_get_dn(ra->dn), ra->port);
        return_value = 0;
    }
    if (ra->timeout < 0) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       "is malformed: invalid timeout %" PRId64 ".\n",
                      slapi_sdn_get_dn(ra->dn), ra->timeout);
        return_value = 0;
    }
    if (ra->busywaittime < 0) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       "is malformed: invalid busy wait time %" PRId64 ".\n",
                      slapi_sdn_get_dn(ra->dn), ra->busywaittime);
        return_value = 0;
    }
    if (ra->pausetime < 0) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       "is malformed: invalid pausetime %" PRId64 ".\n",
                      slapi_sdn_get_dn(ra->dn), ra->pausetime);
        return_value = 0;
    }
    if ((0 == ra->transport_flags) && (BINDMETHOD_SSL_CLIENTAUTH == ra->bindmethod)) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                       " is malformed: cannot use SSLCLIENTAUTH if using plain LDAP - please "
                                                       "change %s to LDAPS or StartTLS before changing %s to use SSLCLIENTAUTH\n",
                      slapi_sdn_get_dn(ra->dn), type_nsds5TransportInfo, type_nsds5ReplicaBindMethod);
        return_value = 0;
    }
    /*
     * If we are not using GSSAPI or SSL Client Auth, then a bind dn and password must be present
     */
    if (BINDMETHOD_SASL_GSSAPI != ra->bindmethod && BINDMETHOD_SSL_CLIENTAUTH != ra->bindmethod) {
        if (strcmp(ra->binddn, "") == 0 || ra->creds->bv_val == NULL) {
            char *auth_mech;

            if (ra->bindmethod == BINDMETHOD_SIMPLE_AUTH) {
                auth_mech = "SIMPLE";
            } else if (ra->bindmethod == BINDMETHOD_SASL_DIGEST_MD5) {
                auth_mech = "SASL/DIGEST-MD5";
            } else {
                auth_mech = "Unknown";
            }
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_is_valid - Replication agreement \"%s\" "
                                                           "is malformed: a bind DN and password must be supplied for authentication "
                                                           "method \"%s\"\n",
                          slapi_sdn_get_dn(ra->dn), auth_mech);
            return_value = 0;
        }
    }
    return return_value;
}


Repl_Agmt *
agmt_new_from_entry(Slapi_Entry *e)
{
    Replica *replica = NULL;
    Repl_Agmt *ra;
    Slapi_Attr *sattr;
    char errormsg[SLAPI_DSE_RETURNTEXT_SIZE];
    char *tmpstr;
    char **denied_attrs = NULL;
    const char *auto_initialize = NULL;
    char *val_nsds5BeginReplicaRefresh = "start";
    Slapi_Backend *be = NULL;
    const char *val = NULL;
    int64_t ptimeout = 0;
    int use_lmdb = 0;
    int rc = 0;

    ra = (Repl_Agmt *)slapi_ch_calloc(1, sizeof(repl5agmt));
    if ((ra->lock = PR_NewLock()) == NULL) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_new_from_entry - Unable to create new lock "
                                                       "for replication agreement \"%s\" - agreement ignored.\n",
                      slapi_entry_get_dn_const(e));
        goto loser;
    }
    if ((ra->attr_lock = slapi_new_rwlock()) == NULL) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_new_from_entry - Unable to create new attr lock "
                                                       "for replication agreement \"%s\" - agreement ignored.\n",
                      slapi_entry_get_dn_const(e));
        goto loser;
    }
    ra->protocol_timeout = slapi_counter_new();

    /* Find all the stuff we need for the agreement */

    /*    To Allow Consumer Initialisation when adding an agreement: */

    /*
        Using 'auto_initialize' member of 'repl5agmt' structure to
        store the effect of 'nsds5BeginReplicaRefresh' attribute's value
        in it.
    */
    auto_initialize = slapi_entry_attr_get_ref(e, type_nsds5BeginReplicaRefresh);
    if ((auto_initialize != NULL) && (strcasecmp(auto_initialize, val_nsds5BeginReplicaRefresh) == 0)) {
        ra->auto_initialize = STATE_PERFORMING_TOTAL_UPDATE;
    } else {
        ra->auto_initialize = STATE_PERFORMING_INCREMENTAL_UPDATE;
    }

    /* Host name of remote replica */
    ra->hostname = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaHost);

    /* Port number for remote replica instance */
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaPort))){
        int64_t port;
        if (repl_config_valid_num(type_nsds5ReplicaPort, (char *)val, 1, 65535, &rc, errormsg, &port) != 0) {
            goto loser;
        }
        ra->port = port;
    }

    /* LDAPS, StartTLS, or other transport stuff */
    ra->transport_flags = 0;
    (void)agmt_set_transportinfo_no_lock(ra, e);
    (void)agmt_set_WaitForAsyncResults(ra, e);

    /* DN to use when binding. May be empty if certain SASL auth is to be used e.g. EXTERNAL GSSAPI. */
    ra->binddn = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaBindDN);
    if (NULL == ra->binddn) {
        ra->binddn = slapi_ch_strdup("");
    }

    /* Credentials to use when binding. */
    ra->creds = (struct berval *)slapi_ch_calloc(1, sizeof(struct berval));
    if (slapi_entry_attr_find(e, type_nsds5ReplicaCredentials, &sattr) == 0) {
        Slapi_Value *sval;
        if (slapi_attr_first_value(sattr, &sval) == 0) {
            const struct berval *bv = slapi_value_get_berval(sval);
            if (NULL != bv) {
                slapi_ber_bvcpy(ra->creds, bv);
            }
        }
    }
    /* How to bind */
    (void)agmt_set_bind_method_no_lock(ra, e);

    /* Process bootstrap settings */
    ra->bootstrapBindDN = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaBootstrapBindDN);
    ra->bootstrapCreds = (struct berval *)slapi_ch_calloc(1, sizeof(struct berval));
    if (slapi_entry_attr_find(e, type_nsds5ReplicaBootstrapCredentials, &sattr) == 0) {
        Slapi_Value *sval;
        if (slapi_attr_first_value(sattr, &sval) == 0) {
            const struct berval *bv = slapi_value_get_berval(sval);
            if (NULL != bv) {
                slapi_ber_bvcpy(ra->bootstrapCreds, bv);
            }
        }
    }
    ra->bootstrapTransportFlags = 0;
    (void)agmt_set_bootstrap_transportinfo_no_lock(ra, e);
    (void)agmt_set_bootstrap_bind_method_no_lock(ra, e);

    /* timeout. */
    ra->timeout = DEFAULT_TIMEOUT;
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaTimeout))) {
        int64_t timeout;
        if (repl_config_valid_num(type_nsds5ReplicaTimeout, (char *)val, 0, INT_MAX, &rc, errormsg, &timeout) != 0) {
            goto loser;
        }
        ra->timeout = timeout;
    }

    /* DN of entry at root of replicated area */
    tmpstr = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaRoot);
    if (NULL != tmpstr) {
        ra->replarea = slapi_sdn_new_dn_passin(tmpstr);

        /* now that we set the repl area, when can bump our agmt count */
        if ((replica = replica_get_replica_from_dn(ra->replarea))) {
            replica_incr_agmt_count(replica);
        }
        be = slapi_be_select(ra->replarea);
    }
    if (!be) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                      "agmt_new_from_entry - Failed to get backend for agreement %s on replicated suffix %s).\n",
                      slapi_entry_get_dn(e), tmpstr ? tmpstr : "<NULL>");
        goto loser;
    }
    if (!replica) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                      "agmt_new_from_entry - Failed to get replica for agreement %s on replicated suffix %s).\n",
                      slapi_entry_get_dn(e), tmpstr ? tmpstr : "<NULL>");
        goto loser;
    }

    /* flow control update window. */
    use_lmdb = dblayer_is_lmdb(be);
    ra->flowControlWindow = use_lmdb ? LMDB_DEFAULT_FLOWCONTROL_WINDOW : DEFAULT_FLOWCONTROL_WINDOW;
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaFlowControlWindow))){
        int64_t flow;
        if (repl_config_valid_num(type_nsds5ReplicaFlowControlWindow, (char *)val, 0, INT_MAX, &rc, errormsg, &flow) != 0) {
            goto loser;
        }
        ra->flowControlWindow = flow;
    }

    /* flow control update pause. */
    ra->flowControlPause = use_lmdb ? LMDB_DEFAULT_FLOWCONTROL_PAUSE : DEFAULT_FLOWCONTROL_PAUSE;
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaFlowControlPause))){
        int64_t pause;
        if (repl_config_valid_num(type_nsds5ReplicaFlowControlPause, (char *)val, 0, INT_MAX, &rc, errormsg, &pause) != 0) {
            goto loser;
        }
        ra->flowControlPause = pause;
    }

    /* continue on missing change ? */
    ra->ignoreMissingChange = 0;
    tmpstr = (char *)slapi_entry_attr_get_ref(e, type_replicaIgnoreMissingChange);
    if (NULL != tmpstr) {
        if (strcasecmp(tmpstr, "off") == 0 || strcasecmp(tmpstr, "never") == 0) {
            ra->ignoreMissingChange = 0;
        } else if (strcasecmp(tmpstr, "on") == 0 || strcasecmp(tmpstr, "once") == 0) {
            ra->ignoreMissingChange = 1;
        } else if (strcasecmp(tmpstr, "always") == 0) {
            ra->ignoreMissingChange = -1;
        };
    }

    /* If this agmt has its own timeout, grab it, otherwise use the replica's protocol timeout */
    if ((val = slapi_entry_attr_get_ref(e, type_replicaProtocolTimeout))){
        if (repl_config_valid_num(type_replicaProtocolTimeout, (char *)val, 0, INT_MAX, &rc, errormsg, &ptimeout) != 0) {
            goto loser;
        }
        slapi_counter_set_value(ra->protocol_timeout, ptimeout);
    }

    /* Replica enabled */
    tmpstr = (char *)slapi_entry_attr_get_ref(e, type_nsds5ReplicaEnabled);
    if (NULL != tmpstr) {
        if (strcasecmp(tmpstr, "off") == 0) {
            ra->is_enabled = PR_FALSE;
        } else if (strcasecmp(tmpstr, "on") == 0) {
            ra->is_enabled = PR_TRUE;
        } else {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_new_from_entry - "
                    "Warning invalid value for nsds5ReplicaEnabled (%s), value must be \"on\" or \"off\".  "
                    "Ignoring this repl agreement.\n", tmpstr);
            goto loser;
        }
    } else {
        ra->is_enabled = PR_TRUE;
    }

    /* Replication schedule */
    ra->schedule = schedule_new(update_window_state_change_callback, ra, agmt_get_long_name(ra));
    if (slapi_entry_attr_find(e, type_nsds5ReplicaUpdateSchedule, &sattr) == 0) {
        schedule_set(ra->schedule, sattr);
    }

    /* busy wait time - time to wait after getting REPLICA BUSY from consumer */
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaBusyWaitTime))){
        int64_t busytime = 0;
        if (repl_config_valid_num(type_nsds5ReplicaBusyWaitTime, (char *)val, 0, INT_MAX, &rc, errormsg, &busytime) != 0) {
            goto loser;
        }
        ra->busywaittime = busytime;
    }

    /* pause time - time to pause after a session has ended */
    if ((val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaSessionPauseTime))){
        int64_t pausetime = 0;
        if (repl_config_valid_num(type_nsds5ReplicaSessionPauseTime, (char *)val, 0, INT_MAX, &rc, errormsg, &pausetime) != 0) {
            goto loser;
        }
        ra->pausetime = pausetime;
    }
    /* consumer's RUV */
    if (slapi_entry_attr_find(e, type_ruvElement, &sattr) == 0) {
        RUV *ruv;

        if (ruv_init_from_slapi_attr(sattr, &ruv) == 0) {
            ra->consumerRUV = object_new(ruv, (FNFree)ruv_destroy);
        }
    }

    ra->consumerRID = 0;

    /* DN and RDN of the replication agreement entry itself */
    ra->dn = slapi_sdn_dup(slapi_entry_get_sdn((Slapi_Entry *)e));
    ra->rdn = slapi_rdn_new_sdn(ra->dn);

    /* Compute long name */
    {
        const char *agmtname = slapi_rdn_get_rdn(ra->rdn);
        char hostname[128];
        char *dot;

        strncpy(hostname, ra->hostname ? ra->hostname : "(unknown)", sizeof(hostname));
        hostname[sizeof(hostname) - 1] = '\0';
        dot = strchr(hostname, '.');
        if (dot) {
            *dot = '\0';
        }
        ra->long_name = slapi_ch_smprintf("agmt=\"%s\" (%s:%" PRId64 ")", agmtname, hostname, ra->port);
    }
    /* init the RA session id structs */
    agmt_init_session_id(ra);

    /* DBDB: review this code */
    if (slapi_entry_attr_hasvalue(e, "objectclass", "nsDSWindowsReplicationAgreement")) {
        if (replica_get_type(replica) == REPLICA_TYPE_PRIMARY
           || (replica_get_type(replica) == REPLICA_TYPE_UPDATABLE && replica_is_flag_set(replica, REPLICA_LOG_CHANGES))
        ) {
            ra->agreement_type = REPLICA_TYPE_WINDOWS;
            windows_init_agreement_from_entry(ra, e);
        } else {
            slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name,
                          "agmt_new_from_entry: type -> %d\n", replica_get_type(replica));
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                          "agmt_new_from_entry: failed to initialise windows replication"
                          "agreement \"%s\" - replica is not a supplier (may be hub or consumer).\n",
                          agmt_get_long_name(ra));
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                          "To proceed, you MUST promote this server to a supplier with: "
                          "dsconf INSTANCENAME replication promote --suffix \"%s\" --newrole supplier --replica-id=NEW_REPLICA_ID\n",
                          slapi_sdn_get_dn(ra->replarea));
            goto loser;
        }
    } else {
        ra->agreement_type = REPLICA_TYPE_MULTISUPPLIER;
        repl_session_plugin_call_agmt_init_cb(ra);
    }


    /* Initialize status information */
    ra->last_update_start_time = 0UL;
    ra->last_update_end_time = 0UL;
    ra->last_update_status[0] = '\0';
    ra->update_in_progress = PR_FALSE;
    ra->stop_in_progress = PR_FALSE;
    val = (char *)slapi_entry_attr_get_ref(e, type_nsds5ReplicaLastInitEnd);
    if (val) {
        time_t init_end_time;

        init_end_time = parse_genTime((char *) val);
        if (init_end_time == NO_TIME || init_end_time == SLAPD_END_TIME) {
            ra->last_init_end_time = 0UL;
        } else {
            ra->last_init_end_time = init_end_time;
        }
    }
    val = (char *)slapi_entry_attr_get_ref(e, type_nsds5ReplicaLastInitStart);
    if (val) {
        time_t init_start_time;

        init_start_time = parse_genTime((char *) val);
        if (init_start_time == NO_TIME || init_start_time == SLAPD_END_TIME) {
            ra->last_init_start_time = 0UL;
        } else {
            ra->last_init_start_time = init_start_time;
        }
    }
    val = (char *)slapi_entry_attr_get_ref(e, type_nsds5ReplicaLastInitStatus);
    if (val) {
        strcpy(ra->last_init_status, val);
    }
    ra->changecounters = (struct changecounter **)slapi_ch_calloc(MAX_NUM_OF_SUPPLIERS + 1,
                                                                  sizeof(struct changecounter *));
    ra->num_changecounters = 0;
    ra->max_changecounters = MAX_NUM_OF_SUPPLIERS;

    /* Fractional attributes */
    slapi_entry_attr_find(e, type_nsds5ReplicatedAttributeList, &sattr);

    /* New set of excluded attributes */
    /* Note: even if sattrs is empty, we have to call this func since there
     * could be a default excluded attr list in cn=plugin default config */
    if (agmt_set_replicated_attributes_from_attr(ra, sattr) != 0) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name,
                      "agmt_new_from_entry: failed to set "
                      "replicated attributes for agreement %s\n",
                      agmt_get_long_name(ra));
    }
    /* Check that there are no verboten attributes in the exclude list */
    denied_attrs = agmt_validate_replicated_attributes(ra, 0 /* incremental */);
    if (denied_attrs) {
        /* Report the error to the client */
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                      "agmt_new_from_entry: Attempt to exclude illegal attributes "
                      "from a fractional agreement\n");
        /* Free the list */
        slapi_ch_array_free(denied_attrs);
        goto loser;
    }

    /* Total update fractional attributes */
    slapi_entry_attr_find(e, type_nsds5ReplicatedAttributeListTotal, &sattr);
    if (sattr && agmt_set_replicated_attributes_total_from_attr(ra, sattr) != 0) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name,
                      "agmt_new_from_entry: failed to parse total "
                      "update replicated attributes for agreement %s\n",
                      agmt_get_long_name(ra));
    }
    /* Check that there are no verboten attributes in the exclude list */
    denied_attrs = agmt_validate_replicated_attributes(ra, 1 /* total */);
    if (denied_attrs) {
        /* Report the error to the client */
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                      "agmt_new_from_entry: Attempt to exclude illegal attributes "
                      "from a fractional agreement for total update protocol\n");
        /* Free the list */
        slapi_ch_array_free(denied_attrs);
        goto loser;
    }
    /*
     *  Extract the attributes to strip for "empty" mods
     */
    ra->attrs_to_strip = NULL;
    tmpstr = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaStripAttrs);
    if (NULL != tmpstr) {
        ra->attrs_to_strip = slapi_str2charray_ext(tmpstr, " ", 0);
        slapi_ch_free_string(&tmpstr);
    }

    if (!agmt_is_valid(ra)) {
        goto loser;
    }

    /* Now that the agreement is done, just check if changelog is configured.
     * This should not with the new per backend changelog design */
    if (!cldb_is_open(replica)) {
        slapi_log_err(SLAPI_LOG_WARNING, repl_plugin_name, "agmt_new_from_entry: "
                                                           "Replication agreement (%s) added but there is no changelog configured. "
                                                           "No change will be replicated until a changelog is configured.\n",
                                                           replica_get_name(replica)
        );
    }

    /*
     * Establish a callback for this agreement's entry, so we can
     * adorn it with status information when read.
     */
    slapi_config_register_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP, slapi_sdn_get_ndn(ra->dn),
                                   LDAP_SCOPE_BASE, "(objectclass=*)", get_agmt_status, ra);

    return ra;
loser:
    slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                  "agmt_new_from_entry - Failed to parse agreement, skipping.\n");
    agmt_delete((void **)&ra);
    return NULL;
}


Repl_Agmt *
agmt_new_from_pblock(Slapi_PBlock *pb)
{
    Slapi_Entry *e;

    slapi_pblock_get(pb, SLAPI_ADD_ENTRY, &e);
    return agmt_new_from_entry(e);
}


/*
 This should never be called directly - only should be called
 as a destructor.  XXXggood this is not finished
 */
void
agmt_delete(void **rap)
{
    Repl_Agmt *ra;
    Replica *replica = NULL;
    PR_ASSERT(NULL != rap);
    PR_ASSERT(NULL != *rap);

    ra = (Repl_Agmt *)*rap;

    /* do prot_delete first - we may be doing some processing using this
       replication agreement, and prot_delete will make sure the
       processing is complete - then it should be safe to clean up the
       other fields below
    */
    prot_delete(&ra->protocol);

    /*
     * Remove the callback for this agreement's entry
     */
    slapi_config_remove_callback(SLAPI_OPERATION_SEARCH, DSE_FLAG_PREOP,
                                 slapi_sdn_get_ndn(ra->dn),
                                 LDAP_SCOPE_BASE, "(objectclass=*)",
                                 get_agmt_status);

    /*
     * Call the replication session cleanup callback.  We
     * need to do this before we free replarea.
     */
    if (ra->agreement_type != REPLICA_TYPE_WINDOWS) {
        repl_session_plugin_call_destroy_agmt_cb(ra);
    }

    slapi_sdn_free((Slapi_DN **)&ra->dn);
    slapi_rdn_free((Slapi_RDN **)&ra->rdn);
    slapi_ch_free_string(&ra->hostname);
    slapi_ch_free_string(&ra->binddn);
    slapi_ch_free_string(&ra->bootstrapBindDN);
    slapi_ch_array_free(ra->frac_attrs);
    slapi_ch_array_free(ra->frac_attrs_total);
    ra->frac_attr_total_defined = PR_FALSE;

    if (NULL != ra->creds) {
        ber_bvfree(ra->creds);
    }
    if (NULL != ra->bootstrapCreds) {
        ber_bvfree(ra->bootstrapCreds);
    }
    if (NULL != ra->replarea) {
        /*
         * Get the replica for this agreement from the repl area
         * so we can decrement the agmt count
         */
        replica = replica_get_replica_from_dn(ra->replarea);
        if (replica) {
            replica_decr_agmt_count(replica);
        }
        slapi_sdn_free(&ra->replarea);
    }

    if (NULL != ra->consumerRUV) {
        object_release(ra->consumerRUV);
    }

    csn_free(&ra->consumerSchemaCSN);
    while (--(ra->num_changecounters) >= 0) {
        slapi_ch_free((void **)&ra->changecounters[ra->num_changecounters]);
    }
    slapi_ch_free((void **)&ra->changecounters);

    if (ra->agreement_type == REPLICA_TYPE_WINDOWS) {
        windows_agreement_delete(ra);
    }
    if (ra->attrs_to_strip) {
        slapi_ch_array_free(ra->attrs_to_strip);
    }
    if (ra->maxcsn) {
        slapi_ch_free_string(&ra->maxcsn);
    }
    schedule_destroy(ra->schedule);
    slapi_ch_free_string(&ra->long_name);
    slapi_ch_free_string(&ra->session_id_prefix);

    slapi_counter_destroy(&ra->protocol_timeout);

    /* free the locks */
    PR_DestroyLock(ra->lock);
    slapi_destroy_rwlock(ra->attr_lock);

    slapi_ch_free((void **)rap);
}


/*
 * Allow replication for this replica to begin. Replication will
 * occur at the next scheduled time. Returns 0 on success, -1 on
 * failure.
 */
int
agmt_start(Repl_Agmt *ra)
{
    Repl_Protocol *prot = NULL;
    Slapi_PBlock *pb = NULL;
    Slapi_Entry **entries = NULL;
    Slapi_DN *repl_sdn = NULL;
    char *attrs[2];
    int protocol_state;
    int found_ruv = 0;
    int rc = 0;

    /*    To Allow Consumer Initialisation when adding an agreement: */
    if (ra->auto_initialize == STATE_PERFORMING_TOTAL_UPDATE) {
        protocol_state = STATE_PERFORMING_TOTAL_UPDATE;
    } else {
        protocol_state = STATE_PERFORMING_INCREMENTAL_UPDATE;
    }

    /* First, create a new protocol object */
    if ((prot = prot_new(ra, protocol_state)) == NULL) {
        return -1;
    }

    /*
     * Set the agmt maxcsn
     *
     * We need to get the replica ruv before we take the
     * agmt lock to avoid potential deadlocks on the nsuniqueid
     * index.
     */
    repl_sdn = agmt_get_replarea(ra);
    if (!repl_sdn) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name,
                      "agmt_start: failed to get repl area.  Please check agreement.\n");
        prot_free(&prot);
        return -1;
    }

    pb = slapi_pblock_new();
    attrs[0] = (char *)type_agmtMaxCSN;
    attrs[1] = NULL;
    slapi_search_internal_set_pb_ext(
        pb,
        repl_sdn,
        LDAP_SCOPE_BASE,
        "objectclass=*",
        attrs,
        0,
        NULL,
        RUV_STORAGE_ENTRY_UNIQUEID,
        repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION),
        OP_FLAG_REPLICATED);
    slapi_search_internal_pb(pb);

    slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
    if (rc == LDAP_SUCCESS) {
        slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_SEARCH_ENTRIES, &entries);
        if (NULL == entries || NULL == entries[0]) {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                          "agmt_start: replica ruv tombstone entry for "
                          "replica %s not found\n",
                          slapi_sdn_get_dn(ra->replarea));
        } else {
            found_ruv = 1;
        }
    }

    /*
     * Now it is safe to own the agreement lock
     */
    PR_Lock(ra->lock);

    /* Check that replication is not already started */
    if (ra->protocol != NULL) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name, "replication already started for agreement \"%s\"\n", agmt_get_long_name(ra));
        prot_free(&prot);
        goto done;
    }

    /* Set and start the protocol */
    ra->protocol = prot;
    prot_start(ra->protocol);

    /*
     * If we found the repl ruv, set the agmt maxcsn...
     */
    if (found_ruv) {
        Replica *r;
        char **maxcsns = NULL;
        int i;

        maxcsns = slapi_entry_attr_get_charray(entries[0], type_agmtMaxCSN);
        if (maxcsns) {
            r = prot_get_replica(ra->protocol);
            if (r) {
                /*
                 * Loop over all the agmt maxcsns and find ours...
                 */
                for (i = 0; maxcsns[i]; i++) {
                    char buf[BUFSIZ];
                    char unavail_buf[BUFSIZ];

                    PR_snprintf(buf, BUFSIZ, "%s;%s;%s;%" PRId64 ";", slapi_sdn_get_dn(repl_sdn),
                                slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(ra->rdn)),
                                ra->hostname, ra->port);
                    PR_snprintf(unavail_buf, BUFSIZ, "%s;%s;%s;%" PRId64 ";unavailable", slapi_sdn_get_dn(repl_sdn),
                                slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(ra->rdn)),
                                ra->hostname, ra->port);
                    if (strstr(maxcsns[i], buf) || strstr(maxcsns[i], unavail_buf)) {
                        /* Set the maxcsn */
                        slapi_ch_free_string(&ra->maxcsn);
                        ra->maxcsn = slapi_ch_strdup(maxcsns[i]);
                        ra->consumerRID = agmt_maxcsn_get_rid(maxcsns[i]);
                        ra->tmpConsumerRID = 1;
                        break;
                    }
                }
            }
        }
        slapi_ch_array_free(maxcsns);
    }

done:
    PR_Unlock(ra->lock);
    slapi_free_search_results_internal(pb);
    slapi_pblock_destroy(pb);
    slapi_sdn_free(&repl_sdn);

    return 0;
}


/*
 * Allow replication for this replica to begin. Replication will
 * occur at the next scheduled time. Returns 0 on success, -1 on
 * failure.
 */
int
windows_agmt_start(Repl_Agmt *ra)
{
    Repl_Protocol *prot = NULL;

    int protocol_state;

    /*    To Allow Consumer Initialisation when adding an agreement: */
    if (ra->auto_initialize == STATE_PERFORMING_TOTAL_UPDATE) {
        protocol_state = STATE_PERFORMING_TOTAL_UPDATE;
    } else {
        protocol_state = STATE_PERFORMING_INCREMENTAL_UPDATE;
    }

    /* First, create a new protocol object */
    if ((prot = prot_new(ra, protocol_state)) == NULL) {
        return -1;
    }

    /* Now it is safe to own the agreement lock */
    PR_Lock(ra->lock);

    /* Check that replication is not already started */
    if (ra->protocol != NULL) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name, "replication already started for agreement \"%s\"\n", agmt_get_long_name(ra));
        PR_Unlock(ra->lock);
        prot_free(&prot);
        return 0;
    }

    ra->protocol = prot;

    /* Start the protocol thread */
    prot_start(ra->protocol);

    PR_Unlock(ra->lock);
    return 0;
}


/*
Cease replicating to this replica as soon as possible.
*/
int
agmt_stop(Repl_Agmt *ra)
{
    int return_value = 0;
    Repl_Protocol *rp = NULL;

    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }
    ra->stop_in_progress = PR_TRUE;
    rp = ra->protocol;
    PR_Unlock(ra->lock);
    if (NULL != rp) /* we use this pointer outside the lock - dangerous? */
    {
        prot_stop(rp);
    }
    PR_Lock(ra->lock);
    ra->stop_in_progress = PR_FALSE;
    /* we do not reuse the protocol object so free it */
    prot_free(&ra->protocol);
    PR_Unlock(ra->lock);
    return return_value;
}

/*
Send any pending updates as soon as possible, ignoring any replication
schedules.
*/
int
agmt_replicate_now(Repl_Agmt *ra __attribute__((unused)))
{
    int return_value = 0;

    return return_value;
}

/*
 * Return a copy of the remote replica's hostname.
 */
char *
agmt_get_hostname(const Repl_Agmt *ra)
{
    char *return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = slapi_ch_strdup(ra->hostname);
    PR_Unlock(ra->lock);
    return return_value;
}

/*
 * Return the port number of the remote replica's instance.
 */
int
agmt_get_port(const Repl_Agmt *ra)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->port;
    PR_Unlock(ra->lock);
    return return_value;
}

/*
 * Return the transport flags for this agreement.
 */
uint32_t
agmt_get_transport_flags(const Repl_Agmt *ra)
{
    uint32_t return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->transport_flags;
    PR_Unlock(ra->lock);
    return return_value;
}

uint32_t
agmt_get_bootstrap_transport_flags(const Repl_Agmt *ra)
{
    uint32_t return_value;

    PR_Lock(ra->lock);
    return_value = ra->bootstrapTransportFlags;
    PR_Unlock(ra->lock);

    return return_value;
}

/*
 * Return a copy of the bind dn to be used with this
 * agreement (may return NULL if no binddn is required,
 * e.g. SSL client auth.
 */
char *
agmt_get_binddn(const Repl_Agmt *ra)
{
    char *return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->binddn == NULL ? NULL : slapi_ch_strdup(ra->binddn);
    PR_Unlock(ra->lock);
    return return_value;
}

char *
agmt_get_bootstrap_binddn(const Repl_Agmt *ra)
{
    char *return_value;

    PR_Lock(ra->lock);
    return_value = ra->bootstrapBindDN == NULL ? NULL : slapi_ch_strdup(ra->bootstrapBindDN);
    PR_Unlock(ra->lock);

    return return_value;
}

/*
 * Return a copy of the credentials.
 */
struct berval *
agmt_get_credentials(const Repl_Agmt *ra)
{
    struct berval *return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = (struct berval *)slapi_ch_malloc(sizeof(struct berval));
    return_value->bv_val = (char *)slapi_ch_malloc(ra->creds->bv_len + 1);
    return_value->bv_len = ra->creds->bv_len;
    memcpy(return_value->bv_val, ra->creds->bv_val, ra->creds->bv_len);
    return_value->bv_val[return_value->bv_len] = '\0'; /* just in case */
    PR_Unlock(ra->lock);
    return return_value;
}

struct berval *
agmt_get_bootstrap_credentials(const Repl_Agmt *ra)
{
    struct berval *return_value;

    PR_Lock(ra->lock);
    return_value = (struct berval *)slapi_ch_malloc(sizeof(struct berval));
    return_value->bv_val = (char *)slapi_ch_malloc(ra->bootstrapCreds->bv_len + 1);
    return_value->bv_len = ra->bootstrapCreds->bv_len;
    memcpy(return_value->bv_val, ra->bootstrapCreds->bv_val, ra->bootstrapCreds->bv_len);
    return_value->bv_val[return_value->bv_len] = '\0'; /* just in case */
    PR_Unlock(ra->lock);

    return return_value;
}

int
agmt_get_bindmethod(const Repl_Agmt *ra)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->bindmethod;
    PR_Unlock(ra->lock);
    return return_value;
}

int64_t
agmt_get_bootstrap_bindmethod(const Repl_Agmt *ra)
{
    int64_t return_value;

    PR_Lock(ra->lock);
    return_value = ra->bootstrapBindmethod;
    PR_Unlock(ra->lock);

    return return_value;
}

/*
 * Return a copy of the dn at the top of the replicated area.
 */
Slapi_DN *
agmt_get_replarea(const Repl_Agmt *ra)
{
    Slapi_DN *return_value = NULL;
    PR_ASSERT(NULL != ra);
    if (!agmtlist_agmt_exists(ra)) {
        return return_value;
    }
    PR_Lock(ra->lock);
    return_value = slapi_sdn_new();
    slapi_sdn_copy(ra->replarea, return_value);
    PR_Unlock(ra->lock);
    return return_value;
}

int
agmt_is_fractional(const Repl_Agmt *ra)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->frac_attrs != NULL;
    PR_Unlock(ra->lock);
    return return_value;
}

/* Returns a COPY of the attr list, remember to free it */
char **
agmt_get_fractional_attrs(const Repl_Agmt *ra)
{
    char **return_value = NULL;
    PR_ASSERT(NULL != ra);
    if (NULL == ra->frac_attrs) {
        return NULL;
    }
    slapi_rwlock_rdlock(ra->attr_lock);
    return_value = charray_dup(ra->frac_attrs);
    slapi_rwlock_unlock(ra->attr_lock);
    return return_value;
}

/* Returns a COPY of the attr list, remember to free it */
char **
agmt_get_fractional_attrs_total(const Repl_Agmt *ra)
{
    char **return_value = NULL;
    PR_ASSERT(NULL != ra);
    if (!ra->frac_attr_total_defined) {
        return agmt_get_fractional_attrs(ra);
    }
    PR_Lock(ra->lock);
    return_value = charray_dup(ra->frac_attrs_total);
    PR_Unlock(ra->lock);
    return return_value;
}

int
agmt_is_fractional_attr(const Repl_Agmt *ra, const char *attrname)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    if (NULL == ra->frac_attrs) {
        return 0;
    }
    slapi_rwlock_rdlock(ra->attr_lock);
    /* Scan the list looking for a match */
    return_value = charray_inlist(ra->frac_attrs, (char *)attrname);
    slapi_rwlock_unlock(ra->attr_lock);
    return return_value;
}

int
agmt_is_fractional_attr_total(const Repl_Agmt *ra, const char *attrname)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    if (!ra->frac_attr_total_defined) {
        return agmt_is_fractional_attr(ra, attrname);
    }
    PR_Lock(ra->lock);
    /* Scan the list looking for a match */
    return_value = charray_inlist(ra->frac_attrs_total, (char *)attrname);
    PR_Unlock(ra->lock);
    return return_value;
}

int
agmt_get_auto_initialize(const Repl_Agmt *ra)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->auto_initialize;
    PR_Unlock(ra->lock);
    return return_value;
}

long
agmt_get_timeout(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->timeout;
    PR_Unlock(ra->lock);
    return return_value;
}

long
agmt_get_busywaittime(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->busywaittime;
    PR_Unlock(ra->lock);
    return return_value;
}
long
agmt_get_pausetime(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->pausetime;
    PR_Unlock(ra->lock);
    return return_value;
}

long
agmt_get_flowcontrolwindow(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->flowControlWindow;
    PR_Unlock(ra->lock);
    return return_value;
}
long
agmt_get_flowcontrolpause(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->flowControlPause;
    PR_Unlock(ra->lock);
    return return_value;
}
long
agmt_get_ignoremissing(const Repl_Agmt *ra)
{
    long return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    return_value = ra->ignoreMissingChange;
    PR_Unlock(ra->lock);
    return return_value;
}
/*
 * Warning - reference to the long name of the agreement is returned.
 * The long name of an agreement is the DN of the agreement entry,
 * followed by the host/port for the replica.
 */
const char *
agmt_get_long_name(const Repl_Agmt *ra)
{
    char *return_value = NULL;

    return_value = ra ? ra->long_name : "";
    return return_value;
}

/*
 * Warning - reference to dn is returned. However, since the dn of
 * the replication agreement is its name, it won't change during the
 * lifetime of the replication agreement object.
 */
const Slapi_DN *
agmt_get_dn_byref(const Repl_Agmt *ra)
{
    const Slapi_DN *return_value = NULL;

    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        return_value = ra->dn;
    }
    return return_value;
}

/* Return 1 if name matches the replication Dn, 0 otherwise */
int
agmt_matches_name(const Repl_Agmt *ra, const Slapi_DN *name)
{
    int return_value = 0;
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        PR_Lock(ra->lock);
        if (slapi_sdn_compare(name, ra->dn) == 0) {
            return_value = 1;
        }
        PR_Unlock(ra->lock);
    }
    return return_value;
}

/* Return 1 if name matches the replication area, 0 otherwise */
int
agmt_replarea_matches(const Repl_Agmt *ra, const Slapi_DN *name)
{
    int return_value = 0;
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        PR_Lock(ra->lock);
        if (slapi_sdn_compare(name, ra->replarea) == 0) {
            return_value = 1;
        }
        PR_Unlock(ra->lock);
    }
    return return_value;
}


int
agmt_schedule_in_window_now(const Repl_Agmt *ra)
{
    int return_value;
    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (NULL != ra->schedule && schedule_in_window_now(ra->schedule)) {
        return_value = 1;
    } else {
        return_value = 0;
    }
    PR_Unlock(ra->lock);
    return return_value;
}


/*
 * Set or reset the credentials used to bind to the remote replica.
 *
 * Returns 0 if credentials set, or -1 if an error occurred.
 */
int
agmt_set_credentials_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicaCredentials, &sattr);
    PR_Lock(ra->lock);
    slapi_ber_bvdone(ra->creds);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const struct berval *bv = slapi_value_get_berval(sval);
            slapi_ber_bvcpy(ra->creds, bv);
        }
    }
    /* If no credentials set, set to zero-length string */
    ra->creds->bv_val = NULL == ra->creds->bv_val ? slapi_ch_strdup("") : ra->creds->bv_val;
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

int32_t
agmt_set_bootstrap_credentials_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int32_t return_value = 0;

    slapi_entry_attr_find(e, type_nsds5ReplicaBootstrapCredentials, &sattr);
    PR_Lock(ra->lock);
    slapi_ber_bvdone(ra->bootstrapCreds);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const struct berval *bv = slapi_value_get_berval(sval);
            slapi_ber_bvcpy(ra->bootstrapCreds, bv);
        }
    }
    /* If no credentials set, set to zero-length string */
    if (ra->bootstrapCreds->bv_val == NULL) {
        ra->bootstrapCreds->bv_val = slapi_ch_strdup("");
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);

    return return_value;
}

/*
 * Set or reset the DN used to bind to the remote replica.
 *
 * Returns 0 if DN set, or -1 if an error occurred.
 */
int
agmt_set_binddn_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicaBindDN, &sattr);
    PR_Lock(ra->lock);
    slapi_ch_free_string(&ra->binddn);
    ra->binddn = NULL;
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            ra->binddn = slapi_ch_strdup(val);
        }
    }
    /* If no BindDN set, set to zero-length string */
    if (ra->binddn == NULL) {
        ra->binddn = slapi_ch_strdup("");
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

int32_t
agmt_set_bootstrap_binddn_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int32_t return_value = 0;

    slapi_entry_attr_find(e, type_nsds5ReplicaBootstrapBindDN, &sattr);
    PR_Lock(ra->lock);
    slapi_ch_free_string(&ra->bootstrapBindDN);
    ra->bootstrapBindDN = NULL;
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            ra->bootstrapBindDN = slapi_ch_strdup(val);
        }
    }
    /* If no BindDN set, set to zero-length string */
    if (ra->bootstrapBindDN == NULL) {
        ra->bootstrapBindDN = slapi_ch_strdup("");
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);

    return return_value;
}

/*
 * Reset the port number of the remote replica.
 *
 * Returns 0 if port set, or -1 if an error occurred.
 */
int
agmt_set_port_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicaPort, &sattr);
    PR_Lock(ra->lock);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            ra->port = slapi_value_get_int(sval);
            return_value = 0;
        }
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

/*
 * Reset the hostname of the remote replica.
 *
 * Returns 0 if hostname is set, or -1 if an error occurred.
 */
int
agmt_set_host_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicaHost, &sattr);
    PR_Lock(ra->lock);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            ra->hostname = (char *)slapi_value_get_string(sval);
            return_value = 0;
        }
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);

    return return_value;
}

static int
agmt_parse_excluded_attrs_filter(const char *attr_string, size_t *offset)
{
    char *filterstring = "(objectclass=*) ";
    size_t filterstringlen = strlen(filterstring);
    int retval = 0;

    if (strncmp(attr_string + *offset, filterstring, filterstringlen) == 0) {
        (*offset) += filterstringlen;
    } else {
        retval = -1;
    }
    return retval;
}

static int
agmt_parse_excluded_attrs_exclude(const char *attr_string, size_t *offset)
{
    char *excludestring = "$ EXCLUDE ";
    size_t excludestringlen = strlen(excludestring);
    int retval = 0;

    if (strncmp(attr_string + *offset, excludestring, excludestringlen) == 0) {
        (*offset) += excludestringlen;
    } else {
        retval = -1;
    }
    return retval;
}

static int
agmt_parse_excluded_attrs_next(const char *attr_string, size_t *offset, char ***attrs)
{
    int retval = 0;
    char *beginstr = ((char *)attr_string) + *offset;
    char *tmpstr = NULL;
    size_t stringlen = 0;
    char c = 0;

    /* Find the end of the current attribute name, if one is present */
    while (1) {
        c = *(beginstr + stringlen);
        if ('\0' == c || ' ' == c) {
            break;
        }
        stringlen++;
    }
    if (0 != stringlen) {
        tmpstr = slapi_ch_malloc(stringlen + 1);
        strncpy(tmpstr, beginstr, stringlen);
        tmpstr[stringlen] = '\0';
        if (charray_inlist(*attrs, tmpstr)) /* tmpstr is already in attrs */
        {
            slapi_ch_free_string(&tmpstr);
        } else {
            charray_add(attrs, tmpstr);
        }
        (*offset) += stringlen;
        /* Skip a delimiting space */
        if (c == ' ') {
            (*offset)++;
        }
    } else {
        retval = -1;
    }
    return retval;
}

/* It looks like this:
 * nsDS5ReplicatedAttributeList: (objectclass=*) $ EXCLUDE jpegPhoto telephoneNumber
 * This function could be called multiple times: to set excluded attrs in the
 * plugin default config and to set the ones in the replica agreement.
 * The excluded attrs from replica agreement are added to the ones from
 * default config.  (Therefore, *attrs should not be initialized in this
 * function.)
 */
static int
agmt_parse_excluded_attrs_config_attr(const char *attr_string, char ***attrs)
{
    int retval = 0;
    size_t offset = 0;
    char **new_attrs = NULL;

    /* First parse and skip the filter */
    retval = agmt_parse_excluded_attrs_filter(attr_string, &offset);
    if (retval) {
        goto error;
    }
    /* Now look for the 'EXCLUDE' keyword */
    retval = agmt_parse_excluded_attrs_exclude(attr_string, &offset);
    if (retval) {
        goto error;
    }
    /* Finally walk the list of attrs, storing in our chararray */
    while (!retval) {
        retval = agmt_parse_excluded_attrs_next(attr_string, &offset, &new_attrs);
    }
    /* If we got to here, we can't have an error */
    retval = 0;
    if (new_attrs) {
        charray_merge_nodup(attrs, new_attrs, 1);
        slapi_ch_array_free(new_attrs);
    }
error:
    return retval;
}

/*
 * _agmt_set_default_fractional_attrs
 *   helper function to set nsds5ReplicatedAttributeList value (from cn=plugin
 *   default config,cn=config) to frac_attrs in Repl_Agmt.
 *   nsds5ReplicatedAttributeList set in each agreement is added to the
 *   default list set in this function.
 */
static int
_agmt_set_default_fractional_attrs(Repl_Agmt *ra)
{
    Slapi_PBlock *newpb = NULL;
    Slapi_Entry **entries = NULL;
    int rc = LDAP_SUCCESS;
    char *attrs[2];

    attrs[0] = (char *)type_nsds5ReplicatedAttributeList;
    attrs[1] = NULL;

    newpb = slapi_pblock_new();
    slapi_search_internal_set_pb(newpb,
                                 SLAPI_PLUGIN_DEFAULT_CONFIG, /* Base DN */
                                 LDAP_SCOPE_BASE,
                                 "(objectclass=*)",
                                 attrs, /* Attrs */
                                 0,     /* AttrOnly */
                                 NULL,  /* Controls */
                                 NULL,  /* UniqueID */
                                 repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION),
                                 0);
    slapi_search_internal_pb(newpb);
    slapi_pblock_get(newpb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
    slapi_pblock_get(newpb, SLAPI_PLUGIN_INTOP_SEARCH_ENTRIES, &entries);
    ra->frac_attrs = NULL;
    if (LDAP_SUCCESS == rc && entries && *entries) /* default config entry exists */
    {
        Slapi_Attr *attr;
        Slapi_Value *sval = NULL;
        if (0 == slapi_entry_attr_find(*entries,
                                       type_nsds5ReplicatedAttributeList, &attr)) {
            int i;
            const char *val = NULL;
            for (i = slapi_attr_first_value(attr, &sval);
                 i >= 0; i = slapi_attr_next_value(attr, i, &sval)) {
                val = slapi_value_get_string(sval);
                rc = agmt_parse_excluded_attrs_config_attr(val,
                                                           &(ra->frac_attrs));
                if (0 != rc) {
                    slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                                  "_agmt_set_default_fractional_attrs: failed to "
                                  "parse default config (%s) attribute %s value: %s\n",
                                  SLAPI_PLUGIN_DEFAULT_CONFIG,
                                  type_nsds5ReplicatedAttributeList, val);
                }
            }
        }
    }

    slapi_free_search_results_internal(newpb);
    slapi_pblock_destroy(newpb);

    return rc;
}

/*
 * Set or reset the set of replicated attributes.
 *
 * Returns 0 if DN set, or -1 if an error occurred.
 */
int
agmt_set_replicated_attributes_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicatedAttributeList, &sattr);
    slapi_rwlock_wrlock(ra->attr_lock);
    if (ra->frac_attrs) {
        slapi_ch_array_free(ra->frac_attrs);
        ra->frac_attrs = NULL;
    }
    _agmt_set_default_fractional_attrs(ra);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            return_value = agmt_parse_excluded_attrs_config_attr(val, &(ra->frac_attrs));
        }
    }
    slapi_rwlock_unlock(ra->attr_lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

/*
 * Set or reset the set of total update replicated attributes.
 *
 * Returns 0 if DN set, or -1 if an error occurred.
 */
int
agmt_set_replicated_attributes_total_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    slapi_entry_attr_find(e, type_nsds5ReplicatedAttributeListTotal, &sattr);
    PR_Lock(ra->lock);
    if (ra->frac_attrs_total) {
        slapi_ch_array_free(ra->frac_attrs_total);
        ra->frac_attrs_total = NULL;
        ra->frac_attr_total_defined = PR_FALSE;
    }
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            return_value = agmt_parse_excluded_attrs_config_attr(val, &(ra->frac_attrs_total));
            if (return_value == 0) {
                ra->frac_attr_total_defined = PR_TRUE;
            }
        }
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

/*
 * Set or reset the set of replicated attributes.
 *
 * Returns 0 if DN set, or -1 if an error occurred.
 */
int
agmt_set_replicated_attributes_from_attr(Repl_Agmt *ra, Slapi_Attr *sattr)
{
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    slapi_rwlock_wrlock(ra->attr_lock);
    if (ra->frac_attrs) {
        slapi_ch_array_free(ra->frac_attrs);
        ra->frac_attrs = NULL;
    }
    _agmt_set_default_fractional_attrs(ra);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            return_value = agmt_parse_excluded_attrs_config_attr(val, &(ra->frac_attrs));
        }
    }
    slapi_rwlock_unlock(ra->attr_lock);
    return return_value;
}

/*
 * Set or reset the set of total update replicated attributes.
 *
 * Returns 0 if DN set, or -1 if an error occurred.
 */
int
agmt_set_replicated_attributes_total_from_attr(Repl_Agmt *ra, Slapi_Attr *sattr)
{
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->frac_attrs_total) {
        slapi_ch_array_free(ra->frac_attrs_total);
        ra->frac_attrs_total = NULL;
        ra->frac_attr_total_defined = PR_FALSE;
    }
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *val = slapi_value_get_string(sval);
            return_value = agmt_parse_excluded_attrs_config_attr(val, &(ra->frac_attrs_total));
            if (return_value == 0) {
                ra->frac_attr_total_defined = PR_TRUE;
            }
        }
    }
    PR_Unlock(ra->lock);
    return return_value;
}

char **
agmt_validate_replicated_attributes(Repl_Agmt *ra, int total)
{

    static char *verbotten_attrs[] = {
        "nsuniqueid",
        "modifiersname",
        "lastmodifiedtime",
        "dc", "o", "ou", "cn", "objectclass",
        NULL};

    char **retval = NULL;
    char **frac_attrs = NULL;

    /* If checking for total update, use the total attr list
     * if it exists.  If total attr list is not set, use the incremental
     * attr list. */
    if (total && ra->frac_attr_total_defined) {
        frac_attrs = ra->frac_attrs_total;
    } else {
        frac_attrs = ra->frac_attrs;
    }

    /* Iterate over the frac attrs */
    if (frac_attrs) {
        char *this_attr = NULL;
        int i = 0;
        for (i = 0; (this_attr = frac_attrs[i]); i++) {
            if (charray_inlist(verbotten_attrs, this_attr)) {
                int k = 0;
                charray_add(&retval, this_attr);
                /* Remove this attr from the list */
                for (k = i; frac_attrs[k]; k++) {
                    frac_attrs[k] = frac_attrs[k + 1];
                }
                i--;
            }
        }
    }

    return retval;
}

/*
 * Set or reset the bind method used to bind to the remote replica.
 *
 * Returns 0 if bind method set, or -1 if an error occurred.
 */
static int
agmt_set_bind_method_no_lock(Repl_Agmt *ra, const Slapi_Entry *e)
{
    const char *tmpstr = NULL;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    tmpstr = slapi_entry_attr_get_ref((Slapi_Entry *)e, type_nsds5ReplicaBindMethod);

    if (NULL == tmpstr || strcasecmp(tmpstr, "SIMPLE") == 0) {
        ra->bindmethod = BINDMETHOD_SIMPLE_AUTH;
    } else if (strcasecmp(tmpstr, "SSLCLIENTAUTH") == 0) {
        ra->bindmethod = BINDMETHOD_SSL_CLIENTAUTH;
    } else if (strcasecmp(tmpstr, "SASL/GSSAPI") == 0) {
        ra->bindmethod = BINDMETHOD_SASL_GSSAPI;
    } else if (strcasecmp(tmpstr, "SASL/DIGEST-MD5") == 0) {
        ra->bindmethod = BINDMETHOD_SASL_DIGEST_MD5;
    } else {
        ra->bindmethod = BINDMETHOD_SIMPLE_AUTH;
    }
    return return_value;
}

/* Set the bootstrap bind method, we only allow SIMPLE or SSLClientAuth */
static int32_t
agmt_set_bootstrap_bind_method_no_lock(Repl_Agmt *ra, const Slapi_Entry *e)
{
    const char *tmpstr = NULL;

    tmpstr = slapi_entry_attr_get_ref((Slapi_Entry *)e, type_nsds5ReplicaBootstrapBindMethod);
    if (NULL == tmpstr || strcasecmp(tmpstr, "SIMPLE") == 0) {
        ra->bootstrapBindmethod = BINDMETHOD_SIMPLE_AUTH;
    } else if (strcasecmp(tmpstr, "SSLCLIENTAUTH") == 0) {
        ra->bootstrapBindmethod = BINDMETHOD_SSL_CLIENTAUTH;
    } else {
        return -1;
    }

    return 0;
}


int
agmt_set_bind_method_from_entry(Repl_Agmt *ra, const Slapi_Entry *e, PRBool bootstrap)
{
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }
    if (bootstrap) {
        return_value = agmt_set_bootstrap_bind_method_no_lock(ra, e);
    } else {
        return_value = agmt_set_bind_method_no_lock(ra, e);
    }
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);
    return return_value;
}

/*
 * Set or reset the transport used to bind to the remote replica.
 *
 * Returns 0 if transport set, or -1 if an error occurred.
 */
static int
agmt_set_transportinfo_no_lock(Repl_Agmt *ra, const Slapi_Entry *e)
{
    const char *tmpstr;
    int rc = 0;

    tmpstr = slapi_entry_attr_get_ref((Slapi_Entry *)e, type_nsds5TransportInfo);
    if (!tmpstr || !strcasecmp(tmpstr, "LDAP")) {
        ra->transport_flags = 0;
    } else if (strcasecmp(tmpstr, "SSL") == 0 || strcasecmp(tmpstr, "LDAPS") == 0) {
        ra->transport_flags = TRANSPORT_FLAG_LDAPS;
    } else if (strcasecmp(tmpstr, "TLS") == 0 || strcasecmp(tmpstr, "StartTLS") == 0) {
        ra->transport_flags = TRANSPORT_FLAG_STARTTLS;
    }
    /* else do nothing - invalid value is a no-op */

    return (rc);
}

static int32_t
agmt_set_bootstrap_transportinfo_no_lock(Repl_Agmt *ra, const Slapi_Entry *e)
{
    const char *tmpstr;

    tmpstr = slapi_entry_attr_get_ref((Slapi_Entry *)e, type_nsds5ReplicaBootstrapTransportInfo);
    if (!tmpstr || !strcasecmp(tmpstr, "LDAP")) {
        ra->bootstrapTransportFlags = 0;
    } else if (strcasecmp(tmpstr, "SSL") == 0 || strcasecmp(tmpstr, "LDAPS") == 0) {
        ra->bootstrapTransportFlags = TRANSPORT_FLAG_LDAPS;
    } else if (strcasecmp(tmpstr, "TLS") == 0 || strcasecmp(tmpstr, "StartTLS") == 0) {
        ra->bootstrapTransportFlags = TRANSPORT_FLAG_STARTTLS;
    } else {
        return -1;
    }

    return 0;
}

int
agmt_set_WaitForAsyncResults(Repl_Agmt *ra, const Slapi_Entry *e)
{
    int wait = 0;
    if (e) {
        wait = slapi_entry_attr_get_int(e, type_nsds5WaitForAsyncResults);
    }
    if (wait <= 0) {
        ra->WaitForAsyncResults = 100; /* 0.1 sec */
    } else {
        ra->WaitForAsyncResults = wait;
    }
    return 0;
}

int
agmt_get_WaitForAsyncResults(Repl_Agmt *ra)
{
    return ra->WaitForAsyncResults;
}

int
agmt_set_transportinfo_from_entry(Repl_Agmt *ra, const Slapi_Entry *e, PRBool bootstrap)
{
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }
    if (bootstrap) {
        return_value = agmt_set_bootstrap_transportinfo_no_lock(ra, e);
    } else {
        return_value = agmt_set_transportinfo_no_lock(ra, e);
    }
    return_value |= agmt_set_transportinfo_no_lock(ra, e);
    PR_Unlock(ra->lock);
    prot_notify_agmt_changed(ra->protocol, ra->long_name);

    return return_value;
}


/*
 * Set or reset the replication schedule.  Notify the protocol handler
 * that a change has been made.
 *
 * Returns 0 if schedule was set or -1 if an error occurred.
 */
int
agmt_set_schedule_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr;
    int return_value = 0;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }
    PR_Unlock(ra->lock);

    if (slapi_entry_attr_find(e, type_nsds5ReplicaUpdateSchedule, &sattr) != 0) {
        sattr = NULL; /* no schedule ==> delete any existing one  */
    }

    /* make it so */
    return_value = schedule_set(ra->schedule, sattr);

    if (0 == return_value) {
        /* schedule set OK -- spread the news */
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }

    return return_value;
}

/*
 * Set or reset the timeout used to bind to the remote replica.
 *
 * Returns 0 if timeout set, or -1 if an error occurred.
 */
int
agmt_set_timeout_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_nsds5ReplicaTimeout, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            long tmpval = slapi_value_get_long(sval);
            if (tmpval >= 0) {
                ra->timeout = tmpval;
                return_value = 0; /* success! */
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}

/*
 * Set or reset the windows of entries sent without acknowledgment.
 * The window is used during update to determine the number of
 * entries will be send by the replica agreement without acknowledgment from the consumer
 *
 * Returns 0 if window set, or -1 if an error occurred.
 */
int
agmt_set_flowcontrolwindow_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_nsds5ReplicaFlowControlWindow, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            long tmpval = slapi_value_get_long(sval);
            if (tmpval >= 0) {
                ra->flowControlWindow = tmpval;
                return_value = 0; /* success! */
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}

/*
 * Set or reset the pause duration when #entries sent without acknowledgment overpass flow control window
 *
 * Returns 0 if pause set, or -1 if an error occurred.
 */
int
agmt_set_flowcontrolpause_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_nsds5ReplicaFlowControlPause, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            long tmpval = slapi_value_get_long(sval);
            if (tmpval >= 0) {
                ra->flowControlPause = tmpval;
                return_value = 0; /* success! */
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}
/* add comment here */
int
agmt_set_ignoremissing_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_replicaIgnoreMissingChange, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            const char *tmpval = slapi_value_get_string(sval);
            if (strcasecmp(tmpval, "off") == 0 || strcasecmp(tmpval, "never") == 0) {
                ra->ignoreMissingChange = 0;
                return_value = 0;
            } else if (strcasecmp(tmpval, "on") == 0 || strcasecmp(tmpval, "once") == 0) {
                ra->ignoreMissingChange = 1;
                return_value = 0;
            } else if (strcasecmp(tmpval, "always") == 0) {
                ra->ignoreMissingChange = -1;
                return_value = 0;
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}

int
agmt_set_timeout(Repl_Agmt *ra, long timeout)
{
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return -1;
    }
    ra->timeout = timeout;
    PR_Unlock(ra->lock);

    return 0;
}
int
agmt_set_flowcontrolwindow(Repl_Agmt *ra, long window)
{
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return -1;
    }
    ra->flowControlWindow = window;
    PR_Unlock(ra->lock);

    return 0;
}
int
agmt_set_flowcontrolpause(Repl_Agmt *ra, long pause)
{
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return -1;
    }
    ra->flowControlPause = pause;
    PR_Unlock(ra->lock);

    return 0;
}
int
agmt_set_ignoremissing(Repl_Agmt *ra, long ignoremissing)
{
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return -1;
    }
    ra->ignoreMissingChange = ignoremissing;
    PR_Unlock(ra->lock);
    /* if reset to 0 update the entry */
    agmt_replica_reset_ignoremissing(ra);
    return 0;
}

/*
 * Set or reset the busywaittime
 *
 * Returns 0 if busywaittime set, or -1 if an error occurred.
 */
int
agmt_set_busywaittime_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_nsds5ReplicaBusyWaitTime, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            long tmpval = slapi_value_get_long(sval);
            if (tmpval >= 0) {
                ra->busywaittime = tmpval;
                return_value = 0; /* success! */
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}

/*
 * Set or reset the pausetime
 *
 * Returns 0 if pausetime set, or -1 if an error occurred.
 */
int
agmt_set_pausetime_from_entry(Repl_Agmt *ra, const Slapi_Entry *e)
{
    Slapi_Attr *sattr = NULL;
    int return_value = -1;

    PR_ASSERT(NULL != ra);
    PR_Lock(ra->lock);
    if (ra->stop_in_progress) {
        PR_Unlock(ra->lock);
        return return_value;
    }

    slapi_entry_attr_find(e, type_nsds5ReplicaSessionPauseTime, &sattr);
    if (NULL != sattr) {
        Slapi_Value *sval = NULL;
        slapi_attr_first_value(sattr, &sval);
        if (NULL != sval) {
            long tmpval = slapi_value_get_long(sval);
            if (tmpval >= 0) {
                ra->pausetime = tmpval;
                return_value = 0; /* success! */
            }
        }
    }
    PR_Unlock(ra->lock);
    if (return_value == 0) {
        prot_notify_agmt_changed(ra->protocol, ra->long_name);
    }
    return return_value;
}

/* XXXggood - also make this pass an arg that tells if there was
 * an update to a priority attribute */
void
agmt_notify_change(Repl_Agmt *agmt, Slapi_PBlock *pb)
{
    if (NULL != pb) {
        /* Is the entry within our replicated area? */
        Slapi_DN *target_sdn = NULL;
        int change_is_relevant = 0;

        PR_ASSERT(NULL != agmt);
        PR_Lock(agmt->lock);
        if (agmt->stop_in_progress) {
            PR_Unlock(agmt->lock);
            return;
        }

        slapi_pblock_get(pb, SLAPI_TARGET_SDN, &target_sdn);
        if (NULL == target_sdn) {
            PR_Unlock(agmt->lock);
            return;
        }

        if (slapi_sdn_issuffix(target_sdn, agmt->replarea)) {
            /*
             * Yep, it's in our replicated area. Is this a fractional
             * replication agreement?
             */
            if (NULL != agmt->frac_attrs) {
                /*
                 * Yep, it's fractional. See if the change should be
                 * tossed because it doesn't affect any of the replicated
                 * attributes.
                 */
                int optype;
                int affects_non_fractional_attribute = 0;

                slapi_pblock_get(pb, SLAPI_OPERATION_TYPE, &optype);
                if (SLAPI_OPERATION_MODIFY == optype) {
                    LDAPMod **mods;
                    int i, j;

                    slapi_pblock_get(pb, SLAPI_MODIFY_MODS, &mods);
                    slapi_rwlock_rdlock(agmt->attr_lock);
                    for (i = 0; mods && !affects_non_fractional_attribute && NULL != agmt->frac_attrs[i]; i++) {
                        for (j = 0; !affects_non_fractional_attribute && NULL != mods[j]; j++) {
                            if (!slapi_attr_types_equivalent(agmt->frac_attrs[i],
                                                             mods[j]->mod_type)) {
                                affects_non_fractional_attribute = 1;
                            }
                        }
                    }
                    slapi_rwlock_unlock(agmt->attr_lock);
                } else {
                    /*
                     * Add, delete, and modrdn always cause some sort of
                     * operation replay, even if agreement is fractional.
                     */
                    affects_non_fractional_attribute = 1;
                }
                if (affects_non_fractional_attribute) {
                    change_is_relevant = 1;
                }
            } else {
                /* Not a fractional agreement */
                change_is_relevant = 1;
            }
        }
        PR_Unlock(agmt->lock);
        if (change_is_relevant) {
            /* Notify the protocol that a change has occurred */
            prot_notify_update(agmt->protocol);
        }
    }
}


int
agmt_is_50_mm_protocol(const Repl_Agmt *agmt __attribute__((unused)))
{
    return 1; /* XXXggood could support > 1 protocol */
}


int
agmt_initialize_replica(const Repl_Agmt *agmt)
{
    PR_ASSERT(NULL != agmt);
    PR_Lock(agmt->lock);
    if (agmt->stop_in_progress) {
        PR_Unlock(agmt->lock);
        return 0;
    }
    PR_Unlock(agmt->lock);
    /* Call prot_initialize_replica only if the suffix is enabled (agmt->protocol != NULL) */
    if (NULL != agmt->protocol) {
        prot_initialize_replica(agmt->protocol);
    } else {
        /* agmt->protocol == NULL --> Suffix is disabled */
        return -1;
    }
    return 0;
}

/* delete nsds5BeginReplicaRefresh attribute to indicate to the clients
   that replica initialization have completed */
void
agmt_replica_init_done(const Repl_Agmt *agmt)
{
    int rc;
    Slapi_PBlock *pb = slapi_pblock_new();
    LDAPMod *mods[2];
    LDAPMod mod;

    mods[0] = &mod;
    mods[1] = NULL;
    mod.mod_op = LDAP_MOD_DELETE | LDAP_MOD_BVALUES;
    mod.mod_type = (char *)type_nsds5ReplicaInitialize;
    mod.mod_bvalues = NULL;

    slapi_modify_internal_set_pb_ext(pb, agmt->dn, mods, NULL /* controls */,
                                     NULL /* uniqueid */, repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION), 0 /* flags */);
    slapi_modify_internal_pb(pb);

    slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
    if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_ATTRIBUTE) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_replica_init_done: "
                                                       "failed to remove (%s) attribute from (%s) entry; LDAP error - %d\n",
                      type_nsds5ReplicaInitialize, slapi_sdn_get_ndn(agmt->dn), rc);
    }

    slapi_pblock_destroy(pb);
}


/* delete nsds5replicaIgnoreMissingChange attribute */
static void
agmt_replica_reset_ignoremissing(const Repl_Agmt *agmt)
{
    int rc;
    Slapi_PBlock *pb = slapi_pblock_new();
    LDAPMod *mods[2];
    LDAPMod mod;

    mods[0] = &mod;
    mods[1] = NULL;
    mod.mod_op = LDAP_MOD_DELETE | LDAP_MOD_BVALUES;
    mod.mod_type = (char *)type_replicaIgnoreMissingChange;
    mod.mod_bvalues = NULL;

    slapi_modify_internal_set_pb_ext(pb, agmt->dn, mods, NULL /* controls */,
                                     NULL /* uniqueid */, repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION), 0 /* flags */);
    slapi_modify_internal_pb(pb);

    slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
    if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_ATTRIBUTE) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_replica_ignoremissing: "
                                                       "failed to remove (%s) attribute from (%s) entry; LDAP error - %d\n",
                      type_replicaIgnoreMissingChange, slapi_sdn_get_ndn(agmt->dn), rc);
    }

    slapi_pblock_destroy(pb);
}

/* Agreement object is acquired on behalf of the caller.
   The caller is responsible for releasing the object
   when it is no longer used */

Object *
agmt_get_consumer_ruv(Repl_Agmt *ra)
{
    Object *rt = NULL;

    PR_ASSERT(NULL != ra);

    PR_Lock(ra->lock);
    if (ra->consumerRUV) {
        object_acquire(ra->consumerRUV);
        rt = ra->consumerRUV;
    }

    PR_Unlock(ra->lock);

    return rt;
}

int
agmt_set_consumer_ruv(Repl_Agmt *ra, RUV *ruv)
{
    if (ra == NULL || ruv == NULL) {
        slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name, "agmt_set_consumer_ruv: invalid argument"
                                                        " agmt - %p, ruv - %p\n",
                      ra, ruv);
        return -1;
    }

    PR_Lock(ra->lock);

    if (ra->consumerRUV) {
        object_release(ra->consumerRUV);
    }

    ra->consumerRUV = object_new(ruv_dup(ruv), (FNFree)ruv_destroy);

    PR_Unlock(ra->lock);

    return 0;
}

void
agmt_update_init_status(Repl_Agmt *ra)
{
    int rc;
    Slapi_PBlock *pb;
    LDAPMod **mods;
    int nb_mods = 0;
    int mod_idx;
    Slapi_Mod smod_start_time = {0};
    Slapi_Mod smod_end_time = {0};
    Slapi_Mod smod_status = {0};

    PR_ASSERT(ra);
    PR_Lock(ra->lock);

    if (ra->last_init_start_time) {
        nb_mods++;
    }
    if (ra->last_init_end_time) {
        nb_mods++;
    }
    if (ra->last_init_status[0] != '\0') {
        nb_mods++;
    }
    if (nb_mods == 0) {
        /* shortcut. no need to go further */
        PR_Unlock(ra->lock);
        return;
    }
    mods = (LDAPMod **) slapi_ch_malloc((nb_mods + 1) * sizeof(LDAPMod *));
    mod_idx = 0;
    if (ra->last_init_start_time) {
        struct berval val;
        char *time_tmp = NULL;
        slapi_mod_init(&smod_start_time, 1);
        slapi_mod_set_type(&smod_start_time, type_nsds5ReplicaLastInitStart);
        slapi_mod_set_operation(&smod_start_time, LDAP_MOD_REPLACE | LDAP_MOD_BVALUES);

        time_tmp = format_genTime(ra->last_init_start_time);
        val.bv_val = time_tmp;
        val.bv_len = strlen(time_tmp);
        slapi_mod_add_value(&smod_start_time, &val);
        slapi_ch_free((void **)&time_tmp);
        mods[mod_idx] = (LDAPMod *)slapi_mod_get_ldapmod_byref(&smod_start_time);
        mod_idx++;
    }
    if (ra->last_init_end_time) {
        struct berval val;
        char *time_tmp = NULL;
        slapi_mod_init(&smod_end_time, 1);
        slapi_mod_set_type(&smod_end_time, type_nsds5ReplicaLastInitEnd);
        slapi_mod_set_operation(&smod_end_time, LDAP_MOD_REPLACE | LDAP_MOD_BVALUES);

        time_tmp = format_genTime(ra->last_init_end_time);
        val.bv_val = time_tmp;
        val.bv_len = strlen(time_tmp);
        slapi_mod_add_value(&smod_end_time, &val);
        slapi_ch_free((void **)&time_tmp);
        mods[mod_idx] = (LDAPMod *)slapi_mod_get_ldapmod_byref(&smod_end_time);
        mod_idx++;
    }
    if (ra->last_init_status[0] != '\0') {
        struct berval val;
        char *init_status = NULL;
        slapi_mod_init(&smod_status, 1);
        slapi_mod_set_type(&smod_status, type_nsds5ReplicaLastInitStatus);
        slapi_mod_set_operation(&smod_status, LDAP_MOD_REPLACE | LDAP_MOD_BVALUES);

        init_status = slapi_ch_strdup(ra->last_init_status);
        val.bv_val = init_status;
        val.bv_len = strlen(init_status);
        slapi_mod_add_value(&smod_status, &val);
        slapi_ch_free((void **)&init_status);
        mods[mod_idx] = (LDAPMod *)slapi_mod_get_ldapmod_byref(&smod_status);
        mod_idx++;
    }

    if (nb_mods) {
        /* it is ok to release the lock here because we are done with the agreement data.
           we have to do it before issuing the modify operation because it causes
           agmtlist_notify_all to be called which uses the same lock - hence the deadlock */
        PR_Unlock(ra->lock);

        pb = slapi_pblock_new();
        mods[nb_mods] = NULL;

        slapi_modify_internal_set_pb_ext(pb, ra->dn, mods, NULL, NULL,
                                         repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION), 0);
        slapi_modify_internal_pb(pb);

        slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
        if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_ATTRIBUTE) {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_update_consumer_ruv - "
                                                           "%s: agmt_update_consumer_ruv: "
                                                           "failed to update consumer's RUV; LDAP error - %d\n",
                          ra->long_name, rc);
        }

        slapi_pblock_destroy(pb);
    } else {
        PR_Unlock(ra->lock);
    }
    slapi_ch_free((void **)&mods);
    slapi_mod_done(&smod_start_time);
    slapi_mod_done(&smod_end_time);
    slapi_mod_done(&smod_status);
}

void
agmt_update_consumer_ruv(Repl_Agmt *ra)
{
    int rc;
    RUV *ruv;
    Slapi_Mod smod;
    Slapi_Mod smod_last_modified;
    Slapi_PBlock *pb;
    LDAPMod *mods[3];

    PR_ASSERT(ra);
    PR_Lock(ra->lock);

    if (ra->consumerRUV) {
        ruv = (RUV *)object_get_data(ra->consumerRUV);
        PR_ASSERT(ruv);

        ruv_to_smod(ruv, &smod);
        ruv_last_modified_to_smod(ruv, &smod_last_modified);

        /* it is ok to release the lock here because we are done with the agreement data.
           we have to do it before issuing the modify operation because it causes
           agmtlist_notify_all to be called which uses the same lock - hence the deadlock */
        PR_Unlock(ra->lock);

        pb = slapi_pblock_new();
        mods[0] = (LDAPMod *)slapi_mod_get_ldapmod_byref(&smod);
        mods[1] = (LDAPMod *)slapi_mod_get_ldapmod_byref(&smod_last_modified);
        mods[2] = NULL;

        slapi_modify_internal_set_pb_ext(pb, ra->dn, mods, NULL, NULL,
                                         repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION), 0);
        slapi_modify_internal_pb(pb);

        slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
        if (rc != LDAP_SUCCESS && rc != LDAP_NO_SUCH_ATTRIBUTE) {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_update_consumer_ruv - "
                                                           "%s: agmt_update_consumer_ruv: "
                                                           "failed to update consumer's RUV; LDAP error - %d\n",
                          ra->long_name, rc);
        }

        slapi_mod_done(&smod);
        slapi_mod_done(&smod_last_modified);
        slapi_pblock_destroy(pb);
    } else
        PR_Unlock(ra->lock);
}

CSN *
agmt_get_consumer_schema_csn(Repl_Agmt *ra)
{
    CSN *rt;

    PR_ASSERT(NULL != ra);

    PR_Lock(ra->lock);
    rt = ra->consumerSchemaCSN;
    PR_Unlock(ra->lock);

    return rt;
}

void
agmt_set_consumer_schema_csn(Repl_Agmt *ra, CSN *csn)
{
    PR_ASSERT(NULL != ra);

    PR_Lock(ra->lock);
    csn_free(&ra->consumerSchemaCSN);
    ra->consumerSchemaCSN = csn;
    PR_Unlock(ra->lock);
}

void
agmt_set_last_update_start(Repl_Agmt *ra, time_t start_time)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        ra->last_update_start_time = start_time;
        ra->last_update_end_time = 0UL;
    }
}


void
agmt_set_last_update_end(Repl_Agmt *ra, time_t end_time)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        ra->last_update_end_time = end_time;
    }
}

void
agmt_set_last_init_start(Repl_Agmt *ra, time_t start_time)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        ra->last_init_start_time = start_time;
        ra->last_init_end_time = 0UL;
    }
}


void
agmt_set_last_init_end(Repl_Agmt *ra, time_t end_time)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        ra->last_init_end_time = end_time;
    }
}

static void
agmt_set_last_update_status_json(Repl_Agmt *ra, char *state, int ldaprc, int replrc)
{
    char ts[SLAPI_TIMESTAMP_BUFSIZE];
    time_t now;

    time(&now);
    strftime(ts, sizeof ts, "%FT%TZ", gmtime(&now));
    PR_snprintf(ra->last_update_status_json, STATUS_LEN,
            "{\"state\": \"%s\", \"ldap_rc\": \"%d\", \"ldap_rc_text\": \"%s\", "
            "\"repl_rc\": \"%d\", \"repl_rc_text\": \"%s\", \"date\": \"%s\", \"message\": \"%s\"}",
            state, ldaprc, ldap_err2string(ldaprc), replrc, protocol_response2string(replrc),
            ts, ra->last_update_status);
}

void
agmt_set_last_update_status(Repl_Agmt *ra, int ldaprc, int replrc, const char *message)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        if (replrc == NSDS50_REPL_UPTODATE) {
            /* no session started, no status update */
        } else if (ldaprc != LDAP_SUCCESS) {
            char *replmsg = NULL;

            if (replrc) {
                replmsg = protocol_response2string(replrc);
                /* Do not mix the unknown replication error with the known ldap one */
                if (strcasecmp(replmsg, "unknown error") == 0) {
                    replmsg = NULL;
                }
            }
            PR_snprintf(ra->last_update_status, STATUS_LEN, "Error (%d) %s%s - LDAP error: %s%s%s%s",
                        ldaprc, message ? message : "", message ? "" : " - ",
                        slapi_err2string(ldaprc), replmsg ? " (" : "", replmsg ? replmsg : "", replmsg ? ")" : "");
            agmt_set_last_update_status_json(ra, STATUS_BAD, ldaprc, replrc);
        }
        /* ldaprc == LDAP_SUCCESS */
        else if (replrc != 0) {
            if (replrc == NSDS50_REPL_REPLICA_BUSY) {
                PR_snprintf(ra->last_update_status, STATUS_LEN,
                            "Error (%d) Can't acquire busy replica (%s)",
                            replrc, message ? message : "");
                agmt_set_last_update_status_json(ra, STATUS_WARNING, ldaprc, replrc);
            } else if (replrc == NSDS50_REPL_TRANSIENT_ERROR  || replrc == NSDS50_REPL_BACKOFF) {
                PR_snprintf(ra->last_update_status, STATUS_LEN,
                            "Error (%d) Can't acquire replica (%s)",
                            replrc, message ? message : "");
                agmt_set_last_update_status_json(ra, STATUS_WARNING, ldaprc, replrc);
            } else if (replrc == NSDS50_REPL_REPLICA_RELEASE_SUCCEEDED) {
                PR_snprintf(ra->last_update_status, STATUS_LEN, "Error (0) Replication session successful");
                agmt_set_last_update_status_json(ra, STATUS_GOOD, ldaprc, replrc);
            } else if (replrc == NSDS50_REPL_DISABLED) {
                PR_snprintf(ra->last_update_status, STATUS_LEN, "Error (%d) Incremental update aborted: "
                                                                "Replication agreement for %s\n can not be updated while the replica is disabled.\n"
                                                                "(If the suffix is disabled you must enable it then restart the server for replication to take place).",
                            replrc, ra->long_name ? ra->long_name : "a replica");
                agmt_set_last_update_status_json(ra, STATUS_BAD, ldaprc, replrc);
                /* Log into the errors log, as "ra->long_name" is not accessible from the caller */
                slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                              "Incremental update aborted: Replication agreement for \"%s\" "
                              "can not be updated while the replica is disabled\n",
                              ra->long_name ? ra->long_name : "a replica");
                slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                              "(If the suffix is disabled you must enable it then restart the server for replication to take place).\n");
            } else {
                PR_snprintf(ra->last_update_status, STATUS_LEN,
                            "Error (%d) Replication error acquiring replica: %s%s(%s)",
                            replrc, message ? message : "", message ? " " : "", protocol_response2string(replrc));
                agmt_set_last_update_status_json(ra, STATUS_BAD, ldaprc, replrc);
            }
        } else if (message != NULL) /* replrc == NSDS50_REPL_REPLICA_READY == 0 */
        {
            PR_snprintf(ra->last_update_status, STATUS_LEN,
                        "Error (0) Replica acquired successfully: %s", message);
            agmt_set_last_update_status_json(ra, STATUS_GOOD, ldaprc, replrc);
        } else { /* agmt_set_last_update_status(0,0,NULL) to reset agmt */
            ra->last_update_status[0] = '\0';
            ra->last_update_status_json[0] = '\0';
        }
    }
}

static void
agmt_set_last_init_status_json(Repl_Agmt *ra, char *state, int ldaprc, int replrc, int connrc)
{
    char ts[SLAPI_TIMESTAMP_BUFSIZE];
    time_t now;

    time(&now);
    strftime(ts, sizeof ts, "%FT%TZ", gmtime(&now));
    PR_snprintf(ra->last_init_status_json, STATUS_LEN,
            "{\"state\": \"%s\", \"ldap_rc\": \"%d\", \"ldap_rc_text\": \"%s\", \"repl_rc\": \"%d\", \"repl_rc_text\": \"%s\", "
            "\"conn_rc\": \"%d\", \"conn_rc_text\": \"%s\", \"date\": \"%s\", \"message\": \"%s\"}",
            state, ldaprc, ldap_err2string(ldaprc), replrc, protocol_response2string(replrc),
            connrc, conn_result2string(connrc), ts, ra->last_init_status);
}

void
agmt_set_last_init_status(Repl_Agmt *ra, int ldaprc, int replrc, int connrc, const char *message)
{
    char *connmsg = NULL;
    char unknown_connrc[100] = {0};

    connmsg = conn_result2string(connrc);
    if (connrc && (connmsg == NULL)) {
        /* That was an unknown connection error */
        PR_snprintf(unknown_connrc, sizeof(unknown_connrc), "Unknown connection error (%d)", connrc);
        connmsg = unknown_connrc;
    }

    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        if (ldaprc != LDAP_SUCCESS) {
            char *replmsg = NULL;

            if (replrc) {
                replmsg = protocol_response2string(replrc);
                /* Do not mix the unknown replication error with the known ldap one */
                if (strcasecmp(replmsg, "unknown error") == 0) {
                    replmsg = NULL;
                }
            }
            PR_snprintf(ra->last_init_status, STATUS_LEN, "Error (%d)%s%sLDAP error: %s%s%s%s%s",
                        ldaprc, message ? message : "", message ? "" : " - ",
                        slapi_err2string(ldaprc), replmsg ? " - " : "", replmsg ? replmsg : "",
                        connrc ? " - " : "", connrc ? connmsg : "");
            agmt_set_last_init_status_json(ra, STATUS_BAD, ldaprc, replrc, connrc);
        }
        /* ldaprc == LDAP_SUCCESS */
        else if (replrc != 0) {
            if (replrc == NSDS50_REPL_REPLICA_RELEASE_SUCCEEDED) {
                PR_snprintf(ra->last_init_status, STATUS_LEN, "Replication session successful");
            } else if (replrc == NSDS50_REPL_DISABLED) {
                if (agmt_is_enabled(ra)) {
                    slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "Total update aborted: "
                                                                   "Replication agreement for \"%s\" can not be updated while the suffix is disabled.\n"
                                                                   "You must enable it then restart the server for replication to take place).\n",
                                  ra->long_name ? ra->long_name : "a replica");
                    PR_snprintf(ra->last_init_status, STATUS_LEN, "Error (%d) Total update aborted: "
                                                                  "Replication agreement for \"%s\" can not be updated while the suffix is disabled.\n"
                                                                  "You must enable it then restart the server for replication to take place).",
                                replrc, ra->long_name ? ra->long_name : "a replica");
                    agmt_set_last_init_status_json(ra, STATUS_BAD, ldaprc, replrc, connrc);
                } else {
                    /* You do not need to restart the server after enabling the agreement */
                    slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "Total update aborted: "
                                                                   "Replication agreement for \"%s\" can not be updated while the agreement is disabled\n",
                                  ra->long_name ? ra->long_name : "a replica");
                    PR_snprintf(ra->last_init_status, STATUS_LEN, "Error (%d) Total update aborted: "
                                                                  "Replication agreement for \"%s\" can not be updated while the agreement is disabled.",
                                replrc, ra->long_name ? ra->long_name : "a replica");
                    agmt_set_last_init_status_json(ra, STATUS_BAD, ldaprc, replrc, connrc);
                }
            } else {
                PR_snprintf(ra->last_init_status, STATUS_LEN,
                            "Error (%d) Replication error acquiring replica: %s%s%s%s%s",
                            replrc, protocol_response2string(replrc),
                            message ? " - " : "", message ? message : "",
                            connrc ? " - " : "", connrc ? connmsg : "");
                agmt_set_last_init_status_json(ra, STATUS_BAD, ldaprc, replrc, connrc);
            }
        } else if (connrc != CONN_OPERATION_SUCCESS) {
            PR_snprintf(ra->last_init_status, STATUS_LEN,
                        "Error (%d) connection error: %s%s%s",
                        connrc, connmsg,
                        message ? " - " : "", message ? message : "");
            agmt_set_last_init_status_json(ra, STATUS_BAD, ldaprc, replrc, connrc);
        } else if (message != NULL) { /* replrc == NSDS50_REPL_REPLICA_READY == 0 */
            PR_snprintf(ra->last_init_status, STATUS_LEN,
                        "Error (%d) %s", ldaprc, message);
            agmt_set_last_init_status_json(ra, STATUS_GOOD, ldaprc, replrc, connrc);
        } else { /* agmt_set_last_init_status(0,0,NULL) to reset agmt */
            ra->last_init_status[0] = '\0';
            ra->last_init_status_json[0] = '\0';
        }
    }
}


void
agmt_set_update_in_progress(Repl_Agmt *ra, PRBool in_progress)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        ra->update_in_progress = in_progress;
    }
}

PRBool
agmt_get_update_in_progress(const Repl_Agmt *ra)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        return ra->update_in_progress;
    }
    return PR_FALSE;
}

void
agmt_inc_last_update_changecount(Repl_Agmt *ra, ReplicaId rid, int skipped)
{
    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        int i;

        for (i = 0; i < ra->num_changecounters; i++) {
            if (ra->changecounters[i]->rid == rid)
                break;
        }

        if (i < ra->num_changecounters) {
            if (skipped)
                ra->changecounters[i]->num_skipped++;
            else
                ra->changecounters[i]->num_replayed++;
        } else {
            ra->num_changecounters++;
            if (ra->num_changecounters > ra->max_changecounters) {
                ra->changecounters = (struct changecounter **)slapi_ch_realloc((char *)ra->changecounters,
                                                                               (ra->num_changecounters + 1) * sizeof(struct changecounter *));
                ra->max_changecounters = ra->num_changecounters;
            }
            ra->changecounters[i] = (struct changecounter *)slapi_ch_calloc(1, sizeof(struct changecounter));
            ra->changecounters[i]->rid = rid;
            if (skipped)
                ra->changecounters[i]->num_skipped = 1;
            else
                ra->changecounters[i]->num_replayed = 1;
        }
    }
}

void
agmt_get_changecount_string(Repl_Agmt *ra, char *buf, int bufsize)
{
    char tmp_buf[32]; /* 5 digit RID, 10 digit each replayed and skipped */
    int i;
    int buflen = 0;

    *buf = '\0';
    if (NULL != ra) {
        for (i = 0; i < ra->num_changecounters; i++) {
            PR_snprintf(tmp_buf, sizeof(tmp_buf), "%u:%u/%u ",
                        ra->changecounters[i]->rid,
                        ra->changecounters[i]->num_replayed,
                        ra->changecounters[i]->num_skipped);
            PR_snprintf(buf + buflen, bufsize - buflen, "%s", tmp_buf);
            buflen += strlen(tmp_buf);
        }
    }
}

static int
get_agmt_status(Slapi_PBlock *pb __attribute__((unused)),
                Slapi_Entry *e,
                Slapi_Entry *entryAfter __attribute__((unused)),
                int *returncode __attribute__((unused)),
                char *returntext __attribute__((unused)),
                void *arg __attribute__((unused)))
{
    char *time_tmp = NULL;
    char changecount_string[BUFSIZ];
    Repl_Agmt *ra = (Repl_Agmt *)arg;

    PR_ASSERT(NULL != ra);
    if (NULL != ra) {
        PRBool reapActive = PR_FALSE;
        Slapi_DN *replarea_sdn = NULL;

        replarea_sdn = agmt_get_replarea(ra);
        if (!replarea_sdn) {
            goto bail;
        }
        Replica *replica = replica_get_replica_from_dn(replarea_sdn);
        slapi_sdn_free(&replarea_sdn);
        if (replica) {
            reapActive = replica_get_tombstone_reap_active(replica);
        }
        slapi_entry_attr_set_int(e, "nsds5replicaReapActive", (int)reapActive);

        /* these values persist in the dse.ldif file, so we delete them
           here to avoid multi valued attributes */
        slapi_entry_attr_delete(e, "nsds5replicaLastUpdateStart");
        slapi_entry_attr_delete(e, "nsds5replicaLastUpdateEnd");
        slapi_entry_attr_delete(e, "nsds5replicaChangesSentSinceStartup");
        slapi_entry_attr_delete(e, "nsds5replicaLastUpdateStatus");
        slapi_entry_attr_delete(e, "nsds5replicaUpdateInProgress");
        slapi_entry_attr_delete(e, "nsds5replicaLastInitStart");
        slapi_entry_attr_delete(e, "nsds5replicaLastInitStatus");
        slapi_entry_attr_delete(e, "nsds5replicaLastInitEnd");

        /* now, add the real values (singly) */
        /* In case last_update_start_time is not set, 19700101000000Z is set. */
        time_tmp = format_genTime(ra->last_update_start_time);
        slapi_entry_add_string(e, "nsds5replicaLastUpdateStart", time_tmp);
        slapi_ch_free((void **)&time_tmp);

        /* In case last_update_end_time is not set, 19700101000000Z is set. */
        time_tmp = format_genTime(ra->last_update_end_time);
        slapi_entry_add_string(e, "nsds5replicaLastUpdateEnd", time_tmp);
        slapi_ch_free((void **)&time_tmp);

        agmt_get_changecount_string(ra, changecount_string, sizeof(changecount_string));
        slapi_entry_add_string(e, "nsds5replicaChangesSentSinceStartup", changecount_string);
        if (ra->last_update_status[0] == '\0') {
            char status_msg[STATUS_LEN];
            char ts[SLAPI_TIMESTAMP_BUFSIZE];
            time_t now;
            time(&now);
            strftime(ts, sizeof ts, "%FT%TZ", gmtime(&now));
            slapi_entry_add_string(e, "nsds5replicaLastUpdateStatus",
                                   "Error (0) No replication sessions started since server startup");
            PR_snprintf(status_msg, STATUS_LEN,
                    "{\"state\": \"green\", \"ldap_rc\": \"0\", \"ldap_rc_text\": \"success\", \"repl_rc\": \"0\", \"repl_rc_text\": \"replica acquired\", "
                    "\"date\": \"%s\", \"message\": \"Error (0) No replication sessions started since server startup\"}", ts);
            slapi_entry_add_string(e, "nsds5replicaLastUpdateStatusJSON", status_msg);
        } else {
            slapi_entry_add_string(e, "nsds5replicaLastUpdateStatus", ra->last_update_status);
            slapi_entry_add_string(e, "nsds5replicaLastUpdateStatusJSON", ra->last_update_status_json);
        }
        slapi_entry_add_string(e, "nsds5replicaUpdateInProgress", ra->update_in_progress ? "TRUE" : "FALSE");

        /* In case last_init_start_time is not set, 19700101000000Z is set. */
        time_tmp = format_genTime(ra->last_init_start_time);
        slapi_entry_add_string(e, "nsds5replicaLastInitStart", time_tmp);
        slapi_ch_free((void **)&time_tmp);

        /* In case last_init_end_time is not set, 19700101000000Z is set. */
        time_tmp = format_genTime(ra->last_init_end_time);
        slapi_entry_add_string(e, "nsds5replicaLastInitEnd", time_tmp);
        slapi_ch_free((void **)&time_tmp);

        if (ra->last_init_status[0] != '\0') {
            slapi_entry_add_string(e, "nsds5replicaLastInitStatus", ra->last_init_status);
            slapi_entry_add_string(e, "nsds5replicaLastInitStatusJSON", ra->last_init_status_json);
        }
    }
bail:
    return SLAPI_DSE_CALLBACK_OK;
}

static void
update_window_state_change_callback(void *arg, PRBool opened)
{
    Repl_Agmt *agmt = (Repl_Agmt *)arg;

    PR_ASSERT(agmt);

    if (opened) {
        prot_notify_window_opened(agmt->protocol);
    } else {
        prot_notify_window_closed(agmt->protocol);
    }
}

ReplicaId
agmt_get_consumer_rid(Repl_Agmt *agmt, void *conn)
{
    if (agmt->consumerRID <= 0 || agmt->tmpConsumerRID) {

        char *mt_node = NULL;
        struct berval **bvals = NULL;

        /* This function converts the old style DN to the new one. */
        mt_node =
            slapi_create_dn_string("cn=replica,cn=\"%s\",cn=mapping tree,cn=config",
                                   slapi_sdn_get_dn(agmt->replarea));
        if (NULL == mt_node) {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                          "agmt_get_consumer_rid: failed to normalize "
                          "replica dn for %s\n",
                          slapi_sdn_get_dn(agmt->replarea));
            agmt->consumerRID = 0;
        }
        conn_read_entry_attribute(conn, mt_node, "nsDS5ReplicaID", &bvals);
        if (NULL != bvals && NULL != bvals[0]) {
            char *ridstr = slapi_ch_malloc(bvals[0]->bv_len + 1);
            memcpy(ridstr, bvals[0]->bv_val, bvals[0]->bv_len);
            ridstr[bvals[0]->bv_len] = '\0';
            agmt->consumerRID = atoi(ridstr);
            slapi_ch_free((void **)&ridstr);
            ber_bvecfree(bvals);
        }
        slapi_ch_free_string(&mt_node);
    }
    agmt->tmpConsumerRID = 0;

    return agmt->consumerRID;
}

int
get_agmt_agreement_type(Repl_Agmt *agmt)
{
    PR_ASSERT(agmt);
    return agmt->agreement_type;
}

void *
agmt_get_priv(const Repl_Agmt *agmt)
{
    PR_ASSERT(agmt);
    return agmt->priv;
}

void
agmt_set_priv(Repl_Agmt *agmt, void *priv)
{
    PR_ASSERT(agmt);
    agmt->priv = priv;
}

ReplicaId
agmt_get_consumerRID(Repl_Agmt *ra)
{
    return ra->consumerRID;
}

void *
agmt_get_connection(Repl_Agmt *ra)
{
    if (ra->protocol) {
        return (void *)prot_get_connection(ra->protocol);
    } else {
        return NULL;
    }
}

Repl_Protocol *
agmt_get_protocol(Repl_Agmt *ra)
{
    if (ra) {
        return ra->protocol;
    } else {
        return NULL;
    }
}

int
agmt_has_protocol(Repl_Agmt *agmt)
{
    if (agmt) {
        return NULL != agmt->protocol;
    }
    return 0;
}

PRBool
agmt_is_enabled(Repl_Agmt *ra)
{
    PRBool state;
    PR_Lock(ra->lock);
    state = ra->is_enabled;
    PR_Unlock(ra->lock);

    return state;
}

int
agmt_set_enabled_from_entry(Repl_Agmt *ra, Slapi_Entry *e, char *returntext)
{
    const char *attr_val = NULL;
    int rc = 0;

    if (ra == NULL) {
        return -1;
    }

    PR_Lock(ra->lock);

    attr_val = slapi_entry_attr_get_ref(e, type_nsds5ReplicaEnabled);
    if (attr_val) {
        PRBool is_enabled = PR_TRUE;
        if (strcasecmp(attr_val, "off") == 0) {
            is_enabled = PR_FALSE;
        } else if (strcasecmp(attr_val, "on") == 0) {
            is_enabled = PR_TRUE;
        } else {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_set_enabled_from_entry: invalid "
                                                           "value for nsds5ReplicaEnabled (%s), the value must be \"on\" or \"off\".\n",
                          attr_val);
            PR_snprintf(returntext, SLAPI_DSE_RETURNTEXT_SIZE, "Invalid value for nsds5ReplicaEnabled, "
                                                               "the value must be \"on\" or \"off\".\n");
            PR_Unlock(ra->lock);
            return -1;
        }
        if (is_enabled) {
            if (!ra->is_enabled) {
                ra->is_enabled = PR_TRUE;
                slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name, "agmt_set_enabled_from_entry: "
                                                                "agreement is now enabled (%s)\n",
                              ra->long_name);
                PR_Unlock(ra->lock);
                agmt_start(ra);
                return rc;
            }
        } else {
            if (ra->is_enabled) {
                ra->is_enabled = PR_FALSE;
                slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name, "agmt_set_enabled_from_entry: "
                                                                "agreement is now disabled (%s)\n",
                              ra->long_name);
                PR_Unlock(ra->lock);
                agmt_stop(ra);
                agmt_update_consumer_ruv(ra);
                agmt_update_init_status(ra);
                agmt_set_last_update_status(ra, 0, 0, "agreement disabled");
                return rc;
            }
        }
    } else {
        rc = -1;
    }
    PR_Unlock(ra->lock);

    return rc;
}

char **
agmt_get_attrs_to_strip(Repl_Agmt *ra)
{
    if (ra) {
        return ra->attrs_to_strip;
    } else {
        return NULL;
    }
}

int
agmt_set_attrs_to_strip(Repl_Agmt *ra, Slapi_Entry *e)
{
    char *tmpstr = NULL;

    tmpstr = slapi_entry_attr_get_charptr(e, type_nsds5ReplicaStripAttrs);

    PR_Lock(ra->lock);
    if (ra->attrs_to_strip) {
        slapi_ch_array_free(ra->attrs_to_strip);
    }
    if (tmpstr == NULL) {
        ra->attrs_to_strip = NULL;
    } else {
        ra->attrs_to_strip = slapi_str2charray_ext(tmpstr, " ", 0);
        slapi_ch_free_string(&tmpstr);
    }
    PR_Unlock(ra->lock);

    prot_notify_agmt_changed(ra->protocol, ra->long_name);

    return 0;
}

/* this is called whenever an update (total/incremental)
   is completed */
void
agmt_update_done(Repl_Agmt *agmt, int is_total)
{
    /* we could do a lot of stuff here - consolidate all of the other stuff that gets
       done at the end of an update - setting status, times, etc.
       but for now, all we want to do is "flush" any pending changes made
       during the update into the proper structures so they are in place for the
       next run
    */
    if (get_agmt_agreement_type(agmt) == REPLICA_TYPE_WINDOWS) {
        windows_update_done(agmt, is_total);
    }
}

uint64_t
agmt_get_protocol_timeout(Repl_Agmt *agmt)
{
    if (agmt) {
        return slapi_counter_get_value(agmt->protocol_timeout);
    } else {
        return 0;
    }
}

void
agmt_set_protocol_timeout(Repl_Agmt *agmt, uint64_t timeout)
{
    if (agmt) {
        slapi_counter_set_value(agmt->protocol_timeout, timeout);
    }
}

/*
 *  Check if all the mods are being removed by fractional/stripped attributes
 */
void
agmt_update_maxcsn(Replica *r, Slapi_DN *sdn, int op, LDAPMod **mods, CSN *csn)
{
    Object *agmt_obj;
    Repl_Agmt *agmt;
    ReplicaId rid = replica_get_rid(r);
    int mod_count = 0, excluded_count = 0;

    agmt_obj = agmtlist_get_first_agreement_for_replica(r);
    if (agmt_obj == NULL) { /* no agreements */
        return;
    }
    while (agmt_obj) {
        agmt = (Repl_Agmt *)object_get_data(agmt_obj);
        if (!agmt_is_enabled(agmt) ||
            !slapi_sdn_issuffix(sdn, agmt->replarea) ||
            get_agmt_agreement_type(agmt) == REPLICA_TYPE_WINDOWS) {
            agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
            continue;
        }

        if (op == SLAPI_OPERATION_MODIFY) {
            slapi_rwlock_rdlock(agmt->attr_lock);
            for (excluded_count = 0, mod_count = 0; mods && (NULL != mods[mod_count]); mod_count++) {
                if (charray_inlist(agmt->frac_attrs, mods[mod_count]->mod_type)) {
                    excluded_count++;
                } else if (charray_inlist(agmt->attrs_to_strip, mods[mod_count]->mod_type)) {
                    excluded_count++;
                }
            }
            slapi_rwlock_unlock(agmt->attr_lock);
        }

        if (excluded_count == 0 || excluded_count != mod_count) {
            /*
             *  This update has not been completely stripped down, update
             *  the agmt maxcsn - if the update did not originate from the consumer.
             */
            char maxcsn[CSN_STRSIZE];
            ReplicaId oprid = csn_get_replicaid(csn);

            csn_as_string(csn, PR_FALSE, maxcsn);
            PR_Lock(agmt->lock);
            if (!agmt->consumerRID) {
                /*
                 * If the RID is 0, that means this is the first update since the
                 * agmt was created.  Since we have not contacted the consumer yet,
                 * we don't know what its rid is.  The consumerRID will be set once
                 * this update is sent, but until then we don't know it. So for now
                 * temporarily mark it as "unavailable".
                 */
                slapi_ch_free_string(&agmt->maxcsn);
                agmt->maxcsn = slapi_ch_smprintf("%s;%s;%s;%" PRId64 ";unavailable;%s", slapi_sdn_get_dn(agmt->replarea),
                                                 slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(agmt->rdn)), agmt->hostname,
                                                 agmt->port, maxcsn);
            } else if (rid == oprid) {
                slapi_ch_free_string(&agmt->maxcsn);
                agmt->maxcsn = slapi_ch_smprintf("%s;%s;%s;%" PRId64 ";%" PRIu16 ";%s", slapi_sdn_get_dn(agmt->replarea),
                                                 slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(agmt->rdn)), agmt->hostname,
                                                 agmt->port, agmt->consumerRID, maxcsn);
            }
            PR_Unlock(agmt->lock);
        }
        agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
    }
}

/*
 * Returns the in-memory agmt maxcsn's
 */
void
add_agmt_maxcsns(Slapi_Entry *e, Replica *r)
{
    Object *agmt_obj;
    Repl_Agmt *agmt;

    agmt_obj = agmtlist_get_first_agreement_for_replica(r);
    if (agmt_obj == NULL) { /* no agreements */
        return;
    }
    while (agmt_obj) {
        agmt = (Repl_Agmt *)object_get_data(agmt_obj);
        if (!agmt_is_enabled(agmt) || get_agmt_agreement_type(agmt) == REPLICA_TYPE_WINDOWS) {
            agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
            continue;
        }
        PR_Lock(agmt->lock);
        if (agmt->maxcsn) {
            slapi_entry_add_string(e, type_agmtMaxCSN, agmt->maxcsn);
        }
        PR_Unlock(agmt->lock);

        agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
    }
}

/*
 * Create a smod of all the agmt maxcsns to add to the tombstone entry.
 * Regardless if there is an error, smod always needs to be freed by the caller.
 */
int
agmt_maxcsn_to_smod(Replica *r, Slapi_Mod *smod)
{
    Object *agmt_obj;
    Repl_Agmt *agmt;
    int rc = 1;

    slapi_mod_init(smod, replica_get_agmt_count(r) + 1);
    slapi_mod_set_type(smod, type_agmtMaxCSN);
    slapi_mod_set_operation(smod, LDAP_MOD_REPLACE | LDAP_MOD_BVALUES);

    agmt_obj = agmtlist_get_first_agreement_for_replica(r);
    if (agmt_obj == NULL) { /* no agreements */
        return rc;
    }

    while (agmt_obj) {
        struct berval val;

        agmt = (Repl_Agmt *)object_get_data(agmt_obj);
        if (!agmt_is_enabled(agmt) || get_agmt_agreement_type(agmt) == REPLICA_TYPE_WINDOWS) {
            agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
            continue;
        }
        PR_Lock(agmt->lock);
        if (agmt->maxcsn == NULL) {
            PR_Unlock(agmt->lock);
            agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
            continue;
        }
        val.bv_val = agmt->maxcsn;
        val.bv_len = strlen(val.bv_val);
        slapi_mod_add_value(smod, &val);
        PR_Unlock(agmt->lock);
        rc = 0;

        agmt_obj = agmtlist_get_next_agreement_for_replica(r, agmt_obj);
    }
    return rc;
}

/*
 * Parse out the consumer replicaID from the agmt maxcsn
 *
 *  "repl area;agmt_rdn;hostname;port;consumer_rid;maxcsn"
 */
static ReplicaId
agmt_maxcsn_get_rid(char *maxcsn)
{
    ReplicaId rid = 0;
    char *token = NULL;
    char *iter = NULL;
    char *value = slapi_ch_strdup(maxcsn);

    (void) ldap_utf8strtok_r(value, ";", &iter);  /* repl area */
    (void) ldap_utf8strtok_r(iter, ";", &iter);   /* agmt rdn */
    (void) ldap_utf8strtok_r(iter, ";", &iter);   /* host */
    (void) ldap_utf8strtok_r(iter, ";", &iter);   /* port */
    token = ldap_utf8strtok_r(iter, ";", &iter);  /* rid */

    if (token && strcmp(token, "Unavailable")) {
        rid = atoi(token);
    }
    slapi_ch_free_string(&value);

    return rid;
}

/*
 * Agmt being deleted, remove the agmt maxcsn from the local ruv.
 */
void
agmt_remove_maxcsn(Repl_Agmt *ra)
{
    Slapi_PBlock *pb = NULL;
    Slapi_PBlock *modpb = NULL;
    Slapi_Entry **entries = NULL;
    Replica *r = NULL;
    const Slapi_DN *tombstone_sdn = NULL;
    char *attrs[2];
    int rc;

    if (ra->protocol == NULL) {
        /* nothing to do, agmt is not started */
        return;
    }

    pb = slapi_pblock_new();
    if (!pb) {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_remove_maxcsn: Out of memory\n");
        goto done;
    }

    r = prot_get_replica(ra->protocol);
    if (r) {
        tombstone_sdn = replica_get_root(r);
    } else {
        slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name, "agmt_remove_maxcsn: Failed to get repl object.\n");
        goto done;
    }
    slapi_ch_free_string(&ra->maxcsn);
    attrs[0] = (char *)type_agmtMaxCSN;
    attrs[1] = NULL;

    slapi_search_internal_set_pb_ext(
        pb,
        (Slapi_DN *)tombstone_sdn,
        LDAP_SCOPE_BASE,
        "objectclass=*",
        attrs,
        0,    /* attrsonly */
        NULL, /* controls */
        RUV_STORAGE_ENTRY_UNIQUEID,
        repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION),
        OP_FLAG_REPLICATED); /* flags */
    slapi_search_internal_pb(pb);
    slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_RESULT, &rc);

    if (rc == LDAP_SUCCESS) {
        /*
         *  Ok we have the db tombstone entry, start looking through the agmt maxcsns
         *  for a match to this replica agmt.
         */
        Slapi_Mod smod;
        LDAPMod *mods[2];
        char **maxcsns = NULL;
        int i;

        slapi_pblock_get(pb, SLAPI_PLUGIN_INTOP_SEARCH_ENTRIES, &entries);
        if (NULL == entries || NULL == entries[0]) {
            slapi_log_err(SLAPI_LOG_ERR, repl_plugin_name,
                          "agmt_remove_maxcsn: replica ruv tombstone entry for "
                          "replica %s not found\n",
                          slapi_sdn_get_dn(ra->replarea));
            goto done;
        }
        maxcsns = slapi_entry_attr_get_charray(entries[0], type_agmtMaxCSN);
        if (maxcsns) {
            r = prot_get_replica(ra->protocol);
            if (r) {
                /*
                 * Loop over all the agmt maxcsns and find ours...
                 */
                for (i = 0; maxcsns[i]; i++) {
                    char buf[BUFSIZ];
                    char unavail_buf[BUFSIZ];
                    struct berval val;

                    PR_snprintf(buf, BUFSIZ, "%s;%s;%s;%" PRId64 ";", slapi_sdn_get_dn(ra->replarea),
                                slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(ra->rdn)),
                                ra->hostname, ra->port);
                    PR_snprintf(unavail_buf, BUFSIZ, "%s;%s;%s;%" PRId64 ";unavailable",
                                slapi_sdn_get_dn(ra->replarea),
                                slapi_rdn_get_value_by_ref(slapi_rdn_get_rdn(ra->rdn)),
                                ra->hostname, ra->port);
                    if (strstr(maxcsns[i], buf) || strstr(maxcsns[i], unavail_buf)) {
                        /*
                         * We found the matching agmt maxcsn, now remove agmt maxcsn
                         * from the tombstone entry.
                         */
                        val.bv_val = maxcsns[i];
                        val.bv_len = strlen(maxcsns[i]);
                        slapi_mod_init(&smod, 2);
                        slapi_mod_set_type(&smod, type_agmtMaxCSN);
                        slapi_mod_set_operation(&smod, LDAP_MOD_DELETE | LDAP_MOD_BVALUES);
                        slapi_mod_add_value(&smod, &val);
                        mods[0] = smod.mod;
                        mods[1] = NULL;

                        modpb = slapi_pblock_new();
                        slapi_modify_internal_set_pb_ext(
                            modpb,
                            tombstone_sdn,
                            mods,
                            NULL, /* controls */
                            RUV_STORAGE_ENTRY_UNIQUEID,
                            repl_get_plugin_identity(PLUGIN_MULTISUPPLIER_REPLICATION),
                            /* Add OP_FLAG_TOMBSTONE_ENTRY so that this doesn't get logged in the Retro ChangeLog */
                            OP_FLAG_REPLICATED | OP_FLAG_REPL_FIXUP | OP_FLAG_TOMBSTONE_ENTRY |
                                OP_FLAG_REPL_RUV);
                        slapi_modify_internal_pb(modpb);
                        slapi_pblock_get(modpb, SLAPI_PLUGIN_INTOP_RESULT, &rc);
                        if (rc != LDAP_SUCCESS) {
                            slapi_log_err(SLAPI_LOG_REPL, repl_plugin_name,
                                          "agmt_remove_maxcsn: failed to remove agmt maxcsn (%s), error(%d)\n", maxcsns[i], rc);
                        }
                        slapi_mod_done(&smod);
                        slapi_pblock_destroy(modpb);
                        break;
                    }
                }
            }
        }
        slapi_ch_array_free(maxcsns);
    }

done:
    if (NULL != pb) {
        slapi_free_search_results_internal(pb);
        slapi_pblock_destroy(pb);
    }
}
