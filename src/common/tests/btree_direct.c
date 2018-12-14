/**
 * (C) Copyright 2018 Intel Corporation.
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
#define D_LOGFAC	DD_FAC(tests)

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include <daos/btree.h>
#include <daos/tests_lib.h>
#include <gurt/common.h>

/**
 * An example for string key
 */

TMMID_DECLARE(struct sk_rec, 0);

#define SK_MAX_KEY_LEN 128

/** string key record */
struct sk_rec {
	uint64_t	sr_key_len;
	uint32_t	sr_val_size;
	uint32_t	sr_val_msize;
	umem_id_t	sr_val_mmid;
	char		sr_key[0];
};

#define SK_TREE_CLASS	100
#define POOL_NAME "/mnt/daos/btree-direct-test"
#define POOL_SIZE ((1024 * 1024  * 1024ULL))

struct umem_attr sk_uma;

#define min(x, y) ((x) < (y) ? (x) : (y))

static void sk_key_encode(struct btr_instance *tins,
			  daos_iov_t *key, daos_anchor_t *anchor)
{
	size_t copy_size = key->iov_len;

	if (key->iov_len > DAOS_ANCHOR_BUF_MAX)
		copy_size = DAOS_ANCHOR_BUF_MAX;

	memcpy(&anchor->da_buf[0], key->iov_buf, copy_size);
}

static void sk_key_decode(struct btr_instance *tins,
			  daos_iov_t *key, daos_anchor_t *anchor)
{
	key->iov_buf = &anchor->da_buf[0];
	key->iov_buf_len = strlen((char *)anchor->da_buf) + 1;
	key->iov_len = key->iov_buf_len;
}

static int
sk_key_cmp(struct btr_instance *tins, struct btr_record *rec,
	   daos_iov_t *key_iov)
{
	struct sk_rec	*srec;
	char		*s1;
	char		*s2;
	uint64_t	 len;
	int		 rc;

	srec = (struct sk_rec *)umem_id2ptr(&tins->ti_umm, rec->rec_mmid);

	/* NB: Since strings are null terminated, this should suffice to
	 * make shorter string less than larger one
	 */
	len = min(srec->sr_key_len, key_iov->iov_len);

	s1 = &srec->sr_key[0];
	s2 = key_iov->iov_buf;
	rc = strncasecmp(s1, s2, len);

	if (rc != 0)
		return dbtree_key_cmp_rc(rc);

	return dbtree_key_cmp_rc(strncmp(s1, s2, len));
}

static int
sk_rec_alloc(struct btr_instance *tins, daos_iov_t *key_iov,
	      daos_iov_t *val_iov, struct btr_record *rec)
{
	struct sk_rec		*srec;
	char			*vbuf;
	umem_id_t		 srec_mmid;

	srec_mmid = umem_zalloc(&tins->ti_umm,
				sizeof(*srec) + key_iov->iov_len);
	D_ASSERT(!UMMID_IS_NULL(srec_mmid)); /* lazy bone... */

	srec = (struct sk_rec *)umem_id2ptr(&tins->ti_umm, srec_mmid);

	memcpy(&srec->sr_key[0], key_iov->iov_buf, key_iov->iov_len);
	srec->sr_key_len = key_iov->iov_len;
	srec->sr_val_size = srec->sr_val_msize = val_iov->iov_len;

	srec->sr_val_mmid = umem_alloc(&tins->ti_umm, val_iov->iov_len);
	D_ASSERT(!UMMID_IS_NULL(srec->sr_val_mmid));

	vbuf = umem_id2ptr(&tins->ti_umm, srec->sr_val_mmid);
	memcpy(vbuf, (char *)val_iov->iov_buf, val_iov->iov_len);

	rec->rec_mmid = srec_mmid;
	return 0;
}

static int
sk_rec_free(struct btr_instance *tins, struct btr_record *rec, void *args)
{
	struct umem_instance *umm = &tins->ti_umm;
	TMMID(struct sk_rec) srec_mmid;
	struct sk_rec *srec;

	srec_mmid = umem_id_u2t(rec->rec_mmid, struct sk_rec);
	srec = umem_id2ptr_typed(umm, srec_mmid);

	if (args != NULL) {
		umem_id_t *rec_ret = (umem_id_t *) args;
		 /** Provide the buffer to user */
		*rec_ret	= rec->rec_mmid;
		rec->rec_mmid	= UMMID_NULL;
		return 0;
	}
	umem_free(umm, srec->sr_val_mmid);
	umem_free_typed(umm, srec_mmid);

	return 0;
}

