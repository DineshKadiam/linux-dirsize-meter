#include <asm/termbits.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <semaphore.h>
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

const char* USAGE =
"Usage: du [OPTIONS]... [path]...\n"
"Default search path is '.'\n"
"Useful environment variables:\n"
"\tNO_COLOR: set to any non-empty value to disable colorized output\n"
"Options:\n"
"\t-c: disable colorized output\n"
"\t-C: enable colorized output (default for tty)\n"
"\t-F: don't truncate file paths\n"
"\t-l: count every instance of a hard link\n"
"\t-s: use size of files instead of size on disk\n"
"\t-b: show sizes in plain bytes instead of human readable\n"
"\t-i <n>: number of spaces for each level of indentation (default 2)\n"
"\t-n <n>: number of rows to show (default is terminal size - 10)\n"
"\t-h: show this help message\n";

struct Config {
	int indentSize;
	int rows;
	bool color : 1;
	bool fullPath : 1;
	bool dedupHardLinks : 1;
	bool fileLength : 1;
	bool humanReadable : 1;
};

static struct Config C = {
	.indentSize = 2,
	.rows = 20,
	.color = true,
	.fullPath = false,
	.dedupHardLinks = true,
	.fileLength = false,
	.humanReadable = true,
};

// All the data about individual files and directories is in this struct
struct entry {
	_Atomic uint64_t unsearched_children;
	uint64_t size;
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

char *displayBuf = NULL;
// Size of stored data (there is no '\0' at the end)
size_t displayPos = 0;
// Write the string to the output buffer, reallocating if necessary
void wr(const char *str) {
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
bool parseInt(const char *input, int *output, const char *paramName) {
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
void insertSorted(struct entry *thing, int size, struct entry **arr) {
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

bool entryComparator(const struct entry *a, const struct entry *b) {
	if (a->size == b->size) {
		// the "bigger" entry is the one with the smaller depth
		return a->depth < b->depth;
	}
	return a->size > b->size;
}

//----------------------------------------------------------
// Traversal code
//----------------------------------------------------------
//
// Atomic stack ops:
// state: num_blocked_threads, semaphore
// push:
//	CAS loop to push on list
//	semaphore increment num_blocked_threads times
// pop:
// 	CAS loop to pop head off list
// 	if empty, increment num_blocked_threads
// 	loop:
// 		check head of list again, goto end if not empty
// 		wait on semaphore
// 		goto loop
// 	end:
// 		decr num_blocked_threads
// 		return
//

void
push_entry(struct entry *node, struct entry *_Atomic *stack, _Atomic int *blocked_threads, sem_t *sema) {
	struct entry *old = atomic_load(stack);
	do {
		node->next = old;
	} while (!atomic_compare_exchange_weak(stack, &old, node));

	int blockedCnt = atomic_load(blocked_threads);
	for (int i = 0; i < blockedCnt; i++) {
		sem_post(sema);
	}
}

// All these queues hold the entry of the file. toStat and toList will
// have blank size and unsearched_children fields
struct thread_data {
	struct entry *_Atomic toStat;
	struct entry *_Atomic toMain;
	struct entry *_Atomic toList;
	int num_threads;
	_Atomic int num_blocked_threads;
	_Atomic int main_blocked;
	sem_t worker_sem;
	sem_t main_sem;
};

// Sends this node to main, and updates parents sending them to statted if
// necessary
void finishNode(struct thread_data *T, struct entry *node) {
	uint64_t prev_size = node->size;
	struct entry *curr = node->parent;
	push_entry(node, &T->toMain, &T->main_blocked, &T->main_sem);
	while (curr != NULL) {
		int prev = atomic_fetch_sub(&curr->unsearched_children, 1);
		curr->size += prev_size;
		if (prev != 1)
			return;
		struct entry *tmp = curr;
		prev_size = tmp->size;
		curr = curr->parent;
		push_entry(tmp, &T->toMain, &T->main_blocked, &T->main_sem);
	}
}

void doStat(struct thread_data *T, struct entry *node) {
	struct stat sbuf;
	int result = lstat(node->name, &sbuf);

	if (result == -1) {
		// TODO: error handling
		finishNode(T, node);
		return;
	}

	node->size = 512 * sbuf.st_blocks;
	node->st_mode = sbuf.st_mode;
	if (S_ISDIR(sbuf.st_mode)) {
		push_entry(node, &T->toList, &T->num_blocked_threads, &T->worker_sem);
	} else {
		finishNode(T, node);
	}
}

void doLs(struct thread_data *T, struct entry *node) {
	DIR *dirp = opendir(node->name);
	if (dirp == NULL) {
		// TODO: error handling
		finishNode(T, node);
		fprintf(stderr, "error opening %s\n", node->name);
		return;
	}

	// In progress entries are stored as a linked list
	struct entry *head = NULL;
	struct entry **lastNextPtr = NULL;
	int count = 0;
	while (true) {
		errno = 0;
		struct dirent *dent = readdir(dirp);
		if (dent == NULL) {
			if (errno == 0) break;
			// else error TODO
			fprintf(stderr, "error reading %s\n", node->name);
			break;
		} else if (strcmp(dent->d_name, ".") == 0 ||
			   strcmp(dent->d_name, "..") == 0) {
			continue;
		}

		count++;
		int fileLen = strlen(dent->d_name);
		int newLen = fileLen + node->nameLen + 2;
		struct entry *tmp = (struct entry*)calloc(1, sizeof(struct entry) + newLen);

		memcpy(tmp->name, node->name, node->nameLen);
		tmp->name[node->nameLen] = '/';
		memcpy(tmp->name + node->nameLen + 1, dent->d_name, fileLen);
		tmp->nameLen = newLen - 1;

		tmp->depth = node->depth + 1;
		tmp->parent = node;
		if (head == NULL) {
			head = tmp;
		} else {
			*lastNextPtr = tmp;
		}
		lastNextPtr = &tmp->next;
	}

	closedir(dirp);
	if (head == NULL) {
		finishNode(T, node);
	} else {
		atomic_store(&node->unsearched_children, count);
		*lastNextPtr = atomic_load(&T->toStat);
		while (!atomic_compare_exchange_weak(&T->toStat, lastNextPtr, head));

		int numToWake = atomic_load(&T->num_blocked_threads);
		if (count - 1 < numToWake) numToWake = count - 1;
		for (int i = 0; i < numToWake; i++) {
			sem_post(&T->worker_sem);
		}
	}
}

int thread_func(void *data) {
	// Stopping condition: work queues are empty and all other threads are blocked
	struct thread_data *T = (struct thread_data*)data;

	while (true) {
		struct entry *node;
		if (NULL != (node = atomic_load(&T->toStat))) {
			if (atomic_compare_exchange_weak(&T->toStat, &node, node->next)) {
				doStat(T, node);
			}
		} else if (NULL != (node = atomic_load(&T->toList))) {
			if (atomic_compare_exchange_weak(&T->toList, &node, node->next)) {
				doLs(T, node);
			}
		} else {
			// Block on worker_sem or quit
			int oldBlocked = atomic_fetch_add(&T->num_blocked_threads, 1);
			if (atomic_load(&T->toStat) != NULL || atomic_load(&T->toList) != NULL) {
				atomic_fetch_sub(&T->num_blocked_threads, 1);
				continue;
			}

			if (oldBlocked == T->num_threads - 1) {
				sem_post(&T->main_sem);
				for (int i = 0; i < T->num_threads; i++) {
					sem_post(&T->worker_sem);
				}
				return 0;
			}
			sem_wait(&T->worker_sem);
			if (atomic_load(&T->num_blocked_threads) == T->num_threads)
				return 0;
			atomic_fetch_sub(&T->num_blocked_threads, 1);
		}
	}
}

void pushHeap(struct entry **heap, int *size, struct entry *node) {
	if (node == NULL) {
		fprintf(stderr, "err\n");
	}
	heap[*size] = node;
	*size += 1;
	if (*size == C.rows + 1) {
		int minIdx = 0;
		for (int i = 0; i < *size; i++) {
			if (entryComparator(heap[minIdx], heap[i])) {
				minIdx = i;
			}
		}
		free(heap[minIdx]);
		heap[minIdx] = heap[*size - 1];
		*size -= 1;
	}
}

struct entry **traverseDirs(struct entry *root, int *res_size) {
	struct thread_data T = {
		.toStat = root,
		.toList = NULL,
		.toMain = NULL,
		.num_threads = 100,
		.num_blocked_threads = 0,
		.main_blocked = 0,
	};
	sem_init(&T.main_sem, 0, 0);
	sem_init(&T.worker_sem, 0, 0);

	int queue_size = 0;
	struct entry **queue = (struct entry **)malloc(sizeof(struct entry*) * (C.rows + 1));

	// Spawn I/O threads
	thrd_t *threads = malloc(sizeof(thrd_t) * T.num_threads);
	for (int i = 0; i < T.num_threads; i++) {
		// TODO error handling
		thrd_create(&threads[i], thread_func, &T);
	}

	while (true) {
		struct entry *node = atomic_load(&T.toMain);
		if (node != NULL) {
			if (!atomic_compare_exchange_weak(&T.toMain, &node, node->next))
				continue;
			// Insert into priority "queue"
			pushHeap(queue, &queue_size, node);
		} else {
			atomic_store(&T.main_blocked, 1);
			int num_blocked = atomic_load(&T.num_blocked_threads);
			if (atomic_load(&T.toMain) == NULL) {
				if (num_blocked == T.num_threads)
					break;
				sem_wait(&T.main_sem);
			}
			atomic_store(&T.main_blocked, 0);
		}
	}

	for (int i = 0; i < T.num_threads; i++) {
		thrd_join(threads[i], NULL);
	}

	free(threads);
	sem_destroy(&T.main_sem);
	sem_destroy(&T.worker_sem);
	*res_size = queue_size;
	return queue;
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

void freeLsColors() {
	free(color_g.env);
	free(color_g.exts);
}

void initLsColors() {
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
			int j;
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

const char *getColorStr(struct entry *node) {
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
void wrFileName(struct entry *node) {
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

	if (!C.fullPath) {
		// Find out where the last '/' is in the filename
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

void renderHumanReadable(uint64_t size) {
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

void
renderDisplay(struct entry *root, int idx, int depth, int size, struct entry **arr) {
	// Find children, remove them from the array and sort them
	int childs = 0;
	for (int i = 0; i < size; i++) {
		if (arr[i]->parent == root) {
			childs++;
		}
	}
	struct entry **children = (struct entry**)malloc(sizeof(struct entry*) * childs);
	int j = 0;
	for (int i = 0; i < size; i++) {
		if (arr[i]->parent == root) {
			// Delete from arr?
			insertSorted(arr[i], j, children);
			j++;
		}
	}
	// render children
	for (j = 0; j < childs; j++) {
		renderDisplay(children[j], childs - j - 1, depth + 1, size, arr);
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
	while (depth > 1) {
		wr("|");
		for (int i = 1; i < C.indentSize; i++) {
			wr(" ");
		}
		depth--;
	}

	// "tree" form
	if (depth == 1) {
		if (idx == 0) {
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
// Venerable main
//----------------------------------------------------------

int main(int argc, char* argv[]) {
	// This has to be done before any printing
	setvbuf(stdout, NULL, _IONBF, 0);

	// Default options
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

	// Parse arguments
	bool argErrors = false;
	while (true) {
		int c = getopt(argc, argv, "cCFlbsi:n:h");
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			C.color = false;
			break;
		case 'C':
			C.color = true;
			break;
		case 'F':
			C.fullPath = true;
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
			break;
		case 'h':
			fprintf(stderr, "%s", USAGE);
			return 0;
		case '?':
			argErrors = true;
		}
	}
	if (argErrors) {
		fprintf(stderr, "%s", USAGE);
		return 1;
	}

	struct entry *root;
	// Gather search paths
	// TODO: Error message improvement
	if (optind == argc) {
		root = (struct entry*)calloc(1, sizeof(struct entry) + 2);
		root->name[0] = '.';
		root->name[1] = '\0';
		root->nameLen = 1;
	} else if (argc - optind == 1) {
		int len = strlen(argv[optind]);
		root = (struct entry*)calloc(1, sizeof(struct entry) + len + 1);
		root->nameLen = len;
		strcpy(root->name, argv[optind]);
	} else {
		fprintf(stderr, "Error: Only 1 search path is supported\n");
		fprintf(stderr, "%s", USAGE);
		return 1;
	}

	if (C.color) {
		initLsColors();
	}


	// Dump it into a plain array for processing
	// Make a tree out of the .parent pointers
	// Format it somehow
	int size;
	struct entry **final_entries = traverseDirs(root, &size);

	if (size != 0) {
		renderDisplay(root, 0, 0, size, final_entries);
		fwrite(displayBuf, 1, displayPos, stdout);
		fflush(stdout);
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

// TODO:
// - Better handling of multiple search directories
// - Convert to C :)
// - Hard link deduplication
// - Custom lock-free stack instead of the locking mess
// - Sparse tree
// - Colored help text
