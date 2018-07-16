/**
 * (C) Copyright 2017 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * rebuild: Scanning the objects
 *
 * This file contains the server API methods and the RPC handlers for scanning
 * the object during rebuild.
 */
#define D_LOGFAC	DD_FAC(rebuild)

#include <daos_srv/pool.h>

#include <daos/btree_class.h>
#include <daos/pool_map.h>
#include <daos/pool.h>
#include <daos/rpc.h>
#include <daos/placement.h>
#include <daos_srv/container.h>
#include <daos_srv/daos_mgmt_srv.h>
#include <daos_srv/daos_server.h>
#include <daos_srv/rebuild.h>
#include <daos_srv/vos.h>
#include "rpc.h"
#include "rebuild_internal.h"

#define REBUILD_SEND_LIMIT	512
struct rebuild_send_arg {
	struct rebuild_root *tgt_root;
	daos_unit_oid_t	    *oids;
	uuid_t		    *uuids;
	unsigned int	    *shards;
	uuid_t		    current_uuid;
	int		    count;
};

struct rebuild_scan_arg {
	daos_handle_t		rebuild_tree_hdl;
	struct rebuild_tgt_pool_tracker *rpt;
	struct pl_target_grp	*tgp_failed;
	d_rank_list_t	*failed_ranks;
	ABT_mutex		scan_lock;
};

static int
rebuild_obj_fill_buf(daos_handle_t ih, daos_iov_t *key_iov,
		     daos_iov_t *val_iov, void *data)
{
	struct rebuild_send_arg *arg = data;
	struct rebuild_root	*root = arg->tgt_root;
	daos_unit_oid_t		*oids = arg->oids;
	uuid_t			*uuids = arg->uuids;
	unsigned int		*shards = arg->shards;
	int			count = arg->count;
	int			rc;

	D_ASSERT(count < REBUILD_SEND_LIMIT);
	oids[count] = *((daos_unit_oid_t *)key_iov->iov_buf);
	shards[count] = *((unsigned int *)val_iov->iov_buf);
	uuid_copy(uuids[count], arg->current_uuid);
	arg->count++;

	rc = dbtree_iter_delete(ih, NULL);
	if (rc != 0)
		return rc;

	D_ASSERT(root->count > 0);
	root->count--;

	D_DEBUG(DB_REBUILD, "send oid/con "DF_UOID"/"DF_UUID" cnt %d"
		" left %d\n", DP_UOID(oids[count]), DP_UUID(arg->current_uuid),
		arg->count, root->count);

	/* re-probe the dbtree after delete */
	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, NULL, NULL);
	if (rc == -DER_NONEXIST)
		return 1;

	/* Exist the loop, if there are enough objects to be sent */
	if (arg->count >= REBUILD_SEND_LIMIT)
		return 1;

	return 0;
}

static int
rebuild_cont_iter_cb(daos_handle_t ih, daos_iov_t *key_iov,
		     daos_iov_t *val_iov, void *data)
{
	struct rebuild_root *root = val_iov->iov_buf;
	struct rebuild_send_arg *arg = data;
	int rc;

	uuid_copy(arg->current_uuid, *(uuid_t *)key_iov->iov_buf);

	while (!dbtree_is_empty(root->root_hdl)) {
		rc = dbtree_iterate(root->root_hdl, false, rebuild_obj_fill_buf,
				    data);
		if (rc < 0)
			return rc;

		/* Exist the loop, if there are enough objects to be sent */
		if (arg->count >= REBUILD_SEND_LIMIT)
			return 1;
	}

	/* Delete the current container tree */
	rc = dbtree_iter_delete(ih, NULL);
	if (rc != 0)
		return rc;

	/* re-probe the dbtree after delete */
	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, NULL, NULL);
	if (rc == -DER_NONEXIST)
		return 1;

	return rc;
}

