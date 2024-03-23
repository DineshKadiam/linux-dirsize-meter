#define _XOPEN_SOURCE 500
#define _GNU_SOURCE
#include <asm/termbits.h>
#include <ctype.h>
#include <dirent.h>
#include <emmintrin.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <semaphore.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <unistd.h>

//----------------------------------------------------------
// Constants, globals, and structures
//----------------------------------------------------------

#define R "\033[m"
#define B "\033[1m"
#define U "\033[4m"
#define L "\033[2m"
#define OPT(flag, desc) B "\t-" flag R ": " desc "\n"
#define OPTa(flag, desc) B "\t-" flag R ": \033[2m<n>" R ": " desc "\n"

const char USAGE[] =
"A disk usage summarizer\n"
"\n"
B U "Usage:" R " dut [OPTIONS]... [path]...\n"
"Default search path is '.'\n"
"\n"
B U "Useful environment variables:\n" R
OPT("NO_COLOR", "set to any non-empty value to disable colorized output")
"\n"
B U "Options:\n" R
OPT("c", "disable colorized output")
OPT("C", "enable colorized output")
OPT("l", "count every instance of a hard link")
OPT("s", "use size of files instead of size on disk")
OPT("b", "show sizes in plain bytes intead of human readable format")
OPT("p", "display full path of files in the tree")
OPTa("d", "depth limit to show in output")
OPTa("i", "number of spaces for each level of indentation (default 2)")
OPTa("n", "number of rows to show (default is terminal size - 10)")
OPTa("t", "number of threads to use")
OPT("h", "show this help message");

#undef L
#undef R
#undef B
#undef U
#undef OPT
#undef OPTa

struct Config {
	int indentSize;
	int rows;
	int maxDepth;
	int threads;
	bool color : 1;
	bool dedupHardLinks : 1;
	bool fileLength : 1;
	bool humanReadable : 1;
	bool fullPath : 1;
};

static struct Config C = {
	.indentSize = 2,
	.rows = 20,
	.maxDepth = 10,
	.threads = 4,
	.color = true,
	.dedupHardLinks = true,
	.fileLength = false,
	.humanReadable = true,
	.fullPath = false,
};

// All the data about individual files and directories is in this struct
struct entry {
	_Atomic uint64_t unsearched_children;
	_Atomic uint64_t size;
	struct entry *parent;
	struct entry *next; // For use in stacks
	uint64_t depth;
	mode_t st_mode;
	size_t nameLen; // Does NOT include null terminator!
	char name[];
};

//----------------------------------------------------------
// Utility functions
//----------------------------------------------------------

static void printUsage() {
	if (!C.color) {
		// Strip out all escapes
		char buf[2048];
		uintptr_t r = 0;
		uintptr_t w = 0;
		while (r < sizeof(USAGE) - 1) {
			while (USAGE[r] == '\033') {
				while (!isalpha(USAGE[r]))
					r++;
				r++;
			}
			buf[w++] = USAGE[r++];
		}
		buf[w] = '\0';
		fprintf(stderr, "%s", buf);
	} else {
		fprintf(stderr, "%s", USAGE);
	}
}

static char *displayBuf = NULL;
// Size of stored data (there is no '\0' at the end)
static size_t displayPos = 0;
// Write the string to the output buffer, reallocating if necessary
static void wr(const char *str) {
	// Size of allocated buffer
	static size_t size = 0;

	size_t len = strlen(str);
	if (displayPos + len > size) {
		if (size == 0) size = 1024;
		while (displayPos + len > size) size *= 2;
		displayBuf = (char*)realloc(displayBuf, size);
	}
	memcpy(displayBuf + displayPos, str, len);
	displayPos += len;
}

// Parse an int of up to 4 digits, printing an error if necessary
static bool parseInt(const char *input, int *output, const char *paramName) {
	int len = strlen(input);
	if (len >= 4) {
		fprintf(stderr, "Error: specified %s is too large\n", paramName);
		return false;
	}

	int res = 0;
	while (*input != '\0') {
		if (*input < '0' || *input > '9') {
			fprintf(stderr, "Error: specified %s is not an integer\n", paramName);
			return false;
		}
		res *= 10;
		res += *input++ - '0';
	}

	*output = res;
	return true;
}

