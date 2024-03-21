extern "C" {
#include "hl.h"
}

#include <unordered_set>
#include <utility>
#include <mutex>

struct hl_hash {
	static std::size_t operator()(const std::pair<dev_t, ino_t> &hl) {
		return hl.first * 31 + hl.second;
	}
};

static std::unordered_set<std::pair<dev_t, ino_t>, hl_hash> table;
static std::mutex lock;

extern "C" bool hl_already_seen(void*, dev_t st_dev, ino_t st_ino) {
	std::lock_guard<std::mutex> l(lock);
	std::pair<dev_t, ino_t> key = {st_dev, st_ino};
	bool alreadySeen = table.find(key) != table.end();
	if (!alreadySeen) {
		table.insert(key);
	}
	return alreadySeen;
}
