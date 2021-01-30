/* -*- C -*- */
/*
 * Copyright (c) 2011-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTM0
#include "lib/trace.h"
#include "lib/string.h"
#include "lib/errno.h"
#include "lib/memory.h"
#include "reqh/reqh_service.h"
#include "reqh/reqh.h"               /* m0_reqh */
#include "rpc/rpc_machine.h"         /* m0_rpc_machine */
#include "dtm0/fop.h"
#include "lib/tlist.h"

#include "conf/confc.h"   /* m0_confc */
#include "conf/diter.h"   /* m0_conf_diter */
#include "conf/obj_ops.h" /* M0_CONF_DIRNEXT */
#include "conf/helpers.h" /* m0_confc_root_open, m0_conf_process2service_get */
#include "reqh/reqh.h"    /* m0_reqh2confc */

static int dtm0_service__ha_subscribe(struct m0_reqh_service *service);
static int dtm0_service__ha_unsubscribe(struct m0_reqh_service *service);
static int dtm0_service_start(struct m0_reqh_service *service);
static void dtm0_service_stop(struct m0_reqh_service *service);
static int dtm0_service_allocate(struct m0_reqh_service **service,
				 const struct m0_reqh_service_type *stype);
static void dtm0_service_fini(struct m0_reqh_service *service);


static const struct m0_reqh_service_type_ops dtm0_service_type_ops = {
	.rsto_service_allocate = dtm0_service_allocate
};

static const struct m0_reqh_service_ops dtm0_service_ops = {
	.rso_start = dtm0_service_start,
	.rso_stop  = dtm0_service_stop,
	.rso_fini  = dtm0_service_fini,
};

struct m0_reqh_service_type dtm0_service_type = {
	.rst_name  = "M0_CST_DTM0",
	.rst_ops   = &dtm0_service_type_ops,
	.rst_level = M0_RS_LEVEL_LATE,
};


/**
 * System process which dtm0 subscribes for state updates
 */
struct dtm0_process {
	struct m0_tlink     dop_link;
	uint64_t            dop_magic;

	/**
	 * Listens for an event on process conf object's HA channel.
	 * Updates dtm0_process status in the clink callback on HA notification.
	 */
	struct m0_clink         dop_ha_link;
	/**
	 * Link connected to remote process
	 */
	struct m0_rpc_link      dop_rlink;
	/**
	 * Remote process fid
	 */
	struct m0_fid           dop_rproc_fid;
	/**
	 * Remote service fid
	 */
	struct m0_fid           dop_rserv_fid;
	/**
	 * Remote process endpoint
	 */
	const char             *dop_rep;
	/**
	 * [Sigh] Current dtm0 service dtm0 process to.
	 */
	struct m0_reqh_service *dop_dtm0_service;
	/**
	 * connect ast
	 */
	struct m0_sm_ast        dop_service_connect_ast;
	struct m0_clink         dop_service_connect_clink;
};

M0_TL_DESCR_DEFINE(dopr, "dtm0_process", static, struct dtm0_process, dop_link,
		   dop_magic, 0x8888888888888888, 0x7777777777777777);
M0_TL_DEFINE(dopr, static, struct dtm0_process);

/**
 * DTM0 service structure
 */
struct m0_dtm0_service {
       struct m0_reqh_service dos_generic;
       struct m0_tl           dos_processes;
};

static void dtm0_service__init(struct m0_dtm0_service *s)
{
       dopr_tlist_init(&s->dos_processes);
}

static void dtm0_service__fini(struct m0_dtm0_service *s)
{
       dopr_tlist_fini(&s->dos_processes);
}

static struct dtm0_process *dtm0_service_process__lookup(struct m0_reqh_service *reqh_dtm0_svc,
							 struct m0_fid *remote_dtm0)
{
	struct m0_dtm0_service *dtm0 =
		container_of(reqh_dtm0_svc, struct m0_dtm0_service, dos_generic);
	struct dtm0_process *process = NULL;

	m0_tl_for(dopr, &dtm0->dos_processes, process) {
		if (m0_fid_eq(&process->dop_rserv_fid, remote_dtm0))
			break;
	} m0_tl_endfor;

	return process;
}

M0_INTERNAL int m0_dtm0_service_process_connect(struct m0_reqh_service *s,
						struct m0_fid *remote_srv,
						const char    *remote_ep,
						bool async)
{
	struct dtm0_process *process;
	struct m0_rpc_machine *mach =
		m0_reqh_rpc_mach_tlist_head(&s->rs_reqh->rh_rpc_machines);
	int rc;

	process = dtm0_service_process__lookup(s, remote_srv);
	if (process == NULL)
		return M0_RC(-ENOENT);

	rc = m0_rpc_link_init(&process->dop_rlink, mach, remote_srv,
			      remote_ep, 10);
	M0_ASSERT(rc == 0);

	M0_LOG(M0_ALWAYS,
	       " async=%d"
	       " dtm0="FID_F
	       " remote_srv="FID_F,
	       async,
	       FID_P(&s->rs_service_fid),
	       FID_P(remote_srv));
	M0_LOG(M0_ALWAYS, "rep=%s", remote_ep);

	if (!async) {
		rc = m0_rpc_link_connect_sync(&process->dop_rlink,
					      M0_TIME_NEVER);
		M0_ASSERT(rc == 0);
	} else {
		m0_rpc_link_connect_async(&process->dop_rlink,
					  M0_TIME_NEVER,
					  &process->dop_service_connect_clink);
	}

	return M0_RC(0);
}

