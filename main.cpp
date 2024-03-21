#include <asm/termbits.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <getopt.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include <queue>
#include <mutex>
#include <string>
#include <condition_variable>
#include <thread>

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
	// TODO bitfields?
	bool color;
	bool fullPath;
	bool dedupHardLinks;
	bool fileLength;
	bool humanReadable;
};

static struct Config C {
	.indentSize = 2,
	.rows = 20,
	.color = true,
	.fullPath = false,
	.dedupHardLinks = true,
	.fileLength = false,
	.humanReadable = true,
};

struct entry {
	std::string name;
	uint64_t unsearched_children;
	uint64_t size;
	entry* parent;
	uint64_t depth;
	mode_t st_mode;
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

// All these queues hold the entry of the file. toStat and toList will
// have blank size and unsearched_children fields
struct thread_data {
	std::queue<entry*> toStat;
	std::queue<entry*> statted;
	std::queue<entry*> toList;
	std::mutex lock;
	int pendingReqs;
	std::condition_variable wakeThreads;
	std::condition_variable wakeMain; // needed
};

// Sends this node to statted, and updates parents sending them to statted if
// necessary
void finishNode(struct thread_data *T, struct entry *node) {
	T->statted.push(node);
	struct entry *curr = node->parent;
	T->wakeMain.notify_all();
	while (curr != nullptr) {
		curr->unsearched_children--;
		curr->size += node->size;
		if (curr->unsearched_children != 0)
			return;
		T->statted.push(curr);
		curr = curr->parent;
		node = node->parent;
	}
}

void thread_func(struct thread_data *T) {
	// Stopping condition: work queues are empty and no pending requests
	std::unique_lock ul(T->lock);
	std::vector<entry*> tmpNodes;

	while (true) {
		if (!T->toStat.empty()) {
			// stat(2) the file/directory
			// if directory, add to toList
			// if file, send to statted and decrement parent
			// if last file in parent to be statted, send it to statted too,
			// going up the tree
			struct entry *node = T->toStat.front();
			T->toStat.pop();
			struct stat sbuf;

			T->pendingReqs++;
			ul.unlock();
			int result = lstat(node->name.c_str(), &sbuf);
			ul.lock();
			T->pendingReqs--;

			if (result == -1) {
				// TODO: error handling
				//node->size = -errno;
				finishNode(T, node);
				continue;
			}

			node->size = 512 * sbuf.st_blocks;
			node->st_mode = sbuf.st_mode;
			if (S_ISDIR(sbuf.st_mode)) {
				T->toList.push(node);
				T->wakeThreads.notify_all();
			} else {
				finishNode(T, node);
			}

		} else if (!T->toList.empty()) {
			// readdir(3) the directory
			// make an entry for each child, and put in toStat
			// fill out unsearched_children
			// if directory is empty, decrement parent and send to statted
			struct entry *node = T->toList.front();
			T->pendingReqs++;
			T->toList.pop();
			ul.unlock();

			DIR *dirp = opendir(node->name.c_str());
			if (dirp == nullptr) {
				// TODO: error handling
				ul.lock();
				T->pendingReqs--;
				finishNode(T, node);
				fprintf(stderr, "error opening %s\n", node->name.c_str());
				continue;
			}
			
			while (true) {
				errno = 0;
				struct dirent *dent = readdir(dirp);
				if (dent == nullptr) {
					if (errno == 0) break;
					// else error TODO
					fprintf(stderr, "error reading %s\n", node->name.c_str());
					break;
				} else if (strcmp(dent->d_name, ".") == 0 ||
					   strcmp(dent->d_name, "..") == 0) {
					continue;
				}

				struct entry *tmp = new entry;
				tmp->size = 0;
				tmp->name = node->name;
				tmp->name += '/';
				tmp->name += dent->d_name;
				tmp->depth = node->depth + 1;
				tmp->parent = node;
				tmpNodes.push_back(tmp);
			}

			closedir(dirp);
			ul.lock();
			T->pendingReqs--;
			if (tmpNodes.empty()) {
				finishNode(T, node);
			} else {
				// add all nodes to toStat
				node->unsearched_children = tmpNodes.size();

				for (size_t i = 0; i < tmpNodes.size(); i++) {
					T->toStat.push(tmpNodes[i]);
				}
				tmpNodes.clear();
				T->wakeThreads.notify_all();
			}

		} else {
			if (T->pendingReqs == 0) {
				return;
			}

			T->wakeThreads.wait(ul);
		}
	}
}

// TODO: remove root param
std::priority_queue<entry*, std::vector<entry*>, decltype(&entryComparator)>
traverseDirs(std::queue<entry*> &&toSearch, struct entry *root) {
	struct thread_data T = {
		.toStat = std::move(toSearch)
	};

	// Spawn I/O threads
	const int kNumThreads = 10;
	std::vector<std::thread> threads;
	for (int i = 0; i < kNumThreads; i++) {
		threads.emplace_back(thread_func, &T);
	}

	// Queue for the finished entries
	std::priority_queue<entry*, std::vector<entry*>, decltype(&entryComparator)> q(entryComparator);

	std::unique_lock ul(T.lock);
	while (T.pendingReqs > 0 || !T.statted.empty() || !T.toList.empty() || !T.toStat.empty()) {
		if (T.statted.empty())
			T.wakeMain.wait(ul);
		while (!T.statted.empty()) {
			struct entry* node = T.statted.front();
			T.statted.pop();
			if (node == root) continue;
			q.push(node);
			if (q.size() > C.rows) {
				delete q.top();
				q.pop();
			}
		}
	}
	ul.unlock();
	
	T.wakeThreads.notify_all();
	for (size_t i = 0; i < threads.size(); i++) {
		threads[i].join();
	}
	return q;
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

// TODO: special file colors
char *colors = NULL;
struct ext_color {
	char *ext;
	int extLen;
	char *color;
};
struct ext_color *ext_entries = NULL;
int num_ext_entries = 0;

void parseLsColors() {
	char *tmp = getenv("LS_COLORS");
	if (tmp == NULL || *tmp == '\0')
		return;

	colors = strdup(tmp);
	int entries = 0;
	for (int i = 0; colors[i] != '\0'; i++) {
		if (colors[i] == ':') {
			entries++;
		}
	}

	ext_entries = (struct ext_color*)malloc(entries * sizeof(struct ext_color));

	// Fill out all the ext_entries structs. In the process, add null terminators
	// to the end of the color strings so wr() works with them.
	tmp = colors;
	int i = 0;
	// inv: tmp points to the first character after a colon
	// FIXME ugly mess
	while (*tmp != '\0') {
		if (*tmp == '*') {
			tmp++;
			if (*tmp == '\0') break;

			ext_entries[i].ext = tmp;
			while (*tmp != '\0' && *tmp != '=') tmp++;
			ext_entries[i].extLen = tmp - ext_entries[i].ext;

			if (*tmp == '\0') break;
			*tmp++ = '\0';
			if (*tmp == '\0') break;
			ext_entries[i].color = tmp;
			i++;
		}
		while (*tmp != '\0' && *tmp != ':') tmp++;
		if (*tmp == ':') {
			*tmp = '\0';
			tmp++;
		}
	}
	num_ext_entries = i;
}

// Write a file name, coloring if necessary
void wrFileName(struct entry *node) {
	const char *filename = node->name.c_str();
	const int filenameLen = node->name.size();

	// Good source for ANSI escape codes:
	// https://en.wikipedia.org/wiki/ANSI_escape_code
	bool needToReset = false;
	if (C.color) {
		// Compare st_mode and file suffix with LS_COLORS
		for (int i = 0; i < num_ext_entries; i++) {
			int extLen = ext_entries[i].extLen;
			const char *ext = ext_entries[i].ext;
			if (filenameLen >= extLen &&
			    0 == strncmp(filename + filenameLen - extLen, ext, extLen)) {
				wr("\033[");
				wr(ext_entries[i].color);
				wr("m");
				needToReset = true;
			}
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
	if (C.color) {
		parseLsColors();
	}

	struct entry root = {
		"total",
		0,
		0,
		nullptr,
		0,
		0,
	};

	std::queue<entry*> toStat;
	// Gather search paths
	// TODO: Error message improvement
	if (optind == argc) {
		root.name = ".";
		toStat.push(&root);
	} else if (argc - optind == 1) {
		root.name = argv[optind];
		toStat.push(&root);
	} else {
		while (optind < argc) {
			entry *node = new entry;
			node->name = argv[optind];
			node->parent = &root;
			toStat.push(node);
			optind++;
		}
	}

	std::priority_queue<entry*, std::vector<entry*>, decltype(&entryComparator)>
		q = traverseDirs(std::move(toStat), &root);

	// Dump it into a plain array for processing
	// Make a tree out of the .parent pointers
	// Format it somehow
	struct entry **final_entries = (struct entry**)malloc(sizeof(entry*) * q.size());
	int size = 0;
	while (!q.empty()) {
		final_entries[size] = q.top();
		q.pop();
		size++;
	}

	renderDisplay(&root, 0, 0, size, final_entries);
	fwrite(displayBuf, 1, displayPos, stdout);
	fflush(stdout);

	// Cleanup
	for (int i = 0; i < size; i++) {
		delete final_entries[i];
	}
	free(final_entries);
	free(displayBuf);
	free(colors);
	free(ext_entries);

	return 0;
}

// TODO:
// - LS_COLORS - special file colors
// - Hard link deduplication
// - Custom lock-free stack instead of the locking mess
// - Unicode tree