// Insert entry into sorted array
static void insertSorted(struct entry *thing, int size, struct entry **arr) {
	int i = size;
	// Set i to where thing needs to go
	while (i > 0 && thing->size < arr[i-1]->size) i--;
	// shift everything up
	while (i < size) {
		struct entry *tmp = thing;
		thing = arr[i];
		arr[i] = tmp;
		i++;
	}
	// last element
	arr[i] = thing;
}

static bool entryComparator(const struct entry *a, const struct entry *b) {
	if (a->size == b->size) {
		// the "bigger" entry is the one with the smaller depth
		return a->depth < b->depth;
	}
	return a->size > b->size;
}

//----------------------------------------------------------
// Hard link table
//----------------------------------------------------------

struct hard_link {
	dev_t st_dev;
	ino_t st_ino;
	bool slotFilled;
};

struct hl_table {
	// Open address table with quadratic probing. Max load factor = 2
	mtx_t lock;
	struct hard_link *table;
	uint64_t size;
	uint64_t count;
};

// Lock must be held. Returns true iff hard link already in hash table
static bool hl_insert(struct hl_table *hl, dev_t st_dev, ino_t st_ino) {
	uint64_t idx = st_ino & (hl->size-1);
	uint64_t step = 0;
	while (true) {
		idx += step;
		step++;
		// Truncate
		idx &= hl->size - 1;
		if (!hl->table[idx].slotFilled) {
			hl->table[idx].slotFilled = true;
			hl->table[idx].st_dev = st_dev;
			hl->table[idx].st_ino = st_ino;
			return false;
		} else if (hl->table[idx].st_dev == st_dev && hl->table[idx].st_ino == st_ino) {
			return true;
		}
	}
}

// External function. Checks if this link has already been seen, and records it
static bool hl_already_seen(struct hl_table *hl, dev_t st_dev, ino_t st_ino) {
	mtx_lock(&hl->lock);
	if (hl->count + 1 >= hl->size / 2) {
		fprintf(stderr, "reallocating hash map\n");
		// Reallocate
		uint64_t oldSize = hl->size;
		struct hard_link *oldTable = hl->table;
		hl->size *= 2;
		if (hl->size < 512) hl->size = 512;
		hl->table = calloc(sizeof(struct hard_link), hl->size);
		if (oldTable != NULL) {
			for (uint64_t i = 0; i < oldSize; i++) {
				if (oldTable[i].slotFilled) {
					hl_insert(hl, oldTable[i].st_dev, oldTable[i].st_ino);
				}
			}
			free(oldTable);
		}
	}

	bool alreadyExist = hl_insert(hl, st_dev, st_ino);
	if (!alreadyExist) hl->count++;
	mtx_unlock(&hl->lock);
	return alreadyExist;
}

//----------------------------------------------------------
// Atomic-ish stack
//----------------------------------------------------------

struct stack {
	struct entry *_Atomic head;
	uint64_t padding[8];
	atomic_flag lock;
	uint64_t morePadding[8];
};

static struct entry *stack_pop(struct stack *s) {
	if (atomic_flag_test_and_set_explicit(&s->lock, memory_order_acquire))
		return NULL;

	struct entry *newHead;
	struct entry *currHead = atomic_load(&s->head);
	do {
		if (currHead == NULL) goto end;
		newHead = currHead->next;
	} while (!atomic_compare_exchange_weak(&s->head, &currHead, newHead));
end:
	atomic_flag_clear_explicit(&s->lock, memory_order_release);
	return currHead;
}

static void stack_push_many(
		struct stack *s,
		struct entry *node,
		struct entry **last_next,
		int num,
		_Atomic int *num_blocked_threads,
		sem_t *sema) {
	*last_next = atomic_load(&s->head);
	while (!atomic_compare_exchange_weak(&s->head, last_next, node));

	int blocked_threads = atomic_load(num_blocked_threads);
	if (blocked_threads < num) num = blocked_threads;
	for (int i = 0; i < num; i++) {
		sem_post(sema);
	}
}

static bool stack_is_empty(struct stack *s) {
	return atomic_load_explicit(&s->head, memory_order_relaxed) == NULL;
}

//----------------------------------------------------------
// Entry heap
//----------------------------------------------------------

// Heap property: Every node in the tree is smaller than either of its children
//                Tree is as broad and shallow as possible
static void siftDown(struct entry **heap, size_t size, int idx) {
	size_t l;
	while ((l = idx * 2 + 1) < size) {
		size_t r = l + 1;
		size_t smallestChild = l;
		if (r < size && entryComparator(heap[l], heap[r])) {
			smallestChild = r;
		}
		if (entryComparator(heap[idx], heap[smallestChild])) {
			struct entry *tmp = heap[idx];
			heap[idx] = heap[smallestChild];
			heap[smallestChild] = tmp;
			idx = smallestChild;
		} else {
			return;
		}
	}
}