M0_INTERNAL int m0_dtm0_service_process_disconnect(struct m0_reqh_service *s,
						   struct m0_fid *remote_srv)
{
	int rc;
	struct dtm0_process *process = NULL;

	M0_LOG(M0_ALWAYS,
	       " dtm0=%p"
	       " remote_srv="FID_F,
	       s,
	       FID_P(remote_srv));

	process = dtm0_service_process__lookup(s, remote_srv);
	if (process == NULL)
		return M0_RC(-ENOENT);

	rc = m0_rpc_link_disconnect_sync(&process->dop_rlink,
					 M0_TIME_NEVER);
	M0_ASSERT(rc == 0);
	m0_rpc_link_fini(&process->dop_rlink);

	return M0_RC(0);
}

M0_INTERNAL struct m0_rpc_session *
m0_dtm0_service_process_session_get(struct m0_reqh_service *s,
				    struct m0_fid *remote_srv)
{
	struct dtm0_process *process =
		dtm0_service_process__lookup(s, remote_srv);
	return process == NULL ? NULL : &process->dop_rlink.rlk_sess;
}

static int dtm0_service__alloc(struct m0_reqh_service **service,
			       const struct m0_reqh_service_type *stype,
			       const struct m0_reqh_service_ops *ops)
{
	struct m0_dtm0_service *s;

	M0_PRE(stype != NULL && service != NULL && ops != NULL);

	M0_ALLOC_PTR(s);
	if (s == NULL)
		return -ENOMEM;

	s->dos_generic.rs_type = stype;
	s->dos_generic.rs_ops  = ops;
	*service = &s->dos_generic;
	dtm0_service__init(s);

	return 0;
}

static int dtm0_service_allocate(struct m0_reqh_service **service,
				 const struct m0_reqh_service_type *stype)
{
	return dtm0_service__alloc(service, stype, &dtm0_service_ops);
}

static int dtm0_service_start(struct m0_reqh_service *service)
{
        M0_PRE(service != NULL);
        return m0_dtm0_fop_init() ?: dtm0_service__ha_subscribe(service);
}

static void dtm0_service_stop(struct m0_reqh_service *service)
{

        M0_PRE(service != NULL);

	dtm0_service__ha_unsubscribe(service);
	m0_dtm0_fop_fini();
}

static void dtm0_service_fini(struct m0_reqh_service *service)
{
	M0_PRE(service != NULL);
	dtm0_service__fini(container_of(service,
					struct m0_dtm0_service,
					dos_generic));
        m0_free(service);
}

int m0_dtm0_stype_init(void)
{
	return m0_reqh_service_type_register(&dtm0_service_type);
}

void m0_dtm0_stype_fini(void)
{
	m0_reqh_service_type_unregister(&dtm0_service_type);
}

/* --------------------------------- EVENTS --------------------------------- */

struct m0_semaphore g_test_wait;
static bool service_connect_clink(struct m0_clink *link)
{
	M0_ENTRY();
	M0_LEAVE();
	M0_LOG(M0_ALWAYS, "123");
	m0_semaphore_up(&g_test_wait);
	return true;
}

static void service_connect_ast(struct m0_sm_group *grp,
				struct m0_sm_ast   *ast)
{
	struct dtm0_process *process = ast->sa_datum;
	int rc;

	M0_ENTRY();
	m0_clink_init(&process->dop_service_connect_clink, service_connect_clink);
	process->dop_service_connect_clink.cl_is_oneshot = true;
	rc = m0_dtm0_service_process_connect(process->dop_dtm0_service,
					     &process->dop_rserv_fid,
					     process->dop_rep,
					     true);
	M0_ASSERT(rc == 0);
	M0_LEAVE();
}

