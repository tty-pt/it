/* SPDX-FileCopyrightText: 2022 Paulo Andre Azevedo Quirino
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * It is important that this program can be understood by people who are not
 * programmers, so I'm adding an in-depth description of the algorithm as
 * comments. The most important ones will be at the top of the functions. To
 * understand the algorithm, I recommend that you start from the bottom of this
 * file, and scroll up as needed. It might also be useful to lookup the
 * definition of specific functions if you want to know how they work in more
 * detail. For that it is enough that you search for "^process_start" for
 * example. It is also recommended that before you do that, you read the
 * README.md to understand the format of the input data files.
 *
 * Mind you, dates are expressed in ISO-8601 format to users, but internally
 * we use unix timestamps. This is to facilitate a user to analyse the input
 * data easily while permitting the software to evaluate datetimes
 * mathematically in a consistent way.
 *
 * Person ids are also particular in this way. In the input file they are
 * textual, but internally we use numeric ids to which they correspond.
 *
 * Currency values are read as float but internally they are integers.
 *
 * The general idea of the algorithm involves a few data structures:
 *
 * One of them is a weighted and directed graph, in which each node represents
 * a person, and the edges connecting the nodes represent the accumulated debt
 * between them.
 *
 * Another is a binary search tree (BST) that stores intervals of time, that
 * we query in order to find out who was present during the billing periods,
 * etc. Actually, there are two of these kinds of BSTs. One That only stores
 * intervals where the person is actually in the house (BST A), another that
 * stores intervals where the person is renting a room there, but might not be
 * present (BST B).
 *
 * Jump to the main function when you are ready to check out how it all works.
 *
 * Happy reading!
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE
#include <ctype.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifdef __OpenBSD__
#include <db4/db.h>
#include <sys/queue.h>
#else
#ifdef ALPINE
#include <stdint.h>
#include <db4/db.h>
#else
#include <db.h>
#endif
#include <bsd/sys/queue.h>
#endif

#define CBUG(c) if (c) { fprintf(stderr, "CBUG! " #c " %s:%s:%d\n", \
		__FILE__, __FUNCTION__, __LINE__); raise(SIGINT); exit(1); }

#ifdef __OpenBSD__
#define TS_MIN LLONG_MIN
#define TS_MAX LLONG_MAX
#else
#define TS_MIN LONG_MIN
#define TS_MAX LONG_MAX
#endif

#define DATE_MAX_LEN 20

#define DB_ITER(whodb) \
	DBC *cur; \
	DBT key, data; \
	\
	CBUG(whodb->cursor(whodb, NULL, &cur, 0)); \
	\
	memset(&key, 0, sizeof(DBT)); \
	memset(&data, 0, sizeof(DBT)); \
	\
	while (1) \
		if (cur->c_get(cur, &key, &data, DB_NEXT) == DB_NOTFOUND) { \
			CBUG(cur->close(cur)); \
			break; \
		} else

#define USERNAME_MAX_LEN 32

struct ti {
	time_t min, max;
	unsigned who;
};

struct isplit {
	time_t ts;
	int max;
	unsigned who;
};

struct match {
	struct ti ti;
	STAILQ_ENTRY(match) entry;
};

STAILQ_HEAD(match_stailq, match);

struct split {
	time_t min;
	time_t max;
	DB *whodb;
	size_t count;
	TAILQ_ENTRY(split) entry;
};

TAILQ_HEAD(split_tailq, split);

struct tidbs {
	DB *ti; // keys and values are struct ti
	DB *max; // secondary DB (BTREE) with interval max as key
	DB *id; // secondary DB (BTREE) with ids as primary key
} pdbs;

enum pflags {
	PF_DETACH = 1, // daemonize
	PF_WAKE = 2, // don't shut down
};

DB *gdb = NULL; // graph primary DB (keys are usernames, values are user ids)
DB *igdb = NULL; // secondary DB to lookup usernames via ids

static DB_ENV *dbe = NULL;

unsigned g_len = 0;
unsigned g_notfound = (unsigned) -1;
unsigned pflags = 0;

const time_t mtinf = (time_t) TS_MIN; // minus infinite
const time_t tinf = (time_t) TS_MAX; // infinite

static int srv_fd = -1;
static fd_set fds_read, fds_active, fds_write;

void sig_shutdown(int i)
{

	pflags &= ~PF_WAKE;
}

/* get timestamp from ISO-8601 date string */
static time_t
sscantime(char *buf)
{
	char *aux;
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	aux = strptime(buf, "%Y-%m-%dT%H:%M:%S", &tm);
	if (!aux && !strptime(buf, "%Y-%m-%d", &tm)) {
		char *endptr;
		unsigned long long int timestamp = strtoull(buf, &endptr, 10);
		if (errno == 0 && *endptr == '\0' && buf != endptr)
			return (time_t)timestamp;
		else
			err(EXIT_FAILURE, "Invalid date or timestamp");
	}

	tm.tm_isdst = -1;
	return mktime(&tm);
}