static int
sk_rec_fetch(struct btr_instance *tins, struct btr_record *rec,
	     daos_iov_t *key_iov, daos_iov_t *val_iov)
{
	struct sk_rec	*srec;
	char		*val;
	size_t		 val_size;
	size_t		 key_size;

	if (key_iov == NULL && val_iov == NULL)
		return -EINVAL;

	srec = (struct sk_rec *)umem_id2ptr(&tins->ti_umm, rec->rec_mmid);
	val_size = srec->sr_val_size;
	key_size = srec->sr_key_len;

	val = umem_id2ptr(&tins->ti_umm, srec->sr_val_mmid);
	if (key_iov != NULL) {
		key_iov->iov_len = key_size;
		key_iov->iov_buf_len = key_size;
		if (key_iov->iov_buf == NULL)
			key_iov->iov_buf = &srec->sr_key[0];
		else if (key_iov->iov_buf_len >= key_size)
			memcpy(key_iov->iov_buf, &srec->sr_key[0], key_size);
	}

	if (val_iov != NULL) {
		val_iov->iov_len = val_size;
		val_iov->iov_buf_len = val_size;
		if (val_iov->iov_buf == NULL)
			val_iov->iov_buf = val;
		else if (val_iov->iov_buf_len >= val_size)
			memcpy(key_iov->iov_buf, val, val_size);

	}
	return 0;
}

static char *
sk_rec_string(struct btr_instance *tins, struct btr_record *rec,
	      bool leaf, char *buf, int buf_len)
{
	struct sk_rec	*srec = NULL;
	char		*val;
	char		*skey;

	if (!leaf) { /* NB: no record body on intermediate node */
		snprintf(buf, buf_len, "--");
		return buf;
	}

	srec = (struct sk_rec *)umem_id2ptr(&tins->ti_umm, rec->rec_mmid);
	skey = &srec->sr_key[0];
	val = umem_id2ptr(&tins->ti_umm, srec->sr_val_mmid);

	snprintf(buf, buf_len, "%s:%s", skey, val);
	buf[buf_len - 1] = 0;

	return buf;
}

static int
sk_rec_update(struct btr_instance *tins, struct btr_record *rec,
	       daos_iov_t *key, daos_iov_t *val_iov)
{
	struct umem_instance	*umm = &tins->ti_umm;
	struct sk_rec		*srec;
	char			*val;
	TMMID(struct sk_rec)	 srec_mmid;

	srec_mmid = umem_id_u2t(rec->rec_mmid, struct sk_rec);
	srec = umem_id2ptr_typed(umm, srec_mmid);

	if (srec->sr_val_msize >= val_iov->iov_len) {
		umem_tx_add(umm, srec->sr_val_mmid, srec->sr_val_msize);

	} else {
		umem_tx_add_mmid_typed(umm, srec_mmid);
		umem_free(umm, srec->sr_val_mmid);

		srec->sr_val_msize = val_iov->iov_len;
		srec->sr_val_mmid = umem_alloc(umm, val_iov->iov_len);
		D_ASSERT(!UMMID_IS_NULL(srec->sr_val_mmid));
	}
	val = umem_id2ptr(umm, srec->sr_val_mmid);

	memcpy(val, val_iov->iov_buf, val_iov->iov_len);
	srec->sr_val_size = val_iov->iov_len;
	return 0;
}

static int
sk_rec_stat(struct btr_instance *tins, struct btr_record *rec,
	    struct btr_rec_stat *stat)
{
	struct umem_instance	*umm = &tins->ti_umm;
	struct sk_rec		*srec;
	TMMID(struct sk_rec)	 srec_mmid;

	srec_mmid = umem_id_u2t(rec->rec_mmid, struct sk_rec);
	srec = umem_id2ptr_typed(umm, srec_mmid);

	stat->rs_ksize = srec->sr_key_len;
	stat->rs_vsize = srec->sr_val_size;
	return 0;
}