static int
rebuild_objects_send(struct rebuild_root *root, unsigned int tgt_id,
		     struct rebuild_scan_arg *scan_arg)
{
	struct rebuild_objs_in	*rebuild_in = NULL;
	struct rebuild_out	*rebuild_out = NULL;
	struct rebuild_tgt_pool_tracker	*rpt = scan_arg->rpt;
	struct rebuild_send_arg *arg = NULL;
	daos_unit_oid_t		*oids = NULL;
	uuid_t			*uuids = NULL;
	unsigned int		*shards = NULL;
	crt_rpc_t		*rpc = NULL;
	crt_endpoint_t		tgt_ep = {0};
	int			rc = 0;

	D_ALLOC_PTR(arg);
	if (arg == NULL)
		return -DER_NOMEM;

	D_ALLOC(oids, sizeof(*oids) * REBUILD_SEND_LIMIT);
	if (oids == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	D_ALLOC(uuids, sizeof(*uuids) * REBUILD_SEND_LIMIT);
	if (uuids == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	D_ALLOC(shards, sizeof(*shards) * REBUILD_SEND_LIMIT);
	if (shards == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	arg->tgt_root = root;
	arg->count = 0;
	arg->oids = oids;
	arg->uuids = uuids;
	arg->shards = shards;

	while (!dbtree_is_empty(root->root_hdl)) {
		ABT_mutex_lock(scan_arg->scan_lock);
		rc = dbtree_iterate(root->root_hdl, false, rebuild_cont_iter_cb,
				    arg);
		ABT_mutex_unlock(scan_arg->scan_lock);
		if (rc < 0)
			D_GOTO(out, rc);

		if (arg->count >= REBUILD_SEND_LIMIT)
			break;
	}

	if (arg->count == 0)
		D_GOTO(out, rc);

	if (daos_fail_check(DAOS_REBUILD_TGT_SEND_OBJS_FAIL))
		D_GOTO(out, rc = 0);

	D_DEBUG(DB_REBUILD, "send rebuild objects "DF_UUID" to tgt %d"
		" cnt %d\n", DP_UUID(rpt->rt_pool_uuid), tgt_id, arg->count);

	tgt_ep.ep_rank = tgt_id;
	tgt_ep.ep_tag = 0;
	while (1) {
		struct pool_target	*targets = NULL;
		unsigned int		failed_tgts_cnt;
		bool			target_failed = false;
		int			i;

		rc = rebuild_req_create(dss_get_module_info()->dmi_ctx, &tgt_ep,
					REBUILD_OBJECTS, &rpc);
		if (rc)
			D_GOTO(out, rc);

		rebuild_in = crt_req_get(rpc);
		rebuild_in->roi_rebuild_ver = rpt->rt_rebuild_ver;
		rebuild_in->roi_oids.ca_count = arg->count;
		rebuild_in->roi_oids.ca_arrays = oids;
		rebuild_in->roi_uuids.ca_count = arg->count;
		rebuild_in->roi_uuids.ca_arrays = uuids;
		rebuild_in->roi_shards.ca_count = arg->count;
		rebuild_in->roi_shards.ca_arrays = shards;
		uuid_copy(rebuild_in->roi_pool_uuid, rpt->rt_pool_uuid);
		crt_group_rank(NULL, &rebuild_in->roi_pad);

		rc = dss_rpc_send(rpc);

		rebuild_out = crt_reply_get(rpc);
		if (rc == 0 && rebuild_out->ro_status == 0)
			break;

		/* If it is failed, but no need retry, let's just fail */
		if ((rc != 0 && rc != -DER_TIMEDOUT &&
		     !daos_crt_network_error(rc)) ||
		    (rebuild_out->ro_status != 0 &&
		     rebuild_out->ro_status != -DER_AGAIN)) {
			if (rc == 0)
				rc = rebuild_out->ro_status;
			break;
		}

		/* Otherwise let's retry. */
		crt_req_decref(rpc);
		rpc = NULL;

		/* but we need check if the remote target is kicked out of the
		 * pool map before retry.
		 */
		rc = pool_map_find_down_tgts(rpt->rt_pool->sp_map, &targets,
					     &failed_tgts_cnt);
		if (rc != 0) {
			D_ERROR("failed create failed tgt list rc %d\n", rc);
			break;
		}

		for (i = 0; i < failed_tgts_cnt; i++) {
			if (targets[i].ta_comp.co_rank == tgt_id) {
				target_failed = true;
				break;
			}
		}

		if (targets)
			D_FREE(targets);

		if (target_failed) {
			/* Remote target has failed, no need retry, but not
			 * report failure as well and next rebuild will handle
			 * it anyway.
			 */
			D_DEBUG(DB_REBUILD, "tgt %d was failed\n", tgt_id);
			break;
		}
		ABT_thread_yield();
	}
out:
	if (rpc)
		crt_req_decref(rpc);
	if (oids != NULL)
		D_FREE(oids);
	if (uuids != NULL)
		D_FREE(uuids);
	if (shards != NULL)
		D_FREE(shards);
	if (arg != NULL)
		D_FREE_PTR(arg);

	return rc;
}

static int
rebuild_tgt_iter_cb(daos_handle_t ih, daos_iov_t *key_iov,
		    daos_iov_t *val_iov, void *data)
{
	struct rebuild_root *root;
	struct rebuild_scan_arg *arg = data;
	unsigned int tgt_id;
	int rc;

	tgt_id = *((unsigned int *)key_iov->iov_buf);
	root = val_iov->iov_buf;

	/* This is called when scanning is done, so the objects
	 * under each target should less than LIMIT. And also
	 * only 1 thread accessing the tree, so no need lock.
	 **/
	rc = rebuild_objects_send(root, tgt_id, arg);
	if (rc < 0)
		return rc;

	rc = dbtree_destroy(root->root_hdl);
	if (rc)
		return rc;

	/* Some one might insert new record to the tree let's reprobe */
	rc = dbtree_iter_probe(ih, BTR_PROBE_EQ, key_iov, NULL);
	if (rc)
		return rc;

	rc = dbtree_iter_delete(ih, NULL);
	if (rc)
		return rc;

	/* re-probe the dbtree after delete */
	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, NULL, NULL);
	if (rc == -DER_NONEXIST)
		return 1;

	return rc;
}

/* Create rebuild tree root */
static int
rebuild_tree_create(daos_handle_t toh, unsigned int tree_class,
		    void *key, daos_size_t key_size,
		    struct rebuild_root **rootp)
{
	daos_iov_t key_iov;
	daos_iov_t val_iov;
	struct umem_attr uma;
	struct rebuild_root root;
	struct btr_root	*broot;
	int rc;

	D_ALLOC_PTR(broot);
	if (broot == NULL)
		return -DER_NOMEM;

	memset(&root, 0, sizeof(root));
	root.root_hdl = DAOS_HDL_INVAL;
	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;

	rc = dbtree_create_inplace(tree_class, 0, 32, &uma,
				   broot, &root.root_hdl);
	if (rc) {
		D_ERROR("failed to create rebuild tree: %d\n", rc);
		D_FREE_PTR(broot);
		D_GOTO(out, rc);
	}

	daos_iov_set(&key_iov, key, key_size);
	daos_iov_set(&val_iov, &root, sizeof(root));
	rc = dbtree_update(toh, &key_iov, &val_iov);
	if (rc)
		D_GOTO(out, rc);

	daos_iov_set(&val_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &val_iov);
	if (rc)
		D_GOTO(out, rc);

	*rootp = val_iov.iov_buf;
	D_ASSERT(*rootp != NULL);
out:
	if (rc < 0) {
		if (!daos_handle_is_inval(root.root_hdl))
			dbtree_destroy(root.root_hdl);
	}
	return rc;
}

/* Create target rebuild tree */
static int
rebuild_tgt_tree_create(daos_handle_t toh, unsigned int tgt_id,
			   struct rebuild_root **rootp)
{
	return rebuild_tree_create(toh, DBTREE_CLASS_UV, &tgt_id,
				   sizeof(tgt_id), rootp);
}

static int
rebuild_uuid_tree_create(daos_handle_t toh, uuid_t uuid,
			    struct rebuild_root **rootp)
{
	return rebuild_tree_create(toh, DBTREE_CLASS_NV, uuid,
				   sizeof(uuid_t), rootp);
}

int
rebuild_obj_insert_cb(struct rebuild_root *cont_root, uuid_t co_uuid,
		      daos_unit_oid_t oid, unsigned int shard,
		      unsigned int *cnt, int ref)
{
	daos_iov_t		key_iov;
	daos_iov_t		val_iov;
	int			rc;

	oid.id_shard = shard;
	/* look up the object under the container tree */
	daos_iov_set(&key_iov, &oid, sizeof(oid));
	daos_iov_set(&val_iov, &shard, sizeof(shard));
	rc = dbtree_lookup(cont_root->root_hdl, &key_iov, &val_iov);
	D_DEBUG(DB_REBUILD, "lookup "DF_UOID" in cont "DF_UUID" rc %d\n",
		DP_UOID(oid), DP_UUID(co_uuid), rc);
	if (rc == -DER_NONEXIST) {
		rc = dbtree_update(cont_root->root_hdl, &key_iov, &val_iov);
		if (rc < 0) {
			D_ERROR("failed to insert "DF_UOID": rc %d\n",
				DP_UOID(oid), rc);
			D_GOTO(out, rc);
		}
		cont_root->count++;
		D_DEBUG(DB_REBUILD, "update "DF_UOID"/"DF_UUID" in"
			" cont_root %p count %d\n", DP_UOID(oid),
			DP_UUID(co_uuid), cont_root, cont_root->count);
		return 1;
	}

out:
	return rc;
}

int
rebuild_cont_obj_insert(daos_handle_t toh, uuid_t co_uuid, daos_unit_oid_t oid,
			unsigned int shard, unsigned int *cnt, int ref,
			rebuild_obj_insert_cb_t obj_cb)
{
	struct rebuild_root	*cont_root;
	daos_iov_t		key_iov;
	daos_iov_t		val_iov;
	int			rc;

	daos_iov_set(&key_iov, co_uuid, sizeof(uuid_t));
	daos_iov_set(&val_iov, NULL, 0);
	rc = dbtree_lookup(toh, &key_iov, &val_iov);
	if (rc < 0) {
		if (rc != -DER_NONEXIST) {
			D_ERROR("lookup cont "DF_UUID" failed, rc %d\n",
				DP_UUID(co_uuid), rc);
			D_GOTO(out, rc);
		}

		rc = rebuild_uuid_tree_create(toh, co_uuid,
					      &cont_root);
		if (rc) {
			D_ERROR("tree_create cont "DF_UUID" failed, rc %d\n",
				DP_UUID(co_uuid), rc);
			D_GOTO(out, rc);
		}
	} else {
		cont_root = val_iov.iov_buf;
	}

	rc = obj_cb(cont_root, co_uuid, oid, shard, cnt, ref);
out:
	return rc;
}

/**
 * The rebuild objects will be gathered into a global objects arrary by
 * target id.
 **/
static int
rebuild_object_insert(struct rebuild_scan_arg *arg, unsigned int tgt_id,
		      unsigned int shard, uuid_t pool_uuid, uuid_t co_uuid,
		      daos_unit_oid_t oid)
{
	daos_iov_t		key_iov;
	daos_iov_t		val_iov;
	struct rebuild_root	*tgt_root;
	daos_handle_t		toh = arg->rebuild_tree_hdl;
	int			rc;

	/* look up the target tree */
	daos_iov_set(&key_iov, &tgt_id, sizeof(tgt_id));
	daos_iov_set(&val_iov, NULL, 0);
	ABT_mutex_lock(arg->scan_lock);
	rc = dbtree_lookup(toh, &key_iov, &val_iov);
	if (rc < 0) {
		/* Try to find the target rebuild tree */
		rc = rebuild_tgt_tree_create(toh, tgt_id, &tgt_root);
		if (rc) {
			ABT_mutex_unlock(arg->scan_lock);
			D_GOTO(out, rc);
		}
	} else {
		tgt_root = val_iov.iov_buf;
	}

	rc = rebuild_cont_obj_insert(tgt_root->root_hdl, co_uuid, oid, shard,
				     NULL, 0, rebuild_obj_insert_cb);
	ABT_mutex_unlock(arg->scan_lock);
	if (rc <= 0)
		D_GOTO(out, rc);

	if (rc == 1) {
		/* Check if we need send the object list */
		if (++tgt_root->count >= REBUILD_SEND_LIMIT) {
			rc = rebuild_objects_send(tgt_root, tgt_id, arg);
			if (rc < 0)
				D_GOTO(out, rc);
		}
		rc = 0;
	}

	D_DEBUG(DB_REBUILD, "insert "DF_UOID"/"DF_UUID" tgt %u cnt %d rc %d\n",
		DP_UOID(oid), DP_UUID(co_uuid), tgt_id, tgt_root->count, rc);
out:
	return rc;
}

static int
placement_check(uuid_t co_uuid, daos_unit_oid_t oid, void *data)
{
	struct rebuild_scan_arg	*arg = data;
	struct rebuild_tgt_pool_tracker *rpt = arg->rpt;
	struct pl_obj_layout	*layout = NULL;
	struct pl_map		*map = NULL;
	struct daos_obj_md	md;
	uint32_t		tgt_rebuild;
	unsigned int		shard_rebuild;
	d_rank_t		myrank;
	int			rc;

	if (rpt->rt_abort)
		return 1;

	dc_obj_fetch_md(oid.id_pub, &md);
	while (1) {
		struct pool_map *poolmap;

		map = pl_map_find(rpt->rt_pool_uuid, oid.id_pub);
		if (map == NULL) {
			D_ERROR(DF_UOID"Cannot find valid placement map"
				DF_UUID"\n", DP_UOID(oid),
				DP_UUID(rpt->rt_pool_uuid));
			D_GOTO(out, rc = -DER_INVAL);
		}

		poolmap = rebuild_pool_map_get(rpt->rt_pool);
		if (pl_map_version(map) >= pool_map_get_version(poolmap)) {
			/* very likely we just break out from here, unless
			 * there is a cascading failure.
			 */
			rebuild_pool_map_put(poolmap);
			break;
		}
		pl_map_decref(map);

		pl_map_update(rpt->rt_pool_uuid, poolmap, false);
		rebuild_pool_map_put(poolmap);
	}

	crt_group_rank(rpt->rt_pool->sp_group, &myrank);

	rc = pl_obj_find_rebuild(map, &md, NULL, arg->tgp_failed->tg_ver,
				 &tgt_rebuild, &shard_rebuild);
	if (rc <= 0) /* No need rebuild */
		D_GOTO(out, rc);

	D_DEBUG(DB_REBUILD, "rebuild obj "DF_UOID"/"DF_UUID"/"DF_UUID
		" on %d for shard %d\n", DP_UOID(oid), DP_UUID(co_uuid),
		DP_UUID(rpt->rt_pool_uuid), tgt_rebuild, shard_rebuild);

	/* During rebuild test, it will manually exclude some target to
	 * trigger the rebuild, then later add it back, so some objects
	 * might exist on some illegal target, so they might use its "own"
	 * target as the spare target, let's skip these object now.
	 * When we have better support from CART exclude/addback, myrank
	 * should always not equal to tgt_rebuild. XXX
	 */
	if (myrank != tgt_rebuild) {
		rc = rebuild_object_insert(arg, tgt_rebuild, shard_rebuild,
					   rpt->rt_pool_uuid, co_uuid, oid);
		if (rc)
			D_GOTO(out, rc);
	} else {
		D_DEBUG(DB_REBUILD, "skip "DF_UOID", not send it to its own.\n",
			DP_UOID(oid));
		rc = 0;
	}
out:
	if (layout != NULL)
		pl_obj_layout_free(layout);

	if (map != NULL)
		pl_map_decref(map);

	return rc;
}

struct rebuild_iter_arg {
	cont_iter_cb_t	callback;
	void		*arg;
};

int
rebuild_scanner(void *data)
{
	struct rebuild_iter_arg *arg = data;
	struct rebuild_scan_arg	*scan_arg = arg->arg;
	struct rebuild_tgt_pool_tracker *rpt = scan_arg->rpt;

	D_ASSERT(rpt != NULL);

	while (daos_fail_check(DAOS_REBUILD_TGT_SCAN_HANG))
		ABT_thread_yield();

	return ds_pool_obj_iter(rpt->rt_pool_uuid, arg->callback,
				arg->arg);
}

static int
rebuild_scan_done(void *data)
{
	struct rebuild_tgt_pool_tracker *rpt = data;
	struct rebuild_pool_tls *tls;

	tls = rebuild_pool_tls_lookup(rpt->rt_pool_uuid,
				      rpt->rt_rebuild_ver);
	D_ASSERT(tls != NULL);

	tls->rebuild_pool_scanning = 0;
	return 0;
}

/**
 * Wait for pool map and setup global status, then spawn scanners for all
 * service xsteams
 */
static void
rebuild_scan_leader(void *data)
{
	struct rebuild_scan_arg	  *arg = data;
	struct pl_target_grp	  *tgp;
	struct pool_map		  *map;
	struct rebuild_tgt_pool_tracker *rpt;
	struct rebuild_pool_tls	  *tls;
	struct rebuild_iter_arg    iter_arg;
	int			   i;
	int			   rc;

	D_ASSERT(arg != NULL);
	D_ASSERT(arg->failed_ranks);
	D_ASSERT(!daos_handle_is_inval(arg->rebuild_tree_hdl));

	rpt = arg->rpt;
	/* refresh placement for the server stack */
	ABT_mutex_lock(rpt->rt_lock);
	map = rebuild_pool_map_get(rpt->rt_pool);
	D_ASSERT(map != NULL);
	rc = pl_map_update(rpt->rt_pool_uuid, map, true);
	if (rc != 0) {
		ABT_mutex_unlock(rpt->rt_lock);
		D_GOTO(out_map, rc = -DER_NOMEM);
	}
	ABT_mutex_unlock(rpt->rt_lock);

	/* initialize parameters for scanners */
	D_ALLOC_PTR(tgp);
	if (tgp == NULL)
		D_GOTO(put_plmap, rc = -DER_NOMEM);

	arg->tgp_failed = tgp;
	tgp->tg_ver = rpt->rt_rebuild_ver;
	tgp->tg_target_nr = arg->failed_ranks->rl_nr;

	D_ALLOC(tgp->tg_targets, tgp->tg_target_nr * sizeof(*tgp->tg_targets));
	if (tgp->tg_targets == NULL)
		D_GOTO(out_group, rc = -DER_NOMEM);

	for (i = 0; i < arg->failed_ranks->rl_nr; i++) {
		struct pool_target      *target;
		d_rank_t		 rank;

		rank = arg->failed_ranks->rl_ranks[i];
		target = pool_map_find_target_by_rank(map, rank);
		if (target == NULL) {
			D_ERROR("Nonexistent target rank=%d\n", rank);
			D_GOTO(out_group, rc = -DER_NONEXIST);
		}

		tgp->tg_targets[i].pt_pos = target - pool_map_targets(map);
	}

	iter_arg.arg = arg;
	iter_arg.callback = placement_check;

	rc = dss_thread_collective(rebuild_scanner, &iter_arg);
	if (rc)
		D_GOTO(out_group, rc);

	D_DEBUG(DB_REBUILD, "rebuild scan collective "DF_UUID" done.\n",
		DP_UUID(rpt->rt_pool_uuid));

	while (!dbtree_is_empty(arg->rebuild_tree_hdl)) {
		/* walk through the rebuild tree and send the rebuild objects */
		rc = dbtree_iterate(arg->rebuild_tree_hdl, false,
				    rebuild_tgt_iter_cb, arg);
		if (rc)
			D_GOTO(out_group, rc);
	}

	ABT_mutex_lock(rpt->rt_lock);
	rc = dss_task_collective(rebuild_scan_done, rpt);
	ABT_mutex_unlock(rpt->rt_lock);
	if (rc) {
		D_ERROR(DF_UUID" send rebuild object list failed:%d\n",
			DP_UUID(rpt->rt_pool_uuid), rc);
		D_GOTO(out_group, rc);
	}

	D_DEBUG(DB_REBUILD, DF_UUID" sent objects to initiator %d\n",
		DP_UUID(rpt->rt_pool_uuid), rc);
out_group:
	if (tgp->tg_targets != NULL)
		D_FREE(tgp->tg_targets);
	D_FREE_PTR(tgp);
put_plmap:
	pl_map_disconnect(rpt->rt_pool_uuid);
out_map:
	rebuild_pool_map_put(map);
	daos_rank_list_free(arg->failed_ranks);
	dbtree_destroy(arg->rebuild_tree_hdl);
	tls = rebuild_pool_tls_lookup(rpt->rt_pool_uuid, rpt->rt_rebuild_ver);
	D_ASSERT(tls != NULL);
	if (tls->rebuild_pool_status == 0 && rc != 0)
		tls->rebuild_pool_status = rc;
	D_DEBUG(DB_REBUILD, DF_UUID"scan leader done %d\n",
		DP_UUID(rpt->rt_pool_uuid), rc);
	ABT_mutex_free(&arg->scan_lock);
	D_FREE_PTR(arg);
	rpt_put(rpt);
}

/* Scan the local target and generate rebuild object list */
void
rebuild_tgt_scan_handler(crt_rpc_t *rpc)
{
	struct rebuild_scan_in		*rsi;
	struct rebuild_scan_out		*ro;
	struct rebuild_scan_arg		*scan_arg;
	struct umem_attr		 uma;
	struct rebuild_tgt_pool_tracker	*rpt = NULL;
	int				 rc;

	rsi = crt_req_get(rpc);
	D_ASSERT(rsi != NULL);

	D_DEBUG(DB_REBUILD, "%d scan rebuild for "DF_UUID" ver %d/%d\n",
		dss_get_module_info()->dmi_tid, DP_UUID(rsi->rsi_pool_uuid),
		rsi->rsi_pool_map_ver, rsi->rsi_rebuild_ver);

	/* check if the rebuild is already started */
	rpt = rpt_lookup(rsi->rsi_pool_uuid, rsi->rsi_rebuild_ver);
	if (rpt != NULL) {
		/* Rebuild should never skip the version */
		D_ASSERTF(rsi->rsi_rebuild_ver == rpt->rt_rebuild_ver,
			  "rsi_rebuild_ver %d != rt_rebuild_ver %d\n",
			  rsi->rsi_rebuild_ver, rpt->rt_rebuild_ver);

		D_DEBUG(DB_REBUILD, DF_UUID" already started.\n",
			DP_UUID(rsi->rsi_pool_uuid));

		/* Ignore the rebuild trigger request if it comes from
		 * an old or same leader.
		 */
		if (rsi->rsi_leader_term <= rpt->rt_leader_term)
			D_GOTO(out, rc = 0);

		if (rpt->rt_pool->sp_iv_ns != NULL &&
		    rpt->rt_pool->sp_iv_ns->iv_master_rank !=
					rsi->rsi_master_rank) {
			D_DEBUG(DB_REBUILD, DF_UUID" master rank"
				" %d -> %d term "DF_U64" -> "DF_U64"\n",
				DP_UUID(rpt->rt_pool_uuid),
				rpt->rt_pool->sp_iv_ns->iv_master_rank,
				rsi->rsi_master_rank,
				rpt->rt_leader_term,
				rsi->rsi_leader_term);
			/* Update master rank */
			rc = ds_pool_iv_ns_update(rpt->rt_pool,
						  rsi->rsi_master_rank,
						  &rsi->rsi_ns_iov,
						  rsi->rsi_ns_id);
			if (rc)
				D_GOTO(out, rc);

			/* If this is the old leader, then also stop the rebuild
			 * tracking ULT.
			 */
			ds_rebuild_leader_stop(rsi->rsi_pool_uuid,
					       rsi->rsi_rebuild_ver);
		}

		rpt->rt_leader_term = rsi->rsi_leader_term;

		D_GOTO(out, rc = 0);
	}

	if (daos_fail_check(DAOS_REBUILD_TGT_START_FAIL))
		D_GOTO(out, rc = -DER_INVAL);

	rc = rebuild_tgt_prepare(rpc, &rpt);
	if (rc)
		D_GOTO(out, rc);

	rpt_get(rpt);
	rc = dss_ult_create(rebuild_tgt_status_check, rpt, -1, 0, NULL);
	if (rc) {
		rpt_put(rpt);
		D_GOTO(out, rc);
	}

	/* step-1: parameters for scanner */
	D_ALLOC_PTR(scan_arg);
	if (scan_arg == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	rc = ABT_mutex_create(&scan_arg->scan_lock);
	if (rc != ABT_SUCCESS) {
		rc = dss_abterr2der(rc);
		D_GOTO(out_arg, rc);
	}

	/* step-2: Create the btree root for global object scan list */
	memset(&uma, 0, sizeof(uma));
	uma.uma_id = UMEM_CLASS_VMEM;
	rc = dbtree_create(DBTREE_CLASS_NV, 0, 4, &uma, NULL,
			   &scan_arg->rebuild_tree_hdl);
	if (rc != 0) {
		D_ERROR("failed to create rebuild tree: %d\n", rc);
		D_GOTO(out_lock, rc);
	}

	rc = daos_rank_list_dup(&scan_arg->failed_ranks, rsi->rsi_tgts_failed);
	if (rc != 0)
		D_GOTO(out_tree, rc);

	rpt_get(rpt);
	scan_arg->rpt = rpt;
	/* step-3: start scann leader */
	rc = dss_ult_create(rebuild_scan_leader, scan_arg, -1, 0, NULL);
	if (rc != 0) {
		rpt_put(rpt);
		D_GOTO(out_f_rankfs, rc);
	}

	D_GOTO(out, rc);
out_f_rankfs:
	daos_rank_list_free(scan_arg->failed_ranks);
out_tree:
	dbtree_destroy(scan_arg->rebuild_tree_hdl);
out_lock:
	ABT_mutex_free(&scan_arg->scan_lock);
out_arg:
	D_FREE_PTR(scan_arg);
out:
	if (rpt)
		rpt_put(rpt);
	ro = crt_reply_get(rpc);
	ro->rso_status = rc;
	if (rc) {
		d_rank_list_t *fail_list;

		/* If it failed, tell the master the target can not
		 * start the rebuild, so master will put the target
		 * into DOWN state.
		 */
		fail_list = d_rank_list_alloc(1);
		if (rpt && rpt->rt_pool)
			crt_group_rank(rpt->rt_pool->sp_group,
				       &fail_list->rl_ranks[0]);
		else
			crt_group_rank(NULL, &fail_list->rl_ranks[0]);

		ro->rso_ranks_list = fail_list;
		if (rpt)
			rpt->rt_abort = 1;
	}

	dss_rpc_reply(rpc, DAOS_REBUILD_DROP_SCAN);
}

int
rebuild_tgt_scan_aggregator(crt_rpc_t *source, crt_rpc_t *result,
			    void *priv)
{
	struct rebuild_scan_out	*src = crt_reply_get(source);
	struct rebuild_scan_out *dst = crt_reply_get(result);
	int i;

	if (src->rso_ranks_list == NULL ||
	    src->rso_ranks_list->rl_nr == 0)
		return 0;

	if (dst->rso_ranks_list == NULL) {
		D_ALLOC_PTR(dst->rso_ranks_list);
		if (dst->rso_ranks_list == NULL)
			return -DER_NOMEM;
	}

	for (i = 0; i < src->rso_ranks_list->rl_nr; i++) {
		int rc;

		rc = d_rank_list_append(dst->rso_ranks_list,
					src->rso_ranks_list->rl_ranks[i]);
		if (rc)
			return rc;
	}

	return 0;
}