/* get ISO-8601 date string from timestamp
 *
 * only use this for debug (memory leak), or free pointer
 */
static char *
printtime(time_t ts)
{
	char *buf;
	struct tm tm;

	if (ts == mtinf)
		return "-inf";

	if (ts == tinf)
		return "inf";

	buf = (char *) malloc(DATE_MAX_LEN);
	tm = *localtime(&ts);

	if (tm.tm_sec || tm.tm_min || tm.tm_hour)
		strftime(buf, DATE_MAX_LEN, "%FT%T", &tm);
	else
		strftime(buf, DATE_MAX_LEN, "%F", &tm);

	return buf;
}

/* read a word */
static size_t
read_word(char *buf, char *input, size_t max_len)
{
	size_t ret = 0;

	for (; *input && isspace(*input); input++, ret++);

	for (; *input && !isspace(*input) && ret < max_len;
		input++, buf++, ret++)

		*buf = *input;

	CBUG(ret > max_len); // FIXME?
	*buf = '\0';

	return ret;
}

/* read date in iso 8601 and convert it to a unix timestamp */
static size_t
read_ts(time_t *target, char *line)
{
	char date_str[DATE_MAX_LEN];
	size_t ret;

	ret = read_word(date_str, line, sizeof(date_str));
	*target = sscantime(date_str);

	return ret;
}

static int
who_init(DB **whodb) {
	return db_create(whodb, dbe, 0) \
		|| gdb->open(*whodb, NULL, NULL, NULL, DB_HASH, DB_CREATE, 0664);
}

/* insert a list of present people into a split */
static int
who_count(DB *whodb)
{
	int len = 0;
	DB_ITER(whodb)
		len++;
	return len;
}

/******
 * read functions
 ******/

static unsigned g_find(char *name);

/* read id and convert it to existing numeric id */
static size_t
read_id(unsigned *id, char *line)
{
	char username[USERNAME_MAX_LEN];
	size_t ret;

	ret = read_word(username, line, sizeof(username));
	*id = g_find(username);

	return ret;
}

/******
 * making secondary indices
 ******/

/* create person id to nickname HASH keys from nickname to person id HASH data
 */
static int
map_gdb_igdb(DB *sec, const DBT *key, const DBT *data, DBT *result)
{
	memset(result, 0, sizeof(DBT));
	result->size = sizeof(unsigned);
	result->data = data->data;
	return 0;
}

/* create time interval BTREE keys from time interval HASH db*/
static int
map_tidb_timaxdb(DB *sec, const DBT *key, const DBT *data, DBT *result)
{
	memset(result, 0, sizeof(DBT));
	result->size = sizeof(time_t);
	result->data = &((struct ti *) data->data)->max;
	return 0;
}

/* create id BTREE keys from time interval HASH db */
static int
map_tidb_tiiddb(DB *sec, const DBT *key, const DBT *data, DBT *result)
{
	memset(result, 0, sizeof(DBT));
	result->size = sizeof(unsigned);
	result->data = &((struct ti *) data->data)->who;
	return 0;
}

/******
 * key ordering compare functions
 ******/

/* compare two time intervals (for sorting BST items) */
static int
#ifdef __APPLE__
timax_cmp(DB *sec, const DBT *a_r, const DBT *b_r, size_t *locp)
#else
timax_cmp(DB *sec, const DBT *a_r, const DBT *b_r)
#endif
{
	time_t	a = * (time_t *) a_r->data,
		b = * (time_t *) b_r->data;
	return b > a ? -1 : (a > b ? 1 : 0);
}