static btr_ops_t sk_ops = {
	.to_key_cmp	= sk_key_cmp,
	.to_key_encode	= sk_key_encode,
	.to_key_decode	= sk_key_decode,
	.to_rec_alloc	= sk_rec_alloc,
	.to_rec_free	= sk_rec_free,
	.to_rec_fetch	= sk_rec_fetch,
	.to_rec_update	= sk_rec_update,
	.to_rec_string	= sk_rec_string,
	.to_rec_stat	= sk_rec_stat,
};

#define SK_ORDER_DEF	16

static int sk_order = SK_ORDER_DEF;

static TMMID(struct btr_root)	sk_root_mmid;
static struct btr_root		sk_root;
static daos_handle_t		sk_toh;

#define SK_SEP		','
#define SK_SEP_VAL	':'

static int
sk_btr_open_create(bool create, char *args)
{
	bool		inplace = false;
	uint64_t	feats = BTR_FEAT_DIRECT_KEY;
	int		rc;

	if (!daos_handle_is_inval(sk_toh)) {
		D_ERROR("Tree has been opened\n");
		return -1;
	}

	if (create && args != NULL) {
		if (args[0] == 'i') { /* inplace create/open */
			inplace = true;
			if (args[1] != SK_SEP) {
				D_ERROR("wrong parameter format %s\n", args);
				return -1;
			}
			args += 2;
		}

		if (args[0] != 'o' || args[1] != SK_SEP_VAL) {
			D_ERROR("incorrect format for tree order: %s\n", args);
			return -1;
		}

		sk_order = atoi(&args[2]);
		if (sk_order < BTR_ORDER_MIN || sk_order > BTR_ORDER_MAX) {
			D_ERROR("Invalid tree order %d\n", sk_order);
			return -1;
		}
	} else if (!create) {
		inplace = (sk_root.tr_class != 0);
		if (TMMID_IS_NULL(sk_root_mmid) && !inplace) {
			D_ERROR("Please create tree first\n");
			return -1;
		}
	}

	if (create) {
		D_PRINT("Create btree with order %d%s feats "DF_X64"\n",
			sk_order, inplace ? " inplace" : "", feats);
		if (inplace) {
			rc = dbtree_create_inplace(SK_TREE_CLASS, feats,
						   sk_order, &sk_uma, &sk_root,
						   &sk_toh);
		} else {
			rc = dbtree_create(SK_TREE_CLASS, feats, sk_order,
					   &sk_uma, &sk_root_mmid, &sk_toh);
		}
	} else {
		D_PRINT("Open btree%s\n", inplace ? " inplace" : "");
		if (inplace)
			rc = dbtree_open_inplace(&sk_root, &sk_uma, &sk_toh);
		else
			rc = dbtree_open(sk_root_mmid, &sk_uma, &sk_toh);
	}
	if (rc != 0) {
		D_ERROR("Tree %s failed: %d\n", create ? "create" : "open",
			 rc);
		return -1;
	}
	return 0;
}

static int
sk_btr_close_destroy(bool destroy)
{
	int rc;

	if (daos_handle_is_inval(sk_toh)) {
		D_ERROR("Invalid tree open handle\n");
		return -1;
	}

	if (destroy) {
		D_PRINT("Destroy btree\n");
		rc = dbtree_destroy(sk_toh);
	} else {
		D_PRINT("Close btree\n");
		rc = dbtree_close(sk_toh);
	}

	sk_toh = DAOS_HDL_INVAL;
	if (rc != 0) {
		D_ERROR("Tree %s failed: %d\n",
			destroy ? "destroy" : "close", rc);
		return -1;
	}
	return rc;
}

static int
btr_rec_verify_delete(umem_id_t *rec, daos_iov_t *key)
{
	TMMID(struct sk_rec)	srec_mmid;
	struct umem_instance	umm;
	struct sk_rec		*srec;
	int			rc;

	rc = umem_class_init(&sk_uma, &umm);
	if (rc != 0) {
		D_ERROR("Failed to instantiate umem while vefify: %d\n", rc);
		return -1;
	}

	srec_mmid = umem_id_u2t(*rec, struct sk_rec);
	srec	  = umem_id2ptr_typed(&umm, srec_mmid);

	if ((srec->sr_key_len != key->iov_len) ||
	    (memcmp(srec->sr_key, key->iov_buf, key->iov_len) != 0)) {
		D_ERROR("Preserved record mismatch while delete\n");
		return -1;
	}

	umem_free(&umm, srec->sr_val_mmid);
	umem_free_typed(&umm, srec_mmid);

	return 0;
}