static void pushHeap(struct entry **heap, size_t *size, struct entry *node) {
	if (node->depth > (unsigned)C.maxDepth) {
		free(node);
		return;
	}
	if (*size <= (unsigned)C.rows - 1) {
		// Don't have enough elements to fill the heap yet
		heap[*size] = node;
		*size += 1;
		if (*size != (unsigned)C.rows) return;

		// Build heap algorithm. See wikipedia.
		for (int root = *size / 2 - 1; root >= 0; root--) {
			siftDown(heap, *size, root);
		}
	} else {
		// Don't bother sifting down if node < heap[0]
		if (entryComparator(node, heap[0])) {
			free(heap[0]);
			heap[0] = node;
			siftDown(heap, *size, 0);
		} else {
			free(node);
		}
	}
}

//----------------------------------------------------------
// Traversal code
//----------------------------------------------------------

// All these queues hold the entry of the file. toStat and toList will
// have blank size and unsearched_children fields
struct thread_data {
	alignas(64) _Atomic int num_blocked_threads;
	alignas(64) _Atomic bool encountered_errors;
	alignas(64) sem_t worker_sem;
	alignas(64) struct stack toList;
	alignas(64) struct hl_table *hl;
};

// Data specific to each thread
struct thread_locals {
	struct thread_data *T;
	// Scratch buffer to be used like an arena
	void *scr;
	size_t scr_size;
	struct entry **heap;
	size_t heap_filled;
};

// Sends this node to main, and updates parents sending them to main as well if
// necessary
static void finishNode(struct thread_locals *L, struct entry *node) {
	struct entry *tmp = node;
	node = node->parent;
	uint64_t prev_size = tmp->size;
	pushHeap(L->heap, &L->heap_filled, tmp);
	// Build a list of all nodes to send to main
	while (node != NULL) {
		atomic_fetch_add(&node->size, prev_size);
		int prev = atomic_fetch_sub(&node->unsearched_children, 1);
		if (prev != 1)
			break;
		prev_size = atomic_load_explicit(&node->size, memory_order_relaxed);
		tmp = node;
		node = node->parent;
		pushHeap(L->heap, &L->heap_filled, tmp);
	}
}

static bool statNode(int at_fd, struct stat *sbuf, struct entry *ent, struct hl_table *hl) {
	const char *name = ent->name;
	if (at_fd != AT_FDCWD)
		name = strrchr(name, '/') + 1;
	int result = fstatat(at_fd, name, sbuf, AT_SYMLINK_NOFOLLOW);
	if (result == -1)
		return false;

	ent->st_mode = sbuf->st_mode;
	if (C.fileLength) {
		ent->size = sbuf->st_size;
	} else {
		ent->size = 512 * sbuf->st_blocks;
	}

	if (!S_ISDIR(sbuf->st_mode)) {
		// Kind of rudimentary. Possible improvement: don't send to toMain at all
		if (C.dedupHardLinks && sbuf->st_nlink > 1 && hl_already_seen(hl, sbuf->st_dev, sbuf->st_ino)) {
			ent->size = 0;
		}
	}
	return true;
}

// From getdents(2)
struct linux_dirent64 {
	ino64_t        d_ino;    // 64-bit inode number
	off64_t        d_off;    // 64-bit offset to next structure
	unsigned short d_reclen; // Size of this dirent
	unsigned char  d_type;   // File type
	char           d_name[]; // Filename (null-terminated)
};

