/*
 * lat_pagefault.c - time a page fault in
 *
 * Usage: lat_pagefault [-C] [-P <parallel>] [-W <warmup>] [-N <repetitions>] file 
 *
 * Copyright (c) 2000 Carl Staelin.
 * Copyright (c) 1994 Larry McVoy.  Distributed under the FSF GPL with
 * additional restriction that results may published only if
 * (1) the benchmark is unmodified, and
 * (2) the version in the sccsid below is included in the report.
 * Support for this development by Sun Microsystems is gratefully acknowledged.
 */
char	*id = "$Id$\n";

#include "bench.h"

#define	CHK(x)	if ((x) == -1) { perror("x"); exit(1); }

typedef struct _state {
	int fd;
	size_t size;
	int npages;
	int clone;
	char* file;
	char* where;
	size_t* pages;
	int fd_drop_cache;
	int drop_cache; // 0: don't drop, 1: drop
	int page_permutation; // 0: serialize page, 1: randomize page
	int verbose;
} state_t;

void	initialize(iter_t iterations, void *cookie);
void	cleanup(iter_t iterations, void *cookie);
void	benchmark(iter_t iterations, void * cookie);
void	benchmark_mmap(iter_t iterations, void * cookie);
size_t* serialize(void *cookie, size_t scale);
void	check_pageout(void *cookie);
void	drop_cache(int fd);

int
main(int ac, char **av)
{
	int parallel = 1;
	int warmup = 0;
	int repetitions = -1;
	int c;
	double t_mmap;
	double t_combined;
	struct stat   st;
	struct _state state;
	char buf[2048];
	char* usage = "[-C] [-D] [-S] [-V] [-P <parallel>] [-W <warmup>] [-N <repetitions>] file\n";

	state.clone = 0;
	state.fd_drop_cache = -1;
	state.drop_cache = 0;
	state.page_permutation = 1;
	state.verbose = 0;

	while (( c = getopt(ac, av, "P:W:N:CDSV")) != EOF) {
		switch(c) {
		case 'P':
			parallel = atoi(optarg);
			if (parallel <= 0) lmbench_usage(ac, av, usage);
			break;
		case 'W':
			warmup = atoi(optarg);
			break;
		case 'N':
			repetitions = atoi(optarg);
			break;
		case 'C':
			state.clone = 1;
			break;
		case 'D':
			state.drop_cache = 1;
			break;
		case 'S':
			state.page_permutation = 0;
			break;
		case 'V':
			state.verbose = 1;
			break;
		default:
			lmbench_usage(ac, av, usage);
			break;
		}
	}
	if (optind != ac - 1 ) {
		lmbench_usage(ac, av, usage);
	}
	
	state.file = av[optind];
	CHK(stat(state.file, &st));
	state.npages = st.st_size / (size_t)getpagesize();

#ifdef	MS_INVALIDATE
	benchmp(initialize, benchmark_mmap, cleanup, 0, parallel, 
		warmup, repetitions, &state);
	t_mmap = gettime() / (double)get_n();

	benchmp(initialize, benchmark, cleanup, 0, parallel, 
		warmup, repetitions, &state);
	t_combined = gettime() / (double)get_n();
	settime(get_n() * (t_combined - t_mmap));

	sprintf(buf, "Pagefaults on %s", state.file);
	micro(buf, state.npages * get_n());

	if(state.fd_drop_cache > 0)
		close(state.fd_drop_cache);
	if(state.verbose)
		printf("iteration=%llu, tBench:%f, tmmap:%f\n", get_n(), t_combined-t_mmap, t_mmap);
#endif
	return(0);
}

