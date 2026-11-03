// Batched binary shard output plus the manifest.
//
// Shard layout (all integers little-endian):
//   [magic "OCRB" 4B][version u32][num_records u32]
//   then per record:
//     [label_len u32][label UTF-8][img_h u32][img_w u32][img_channels u32][pixels u8]
//
// Records are written in record-index order regardless of which worker
// produced them, so a given (seed, config) always yields byte-identical shards.
#pragma once

#include "common/image.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ocr {

struct Record {
	uint64_t index = 0;
	std::string label;
	Image image;
};

class BoundedQueue {
public:
	explicit BoundedQueue( size_t capacity ) : capacity( capacity ) {}

	// Blocks while full. Returns false if the queue was closed.
	bool push( Record&& record );
	// Blocks until a record is available. Returns false once closed and empty.
	bool pop( Record& out );
	void close();

private:
	std::mutex mutex;
	std::condition_variable notEmpty;
	std::condition_variable notFull;
	std::deque<Record> items;
	size_t capacity;
	bool closed = false;
};

struct ShardInfo {
	std::string file;
	uint64_t records = 0;
	uint64_t bytes = 0;
	std::string sha256;
};

class ShardWriter {
public:
	ShardWriter( std::string outputDir, uint64_t shardSize, uint64_t seed, int targetWidth, int targetHeight,
		int channels, std::string charset );
	~ShardWriter();

	ShardWriter( const ShardWriter& ) = delete;
	ShardWriter& operator=( const ShardWriter& ) = delete;

	bool start( std::string* error );
	BoundedQueue& queue() { return recordQueue; }

	// Signals end of input, joins the writer thread and writes the manifest.
	bool finish( std::string* error );

	const std::vector<ShardInfo>& shards() const { return shardInfos; }
	uint64_t recordCount() const { return writtenRecords; }

private:
	void run();
	bool openShard( std::string* error );
	bool closeShard( std::string* error );
	bool writeRecord( const Record& record, std::string* error );
	bool writeManifest( std::string* error );

	std::string outputDir;
	uint64_t shardSize;
	uint64_t seed;
	int targetWidth;
	int targetHeight;
	int channels;
	std::string charset;

	BoundedQueue recordQueue;
	std::thread thread;
	std::mutex errorMutex;
	std::string firstError;

	// Reordering buffer, keyed by record index.
	std::map<uint64_t, Record> pending;
	uint64_t nextIndex = 0;
	uint64_t writtenRecords = 0;

	std::FILE* file = nullptr;
	uint64_t recordsInCurrentShard = 0;
	std::string currentShardPath;
	std::vector<ShardInfo> shardInfos;
	std::vector<std::string> shardFiles;
};

} // namespace ocr