enum sk_btr_opc {
	BTR_OPC_UPDATE,
	BTR_OPC_LOOKUP,
	BTR_OPC_DELETE,
	BTR_OPC_DELETE_RETAIN,
};

static char *
btr_opc2str(enum sk_btr_opc opc)
{
	switch (opc) {
	default:
		return "unknown";
	case BTR_OPC_UPDATE:
		return "update";
	case BTR_OPC_LOOKUP:
		return "lookup";
	case BTR_OPC_DELETE:
		return "delete";
	case BTR_OPC_DELETE_RETAIN:
		return "delete and retain";
	}
}

static int
sk_btr_kv_operate(enum sk_btr_opc opc, char *str, bool verbose)
{
	int		count = 0;
	umem_id_t	rec_mmid;
	int		rc;

	if (daos_handle_is_inval(sk_toh)) {
		D_ERROR("Can't find opened tree\n");
		return -1;
	}

	while (str != NULL && !isspace(*str) && *str != '\0') {
		char	   *val = NULL;
		char	   *key = str;
		daos_iov_t  key_iov;
		daos_iov_t  val_iov;

		if (opc == BTR_OPC_UPDATE) {
			val = strchr(str, SK_SEP_VAL);
			if (val == NULL) {
				D_ERROR("Invalid parameters %s (errno %d)\n",
					str, errno);
				return -1;
			}
			*val = 0;
			str = ++val;
		}

		str = strchr(str, SK_SEP);
		if (str != NULL) {
			*str = '\0';
			str++;
		}

		daos_iov_set(&key_iov, key, strlen(key) + 1);
		switch (opc) {
		default:
			return -1;
		case BTR_OPC_UPDATE:
			daos_iov_set(&val_iov, val, strlen(val) + 1);
			rc = dbtree_update(sk_toh, &key_iov, &val_iov);
			if (rc != 0) {
				D_ERROR("Failed to update %s:%s\n", key, val);
				return -1;
			}
			break;

		case BTR_OPC_DELETE:
			rc = dbtree_delete(sk_toh, &key_iov, NULL);
			if (rc != 0) {
				D_ERROR("Failed to delete %s\n", key);
				return -1;
			}
			if (verbose)
				D_PRINT("Deleted key %s\n", key);

			if (dbtree_is_empty(sk_toh) && verbose)
				D_PRINT("Tree is empty now\n");
			break;

		case BTR_OPC_DELETE_RETAIN:
			rc = dbtree_delete(sk_toh, &key_iov, &rec_mmid);
			if (rc != 0) {
				D_ERROR("Failed to delete %s\n", key);
				return -1;
			}

			/** Verify and delete rec_mmid here */
			rc = btr_rec_verify_delete(&rec_mmid, &key_iov);
			if (rc != 0) {
				D_ERROR("Failed to verify and delete rec\n");
				return -1;
			}

			if (verbose)
				D_PRINT("Deleted key %s\n", key);
			if (dbtree_is_empty(sk_toh) && verbose)
				D_PRINT("Tree is empty now\n");
			break;

		case BTR_OPC_LOOKUP:
			D_DEBUG(DB_TEST, "Looking for %s\n", key);

			daos_iov_set(&val_iov, NULL, 0); /* get address */
			rc = dbtree_lookup(sk_toh, &key_iov, &val_iov);
			if (rc != 0) {
				D_ERROR("Failed to lookup %s\n", key);
				return -1;
			}

			if (verbose) {
				D_PRINT("Found key %s, value %s\n",
					key, (char *)val_iov.iov_buf);
			}
			break;
		}
		count++;
	}
	if (verbose)
		D_PRINT("%s %d record(s)\n", btr_opc2str(opc), count);
	return 0;
}

static int
sk_btr_query(void)
{
	struct btr_attr		attr;
	struct btr_stat		stat;
	int			rc;

	rc = dbtree_query(sk_toh, &attr, &stat);
	if (rc != 0) {
		D_ERROR("Failed to query btree: %d\n", rc);
		return -1;
	}

	D_PRINT("tree   [order=%d, depth=%d]\n", attr.ba_order, attr.ba_depth);
	D_PRINT("node   [total="DF_U64"]\n"
		"record [total="DF_U64"]\n"
		"key    [total="DF_U64", max="DF_U64"]\n"
		"val    [total="DF_U64", max="DF_U64"]\n",
		stat.bs_node_nr, stat.bs_rec_nr,
		stat.bs_key_sum, stat.bs_key_max,
		stat.bs_val_sum, stat.bs_val_max);

	return 0;
}