// node MUST be a directory, and size needs to be already filled out
static void doLs(struct thread_locals *const L, struct entry *const node) {
	struct thread_data *const T = L->T;
	int fd = open(node->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd == -1) {
		atomic_store_explicit(&T->encountered_errors, true, memory_order_relaxed);
		finishNode(L, node);
		return;
	}

	// Read entire directory stream into scratch
	size_t dents_len = 0;
	while (true) {
		void *wr = (void*)((char*)L->scr + dents_len);
		ssize_t result = getdents64(fd, wr, L->scr_size - dents_len);
		if (result == -1) {
			if (errno == EINVAL) {
				L->scr_size *= 2;
				L->scr = realloc(L->scr, L->scr_size);
				continue;
			}
			atomic_store_explicit(&T->encountered_errors, true, memory_order_relaxed);
			finishNode(L, node);
			return;
		} else if (result == 0) {
			break;
		}

		dents_len += result;
	}
			
	// Process each entry one at a time, only allocating a struct for dirs or large files
	struct entry *dirs = NULL;
	struct entry **dirsLastPtr = &dirs;
	uint64_t children = 0;
	size_t pos = 0;
	while (pos < dents_len) {
		struct stat sbuf;
		const struct linux_dirent64 *dent = (struct linux_dirent64*)((char*)L->scr + pos);
		pos += dent->d_reclen;

		int result = fstatat(fd, dent->d_name, &sbuf, AT_SYMLINK_NOFOLLOW);
		if (result == -1) {
			atomic_store_explicit(&T->encountered_errors, true, memory_order_relaxed);
			continue;
		}
		if (strcmp(dent->d_name, ".") == 0
				|| strcmp(dent->d_name, "..") == 0) {
			continue;
		}

		// TODO: Hard link deduplication
		// Figure out if big enough to make struct for or if dir
		uint64_t size;
		if (C.fileLength) {
			size = sbuf.st_size;
		} else {
			size = 512 * sbuf.st_blocks;
		}

		if (!S_ISDIR(sbuf.st_mode)) {
			node->size += size; // optimize?
			if (L->heap_filled == (unsigned)C.rows) {
				const struct entry *s = L->heap[0];
				off64_t s_size = (off64_t)s->size;
				if (s_size > sbuf.st_size || (s_size == sbuf.st_size && s->depth <= node->depth+1))
					continue;
			}
		}

		const size_t fileLen = strlen(dent->d_name);
		struct entry *tmp = (struct entry*)calloc(1, sizeof(struct entry) + 2 + fileLen + node->nameLen);

		if (node->name[0] == '/' && node->name[1] == '\0') {
			tmp->name[0] = '/';
			tmp->nameLen = 1;
		} else {
			tmp->nameLen = node->nameLen;
			memcpy(tmp->name, node->name, node->nameLen);
			tmp->name[tmp->nameLen++] = '/';
		}
		memcpy(tmp->name + tmp->nameLen, dent->d_name, fileLen);
		tmp->nameLen += fileLen;
		tmp->depth = node->depth + 1;
		tmp->parent = node;
		tmp->st_mode = sbuf.st_mode;
		tmp->size = size;

		if (S_ISDIR(sbuf.st_mode)) {
			*dirsLastPtr = tmp;
			dirsLastPtr = &tmp->next;
			children++;
		} else {
			pushHeap(L->heap, &L->heap_filled, tmp);
		}
	}

	close(fd);

	// deal with output
	atomic_store(&node->unsearched_children, children);
	if (dirs != NULL) {
		stack_push_many(&T->toList, dirs, dirsLastPtr, children, &T->num_blocked_threads, &T->worker_sem);
	} else {
		finishNode(L, node);
	}
	return;
}

static int thread_func(void *data) {
	// Stopping condition: work queues are empty and all other threads are blocked
	struct thread_locals *const L = (struct thread_locals*)data;
	struct thread_data *const T = L->T;
	const size_t init_scr_size = 512 * 1024;
	L->scr = malloc(init_scr_size);
	L->scr_size = init_scr_size;
	L->heap = (struct entry**)malloc(sizeof(struct entry *) * C.rows);
	L->heap_filled = 0;

	while (true) {
		struct entry *node;
		if (NULL != (node = stack_pop(&T->toList))) {
			doLs(L, node);
			continue;
		}

		int oldBlocked = atomic_fetch_add(&T->num_blocked_threads, 1);
		if (!stack_is_empty(&T->toList)) {
			atomic_fetch_sub(&T->num_blocked_threads, 1);
			continue;
		}

		if (oldBlocked == C.threads - 1) {
			// We're the last thread left, so we need to wake everyone up to die
			for (int i = 0; i < C.threads; i++) {
				sem_post(&T->worker_sem);
			}
			break;
		}
		sem_wait(&T->worker_sem);
		if (atomic_load(&T->num_blocked_threads) == C.threads) {
			break;
		}
		atomic_fetch_sub(&T->num_blocked_threads, 1);
	}

	free(L->scr);
	return 0;
}

