/**
 * distgen internal functions.
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 * Josef Weidendorfer <weidendo@in.tum.de>
 *
 * Licensed under GNU Lesser General Public License 2.1 or later.
 * Some rights reserved. See LICENSE
 */

#include "distgen/distgen_internal.h"
#include "distgen/distgen.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <pthread.h>
#include <sched.h>

#ifdef __QNXNTO__
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif

static pthread_t threads[DISTGEN_MAXTHREADS];
static pthread_attr_t thread_attr[DISTGEN_MAXTHREADS];

// make sure that gcd(size,diff) is 1 by increasing size, return size
static u64 adjustSize(u64 size, u64 diff);

static u64 distSize[MAXDISTCOUNT];
static u64 distBlocks[MAXDISTCOUNT];
static u64 distIter[MAXDISTCOUNT];

struct entry *buffer[DISTGEN_MAXTHREADS];

// options (to be reset to default if 0)
static int distsUsed = 0;
size_t tcount = 1; // number of threads to use (default: 1)
int pseudoRandom = 0;
int depChain = 0;
int doWrite = 0;
size_t iter = 0;
static int verbose = 0;

static u64 blocks, blockDiff;

double wtime() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + 1e-6 * tv.tv_usec;
}

static char *prettyVal(char *s, u64 v) {
	static char str[50];

	if (!s) s = str;
	if (v > 1000000000000ull)
		sprintf(s, "%.1f T", 1.0 / 1024.0 / 1024.0 / 1024.0 / 1024.0 * v);
	else if (v > 1000000000ull)
		sprintf(s, "%.1f G", 1.0 / 1024.0 / 1024.0 / 1024.0 * v);
	else if (v > 1000000ull)
		sprintf(s, "%.1f M", 1.0 / 1024.0 / 1024.0 * v);
	else if (v > 1000ull)
		sprintf(s, "%.1f K", 1.0 / 1024.0 * v);
	else
		sprintf(s, "%llu", v);

	return s;
}

void addDist(u64 size) {
	int d, dd;

	// distances are sorted
	for (d = 0; d < distsUsed; d++) {
		if (distSize[d] == size) return; // one dist only once
		if (distSize[d] < size) break;
	}

	assert(distsUsed < MAXDISTCOUNT);
	for (dd = distsUsed; dd >= d; dd--) distSize[dd] = distSize[dd - 1];

	distSize[d] = size;
	distsUsed++;
}

static void *init_memory_per_thread(void *arg) {
	size_t tid = *static_cast<size_t *>(arg);

#ifdef __QNXNTO__
	// QNX does not support pthread thread affinity. Set affinity here.
	// TODO duplicated code in libdistgen.cpp
	unsigned int number_of_cpus = RMSK_SIZE(_syspage_ptr->num_cpu);
	const int size = number_of_cpus * sizeof(unsigned int) * 2;

	unsigned int *rsizep = (unsigned int *)malloc(size);
	assert(rsizep != nullptr);
	memset((void *)rsizep, 0x00, size);

	*rsizep = number_of_cpus;

	/* Set the runmask. Call this macro once for each processor
	the thread can run on. */
	unsigned int *rmaskp = rsizep + 1;
	RMSK_SET(tid, rmaskp);

	/* Set the inherit mask. Call this macro once for each
	processor the thread's children can run on. */
	unsigned int *imaskp = rmaskp + number_of_cpus;
	RMSK_SET(tid, imaskp);

	if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, rsizep) == -1) {
		assert(false);
	}
#endif

	struct entry *buf;
	u64 idx, blk, nextIdx;
	u64 idxMax = blocks * BLOCKLEN / sizeof(entry);
	u64 idxIncr = blockDiff * BLOCKLEN / sizeof(entry);

	// allocate and initialize used memory
	int err = posix_memalign((void **)&buffer[tid], 64, blocks * BLOCKLEN);
	assert(err == 0);
	buf = buffer[tid];
	assert(buf != nullptr);
	for (idx = 0; idx < idxMax; idx++) {
		buf[idx].v = static_cast<double>(idx);
		buf[idx].next = 0;
	}

	// generate dependency chain
	idx = 0;
	for (blk = 0; blk < blocks; blk++) {
		nextIdx = idx + idxIncr;
		if (nextIdx >= idxMax) nextIdx -= idxMax;
		// fprintf(stderr, " Blk %d, POff %d\n", blk, nextIdx);
		assert(buf[idx].next == 0);
		buf[idx].next = buf + nextIdx;
		idx = nextIdx;
	}

#ifdef __QNXNTO__
	free(rsizep);
#endif

	return nullptr;
}

