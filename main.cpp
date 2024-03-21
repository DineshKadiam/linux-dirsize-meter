#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <getopt.h>
#include <stdint.h>
#include <string.h>

#include <queue>
#include <mutex>
#include <string>
#include <condition_variable>
#include <thread>

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
	bool color;
	bool fullPath;
	bool dedupHardLinks;
	bool fileLength;
	bool humanReadable;
};

static struct Config C {
	.indentSize = 2,
	.rows = 20, // FIXME
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
};


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

int countSlashes(const std::string &str) {
	int slashes = 0;
	for (size_t i = 0; i < str.length(); i++) {
		slashes += str[i] == '/';
	}
	return slashes;
}
bool entryComparator(const struct entry *a, const struct entry *b) {
	if (a->size == b->size) {
		// the "bigger" entry is the one with the shorter path
		int slashes = countSlashes(a->name) - countSlashes(b->name);
		if (slashes == 0) {
			// Figure something out
			return false;
		}
		return slashes < 0;
	}
	return a->size > b->size;
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

size_t calcBufSize(struct entry *root, int depth, int size, struct entry **arr) {
	size_t bufSize = 0;
	for (int i = 0; i < size; i++) {
		if (arr[i]->parent == root) {
			bufSize += calcBufSize(arr[i], depth + 1, size, arr);
		}
	}
	bufSize += root->name.size();
	bufSize += 1; // newline
	bufSize += 5; // Size
	bufSize += depth * C.indentSize + 1; // FIXME indentation customization
	bufSize += 6 + 4 * 3 + 1; // FIXME color: ESC[38;2 + ;255 * 4 + m
	bufSize += 4; // Color reset \e[0m
	return bufSize;
}

void insertSorted(struct entry *thing, int size, struct entry **arr) {
	int i = 0;
	// Find where it goes
	while (i < size && thing->size <= arr[i]->size) i++;
	// shift everything down
	while (i < size) {
		struct entry *tmp = thing;
		thing = arr[i];
		arr[i] = tmp;
		i++;
	}
	// last element
	arr[i] = thing;
}

size_t
renderDisplay(struct entry *root, int idx, int depth, int size, struct entry **arr, char *buf) {
	// Find children, remove them from the array and sort them
	char *oldBuf = buf;
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
	for (j = childs - 1; j >= 0; j--) {
		buf += renderDisplay(children[j], j, depth + 1, size, arr, buf);
	}

	// render self
	// TODO print size
	while (depth > 1) {
		*buf++ = '|';
		for (int i = 1; i < C.indentSize; i++) {
			*buf++ = ' ';
		}
		depth--;
	}
	*buf = '\0';

	if (depth == 1) {
		if (idx == 0) {
			strcpy(buf, "/- ");
		} else {
			strcpy(buf, "|- ");
		}
		buf += 3;
	}

	const char *filename = root->name.c_str();
	if (!C.fullPath) {
		// Find out where the last '/' is in the filename
		const char *tmp = strrchr(filename, '/');
		if (tmp != NULL) {
			filename = tmp + 1;
		}
	}
	int filenameLen = strlen(filename);
	memcpy(buf, filename, filenameLen);
	buf += filenameLen;
	*buf++ = '\n';
	*buf = '\0';

	free(children);
	return buf - oldBuf;
}

int main(int argc, char* argv[]) {
	// Default options
	C.color = isatty(STDOUT_FILENO);
	char* no_color = getenv("NO_COLOR");
	if (no_color != NULL && *no_color != '\0') {
		C.color = false;
	}

	// Parse arguments
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
			case 'b':
				C.fileLength = true;
				break;
			case 's':
				C.humanReadable = false;
				break;
			case 'i':
				// indentation level
				break;
			case 'n':
				// number of rows
				break;
			case 'h':
			case '?':
				fprintf(stderr, "%s", USAGE);
				return 1;
		}
	}

	struct entry root = {
		"total",
		0,
		0,
		nullptr,
	};

	std::queue<entry*> toStat;
	// Gather search paths
	// TODO: Error message improvement
	if (optind == argc) {
		root.name = ".";
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

	// Calculate size of buffer needed
	size_t bufSize = calcBufSize(&root, 0, size, final_entries);
	char *displayBuf = (char*)malloc(bufSize);

	int written = renderDisplay(&root, 0, 0, size, final_entries, displayBuf);
	// TODO: replace fwrite with write(2) so no line buffering
	fwrite(displayBuf, 1, written, stdout);
	fflush(stdout);

	// Cleanup
	for (int i = 0; i < size; i++) {
		delete final_entries[i];
	}
	free(final_entries);
	free(displayBuf);

	return 0;
}

// TODO:
// - Display queue as tree
// - Option granularity
// - Human readable numbers
// - LS_COLORS
// - Custom lock-free stack instead of the locking mess