static struct entry **
traverseDirs(struct entry *dirs, struct entry *files, int *res_size, struct hl_table *hl) {
	struct thread_data T = {
		.toList = {.head = dirs, .lock = ATOMIC_FLAG_INIT},
		.hl = hl,
		.encountered_errors = false,
		.num_blocked_threads = 0,
	};
	sem_init(&T.worker_sem, 0, 0);

	// Spawn I/O threads
	struct thread_locals *locals = (struct thread_locals*)malloc(sizeof(struct thread_locals) * C.threads);
	thrd_t *threads = malloc(sizeof(thrd_t) * (C.threads - 1));
	for (int i = 0; i < C.threads - 1; i++) {
		locals[i].T = &T;
		if (thrd_success != thrd_create(&threads[i], thread_func, &locals[i])) {
			fprintf(stderr, "Fatal error: unable to create thread\n");
			abort();
		}
	}

	// Also run thread_func with main thread
	struct thread_locals *L = &locals[C.threads-1];
	L->T = &T;
	thread_func(L);

	// Merge all the heaps from the various threads into this one
	struct entry **heap = L->heap;
	size_t heap_filled = L->heap_filled;
	for (int i = 0; i < C.threads - 1; i++) {
		for (size_t j = 0; j < locals[i].heap_filled; j++) {
			pushHeap(heap, &heap_filled, locals[i].heap[j]);
		}
		free(locals[i].heap);
	}

	while (files != NULL) {
		struct entry *tmp = files;
		files = files->next;
		pushHeap(heap, &heap_filled, tmp);
	}

	for (int i = 0; i < C.threads - 1; i++) {
		// Errors here aren't fatal, so ignore
		thrd_join(threads[i], NULL);
	}

	if (T.encountered_errors) {
		fprintf(stderr, "Encountered errors while searching directories. Reported sizes may be innaccurate.\n");
	}

	free(threads);
	free(locals);
	sem_destroy(&T.worker_sem);
	*res_size = heap_filled;
	return heap;
}

//----------------------------------------------------------
// LS_COLORS support
//----------------------------------------------------------

struct ext_color {
	char *ext;
	int extLen;
	char *color;
};

enum {
	DIR_OTH_WR,
	DIRECTORY,
	EXEC,
	FILE_DEFAULT,
	NUM_SPECIAL,
};

static struct {
	char *env;
	uint64_t num_exts;
	struct ext_color *exts;
	const char *special[NUM_SPECIAL];
} color_g = {
	NULL,
	0,
	NULL,
	// Defaults from ls.c in coreutils, line 616
	{"34;42", "01;34", "01;32", NULL},
};

static void freeLsColors(void) {
	free(color_g.env);
	free(color_g.exts);
}

static void initLsColors(void) {
	char *tmp = getenv("LS_COLORS");
	if (tmp == NULL || *tmp == '\0') {
		C.color = false;
		return;
	}

	color_g.env = strdup(tmp);
	int entries = 0;
	for (int i = 0; color_g.env[i] != '\0'; i++) {
		if (color_g.env[i] == ':') {
			entries++;
		}
	}

	color_g.exts = (struct ext_color*)malloc(entries * sizeof(struct ext_color));

	// Fill out all the exts structs. In the process, add null terminators
	// to the end of the color strings so wr() works with them.
	tmp = color_g.env;
	int i = 0;
	int j = 0;
	// FIXME ugly mess
	const char *special[] = {"ow", "di", "ex", "fi"};
	enum {EXT, SPECIAL, CONT, BEGIN} state = BEGIN;
	for (char *tmp = color_g.env; *tmp != '\0'; tmp++) {
		if (*tmp == ':') {
			*tmp = '\0';
			state = BEGIN;
			continue;
		}

		switch (state) {
		case BEGIN:
			if (*tmp == '*') {
				state = EXT;
				color_g.exts[i].ext = tmp + 1;
			}
			else state = SPECIAL;
			break;
		case EXT:
			if (*tmp == '=') {
				state = CONT;
				color_g.exts[i].color = tmp + 1;
				color_g.exts[i].extLen = tmp - color_g.exts[i].ext;
				i++;
			}
			break;
		case SPECIAL:
			// 2nd character of match string
			for (j = 0; j < NUM_SPECIAL; j++) {
				if (tmp[-1] == special[j][0] && tmp[0] == special[j][1])
					break;
			}
			if (j == NUM_SPECIAL || tmp[1] != '=') {
				state = CONT;
				continue;
			}
			color_g.special[j] = tmp + 2;
			state = CONT;
			break;
		default:
			// Do nothing so that * doesn't start a new extension
			break;
		}
	}
	color_g.num_exts = i;
}