/* compare two person ids (for sorting BST items) */
static int
#ifdef __APPLE__
tiid_cmp(DB *sec, const DBT *a_r, const DBT *b_r, size_t *locp)
#else
tiid_cmp(DB *sec, const DBT *a_r, const DBT *b_r)
#endif
{
	unsigned a = * (unsigned *) a_r->data,
		 b = * (unsigned *) b_r->data;
	return b > a ? -1 : (a > b ? 1 : 0);
}

/******
 * Database initializers
 ******/

/* initialize ti dbs */
static int
tidbs_init(struct tidbs *dbs, char *fname)
{
	return db_create(&dbs->ti, dbe, 0)
		|| dbs->ti->open(dbs->ti, NULL, fname, "ti", DB_HASH, DB_CREATE, 0664)

		|| db_create(&dbs->max, dbe, 0)
		|| dbs->max->set_bt_compare(dbs->max, timax_cmp)
		|| dbs->max->set_flags(dbs->max, DB_DUP)
		|| dbs->max->open(dbs->max, NULL, fname, "max", DB_BTREE, DB_CREATE, 0664)
		|| dbs->ti->associate(dbs->ti, NULL, dbs->max, map_tidb_timaxdb, DB_CREATE | DB_IMMUTABLE_KEY)

		|| db_create(&dbs->id, dbe, 0)
		|| dbs->id->set_bt_compare(dbs->id, tiid_cmp)
		|| dbs->id->set_flags(dbs->id, DB_DUP)
		|| dbs->id->open(dbs->id, NULL, fname, "id", DB_BTREE, DB_CREATE, 0664)
		|| dbs->ti->associate(dbs->ti, NULL, dbs->id, map_tidb_tiiddb, DB_CREATE | DB_IMMUTABLE_KEY);
}

/* Initialize all dbs */
static void
dbs_init(char *fname)
{
	int ret = db_create(&gdb, dbe, 0)
		|| gdb->open(gdb, NULL, fname, "g", DB_HASH, DB_CREATE, 0664)

		|| db_create(&igdb, dbe, 0)
		|| igdb->open(igdb, NULL, fname, "ig", DB_HASH, DB_CREATE, 0664)
		|| gdb->associate(gdb, NULL, igdb, map_gdb_igdb, DB_CREATE)

		|| tidbs_init(&pdbs, fname);

	CBUG(ret);
}

/******
 * g (usernames to user ids) related functions
 ******/

/* insert new person id (auto-generated) */
unsigned
g_insert(char *name)
{
	DBT key, data;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = name;
	key.size = strlen(name) + 1;
	data.data = &g_len;
	data.size = sizeof(g_len);

	CBUG(gdb->put(gdb, NULL, &key, &data, 0));
	return g_len++;
}

/* find existing person id from their nickname */
static unsigned
g_find(char *name)
{
	DBT key, data;
	int ret;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = name;
	key.size = strlen(name) + 1;

	ret = gdb->get(gdb, NULL, &key, &data, 0);

	if (ret == DB_NOTFOUND)
		return g_notfound;
 
	CBUG(ret);
	return * (unsigned *) data.data;
}

/******
 * gi (ids to usernames) functions
 ******/

/* get person nickname from numeric id
 *
 * assumes strings of length 31 tops
 */
