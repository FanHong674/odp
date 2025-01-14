/* Copyright (c) 2015-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <inttypes.h>

#include <odp_api.h>
#include <odp/helper/odph_api.h>

#define MAX_WORKERS		32
#define MAX_NUM_BUF		8192
#define ITERATION_NUM		2048
#define LOG_BASE		8
#define LOG_ENTRY_SIZE		19
#define LOG_LINE_SIZE		(LOG_BASE * LOG_ENTRY_SIZE + 1)

#define QUEUE_NAME_PREFIX	"thread_queue_"

typedef struct {
	odp_time_t timestamp;
	int id;
} timestamp_event_t;

typedef struct {
	uint8_t thr;
	uint8_t id;
	odp_time_t time;
} log_entry_t;

typedef struct {
	uint32_t iteration_num;
	odp_atomic_u32_t iteration_counter;
	odp_atomic_u32_t id_counter;
	odp_atomic_u32_t log_counter;
	odp_atomic_u32_t err_counter;
	odp_barrier_t start_barrier;
	odp_barrier_t end_barrier;
	int thread_num;
	log_entry_t *log;
	int log_enries_num;
} test_globals_t;

static void print_log(test_globals_t *gbls)
{
	uint32_t err_num;
	int i, j, k, pad;
	char line[LOG_LINE_SIZE];

	memset(line, '-', LOG_LINE_SIZE - 1);
	line[LOG_LINE_SIZE - 1] = 0;
	for (i = 1; i <= gbls->thread_num; i++) {
		printf("\n==== history of %d buffer, time,ns (thread) ====\n%s\n",
		       i, line);

		/* print log for buffer */
		k = 0;
		for (j = 0; j < gbls->log_enries_num; j++)
			if (gbls->log[j].id == i) {
				printf("%10" PRIu64 " (%-3d)",
				       odp_time_to_ns(gbls->log[j].time),
				       gbls->log[j].thr);

				if (!(++k % LOG_BASE))
					printf("  |\n");
				else
					printf(" =>");
			}

		if ((k % LOG_BASE)) {
			pad = (LOG_BASE - k % LOG_BASE) * LOG_ENTRY_SIZE - 4;
			printf(" end%*c\n%s\n", pad, '|', line);
		} else {
			printf("%s\n", line);
		}
	}

	printf("\n\n");

	err_num = odp_atomic_load_u32(&gbls->err_counter);
	if (err_num)
		printf("Number of errors: %u\n", err_num);
}

static void generate_next_queue(test_globals_t *gbls, odp_queue_t *queue,
				unsigned int id)
{
	int thr;
	uint8_t rand_u8;
	char queue_name[sizeof(QUEUE_NAME_PREFIX) + 2];
	unsigned int rand_id = 1;

	thr = odp_thread_id();

	/* generate next random id */
	if (gbls->thread_num > 1) {
		do {
			odp_random_data(&rand_u8, 1, ODP_RANDOM_BASIC);
			rand_id = rand_u8 % gbls->thread_num + 1;
		} while (rand_id == id);
	}

	sprintf(queue_name, QUEUE_NAME_PREFIX "%d", rand_id);
	*queue = odp_queue_lookup(queue_name);

	if (ODP_QUEUE_INVALID == *queue)
		ODPH_ABORT("Cannot lookup thread queue \"%s\", thread %d\n",
			   queue_name, thr);
}

static void test_global_timestamps(test_globals_t *gbls,
				   odp_queue_t queue, unsigned int id)
{
	int thr;
	int log_entry;
	odp_event_t ev;
	odp_time_t time;
	odp_buffer_t buf;
	odp_queue_t queue_next;
	timestamp_event_t *timestamp_ev;

	thr = odp_thread_id();
	while (odp_atomic_load_u32(&gbls->iteration_counter) <
				   gbls->iteration_num) {
		ev = odp_queue_deq(queue);

		if (ev == ODP_EVENT_INVALID)
			continue;

		buf = odp_buffer_from_event(ev);
		timestamp_ev = (timestamp_event_t *)odp_buffer_addr(buf);

		time = odp_time_global();
		if (odp_time_cmp(time, timestamp_ev->timestamp) < 0) {
			ODPH_ERR("timestamp is less than previous time_prev=%"
				 PRIu64 "ns, time_next=%"
				 PRIu64 "ns, thread %d\n",
				 odp_time_to_ns(timestamp_ev->timestamp),
				 odp_time_to_ns(time), thr);
			odp_atomic_inc_u32(&gbls->err_counter);
		}

		/* update the log */
		log_entry = odp_atomic_fetch_inc_u32(&gbls->log_counter);
		gbls->log[log_entry].time = timestamp_ev->timestamp;
		gbls->log[log_entry].id = timestamp_ev->id;
		gbls->log[log_entry].thr = thr;

		/* assign new current time and send */
		generate_next_queue(gbls, &queue_next, id);
		timestamp_ev->timestamp = time;
		if (odp_queue_enq(queue_next, ev))
			ODPH_ABORT("Cannot enqueue event %" PRIu64 " on queue "
				   "%" PRIu64 ", thread %d\n",
				   odp_event_to_u64(ev),
				   odp_queue_to_u64(queue_next), thr);

		odp_atomic_inc_u32(&gbls->iteration_counter);
	}
}