static int
sk_btr_iterate(char *args)
{
	daos_handle_t	ih;
	int		i;
	int		d;
	int		del;
	int		rc;
	int		opc;
	char		*start;
	char		*err;
	daos_anchor_t	anchor = {0};
	daos_key_t	anchor_key = {0};

	if (daos_handle_is_inval(sk_toh)) {
		D_ERROR("Can't find opened tree\n");
		return -1;
	}

	rc = dbtree_iter_prepare(sk_toh, BTR_ITER_EMBEDDED, &ih);
	if (rc != 0) {
		err = "initialize";
		goto failed;
	}

	if (args[0] == 'b') {
		opc = BTR_PROBE_LAST;
		args++;
	} else if (args[0] == 'f') {
		opc = BTR_PROBE_FIRST;
		args++;
	} else {
		opc = BTR_PROBE_FIRST;
	}

	if (args[0] == 'd' && args[1] == ':' && strlen(args) >= 3)
		del = atoi(&args[2]);
	else
		del = 0;

	if (args[0] == 's' && args[1] == ':' && strlen(args) >= 3) {
		start = &args[2];
		opc |= BTR_PROBE_SPEC;
	} else {
		start = "";
	}

	anchor_key.iov_buf = (void *)start;
	anchor_key.iov_len = anchor_key.iov_buf_len = strlen(start) + 1;
	sk_key_encode(NULL, &anchor_key, &anchor);
	anchor.da_type = DAOS_ANCHOR_TYPE_KEY;
	for (i = d = 0;; i++) {
		char		*key;
		daos_iov_t	 key_iov;
		daos_iov_t	 val_iov;

		if (i == 0 || (del != 0 && d <= del)) {
			rc = dbtree_iter_probe(ih, opc, NULL, &anchor);
			if (rc == -DER_NONEXIST)
				break;

			if (rc != 0) {
				err = "probe";
				goto failed;
			}

			if (del != 0) {
				if (d == del)
					del = d = 0; /* done */
				else
					d++;
			}
		}

		daos_iov_set(&key_iov, NULL, 0);
		daos_iov_set(&val_iov, NULL, 0);
		rc = dbtree_iter_fetch(ih, &key_iov, &val_iov, &anchor);

		if (rc != 0) {
			err = "fetch";
			goto failed;
		}

		key = key_iov.iov_buf;

		if (d != 0) { /* delete */
			rc = dbtree_iter_delete(ih, NULL);
			if (rc != 0) {
				err = "delete";
				goto failed;
			}

		} else { /* iterate */
			D_PRINT("%s: %s\n", key, (char *)val_iov.iov_buf);

			if (opc == BTR_PROBE_LAST)
				rc = dbtree_iter_prev(ih);
			else
				rc = dbtree_iter_next(ih);

			if (rc == -DER_NONEXIST)
				break;

			if (rc != 0) {
				err = "move";
				goto failed;
			}
		}
	}

	D_PRINT("%s iterator: total %d, deleted %d\n",
		opc == BTR_PROBE_FIRST ? "forward" : "backward", i, d);
	dbtree_iter_finish(ih);
	return 0;
 failed:
	D_PRINT("Iterator %s failed: %d\n", err, rc);
	dbtree_iter_finish(ih);
	return -1;
}

struct kv_node {
	daos_iov_t key;
	daos_iov_t val;
};


/* Mix up the keys */
static void
sk_btr_mix_keys(struct kv_node *kv, unsigned int key_nr)
{
	struct timeval	tv;
	int		nr;

	gettimeofday(&tv, NULL);
	srand(tv.tv_usec);

	for (nr = key_nr; nr > 0; nr--) {
		struct kv_node	tmp;
		int		j;

		j = rand() % nr;
		if (j != nr - 1) {
			tmp = kv[j];
			kv[j] = kv[nr - 1];
			kv[nr - 1] = tmp;
		}
	}
}

static int
key_cmp(const void *k1, const void *k2)
{
	const daos_iov_t	*key1 = k1;
	const daos_iov_t	*key2 = k2;
	const char		*s1 = key1->iov_buf;
	const char		*s2 = key2->iov_buf;
	uint64_t		 len;
	int			 rc;

	len = min(key1->iov_len, key2->iov_len);

	rc = strncasecmp(s1, s2, len);

	if (rc != 0)
		return rc;

	return strncmp(s1, s2, len);
}