static char *
gi_get(unsigned id)
{
	DBT key, pkey, data;

	memset(&key, 0, sizeof(DBT));
	memset(&pkey, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &id;
	key.size = sizeof(id);;

	CBUG(igdb->pget(igdb, NULL, &key, &pkey, &data, 0));
	return pkey.data;
}

/******
 * who (db of "current" people, for use in split calculation) related functions
 ******/

/* drop the db of present people */
static void
who_drop(DB *whodb)
{
	DBC *cur;
	DBT key, data;
	int res;

	CBUG(whodb->cursor(whodb, NULL, &cur, 0));

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	while (1) {
		res = cur->c_get(cur, &key, &data, DB_NEXT);

		if (res == DB_NOTFOUND)
			break;

		CBUG(res);
		CBUG(cur->c_del(cur, 0));
	}

	CBUG(cur->close(cur));
}

/* put a person in the db of present people */
static void
who_insert(DB *whodb, unsigned who)
{
	DBT key, data;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &who;
	key.size = sizeof(who);
	data.data = NULL;
	data.size = 0;

	CBUG(whodb->put(whodb, NULL, &key, &data, 0));
}

/* remove a person from the db of present people */
static int
who_remove(DB *whodb, unsigned who) {
	DBT key;

	memset(&key, 0, sizeof(DBT));

	key.data = &who;
	key.size = sizeof(who);

	CBUG(whodb->del(whodb, NULL, &key, 0));
}

static int
who_get(DB *whodb, unsigned who) {
	DBT key, data;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &who;
	key.size = sizeof(who);

	switch (whodb->get(whodb, NULL, &key, &data, 0)) {
	case 0:
		return 1;
	case DB_NOTFOUND:
		return 0;
	default:
		CBUG(1);
		return -1;
	}
}

/******
 * ti (struct ti to struct ti primary db) related functions
 ******/
 
/* insert a time interval into an AVL */
static void
ti_insert(struct tidbs *dbs, unsigned id, time_t start, time_t end)
{
	struct ti ti = { .min = start, .max = end, .who = id };
	DBT key, data;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &ti;
	key.size = sizeof(ti);
	data.data = &ti;
	data.size = sizeof(ti);

	CBUG(dbs->ti->put(dbs->ti, NULL, &key, &data, 0));
}

/* finish the last found interval at the provided timestamp for a certain
 * person id
 */
static void
ti_finish_last(struct tidbs *dbs, unsigned id, time_t end)
{
	struct ti ti;
	DBT key, data;
	DBT pkey;
	DBC *cur;
	int dbflags = DB_SET;

	CBUG(dbs->id->cursor(dbs->id, NULL, &cur, 0));

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &id;
	key.size = sizeof(id);

	do {
		CBUG(cur->c_get(cur, &key, &data, dbflags)); // even DB_NOTFOUND
		CBUG(* (unsigned *) key.data != id);
		memcpy(&ti, data.data, sizeof(ti));
		dbflags = DB_NEXT;
	} while (ti.max != tinf);

	CBUG(cur->del(cur, 0));
	cur->close(cur);
	memset(&key, 0, sizeof(DBT));
	key.data = data.data = &ti;
	key.size = data.size = sizeof(ti);
	/* ti.who = id; */
	ti.max = end;
	data.data = &ti;
	data.size = sizeof(ti);
	CBUG(dbs->ti->put(dbs->ti, NULL, &key, &data, 0));
}

/* intersect an interval with an AVL of intervals */
static inline unsigned
ti_intersect(struct tidbs *dbs, struct match_stailq *matches, time_t min, time_t max)
{
	struct ti tmp;
	DBC *cur;
	DBT key, data;
	int ret = 0, dbflags = DB_SET_RANGE;

	STAILQ_INIT(matches);
	CBUG(dbs->max->cursor(dbs->max, NULL, &cur, 0));

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &min;
	key.size = sizeof(time_t);

	while (1) {
		int res = cur->c_get(cur, &key, &data, dbflags);

		if (res == DB_NOTFOUND)
			break;

		CBUG(res);

		dbflags = DB_NEXT;
		memcpy(&tmp, data.data, sizeof(struct ti));

		if (tmp.max > min && tmp.min <= max) {
			// its a match
			struct match *match = (struct match *) malloc(sizeof(struct match));
			memcpy(&match->ti, &tmp, sizeof(tmp));
			STAILQ_INSERT_TAIL(matches, match, entry);
			ret++;
		}
	}

	cur->close(cur);
	return ret;
}

/* intersect a point with an AVL of intervals */
static inline unsigned
ti_pintersect(struct tidbs *dbs, struct match_stailq *matches, time_t ts)
{
	return ti_intersect(dbs, matches, ts, ts);
}

int
ti_present(struct tidbs *dbs, time_t when, unsigned who) {
	int ret = 0, dbflags = DB_SET_RANGE;
	struct ti tmp;
	DBC *cur;
	DBT key, data;

	CBUG(dbs->max->cursor(dbs->max, NULL, &cur, 0));

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));

	key.data = &when;
	key.size = sizeof(time_t);

	while (1) {
		int res = cur->c_get(cur, &key, &data, dbflags);

		if (res == DB_NOTFOUND)
			break;

		CBUG(res);

		dbflags = DB_NEXT;
		memcpy(&tmp, data.data, sizeof(struct ti));

		if (tmp.who == who && tmp.max > when && tmp.min <= when) {
			ret++;
			break;
		}
	}

	cur->close(cur);
	return ret;
}

/******
 * matches related functions
 ******/