/**
 * @internal Worker thread
 *
 * @param ptr  Pointer to test arguments
 *
 * @return Pointer to exit status
 */
static int run_thread(void *ptr)
{
	int thr;
	uint32_t id;
	odp_event_t ev;
	odp_buffer_t buf;
	test_globals_t *gbls;
	odp_pool_t buffer_pool;
	odp_queue_t queue, queue_next;
	timestamp_event_t *timestamp_ev;
	char queue_name[sizeof(QUEUE_NAME_PREFIX) + 2];

	gbls = ptr;
	thr = odp_thread_id();
	printf("Thread %i starts on cpu %i\n", thr, odp_cpu_id());

	/*
	 * Allocate own queue for receiving timestamps.
	 * Own queue is needed to guarantee that next thread for receiving
	 * buffer is not the same thread.
	 */
	id = odp_atomic_fetch_inc_u32(&gbls->id_counter);
	sprintf(queue_name, QUEUE_NAME_PREFIX "%d", id);
	queue = odp_queue_create(queue_name, NULL);
	if (queue == ODP_QUEUE_INVALID)
		ODPH_ABORT("Cannot create thread queue, thread %d", thr);

	/* allocate buffer for timestamp */
	buffer_pool = odp_pool_lookup("time buffers pool");
	if (buffer_pool == ODP_POOL_INVALID)
		ODPH_ABORT("Buffer pool was not found, thread %d\n", thr);

	buf = odp_buffer_alloc(buffer_pool);
	if (buf == ODP_BUFFER_INVALID)
		ODPH_ABORT("Buffer was not allocated, thread %d\n", thr);

	/* wait all threads allocated their queues */
	odp_barrier_wait(&gbls->start_barrier);

	/* enqueue global timestamp to some queue of some other thread */
	generate_next_queue(gbls, &queue_next, id);

	/* save global timestamp and id for tracing */
	ev = odp_buffer_to_event(buf);
	timestamp_ev = (timestamp_event_t *)odp_buffer_addr(buf);
	timestamp_ev->id = id;
	timestamp_ev->timestamp = odp_time_global();
	if (odp_queue_enq(queue_next, ev))
		ODPH_ABORT("Cannot enqueue timestamp event %" PRIu64 " on "
			   "queue %" PRIu64 ", thread %d", odp_event_to_u64(ev),
			   odp_queue_to_u64(queue_next), thr);

	test_global_timestamps(gbls, queue, id);

	/* wait all threads are finished their jobs */
	odp_barrier_wait(&gbls->end_barrier);

	/* free all events on the allocated queue */
	while (1) {
		ev = odp_queue_deq(queue);
		if (ev == ODP_EVENT_INVALID)
			break;

		buf = odp_buffer_from_event(ev);
		odp_buffer_free(buf);
	}

	/* free allocated queue */
	if (odp_queue_destroy(queue))
		ODPH_ABORT("Cannot destroy queue %" PRIu64 "",
			   odp_queue_to_u64(queue));

	printf("Thread %i exits\n", thr);
	fflush(NULL);
	return 0;
}

