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

enum options {
	NOCOLOR,
};

struct option opts[] = {
	{"nocolor", no_argument, nullptr, NOCOLOR},
	{nullptr, 0, nullptr, 0},
};

const char* USAGE =
"Usage: du [OPTIONS]... [path]...\n"
"Default search path is '.'\n"
"Options:\n"
"\t--nocolor: disable colorized output\n";

struct Config {
	bool nocolor;
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
			//fprintf(stderr, "%s: %i\n", node->name.c_str(), node->size);

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
			//fprintf(stderr, "%s\n", node->name.c_str());

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
					//fprintf(stderr, "%s\n", tmpNodes[i]->name.c_str());
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
	const int kQueueSize = 50;
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
			if (q.size() > kQueueSize) {
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

int main(int argc, char* argv[]) {
	// Default options
	struct Config config {
		.nocolor = false,
	};
	while (true) {
		int c = getopt_long(argc, argv, "", opts, nullptr);
		if (c == -1)
			break;

		switch (c) {
			case NOCOLOR:
				// Set some config option
				config.nocolor = true;
				break;
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

	// Print out the queue
	while (!q.empty()) {
		struct entry *node = q.top();
		q.pop();
		printf("%s: %li\n", node->name.c_str(), node->size);
		delete node;
	}

	return 0;
}

// TODO:
// - Display queue as tree
// - Option granularity
// - Human readable numbers
// - LS_COLORS