/* makes all provided matches lie within the provided interval [min, max] */
static inline void
matches_fix(struct match_stailq *matches, time_t min, time_t max)
{
	struct match *match;

	STAILQ_FOREACH(match, matches, entry) {
		if (match->ti.min < min)
			match->ti.min = min;
		if (match->ti.max > max)
			match->ti.max = max;
	}
}

static void
matches_free(struct match_stailq *matches)
{
	struct match *match, *match_tmp;

	STAILQ_FOREACH_SAFE(match, matches, entry, match_tmp) {
		STAILQ_REMOVE_HEAD(matches, entry);
		free(match);
	}
}

/******
 * isplit related functions
 ******/

/* compares isplits, so that we can sort them */
static int
isplit_cmp(const void *ap, const void *bp)
{
	struct isplit a, b;
	memcpy(&a, ap, sizeof(struct isplit));
	memcpy(&b, bp, sizeof(struct isplit));
	if (b.ts > a.ts)
		return -1;
	if (a.ts > b.ts)
		return 1;
	if (b.max > a.max)
		return -1;
	if (a.max > b.max)
		return 1;
	return 0;
}

// assumes isplits is of size matches_l * 2
/* creates intermediary isplits */
static inline struct isplit *
isplits_create(struct match_stailq *matches, size_t matches_l) {
	struct isplit *isplits = (struct isplit *) malloc(sizeof(struct isplit) * matches_l * 2);
	struct match *match;
	unsigned i = 0;

	STAILQ_FOREACH(match, matches, entry) {
		struct isplit *isplit = isplits + i * 2;
		isplit->ts = match->ti.min;
		isplit->max = 0;
		isplit->who = match->ti.who;
		isplit++;
		isplit->ts = match->ti.max;
		isplit->max = 1;
		isplit->who = match->ti.who;
		i ++;
	};

	return isplits;
}

/******
 * split related functions
 ******/

/* Creates one split from its interval, and the list of people that are present
 */
static inline struct split *
split_create(DB *whodb, time_t min, time_t max)
{
	struct split *split = (struct split *) malloc(sizeof(struct split));
	split->min = min;
	split->max = max;
	who_init(&split->whodb);
	split->count = who_count(split->whodb);
	{ DB_ITER(whodb) who_insert(split->whodb, * (unsigned *) key.data); }
	return split;
}

/* Creates splits from the intermediary isplit array */
static inline void
splits_create(
		struct split_tailq *splits,
		struct isplit *isplits,
		size_t matches_l)
{
	DB *whodb = NULL;
	int i;

	TAILQ_INIT(splits);

	who_init(&whodb);
	for (i = 0; i < matches_l * 2 - 1; i++) {
		struct isplit *isplit = isplits + i;
		struct isplit *isplit2 = isplits + i + 1;
		struct split *split;
		time_t n, m;

		if (isplit->max)
			who_remove(whodb, isplit->who);
		else
			who_insert(whodb, isplit->who);

		n = isplit->ts;
		m = isplit2->ts;

		if (n == m)
			continue;

		split = split_create(whodb, n, m);
		TAILQ_INSERT_TAIL(splits, split, entry);
	}
	whodb->close(whodb, 0);
}

/* From a list of matched intervals, this creates the tail queue of splits
 */
static void
splits_init(struct split_tailq *splits, struct match_stailq *matches, unsigned matches_l)
{
	struct isplit *isplits;
	struct isplit *isplit;
	struct isplit *buf;
	int i = 0;

	isplits = isplits_create(matches, matches_l);
	qsort(isplits, matches_l * 2, sizeof(struct isplit), isplit_cmp);
	splits_create(splits, isplits, matches_l);
	free(isplits);
}

/* Obtains a tail queue of splits from the intervals that intersect the query
 * interval [min, max]
 */
static void
splits_get(struct split_tailq *splits, struct tidbs *dbs, time_t min, time_t max)
{
	struct match_stailq matches;
	unsigned matches_l = ti_intersect(dbs, &matches, min, max);
	matches_fix(&matches, min, max);
	splits_init(splits, &matches, matches_l);
	matches_free(&matches);
}

/* Inserts a tail queue of splits within another, before the element provided
 */
static inline void
splits_concat_before(
		struct split_tailq *target,
		struct split_tailq *origin,
		struct split *before)
{
	struct split *split, *tmp;
	TAILQ_FOREACH_SAFE(split, origin, entry, tmp) {
		TAILQ_REMOVE(origin, split, entry);
		TAILQ_INSERT_BEFORE(before, split, entry);
	}
}