void
initialize(iter_t iterations, void* cookie)
{
	int 		pagesize;
	struct stat 	sbuf;
	state_t 	*state = (state_t *) cookie;

	// it's an quick but ugly workaround (due to the framework of lib_timing.c)
	// to drop cache before each benchmark() so that drop_caches overhead
	// is not accumulated in final result.
	if(state->drop_cache && iterations)
	{
		drop_cache(state->fd_drop_cache);
		if(state->verbose)
			check_pageout(state);
	}

	if (iterations) return;

	if (state->clone) {
		char buf[128];
		char* s;

		/* copy original file into a process-specific one */
		sprintf(buf, "%d", (int)getpid());
		s = (char*)malloc(strlen(state->file) + strlen(buf) + 1);
		if (!s) {
			perror("malloc");
			exit(1);
		}
		sprintf(s, "%s%d", state->file, (int)getpid());
		if (cp(state->file, s, S_IREAD|S_IWRITE) < 0) {
			perror("Could not copy file");
			unlink(s);
			exit(1);
		}
		state->file = s;
	}
	CHK(state->fd = open(state->file, 0));
	if (state->clone) unlink(state->file);
	CHK(fstat(state->fd, &sbuf));

	srand(getpid());
	pagesize = getpagesize();
	state->size = sbuf.st_size;
	state->size -= state->size % pagesize;
	state->npages = state->size / pagesize;
	if(state->page_permutation > 0)
		state->pages = permutation(state->npages, pagesize);
	else
		state->pages = serialize(state, pagesize);

	if (state->size < 1024*1024) {
		fprintf(stderr, "lat_pagefault: %s too small\n", state->file);
		exit(1);
	}
	state->where = mmap(0, state->size, 
			    PROT_READ, MAP_SHARED, state->fd, 0);

#ifdef	MS_INVALIDATE
	if (msync(state->where, state->size, MS_INVALIDATE) != 0) {
		perror("msync");
		exit(1);
	}

	CHK(state->fd_drop_cache = open("/proc/sys/vm/drop_caches", O_RDWR));
	drop_cache(state->fd_drop_cache);
#endif
}

void
cleanup(iter_t iterations, void* cookie)
{	
	state_t *state = (state_t *) cookie;

	if (iterations) return;

	munmap(state->where, state->size);
	if (state->fd >= 0) close(state->fd);
	free(state->pages);
}

// write "1" to /proc/sys/vm/drop_caches
void drop_cache(int fd)
{
	if(fd < 0)
		return;

	sync();
	write(fd, "1", 1);
}

size_t*
serialize(void *cookie, size_t scale)
{
	state_t *state = (state_t *) cookie;
	size_t max = state->npages;
	size_t	i, v, o;
	static size_t r = 0;
	size_t*	result = (size_t*)malloc(max * sizeof(size_t));

	if (result == NULL) return NULL;

	if(state->verbose)
		printf("serializing pages...\n");

	for (i = 0; i < max; ++i) {
		result[i] = i * scale;
	}

	return (result);
}

void
check_pageout(void *cookie)
{
	state_t *state = (state_t *) cookie;

	long length = state->size;
	long numPages = state->npages;
	unsigned int pageoutCount = 0;
	static long j = 0;
	unsigned char *vec = (unsigned char *) malloc(numPages);
	if(NULL == vec)
	{
		printf("malloc failed\n");
		return;
	}

	memset(vec, 0, numPages);

	if(mincore(state->where, length, vec) == -1)
		printf("Err: mincore() failed\n");
	else
	{
		int i = 0;
		for(i=0; i < state->npages; i++)
		{
		   if(0 == (vec[i] & 1))
			pageoutCount++;
		}
	}

	free(vec);
	printf("[%ld] %u of total %u pages was paged out (not in memory)\n",
		++j, pageoutCount, state->npages);
}

void
benchmark(iter_t iterations, void* cookie)
{
	int	i;
	int	sum = 0;
	state_t *state = (state_t *) cookie;

	while (iterations-- > 0) {
		for (i = 0; i < state->npages; ++i) {
			sum += *(state->where + state->pages[i]);
		}
		munmap(state->where, state->size);
		state->where = mmap(0, state->size, 
				    PROT_READ, MAP_SHARED, state->fd, 0);
#ifdef	MS_INVALIDATE
		if (msync(state->where, state->size, MS_INVALIDATE) != 0) {
			perror("msync");
			exit(1);
		}
#endif
	}
	use_int(sum);
}

void
benchmark_mmap(iter_t iterations, void* cookie)
{
	int	sum = 0;
	state_t *state = (state_t *) cookie;

	while (iterations-- > 0) {
		munmap(state->where, state->size);
		state->where = mmap(0, state->size, 
				    PROT_READ, MAP_SHARED, state->fd, 0);
#ifdef	MS_INVALIDATE
		if (msync(state->where, state->size, MS_INVALIDATE) != 0) {
			perror("msync");
			exit(1);
		}
#endif
	}
	use_int(sum);
}