static const char *getColorStr(struct entry *node) {
	if (S_ISDIR(node->st_mode)) {
		if (node->st_mode & S_IWOTH) {
			return color_g.special[DIR_OTH_WR];
		} else {
			return color_g.special[DIRECTORY];
		}
	} else if (S_ISREG(node->st_mode)) {
		if (node->st_mode & (S_IXOTH | S_IXGRP | S_IXUSR)) {
			return color_g.special[EXEC];
		}
	}
	for (unsigned int i = 0; i < color_g.num_exts; i++) {
		size_t extLen = color_g.exts[i].extLen;
		const char *ext = color_g.exts[i].ext;
		if (node->nameLen >= extLen &&
		    0 == strncmp(node->name + node->nameLen - extLen, ext, extLen)) {
			return color_g.exts[i].color;
		}
	}

	if (S_ISREG(node->st_mode))
		return color_g.special[FILE_DEFAULT];
	return NULL;
}

// Write a file name, coloring if necessary
static void wrFileName(struct entry *node) {
	const char *filename = node->name;

	// Good source for ANSI escape codes:
	// https://en.wikipedia.org/wiki/ANSI_escape_code
	bool needToReset = false;
	if (C.color) {
		// Compare st_mode and file suffix with LS_COLORS
		const char *colStr = getColorStr(node);
		if (colStr != NULL && colStr[0] != '\0') {
			wr("\033[");
			wr(colStr);
			wr("m");
			needToReset = true;
		}
	}

	// Find out where the last '/' is in the filename
	if (!C.fullPath && !(filename[0] == '/' && filename[1] == '\0')) {
		const char *tmp = strrchr(filename, '/');
		if (tmp != NULL) {
			filename = tmp + 1;
		}
	}
	wr(filename);

	if (C.color && needToReset) {
		wr("\033[0m");
	}
}

//----------------------------------------------------------
// Rendering functions
//----------------------------------------------------------

static void renderHumanReadable(uint64_t size) {
	if (size == 0) {
		wr("   0B ");
		return;
	}

	static const char *suffixes = "BKMGTP";
	// Sizes greater than 1023P are unsupported
	static const uint64_t maxSize = (uint64_t)1024 * 1024 * 1024 * 1024 * 1024 * 1023;
	if (size > maxSize) {
		wr(" HUGE ");
		return;
	}

	// Get order of magnitude and reduce size
	size *= 10;
	const char *suffix = suffixes;
	while (size > 10240) {
		size /= 1024;
		suffix++;
	}

	// Render backwards to finalBuf
	char finalBuf[7] = {0};
	int idx = 5;
	finalBuf[idx--] = ' ';
	finalBuf[idx--] = *suffix;
	if (size < 100) {
		finalBuf[idx--] = size % 10 + '0';
		finalBuf[idx--] = '.';
	}
	size /= 10;

	// Integer part and padding
	while (idx >= 0) {
		finalBuf[idx--] = (size == 0) ? ' ' : (size % 10) + '0';
		size /= 10;
	}

	wr(finalBuf);
}

struct indent_info {
	// Idx is the index of current node in parent's level. 0 = first thing printed
	int idx;
	int childs;
};

static void
renderDisplay(struct entry *root, struct indent_info *stack, int depth, int size, struct entry **arr) {
	// Find children, remove them from the array and sort them
	stack[depth].childs = 0;
	for (int i = 0; i < size; i++) {
		if (arr[i]->parent == root) {
			stack[depth].childs++;
		}
	}
	struct entry **children = (struct entry**)malloc(sizeof(struct entry*) * stack[depth].childs);
	int j = 0;
	for (int i = 0; i < size; i++) {
		if (arr[i]->parent == root) {
			// Delete from arr?
			insertSorted(arr[i], j, children);
			j++;
		}
	}

	// render children
	for (stack[depth].idx = 0; stack[depth].idx < stack[depth].childs; stack[depth].idx++) {
		renderDisplay(children[stack[depth].idx], stack, depth + 1, size, arr);
	}

	// render self, starting with size
	if (!C.humanReadable) {
		// Pad out to 16 characters for size
		char sizeBuf[17] = {0};
		snprintf(sizeBuf, 17, "%15" PRIu64 " ", root->size);
		wr(sizeBuf);
	} else {
		renderHumanReadable(root->size);
	}

	// Indentation
	int i = 0;
	while (i <= depth - 2) {
		// Sparse tree. Only put a | if there's stuff after it
		if (stack[i].idx == 0) {
			wr(" ");
		} else {
			wr("|");
		}
		for (int j = 1; j < C.indentSize; j++) {
			wr(" ");
		}
		i++;
	}

	// tree form
	if (depth > 0) {
		if (stack[depth-1].idx == stack[depth-1].childs - 1) {
			wr("/- ");
		} else {
			wr("|- ");
		}
	}

	wrFileName(root);
	wr("\n");

	free(children);
}