/* Fills the spaces between splits (or on empty splits) with splits from BST B,
 * in order to resolve the situation where none of the people are present for
 * periods of time within the billing period (the aforementined gaps).
 */
static inline void
splits_fill(struct split_tailq *splits, time_t min, time_t max)
{
	struct split *split, *tmp;
	struct split_tailq more_splits;
	time_t last_max;

	split = TAILQ_FIRST(splits);
	if (!split) {
		splits_get(splits, &pdbs, min, max);
		return;
	}

	last_max = min;

	if (split->min > last_max) {
		struct split_tailq more_splits;
		splits_get(&more_splits, &pdbs, last_max, split->min);
		splits_concat_before(splits, &more_splits, split);
	}

	last_max = split->max;

	TAILQ_FOREACH_SAFE(split, splits, entry, tmp) {
		if (!split->count) {
			struct split_tailq more_splits;
			splits_get(&more_splits, &pdbs, split->min, split->max);
			splits_concat_before(splits, &more_splits, split);
			TAILQ_REMOVE(splits, split, entry);
		}

		last_max = split->max;
	}

	if (max > last_max) {
		struct split_tailq more_splits;
		splits_get(&more_splits, &pdbs, last_max, max);
		TAILQ_CONCAT(splits, &more_splits, entry);
	}
}

/* adds debt for each person of the tail queue of splits to the payer of the
 * bill
 */
static inline void
splits_print(struct split_tailq *splits)
{
	struct split *split;

	TAILQ_FOREACH(split, splits, entry) {
		time_t interval = split->max - split->min;
		printf("%ld", interval);
		DB_ITER(split->whodb)
			printf(" %s", gi_get(* (unsigned *) key.data));
		printf("\n");
	}
}

/* Frees a tail queue of splits */
static void
splits_free(struct split_tailq *splits)
{
	struct split *split, *split_tmp;

	TAILQ_FOREACH_SAFE(split, splits, entry, split_tmp) {
		split->whodb->close(split->whodb, 0);
		TAILQ_REMOVE(splits, split, entry);
		free(split);
	}
}

/******
 * functions that process a valid type of line
 ******/

/* This function is for handling lines in the format:
 *
 * STOP <DATE> <PERSON_ID>
 *
 * It reads a PERSON_ID, and it checks if there is a correspondant graph node
 * (and so a numeric id). If there is one, it finishes the last intervals in
 * both BSTs (much like process_pause does for BST A). If there isn't a graph
 * node for that user (and therefore no numeric id), it generates one. Then it
 * inserts the time interval [-∞, DATE] (and the newly created id) into both
 * BSTs.
 */
static inline void
process_stop(time_t ts, char *line)
{
	char username[USERNAME_MAX_LEN];
	unsigned id;

	line += read_word(username, line, sizeof(username));
	id = g_find(username);

	if (id != g_notfound) {
		if (ti_present(&pdbs, ts, id))
			ti_finish_last(&pdbs, id, ts);
	} else {
		id = g_insert(username);
		ti_insert(&pdbs, id, mtinf, ts);
	}
}

/* This function is for handling lines in the format:
 *
 * START <DATE> <PERSON_ID> [<PHONE_NUMBER> <EMAIL> ... <NAME>]
 *
 * So it reads a textual person id, then it inserts it into the graph as a
 * node, generating a numeric id. Then it inserts the time interval [DATE, +∞]
 * along with that numeric id into both BST A and BST B.
 */
static inline void
process_start(time_t ts, char *line)
{
	char username[USERNAME_MAX_LEN];
	unsigned id;

	line += read_word(username, line, sizeof(username));
	id = g_find(username);
	if (id == g_notfound)
		id = g_insert(username);
	if (!ti_present(&pdbs, ts, id))
		ti_insert(&pdbs, id, ts, tinf);
}

/******
 * etc
 ******/

/* This function is what processes each line. For each of them, it first checks
 * if it starts with a "#", in other words, if it is totally commented out.
 * If it is, it ignores this line. If it isn't, it then proceeds to read a
 * word: the TYPE of operation or event that the line represents. It also reads
 * the DATE. After this, it checks what the TYPE of operation is. Depending on
 * that, it does different things. In all valid cases (and for every different
 * TYPE), it calls a function named process_<TYPE> (lowercase), all of these
 * functions receive the DATE that was read, and also a pointer to the part of
 * the line that wasn't read yet.
 *
 * Check out the functions in the section above to understand how these work
 * internally.
 */