void initBufs() {
	assert(tcount < DISTGEN_MAXTHREADS);
	assert(sizeof(struct entry) == 16);

	for (int d = 0; d < distsUsed; d++) {
		// each memory block of cacheline size gets accessed
		distBlocks[d] = (distSize[d] + BLOCKLEN - 1) / BLOCKLEN;
		distIter[d] = distSize[0] / distSize[d];
	}

	if (verbose) {
		fprintf(stderr, "  number of distances: %d\n", distsUsed);
		for (int d = 0; d < distsUsed; d++)
			fprintf(stderr, "    D%2d: size %llu (%llu traversals per iteration)\n", d + 1, distSize[d], distIter[d]);
	}

	blocks = (distSize[0] + BLOCKLEN - 1) / BLOCKLEN;
	blockDiff = pseudoRandom ? (blocks * 7 / 17) : 1;
	blocks = adjustSize(blocks, blockDiff);

	if (verbose) {
		// calculate expected number of accesses
		size_t aCount = 0;
		for (int d = 0; d < distsUsed; d++) aCount += distIter[d] * distBlocks[d];
		if (doWrite) aCount += aCount;
		char sBuf[20], tsBuf[20], acBuf[20], tacBuf[20], tasBuf[20];
		prettyVal(sBuf, BLOCKLEN * blocks);
		prettyVal(tsBuf, BLOCKLEN * blocks * tcount);
		prettyVal(acBuf, aCount);
		prettyVal(tacBuf, aCount * tcount * iter);
		prettyVal(tasBuf, aCount * tcount * iter * 64.0);

		fprintf(stderr, "  buffer size per thread %sB (total %sB), address diff %llu\n", sBuf, tsBuf,
				BLOCKLEN * blockDiff);
		fprintf(stderr, "  accesses per iteration and thread: %s (total %s accs = %sB)\n", acBuf, tacBuf, tasBuf);
	}

	// initialize buffers in general
	size_t thread_ids[tcount];
	for (size_t i = 0; i < tcount; i++) {
		thread_ids[i] = i;
		int res = pthread_create(&threads[i], &thread_attr[i], init_memory_per_thread, &thread_ids[i]);
		assert(res == 0);
	}

	for (size_t i = 0; i < tcount; i++) {
		int res = pthread_join(threads[i], NULL);
		assert(res == 0);
	}
}

// helper for adjustSize
static u64 gcd(u64 a, u64 b) {
	if (b == 0) return a;
	return gcd(b, a % b);
}

// make sure that gcd(size,diff) is 1 by increasing size, return size
static u64 adjustSize(u64 size, u64 diff) {
	while (gcd(size, diff) > 1) size++;
	return size;
}

void runBench(struct entry *buffer, size_t iter, int depChain, int doWrite, double *sum, u64 *aCount) {
	int d, k;
	u64 j, idx, max;
	double lsum, v = 1.23;
	u64 idxIncr = blockDiff * BLOCKLEN / sizeof(struct entry);
	u64 idxMax = blocks * BLOCKLEN / sizeof(struct entry);
	int benchType = depChain + 2 * doWrite;

	lsum = *sum;
	for (size_t i = 0; i < iter; i++) {
		lsum += buffer[0].v;
		for (d = 0; d < distsUsed; d++) {
			lsum += buffer[0].v;
			for (k = 0; k < distIter[d]; k++) {
				// fprintf(stderr, "D %d, I %d\n", d, k);
				*aCount += distBlocks[d];
				max = distBlocks[d];

				switch (benchType) {
				case 0: // no dep chain, no write
					idx = 0;
					for (j = 0; j < max; j++) {
						lsum += buffer[idx].v;
						idx += idxIncr;
						if (idx >= idxMax) idx -= idxMax;
						// fprintf(stderr, " Off %d\n", idx);
					}
					break;

				case 1: // dep chain, no write
				{
					struct entry *p = buffer;
					for (j = 0; j < max; j++) {
						lsum += p->v;
						p = p->next;
						// fprintf(stderr, " POff %d\n", (int)(p - buffer));
					}
				} break;

				case 2: // no dep chain, write
					idx = 0;
					for (j = 0; j < max; j++) {
						buffer[idx].v += 1.0;
						lsum += buffer[idx].v;
						idx += idxIncr;
						if (idx >= idxMax) idx -= idxMax;
						// fprintf(stderr, " Off %d\n", idx);
					}
					break;

				case 3: // dep chain, write
				{
					struct entry *p = buffer;
					for (j = 0; j < max; j++) {
						p->v += 1.0;
						lsum += p->v;
						p->v = v;
						p = p->next;
						// fprintf(stderr, " POff %d\n", (int)(p - buffer));
					}
				} break;
				default:
					assert(0);
				}
			}
		}
	}

	*sum = lsum;
}
