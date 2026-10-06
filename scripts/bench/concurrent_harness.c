/*
 * Many writers and readers through ONE DuckDB: one database, one ATTACH of the lake, a connection
 * per thread (specs/016). The CLI has a single connection, and no Python package matches a dev
 * build, so this links the build's own libduckdb - the same source builds against any line.
 *
 *   concurrent_harness <setup.sql> <writer.sql|-> <writers> <reader.sql|-> <readers> <ops> <check.sql|->
 *
 * setup.sql runs once on its own connection (LOAD, ATTACH, tables). writer.sql and reader.sql are
 * templates run <ops> times by each thread of their role: {t} is the thread's number within its
 * role, {i} the iteration. In every file {MSSQL_DSN} and {PG_DSN} are replaced from the
 * environment (MSSQL_DUCKLAKE_TEST_DSN, MSSQL_DUCKLAKE_PG_DSN), so no connection string is written
 * anywhere. check.sql runs after all threads have finished; its first row is printed.
 *
 * Output, one line per role: role threads ops errors wall_s ops_per_s p50_ms p95_ms max_ms, then
 * `first_error <role> <message>` when there was one, then `check <values>`.
 */
#include "duckdb.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static duckdb_database db;

static char *read_file(const char *path) {
	if (strcmp(path, "-") == 0) {
		return NULL;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *s = malloc((size_t)n + 1);
	if (fread(s, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	s[n] = 0;
	fclose(f);
	return s;
}

/* every occurrence of `key` in `s` replaced by `value`; a new string */
static char *replace(const char *s, const char *key, const char *value) {
	size_t kn = strlen(key), vn = strlen(value), count = 0;
	for (const char *p = strstr(s, key); p; p = strstr(p + kn, key)) {
		count++;
	}
	char *out = malloc(strlen(s) + count * (vn > kn ? vn - kn : 0) + 1);
	char *o = out;
	const char *p = s;
	for (const char *q = strstr(p, key); q; q = strstr(p, key)) {
		memcpy(o, p, (size_t)(q - p));
		o += q - p;
		memcpy(o, value, vn);
		o += vn;
		p = q + kn;
	}
	strcpy(o, p);
	return out;
}

static char *with_env(const char *s) {
	const char *mssql = getenv("MSSQL_DUCKLAKE_TEST_DSN");
	const char *pg = getenv("MSSQL_DUCKLAKE_PG_DSN");
	char *a = replace(s, "{MSSQL_DSN}", mssql ? mssql : "");
	char *b = replace(a, "{PG_DSN}", pg ? pg : "");
	free(a);
	return b;
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* run one statement text; on error the message is copied into `error` (if empty) and 1 returned */
static int run(duckdb_connection con, const char *sql, char *error, size_t error_size, char *first_row,
               size_t row_size) {
	duckdb_result result;
	int failed = duckdb_query(con, sql, &result) == DuckDBError;
	if (failed) {
		if (error && !error[0]) {
			const char *msg = duckdb_result_error(&result);
			snprintf(error, error_size, "%s", msg ? msg : "(no message)");
		}
	} else if (first_row && duckdb_row_count(&result) > 0) {
		size_t off = 0;
		for (idx_t c = 0; c < duckdb_column_count(&result); c++) {
			char *v = duckdb_value_varchar(&result, c, 0);
			off += (size_t)snprintf(first_row + off, row_size - off, "%s%s", c ? " " : "", v ? v : "NULL");
			duckdb_free(v);
		}
	}
	duckdb_destroy_result(&result);
	return failed;
}

typedef struct {
	const char *template_sql;
	int thread;
	int ops;
	double *latencies;
	int errors;
	char error[512];
} worker_t;

static void *work(void *arg) {
	worker_t *w = arg;
	duckdb_connection con;
	if (duckdb_connect(db, &con) == DuckDBError) {
		snprintf(w->error, sizeof(w->error), "connect failed");
		w->errors = w->ops;
		return NULL;
	}
	char number[32];
	snprintf(number, sizeof(number), "%d", w->thread);
	char *mine = replace(w->template_sql, "{t}", number);
	for (int i = 0; i < w->ops; i++) {
		snprintf(number, sizeof(number), "%d", i);
		char *sql = replace(mine, "{i}", number);
		double start = now_ms();
		w->errors += run(con, sql, w->error, sizeof(w->error), NULL, 0);
		w->latencies[i] = now_ms() - start;
		free(sql);
	}
	free(mine);
	duckdb_disconnect(&con);
	return NULL;
}

static int compare(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

typedef struct {
	const char *name;
	char *template_sql;
	int threads;
	worker_t *workers;
	pthread_t *handles;
} role_t;

int main(int argc, char **argv) {
	if (argc != 8) {
		fprintf(stderr, "usage: %s setup.sql writer.sql|- writers reader.sql|- readers ops check.sql|-\n", argv[0]);
		return 2;
	}
	char *setup_raw = read_file(argv[1]);
	char *check_raw = read_file(argv[7]);
	int ops = atoi(argv[6]);
	role_t roles[2] = {{"writer", NULL, atoi(argv[3]), NULL, NULL}, {"reader", NULL, atoi(argv[5]), NULL, NULL}};
	const char *templates[2] = {argv[2], argv[4]};
	for (int r = 0; r < 2; r++) {
		char *raw = read_file(templates[r]);
		roles[r].template_sql = raw ? with_env(raw) : NULL;
		free(raw);
		if (!roles[r].template_sql) {
			roles[r].threads = 0;
		}
	}

	duckdb_config config;
	duckdb_create_config(&config);
	duckdb_set_config(config, "allow_unsigned_extensions", "true");
	duckdb_set_config(config, "autoload_known_extensions", "false");
	duckdb_set_config(config, "autoinstall_known_extensions", "false");
	char *open_error = NULL;
	if (duckdb_open_ext(NULL, &db, config, &open_error) == DuckDBError) {
		fprintf(stderr, "open failed: %s\n", open_error ? open_error : "");
		return 1;
	}
	duckdb_destroy_config(&config);

	duckdb_connection setup;
	duckdb_connect(db, &setup);
	char error[2048] = {0};
	char *setup_sql = with_env(setup_raw);
	if (run(setup, setup_sql, error, sizeof(error), NULL, 0)) {
		fprintf(stderr, "setup failed: %s\n", error);
		return 1;
	}
	free(setup_sql);

	double start = now_ms();
	for (int r = 0; r < 2; r++) {
		role_t *role = &roles[r];
		role->workers = calloc((size_t)role->threads, sizeof(worker_t));
		role->handles = calloc((size_t)role->threads, sizeof(pthread_t));
		for (int t = 0; t < role->threads; t++) {
			worker_t *w = &role->workers[t];
			w->template_sql = role->template_sql;
			w->thread = t;
			w->ops = ops;
			w->latencies = calloc((size_t)ops, sizeof(double));
			pthread_create(&role->handles[t], NULL, work, w);
		}
	}
	double finished[2] = {start, start};
	for (int r = 0; r < 2; r++) {
		for (int t = 0; t < roles[r].threads; t++) {
			pthread_join(roles[r].handles[t], NULL);
		}
		finished[r] = now_ms();
	}

	for (int r = 0; r < 2; r++) {
		role_t *role = &roles[r];
		if (role->threads == 0) {
			continue;
		}
		size_t n = (size_t)role->threads * (size_t)ops;
		double *all = malloc(n * sizeof(double));
		int errors = 0;
		const char *first_error = NULL;
		for (int t = 0; t < role->threads; t++) {
			memcpy(all + (size_t)t * (size_t)ops, role->workers[t].latencies, (size_t)ops * sizeof(double));
			errors += role->workers[t].errors;
			if (!first_error && role->workers[t].error[0]) {
				first_error = role->workers[t].error;
			}
		}
		qsort(all, n, sizeof(double), compare);
		double wall = (finished[r] - start) / 1000.0;
		printf("%s %d %zu %d %.3f %.1f %.1f %.1f %.1f\n", role->name, role->threads, n, errors, wall,
		       (double)n / wall, all[n / 2], all[(size_t)((double)n * 0.95)], all[n - 1]);
		if (first_error) {
			char flat[512];
			snprintf(flat, sizeof(flat), "%s", first_error);
			for (char *p = flat; *p; p++) {
				if (*p == '\n') {
					*p = ' ';
				}
			}
			printf("first_error %s %s\n", role->name, flat);
		}
		free(all);
	}

	if (check_raw) {
		char *check_sql = with_env(check_raw);
		char row[1024] = {0};
		error[0] = 0;
		if (run(setup, check_sql, error, sizeof(error), row, sizeof(row))) {
			printf("check_error %s\n", error);
		} else {
			printf("check %s\n", row);
		}
		free(check_sql);
	}
	duckdb_disconnect(&setup);
	duckdb_close(&db);
	return 0;
}