static void
process_line(char *line)
{
	char op_type_str[9], date_str[DATE_MAX_LEN];
	time_t ts;

	if (line[0] == '#' || line[0] == '\n')
		return;

	line += read_word(op_type_str, line, sizeof(op_type_str));
	line += read_ts(&ts, line);

	switch (*op_type_str) {
	case 'S':
		if (op_type_str[1] != 'T')
			goto error;

		switch(op_type_str[2]) {
		case 'A':
			return process_start(ts, line);
		case 'O':
			return process_stop(ts, line);
		}
	}

error:
	/* err(EXIT_FAILURE, "Invalid format"); */
}

static void
process_query(int fd, char *line)
{
	char buf[BUFSIZ * 3], *s;
	int type = 0;
	char *space;
	time_t min;

	switch (*line) {
		case '*':
			type = 1;
			line += 2;
		case '+':
			type = 2;
			line += 2;
	}

	s = buf;
	s += sprintf(s, "# %s\n", line);
	space = strchr(line, ' ');
	line += read_ts(&min, line);
	if (space) {
		// https://softwareengineering.stackexchange.com/questions/363091/split-overlapping-ranges-into-all-unique-ranges/363096#363096

		time_t max;
		struct split_tailq splits;
		struct split *split;
		unsigned id;

		read_ts(&max, line);

		splits_get(&splits, &pdbs, min, max);
		splits_fill(&splits, min, max);
		if (type == 1) TAILQ_FOREACH(split, &splits, entry) {
			time_t interval = split->max - split->min;
			s += sprintf(s, "%ld", interval);
			DB_ITER(split->whodb)
				dprintf(fd, " %s", gi_get(* (unsigned *) key.data));
			s += sprintf(s, "\n");
		} else {
			DB *whodb = NULL;
			who_init(&whodb);

			TAILQ_FOREACH(split, &splits, entry) {
				DB_ITER(split->whodb) {
					unsigned who = * (unsigned *) key.data;
					if (!who_get(whodb, who))
						who_insert(whodb, who);
				}
			}

			if (type == 2) TAILQ_FOREACH(split, &splits, entry) {
				DB_ITER(whodb) {
					unsigned who = * (unsigned *) key.data;
					if (!who_get(split->whodb, who))
						who_remove(whodb, who);
				}
			}

			DB_ITER(whodb) {
				unsigned who = * (unsigned *) key.data;
				s += sprintf(s, "%s\n", gi_get(who));
			}

			whodb->close(whodb, 0);
		}
		splits_free(&splits);
	} else {
		struct match_stailq matches;
		struct match *match, *match_tmp;
		unsigned matches_l = ti_intersect(&pdbs, &matches, min, min);
		STAILQ_FOREACH_SAFE(match, &matches, entry, match_tmp) {
			s += sprintf(s, "%s\n", gi_get(match->ti.who));
			STAILQ_REMOVE_HEAD(&matches, entry);
			free(match);
		}
	}

	dprintf(fd, "%s", buf);
}

static inline void
usage(char *prog)
{
	fprintf(stderr, "Usage: %s [-d] [-f FILE] [-C DIR] [-S PATH]\n", prog);
	fprintf(stderr, "    Options:\n");
	fprintf(stderr, "        -f FILE   Change db filename (it.db)\n");
	fprintf(stderr, "        -C DIR    Change db home (/var/lib/it)\n");
	fprintf(stderr, "        -S PATH   Set socket path (/tmp/it-sock)\n");
	fprintf(stderr, "        -d        Daemonize.\n");
}

int
descr_read(int fd)
{
	char buf[BUFSIZ * 3], *line = NULL, *eol = NULL;
	int query = 0;
	int ret;

	// read INPUT
	do {
		memset(buf, 0, sizeof(buf));
		ret = read(fd, buf, sizeof(buf));
		switch (ret) {
			case -1: if (errno == EAGAIN) return 0;
			case 0: return -1;
		}

		line = buf;

again:
		eol = strchr(line, '\n');
		if (eol)
			*eol = '\0';

		if (strcmp(line, "EOF")) {
			if (query)
				process_query(fd, line);
			else
				process_line(line);
		} else
			query = 1;

		if (eol) {
			eol ++;
			if (eol - buf >= ret)
				continue;

			line = eol;
			goto again;
		}
	} while (1);

	return 0;
}