/* Sort the keys (for sanity check) */
static void
sk_btr_sort_keys(struct kv_node *kv, unsigned int key_nr)
{
	qsort(kv, key_nr, sizeof(*kv), key_cmp);
}

const char valid[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

#define INT_LEN 32
/* fill in @kv with random string keys/values */
static void
sk_btr_gen_keys(struct kv_node *kv, unsigned int key_nr)
{
	char		*key;
	char		*value;
	struct timeval	tv;
	int		len;
	int		i;
	int		j;

	gettimeofday(&tv, NULL);
	srand(tv.tv_usec);

	for (i = 0; i < key_nr; i++) {
		len = rand() % SK_MAX_KEY_LEN;
		kv[i].val.iov_len = len + 4; /* space for KEY\0 */
		key = kv[i].key.iov_buf = malloc(len + INT_LEN);
		value = kv[i].val.iov_buf = malloc(kv[i].val.iov_len);
		for (j = 0; j < len; j++) {
			int letter = rand() % (sizeof(valid) - 1);

			key[j] = valid[letter];

			letter = (letter + 1) % (sizeof(valid) - 1);
			value[j] = valid[letter];
		}
		strcpy(&value[j], "VAL");
		j = snprintf(key + j, INT_LEN, "key%d", i);
		kv[i].key.iov_len = len + j + 1;
	}
}

static int
sk_btr_check_order(struct kv_node *kv, unsigned int key_nr)
{
	char		*key1;
	char		*val1;
	char		*key2;
	char		*val2;
	char		*err;
	daos_handle_t	ih;
	int		i;
	int		rc;

	sk_btr_sort_keys(kv, key_nr);

	rc = dbtree_iter_prepare(sk_toh, BTR_ITER_EMBEDDED, &ih);
	if (rc != 0) {
		err = "initialize";
		goto failed;
	}

	rc = dbtree_iter_probe(ih, BTR_PROBE_FIRST, NULL, NULL);
	if (rc == -DER_NONEXIST) {
		err = "nonexist";
		goto failed;
	}

	D_PRINT("Checking %d records\n", key_nr);
	/* check the order */
	i = 0;
	for (;;) {
		daos_iov_t	key_iov;
		daos_iov_t	val_iov;

		daos_iov_set(&key_iov, NULL, 0);
		daos_iov_set(&val_iov, NULL, 0);
		rc = dbtree_iter_fetch(ih, &key_iov, &val_iov, NULL);
		if (rc != 0) {
			err = "fetch";
			goto failed;
		}

		key1 = key_iov.iov_buf;
		val1 = val_iov.iov_buf;
		key2 = kv[i].key.iov_buf;
		val2 = kv[i].val.iov_buf;
		if (key_iov.iov_len != kv[i].key.iov_len) {
			err = "key length mismatch";
			D_PRINT("key: " DF_U64 " != " DF_U64 "\n",
				 key_iov.iov_len, kv[i].key.iov_len);
			D_PRINT("key: %s != %s\n", key1, key2);
			goto failed;
		}
		if (val_iov.iov_len != kv[i].val.iov_len) {
			err = "value length mismatch";
			D_PRINT("value: " DF_U64 " != " DF_U64 "\n",
				 val_iov.iov_len, kv[i].val.iov_len);
			D_PRINT("val: %s != %s\n", val1, val2);
			goto failed;
		}
		if (memcmp(key_iov.iov_buf, kv[i].key.iov_buf,
			   kv[i].key.iov_len)) {
			err = "key mismatch";
			D_PRINT("key: %s != %s\n", key1, key2);
			goto failed;
		}
		if (memcmp(val_iov.iov_buf, kv[i].val.iov_buf,
			   kv[i].val.iov_len)) {
			err = "value mismatch";
			D_PRINT("val: %s != %s\n", val1, val2);
			goto failed;
		}

		i++;
		if (i == key_nr)
			break;

		rc = dbtree_iter_next(ih);
		if (rc != 0) {
			D_PRINT("rc = %d\n", rc);
			err = "move";
			goto failed;
		}
	}

	D_PRINT("Order is ok\n");
	dbtree_iter_finish(ih);
	return 0;
 failed:
	D_PRINT("Unexpected ordering, error = %s\n", err);
	dbtree_iter_finish(ih);
	return -1;
}

#define DEL_BATCH	10000
/**
 * batch btree operations:
 * 1) insert @key_nr number of integer keys
 * 2) lookup all the rest keys
 * 3) delete nr=DEL_BATCH keys
 * 4) repeat 2) and 3) util all keys are deleted
 */
static int
sk_btr_batch_oper(unsigned int key_nr)
{
	struct kv_node	*kv;
	char		*key;
	char		*value;
	char		 buf[1024];
	int		 i;
	int		 rc;
	bool		 verbose = key_nr < 20;

	if (key_nr == 0 || key_nr > (1U << 28)) {
		D_PRINT("Invalid key number: %d\n", key_nr);
		return -1;
	}

	kv = malloc(key_nr * sizeof(*kv));
	D_ASSERT(kv != NULL);

	D_PRINT("Batch add %d records.\n", key_nr);
	sk_btr_gen_keys(kv, key_nr);
	for (i = 0; i < key_nr; i++) {
		key = kv[i].key.iov_buf;
		value = kv[i].val.iov_buf;
		sprintf(buf, "%s:%s", key, value);

		rc = sk_btr_kv_operate(BTR_OPC_UPDATE, buf, verbose);
		if (rc != 0) {
			D_PRINT("Batch update failed: %d\n", rc);
			return -1;
		}
	}

	sk_btr_query();

	rc = sk_btr_check_order(kv, key_nr);
	if (rc != 0)
		return -1;

	/* lookup all rest records, delete 10000 of them, and repeat until
	 * deleting all records.
	 */
	sk_btr_mix_keys(kv, key_nr);
	for (i = 0; i < key_nr;) {
		int	j;

		D_PRINT("Batch lookup %d records.\n", key_nr - i);
		for (j = i; j < key_nr; j++) {
			key = kv[j].key.iov_buf;
			sprintf(buf, "%s", key);

			rc = sk_btr_kv_operate(BTR_OPC_LOOKUP, buf, verbose);
			if (rc != 0) {
				D_PRINT("Batch lookup failed: %d\n", rc);
				return -1;
			}
		}

		D_PRINT("Batch delete %d records.\n",
			min(key_nr - i, DEL_BATCH));

		for (j = 0; i < key_nr && j < DEL_BATCH; i++, j++) {
			key = kv[i].key.iov_buf;
			sprintf(buf, "%s", key);

			rc = sk_btr_kv_operate(BTR_OPC_DELETE, buf, verbose);
			if (rc != 0) {
				D_PRINT("Batch delete failed: %d\n", rc);
				return -1;
			}
		}
	}
	sk_btr_query();
	return 0;
}

static int
sk_btr_perf(unsigned int key_nr)
{
	struct kv_node	*kv;
	char		*key;
	char		*value;
	char		 buf[1024];
	int		 i;
	int		 rc = 0;
	double		 then;
	double		 now;

	if (key_nr == 0 || key_nr > (1U << 28)) {
		D_PRINT("Invalid key number: %d\n", key_nr);
		return -1;
	}

	D_PRINT("Btree performance test, order=%u, keys=%u\n",
		sk_order, key_nr);

	kv = malloc(key_nr * sizeof(*kv));
	D_ASSERT(kv != NULL);

	/* step-1: Insert performance */
	sk_btr_gen_keys(kv, key_nr);
	then = dts_time_now();

	for (i = 0; i < key_nr; i++) {
		key = kv[i].key.iov_buf;
		value = kv[i].val.iov_buf;
		sprintf(buf, "%s:%s", key, value);

		rc = sk_btr_kv_operate(BTR_OPC_UPDATE, buf, false);
		if (rc != 0) {
			D_PRINT("update failed: %d\n", rc);
			D_GOTO(out, rc = -1);
		}
	}
	now = dts_time_now();
	D_PRINT("insert = %10.2f/sec\n", key_nr / (now - then));

	/* step-2: lookup performance */
	sk_btr_mix_keys(kv, key_nr);
	then = dts_time_now();

	for (i = 0; i < key_nr; i++) {
		key = kv[i].key.iov_buf;
		sprintf(buf, "%s", key);

		rc = sk_btr_kv_operate(BTR_OPC_LOOKUP, buf, false);
		if (rc != 0) {
			D_PRINT("lookup failed: %d\n", rc);
			D_GOTO(out, rc = -1);
		}
	}
	now = dts_time_now();
	D_PRINT("lookup = %10.2f/sec\n", key_nr / (now - then));

	/* step-3: delete performance */
	sk_btr_mix_keys(kv, key_nr);
	then = dts_time_now();

	for (i = 0; i < key_nr; i++) {
		key = kv[i].key.iov_buf;
		sprintf(buf, "%s", key);

		rc = sk_btr_kv_operate(BTR_OPC_DELETE, buf, false);
		if (rc != 0) {
			D_PRINT("delete failed: %d\n", rc);
			D_GOTO(out, rc = -1);
		}
	}
	now = dts_time_now();
	D_PRINT("delete = %10.2f/sec\n", key_nr / (now - then));

out:
	free(kv);
	return rc;
}

static struct option btr_ops[] = {
	{ "create",	required_argument,	NULL,	'C'	},
	{ "destroy",	no_argument,		NULL,	'D'	},
	{ "open",	no_argument,		NULL,	'o'	},
	{ "close",	no_argument,		NULL,	'c'	},
	{ "update",	required_argument,	NULL,	'u'	},
	{ "find",	required_argument,	NULL,	'f'	},
	{ "delete",	required_argument,	NULL,	'd'	},
	{ "del_retain", required_argument,	NULL,	'r'	},
	{ "query",	no_argument,		NULL,	'q'	},
	{ "iterate",	required_argument,	NULL,	'i'	},
	{ "batch",	required_argument,	NULL,	'b'	},
	{ "perf",	required_argument,	NULL,	'p'	},
	{ NULL,		0,			NULL,	0	},
};

int
main(int argc, char **argv)
{
	int parse_loc;
	int	rc;

	sk_toh = DAOS_HDL_INVAL;
	sk_root_mmid = TMMID_NULL(struct btr_root);
	sk_root.tr_class = 0;

	rc = daos_debug_init(NULL);
	if (rc != 0)
		return rc;

	rc = dbtree_class_register(SK_TREE_CLASS, BTR_FEAT_DIRECT_KEY, &sk_ops);
	D_ASSERT(rc == 0);

	optind = 0;
	sk_uma.uma_id = UMEM_CLASS_VMEM;
	D_PRINT("--------------------------------------\n");
	while ((parse_loc = getopt_long(argc, argv, "mC:Docqu:d:r:f:i:b:p:",
				 btr_ops, NULL)) != -1) {
		switch (parse_loc) {
		case 'C':
			rc = sk_btr_open_create(true, optarg);
			break;
		case 'D':
			rc = sk_btr_close_destroy(true);
			break;
		case 'o':
			rc = sk_btr_open_create(false, NULL);
			break;
		case 'c':
			rc = sk_btr_close_destroy(false);
			break;
		case 'q':
			rc = sk_btr_query();
			break;
		case 'u':
			rc = sk_btr_kv_operate(BTR_OPC_UPDATE, optarg, true);
			break;
		case 'f':
			rc = sk_btr_kv_operate(BTR_OPC_LOOKUP, optarg, true);
			break;
		case 'd':
			rc = sk_btr_kv_operate(BTR_OPC_DELETE, optarg, true);
			break;
		case 'r':
			rc = sk_btr_kv_operate(BTR_OPC_DELETE_RETAIN, optarg,
					       true);
			break;
		case 'i':
			rc = sk_btr_iterate(optarg);
			break;
		case 'b':
			rc = sk_btr_batch_oper(atoi(optarg));
			break;
		case 'p':
			rc = sk_btr_perf(atoi(optarg));
			break;
		case 'm':
			sk_uma.uma_id = UMEM_CLASS_PMEM;
			sk_uma.uma_pool = pmemobj_create(POOL_NAME,
						"btree-perf-test", POOL_SIZE,
						0666);
			break;
		default:
			D_PRINT("Unsupported command %c\n", rc);
			break;
		}
		D_PRINT("--------------------------------------\n");
	}
	daos_debug_fini();
	if (sk_uma.uma_id == UMEM_CLASS_PMEM) {
		pmemobj_close(sk_uma.uma_pool);
		remove(POOL_NAME);
	}
	if (rc != 0)
		printf("Error: %d\n", rc);

	return rc;
}