//----------------------------------------------------------
// Path deduplication
//----------------------------------------------------------

// This function normalizes a path by removing all extraneous . .. and / segments
static size_t normalizePath(char *const path) {
	const bool absolute = path[0] == '/';
	uintptr_t begin = 0;
	if (absolute) begin++;
	uintptr_t write = begin;
	uintptr_t read = begin;
	//inv:
	//read..end is remaining path
	//write..read is skipped chars
	//path..begin is / for absolute or ../../.. for relative
	//begin..write is relative path with no trailing /
	while (path[read] != '\0') {
		if (path[read] == '/') {
			read++;
			continue;
		}

		if (path[read] == '.' && (path[read+1] == '/' || path[read+1] == '\0')) {
			read++;
		} else if (path[read] == '.' && path[read+1] == '.'
				&& (path[read+2] == '/' || path[read+2] == '\0')) {
			// Handle .. - Go up a directory or add this path..begin segment
			read += 2;
			if (!absolute && begin == write) {
				if (begin != 0) path[begin++] = '/';
				path[begin++] = '.';
				path[begin++] = '.';
				write = begin;
			} else if (write > begin) {
				write--;
				while (write != begin && path[write] != '/')
					write--;
			}
		} else {
			if (write > 0 && path[write-1] != '/')
				path[write++] = '/';
			while (path[read] != '\0' && path[read] != '/')
				path[write++] = path[read++];
		}
	}
	if (write == 0) path[write++] = '.';
	path[write] = '\0';
	return write;
}

// Returns root node. dirs and files are output args for children of root and potentially root
static struct entry *
makeSearchList(int count, char *paths[], struct entry **dirs, struct entry **files, struct hl_table *hl) {
	struct entry *root;
	struct stat sbuf;
	if (count == 0) {
		root = (struct entry*)calloc(1, sizeof(struct entry) + 2);
		root->name[0] = '.';
		root->nameLen = 1;
		*dirs = root;
		statNode(AT_FDCWD, &sbuf, root, hl);
		return root;
	}

	// This is a special node to represent the grand total
	const char total[] = "(total)";
	root = (struct entry*)calloc(1, sizeof(struct entry) + sizeof(total));
	memcpy(root->name, total, sizeof(total));
	root->nameLen = sizeof(total) - 1;

	// Stat all inputs and get absolute path
	// If node is NULL then this candidate has been rejected
	struct candidate {
		struct entry *node;
		char *abs;
		size_t absLen;
	};
	struct candidate *const cands = (struct candidate*)malloc(sizeof(struct candidate) * count);
	for (int i = 0; i < count; i++) {
		const uintptr_t nameLen = strlen(paths[i]);
		cands[i].node = (struct entry*)calloc(1, sizeof(struct entry) + nameLen + 2);
		struct entry *n = cands[i].node;
		n->parent = root;
		strcpy(n->name, paths[i]);
		n->nameLen = normalizePath(n->name);

		// Fill out size-related members
		if (!statNode(AT_FDCWD, &sbuf, n, hl)) {
			fprintf(stderr, "Error resolving argument %s: %s\n", paths[i], strerror(errno));
			free(n);
			cands[i].node = NULL;
			continue;
		}
		cands[i].abs = realpath(n->name, NULL);
		if (cands[i].abs == NULL) {
			fprintf(stderr, "Invalid argument %s: %s\n", paths[i], strerror(errno));
			free(n);
			cands[i].node = NULL;
		} else {
			cands[i].absLen = strlen(cands[i].abs);
		}
	}

	// See if any argument encompasses others
	for (int i = 0; i < count; i++) {
		const struct candidate *const a = &cands[i];
		if (a->node == NULL || S_ISLNK(a->node->st_mode))
			continue;
		for (int j = 0; j < count; j++) {
			struct candidate *const b = &cands[j];
			if (i != j && b->node != NULL && !S_ISLNK(b->node->st_mode)
					&& a->absLen <= b->absLen
					&& strncmp(a->abs, b->abs, a->absLen) == 0) {
				char sep = b->abs[a->absLen];
				if (sep == '\0' || sep == '/') {
					// a is a prefix for b
					free(b->node);
					b->node = NULL;
				}
			}
		}
	}

	// Build the dirs and files lists
	for (int i = 0; i < count; i++) {
		if (cands[i].node != NULL) {
			if (S_ISDIR(cands[i].node->st_mode)) {
				cands[i].node->next = *dirs;
				*dirs = cands[i].node;
				root->unsearched_children++;
			} else {
				cands[i].node->next = *files;
				*files = cands[i].node;
			}
		}
		free(cands[i].abs);
	}
	free(cands);

	if (root->unsearched_children == 1 && *files == NULL) {
		free(root);
		(*dirs)->parent = NULL;
		return *dirs;
	}
	return root;
}