int main(int argc, char *argv[])
{
	int err = 0;
	odp_pool_t pool = ODP_POOL_INVALID;
	int num_workers;
	test_globals_t *gbls;
	odp_cpumask_t cpumask;
	odp_pool_capability_t pool_capa;
	odp_pool_param_t pool_param;
	odp_shm_t shm_glbls = ODP_SHM_INVALID;
	odp_shm_t shm_log = ODP_SHM_INVALID;
	int log_size, log_enries_num;
	odph_helper_options_t helper_options;
	odph_thread_t thread_tbl[MAX_WORKERS];
	odp_instance_t instance;
	odp_init_t init_param;
	odph_thread_common_param_t thr_common;
	odph_thread_param_t thr_param;

	printf("\nODP global time test starts\n");

	/* Let helper collect its own arguments (e.g. --odph_proc) */
	argc = odph_parse_options(argc, argv);
	if (odph_options(&helper_options)) {
		ODPH_ERR("Error: reading ODP helper options failed.\n");
		exit(EXIT_FAILURE);
	}

	odp_init_param_init(&init_param);
	init_param.mem_model = helper_options.mem_model;

	if (odp_init_global(&instance, &init_param, NULL)) {
		err = 1;
		ODPH_ERR("ODP global init failed.\n");
		goto end;
	}

	/* Init this thread. */
	if (odp_init_local(instance, ODP_THREAD_CONTROL)) {
		err = 1;
		ODPH_ERR("ODP local init failed.\n");
		goto err_global;
	}

	num_workers = MAX_WORKERS;
	num_workers = odp_cpumask_default_worker(&cpumask, num_workers);

	shm_glbls = odp_shm_reserve("test_globals", sizeof(test_globals_t),
				    ODP_CACHE_LINE_SIZE, 0);
	if (ODP_SHM_INVALID == shm_glbls) {
		err = 1;
		ODPH_ERR("Error: shared mem reserve failed.\n");
		goto err;
	}

	log_enries_num = num_workers * (ITERATION_NUM + num_workers);
	log_size = sizeof(log_entry_t) * log_enries_num;
	shm_log = odp_shm_reserve("test_log", log_size, ODP_CACHE_LINE_SIZE, 0);
	if (ODP_SHM_INVALID == shm_log) {
		err = 1;
		ODPH_ERR("Error: shared mem reserve failed.\n");
		goto err;
	}

	gbls = odp_shm_addr(shm_glbls);
	gbls->thread_num = num_workers;
	gbls->iteration_num = ITERATION_NUM;
	odp_atomic_store_u32(&gbls->iteration_counter, 0);
	odp_atomic_store_u32(&gbls->id_counter, 1);
	odp_atomic_store_u32(&gbls->log_counter, 0);
	odp_atomic_store_u32(&gbls->err_counter, 0);
	gbls->log_enries_num = log_enries_num;
	gbls->log = odp_shm_addr(shm_log);
	odp_barrier_init(&gbls->start_barrier, num_workers);
	odp_barrier_init(&gbls->end_barrier, num_workers);
	memset(gbls->log, 0, log_size);

	if (odp_pool_capability(&pool_capa)) {
		err = 1;
		ODPH_ERR("Error: pool capability failed.\n");
		goto err;
	}

	odp_pool_param_init(&pool_param);

	pool_param.buf.size  = sizeof(timestamp_event_t);
	pool_param.buf.num   = MAX_NUM_BUF;
	pool_param.type      = ODP_POOL_BUFFER;

	if (pool_capa.buf.max_num && MAX_NUM_BUF > pool_capa.buf.max_num)
		pool_param.buf.num = pool_capa.buf.max_num;

	pool = odp_pool_create("time buffers pool", &pool_param);
	if (pool == ODP_POOL_INVALID) {
		err = 1;
		ODPH_ERR("Pool create failed.\n");
		goto err;
	}

	odph_thread_common_param_init(&thr_common);
	odph_thread_param_init(&thr_param);

	thr_param.start    = run_thread;
	thr_param.arg      = gbls;
	thr_param.thr_type = ODP_THREAD_WORKER;

	thr_common.instance = instance;
	thr_common.cpumask = &cpumask;
	thr_common.share_param = 1;

	/* Create and launch worker threads */
	odph_thread_create(thread_tbl, &thr_common, &thr_param, num_workers);

	/* Wait for worker threads to exit */
	odph_thread_join(thread_tbl, num_workers);

	print_log(gbls);

err:
	if (pool != ODP_POOL_INVALID)
		if (odp_pool_destroy(pool))
			err = 1;

	if (shm_log != ODP_SHM_INVALID)
		if (odp_shm_free(shm_log))
			err = 1;

	if (shm_glbls != ODP_SHM_INVALID)
		if (odp_shm_free(shm_glbls))
			err = 1;

	if (odp_term_local())
		err = 1;
err_global:
	if (odp_term_global(instance))
		err = 1;
end:
	if (err) {
		ODPH_ERR("Err: ODP global time test failed\n\n");
		return -1;
	}

	printf("ODP global time test complete\n\n");
	return 0;
}