void descr_proc() {
	for (register int fd = 0; fd < FD_SETSIZE; fd++)
		if (!FD_ISSET(fd, &fds_read)) ;
		else if (fd == srv_fd) {
			struct sockaddr_un addr;
			socklen_t addr_len = (socklen_t)sizeof(addr);
			int fd = accept(srv_fd, (struct sockaddr *) &addr, &addr_len);
			if (fd <= 0)
				continue;
			FD_SET(fd, &fds_active);
		} else if (descr_read(fd) < 0) {
			shutdown(fd, 2);
			close(fd);
			FD_CLR(fd, &fds_active);
			FD_CLR(fd, &fds_read);
		}
}
 
/* The main function is the entry point to the application. In this case, it
 * is very basic. What it does is it reads each line that was fed in standard
 * input. This allows you to feed it any file you want by running:
 *
 * $ cat file.txt | ./it "2022-03-01 2022-05-15T10:00:00" "now" "2023-11-18"
 *
 * You can also just run "./it", input manually, and then hit ctrl+D.
 *
 * For each line read, it then calls process_line, with that line as an
 * argument (a pointer). Look at process_line right above this comment to
 * understand how it works.
 *
 * After reading each line in standard input, the program shows the debt
 * that was calculated, that is owed between the people (ge_show_all).
 */
int
main(int argc, char *argv[])
{
	char *line = NULL;
	char *fname = "it.db";
	char *dbhome = "/var/lib/it/";
	char *sockpath = "/tmp/it-sock";
	ssize_t linelen;
	size_t linesize;
	int ret;
	char c;

	while ((c = getopt(argc, argv, "df:C:S:")) != -1) {
		switch (c) {
		case 'd':
			pflags |= PF_DETACH;
			break;
		case 'f':
			fname = optarg;
			break;
		case 'C':
			dbhome = optarg;
			break;
		case 'S':
			sockpath = optarg;
			break;
		default:
			usage(*argv);
			return 1;
	}

	db_env_create(&dbe, 0);
	CBUG(dbe->open(dbe, dbhome, DB_CREATE | DB_INIT_LOCK | DB_INIT_MPOOL, 0664));
	dbs_init(fname);

	if ((pflags & PF_DETACH) && daemon(1, 1) != 0)
		return 0;

	pflags |= PF_WAKE;
	signal(SIGTERM, sig_shutdown);
	signal(SIGINT, sig_shutdown);
	signal(SIGPIPE, SIG_IGN);

	srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);

	int opt = 1;
	if (setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)) < 0)
		err(1, "srv_fd setsockopt SO_REUSEADDR");

	opt = 1;
	if (setsockopt(srv_fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &opt, sizeof(opt)) < 0)
		err(1, "srv_fd setsockopt SO_KEEPALIVE");

	if (fcntl(srv_fd, F_SETFL, O_NONBLOCK) == -1)
		err(1, "srv_fd fcntl F_SETFL O_NONBLOCK");

	struct sockaddr_un server;
	memset(&server, 0, sizeof(struct sockaddr_un));
	server.sun_family = AF_UNIX;
	strcpy(server.sun_path, sockpath);
	unlink(server.sun_path);

	if (bind(srv_fd, (struct sockaddr *) &server, sizeof(server))) {
		close(srv_fd);
		err(4, "bind");
	}

	if (listen(srv_fd, 5) == -1) {
		close(srv_fd);
		err(4, "listen");
	}

	FD_SET(srv_fd, &fds_active);
	FD_ZERO(&fds_read);
	while (pflags & PF_WAKE) {
		struct timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		fds_read = fds_active;
		int select_n = select(FD_SETSIZE, &fds_read, NULL, NULL, &timeout);

		switch (select_n) {
		case -1:
			switch (errno) {
			case EAGAIN: /* return 0; */
			case EINTR:
			case EBADF: continue;
			}

			perror("select");
			break;

		case 0: continue;
		}

		for (; select_n; select_n--)
			descr_proc();
	}

	CBUG(pdbs.max->close(pdbs.max, 0));
	CBUG(pdbs.id->close(pdbs.id, 0));
	CBUG(pdbs.ti->close(pdbs.ti, 0));
	CBUG(igdb->close(igdb, 0));
	CBUG(gdb->close(gdb, 0));
	dbe->close(dbe, 0);
	return EXIT_SUCCESS;
}