//----------------------------------------------------------
// Main and arg parsing
//----------------------------------------------------------

void fillOutDefaults(void) {
	bool isTty = isatty(STDOUT_FILENO);
	C.color = isTty;
	char* no_color = getenv("NO_COLOR");
	if (no_color != NULL && *no_color != '\0') {
		C.color = false;
	}

	// Default size to terminal height - 10
	if (isTty) {
		struct winsize ws;
		ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
		C.rows = ws.ws_row - 10;
		if (C.rows < 10) C.rows = 10;
	}

	// Linuxism
	const int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cpus > C.threads) {
		C.threads = num_cpus;
	}
}

int main(int argc, char* argv[]) {
	// This has to be done before any printing
	setvbuf(stdout, NULL, _IONBF, 0);

	// Parse arguments
	fillOutDefaults();
	bool argErrors = false;
	while (true) {
		int c = getopt(argc, argv, "cClbpsi:n:d:t:h");
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			C.color = false;
			break;
		case 'C':
			C.color = true;
			break;
		case 'l':
			C.dedupHardLinks = false;
			break;
		case 's':
			C.fileLength = true;
			break;
		case 'b':
			C.humanReadable = false;
			break;
		case 'p':
			C.fullPath = true;
			break;
		case 'i':
			// indentation level
			if (!parseInt(optarg, &C.indentSize, "indentation level")) {
				argErrors = true;
			}
			break;
		case 'n':
			// number of rows
			if (!parseInt(optarg, &C.rows, "number of rows")) {
				argErrors = true;
			}
			if (C.rows == 0) {
				fprintf(stderr, "Error: please specify at least 1 row of output\n");
				argErrors = true;
			}
			break;
		case 'd':
			// max depth
			if (!parseInt(optarg, &C.maxDepth, "max depth")) {
				argErrors = true;
			}
			break;
		case 't':
			if (!parseInt(optarg, &C.threads, "number of threads")) {
				argErrors = true;
			}
			if (C.threads == 0) {
				fprintf(stderr, "Error: at least one thread is required\n");
				argErrors = true;
			}
			break;
		case 'h':
			printUsage();
			return 0;
		case '?':
			argErrors = true;
		}
	}
	if (argErrors) {
		printUsage();
		return 1;
	}

	const int startTableSize = 131072;
	struct hl_table hl = {
		.table = calloc(sizeof(struct hard_link), startTableSize),
		.size = startTableSize,
		.count = 0,
	};
	mtx_init(&hl.lock, mtx_plain);
	struct entry *files = NULL;
	struct entry *dirs = NULL;
	struct entry *root = makeSearchList(argc - optind, &argv[optind], &dirs, &files, &hl);
	if (root == NULL) {
		return 1;
	}

	if (C.color) {
		initLsColors();
	}

	int size;
	struct entry **final_entries = traverseDirs(dirs, files, &size, &hl);
	mtx_destroy(&hl.lock);
	free(hl.table);

	if (size != 0) {
		struct indent_info *stack = (struct indent_info*)malloc(sizeof(struct indent_info) * (C.maxDepth+1));
		renderDisplay(root, stack, 0, size, final_entries);
		fwrite(displayBuf, 1, displayPos, stdout);
		fflush(stdout);
		free(stack);
	}

	// Cleanup
	for (int i = 0; i < size; i++) {
		free(final_entries[i]);
	}
	free(final_entries);
	free(displayBuf);
	freeLsColors();
	return 0;
}