static bool process_clink_cb(struct m0_clink *clink)
{
       struct dtm0_process *process    = M0_AMB(process, clink, dop_ha_link);
       struct m0_conf_obj *process_obj = container_of(clink->cl_chan,
                                                      struct m0_conf_obj,
                                                      co_ha_chan);
       struct m0_reqh_service *dtm0 = process->dop_dtm0_service;
       struct m0_reqh         *reqh = dtm0->rs_reqh;
       struct m0_fid          *current_proc_fid = &reqh->rh_fid;
       struct m0_fid          *evented_proc_fid = &process_obj->co_id;
       enum m0_ha_obj_state    evented_proc_state = process_obj->co_ha_state;

       M0_ENTRY();
       M0_PRE(m0_conf_obj_type(process_obj) == &M0_CONF_PROCESS_TYPE);
       M0_PRE(m0_fid_eq(evented_proc_fid, &process->dop_rproc_fid));

       if (m0_fid_eq(evented_proc_fid, current_proc_fid) || /* skip current process        */
	   m0_fid_eq(&process->dop_rserv_fid, &M0_FID0))    /* skip processes without DTM0 */
	       return true;

       /* (M0_IN(obj->co_ha_state, (M0_NC_ONLINE, M0_NC_FAILED, M0_NC_TRANSIENT))) */
       M0_LOG(M0_ALWAYS,
	      " evented_proc_state=%d"
	      " evented_proc_fid="FID_F
	      " dop_rserv_fid="FID_F
	      " dop_rproc_fid="FID_F
	      " curr_proc_fid="FID_F,
              evented_proc_state,
	      FID_P(evented_proc_fid),
	      FID_P(&process->dop_rserv_fid),
	      FID_P(&process->dop_rproc_fid),
              FID_P(current_proc_fid));
       M0_LOG(M0_ALWAYS, "dtm0=%p dop_rep=%s", dtm0, process->dop_rep);

       process->dop_service_connect_ast = (struct m0_sm_ast){
       	       .sa_cb    = &service_connect_ast,
       	       .sa_datum = process,
       };
       m0_sm_ast_post(m0_locality_here()->lo_grp,
       		      &process->dop_service_connect_ast);

       M0_LEAVE();

       return false;
}

static void dtm0_process__ha_state_subscribe(struct dtm0_process *process,
					     struct m0_conf_obj  *obj)
{
	M0_ENTRY();
	m0_clink_init(&process->dop_ha_link, process_clink_cb);
	m0_clink_add_lock(&obj->co_ha_chan, &process->dop_ha_link);
	M0_LEAVE();
}

static void dtm0_process__ha_state_unsubscribe(struct dtm0_process *process)
{
	M0_ENTRY();
	m0_clink_del_lock(&process->dop_ha_link);
	m0_clink_fini(&process->dop_ha_link);
	M0_LEAVE();
}

static bool conf_obj_is_process(const struct m0_conf_obj *obj)
{
       return m0_conf_obj_type(obj) == &M0_CONF_PROCESS_TYPE;
}

static int dtm0_service__ha_subscribe(struct m0_reqh_service *service)
{
       struct m0_confc        *confc = m0_reqh2confc(service->rs_reqh);
       struct m0_conf_root    *root;
       struct m0_conf_diter    it;
       struct m0_conf_obj     *obj;
       struct m0_conf_process *process;
       struct dtm0_process    *dtm0_process;
       struct m0_dtm0_service    *s;
       int rc;

       M0_ENTRY();

       M0_PRE(service != NULL);
       s = container_of(service, struct m0_dtm0_service, dos_generic);

       /** UT workaround */
       if (!m0_confc_is_inited(confc)) {
	       M0_LOG(M0_WARN, "confc is not initiated!");
	       return M0_RC(0);
       }

       rc = m0_confc_root_open(confc, &root);
       if (rc != 0)
               return M0_ERR(rc);

       rc = m0_conf_diter_init(&it, confc,
                               &root->rt_obj,
                               M0_CONF_ROOT_NODES_FID,
                               M0_CONF_NODE_PROCESSES_FID);
       if (rc != 0) {
               m0_confc_close(&root->rt_obj);
               return M0_ERR(rc);
       }

       while ((rc = m0_conf_diter_next_sync(&it, conf_obj_is_process)) > 0) {
               obj = m0_conf_diter_result(&it);
               process = M0_CONF_CAST(obj, m0_conf_process);
               M0_ALLOC_PTR(dtm0_process);
               M0_ASSERT(dtm0_process != NULL); /* XXX */
               dtm0_process__ha_state_subscribe(dtm0_process, obj);
               dopr_tlink_init(dtm0_process);
               dopr_tlist_add(&s->dos_processes, dtm0_process);
	       dtm0_process->dop_rproc_fid = process->pc_obj.co_id;
	       dtm0_process->dop_rep       = m0_strdup(process->pc_endpoint);
	       m0_conf_process2service_get(confc, &dtm0_process->dop_rproc_fid,
					   M0_CST_DTM0, &dtm0_process->dop_rserv_fid);
	       dtm0_process->dop_dtm0_service = service;
       }

       m0_conf_diter_fini(&it);
       m0_confc_close(&root->rt_obj);

       return M0_RC(0);
}

static int dtm0_service__ha_unsubscribe(struct m0_reqh_service *reqh_service)
{
       struct dtm0_process *process;
       struct m0_dtm0_service *service;
       /* int rc; */

       M0_PRE(reqh_service != NULL);
       service = container_of(reqh_service, struct m0_dtm0_service, dos_generic);

       M0_ENTRY();

       while ((process = dopr_tlist_pop(&service->dos_processes)) != NULL) {
	       /* if (process->dop_rserv_fid.f_key == 0x1a) { */
	       /* 	       rc = m0_dtm0_service_process_disconnect(reqh_service, */
	       /* 						       &process->dop_rserv_fid); */
	       /* 	       M0_ASSERT(rc == 0 || rc == -ENOENT); */
	       /* } */
	       dtm0_process__ha_state_unsubscribe(process);
               dopr_tlink_fini(process);
               m0_free(process);
       }

       M0_LEAVE();

       return M0_RC(0);
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
