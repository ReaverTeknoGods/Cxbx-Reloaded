#include "NV2AConstDump.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace {

constexpr uint32_t kNV2AConstDumpMagic = 0x3143564Eu; // NVC1
constexpr uint32_t kNV2AConstDumpVersion = 1;

struct NV2AConstDumpFileHeader {
	uint32_t magic;
	uint32_t version;
};

struct NV2AConstDumpDiskRecord {
	uint32_t frame_number;
	uint32_t draw_number;
	uint32_t flags;
	uint32_t reserved;
	uint8_t dirty_mask[NV2A_VERTEXSHADER_CONSTANTS];
	uint32_t parsed_constants[NV2A_VERTEXSHADER_CONSTANTS * 4];
	uint32_t gold_constants[NV2A_VERTEXSHADER_CONSTANTS * 4];
};

std::mutex s_dumpMutex;
bool s_dumpStateInitialized = false;
bool s_dumpCaptureEnabled = false;
bool s_dumpFilePrepared = false;
std::filesystem::path s_dumpPath;

bool env_var_enabled(const char *name)
{
	const char *value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return false;
	}

	return !(value[0] == '0' && value[1] == '\0');
}

std::filesystem::path source_root_default_dump_path()
{
	std::filesystem::path source_path(__FILE__);
	return source_path.parent_path().parent_path().parent_path().parent_path() / "temp" / "nv2a_const_cases.bin";
}

void initialize_dump_state_locked()
{
	if (s_dumpStateInitialized) {
		return;
	}

	s_dumpStateInitialized = true;

	const char *override_path = std::getenv("CXBXR_NV2A_CONST_DUMP_PATH");
	const char *capture_env = std::getenv("CXBXR_NV2A_CONST_DUMP");
	if (override_path != nullptr && override_path[0] != '\0') {
		s_dumpPath = override_path;
	}
	else {
		s_dumpPath = source_root_default_dump_path();
	}

	// Default to capture-on for the current investigation so runs always emit
	// the verifier input file unless explicitly disabled with CXBXR_NV2A_CONST_DUMP=0.
	s_dumpCaptureEnabled = (capture_env == nullptr || capture_env[0] == '\0')
		? true
		: env_var_enabled("CXBXR_NV2A_CONST_DUMP");

	if (s_dumpCaptureEnabled) {
		std::error_code error;
		std::filesystem::remove(s_dumpPath, error);
	}
}

bool ensure_dump_file_header_locked(std::FILE *file)
{
	if (file == nullptr) {
		return false;
	}

	if (std::fseek(file, 0, SEEK_END) != 0) {
		return false;
	}

	long file_size = std::ftell(file);
	if (file_size < 0) {
		return false;
	}

	if (file_size == 0) {
		NV2AConstDumpFileHeader header = {
			kNV2AConstDumpMagic,
			kNV2AConstDumpVersion,
		};
		if (std::fwrite(&header, sizeof(header), 1, file) != 1) {
			return false;
		}
	}

	return std::fseek(file, 0, SEEK_END) == 0;
}

} // namespace

bool NV2AConstDumpCaptureEnabled()
{
	std::lock_guard<std::mutex> lock(s_dumpMutex);
	initialize_dump_state_locked();
	return s_dumpCaptureEnabled;
}

bool NV2AConstDumpAppend(const NV2AConstDumpRecord &record)
{
	std::lock_guard<std::mutex> lock(s_dumpMutex);
	initialize_dump_state_locked();
	if (!s_dumpCaptureEnabled) {
		return false;
	}

	std::error_code error;
	std::filesystem::create_directories(s_dumpPath.parent_path(), error);

	const char *mode = s_dumpFilePrepared ? "ab+" : "wb";
	std::FILE *file = std::fopen(s_dumpPath.string().c_str(), mode);
	if (file == nullptr) {
		return false;
	}

	bool ok = ensure_dump_file_header_locked(file);
	if (ok) {
		s_dumpFilePrepared = true;
		NV2AConstDumpDiskRecord disk_record = {};
		disk_record.frame_number = record.frame_number;
		disk_record.draw_number = record.draw_number;
		disk_record.flags = record.flags;
		std::memcpy(disk_record.dirty_mask, record.dirty_mask.data(), sizeof(disk_record.dirty_mask));
		std::memcpy(disk_record.parsed_constants, record.parsed_constants.data(), sizeof(disk_record.parsed_constants));
		std::memcpy(disk_record.gold_constants, record.gold_constants.data(), sizeof(disk_record.gold_constants));
		ok = std::fwrite(&disk_record, sizeof(disk_record), 1, file) == 1;
	}

	std::fclose(file);
	return ok;
}

std::string NV2AConstDumpDefaultPath()
{
	return source_root_default_dump_path().string();
}

bool NV2AConstDumpLoadFile(const std::string &path,
	std::vector<NV2AConstDumpRecord> *outRecords,
	std::string *outError)
{
	if (outRecords == nullptr) {
		if (outError != nullptr) {
			*outError = "output record vector is null";
		}
		return false;
	}

	outRecords->clear();

	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		if (outError != nullptr) {
			*outError = "unable to open dump file: " + path;
		}
		return false;
	}

	NV2AConstDumpFileHeader header = {};
	if (std::fread(&header, sizeof(header), 1, file) != 1) {
		if (outError != nullptr) {
			*outError = "dump file is missing a valid header: " + path;
		}
		std::fclose(file);
		return false;
	}

	if (header.magic != kNV2AConstDumpMagic || header.version != kNV2AConstDumpVersion) {
		if (outError != nullptr) {
			*outError = "unsupported dump file version: " + path;
		}
		std::fclose(file);
		return false;
	}

	for (;;) {
		NV2AConstDumpDiskRecord disk_record = {};
		size_t read_count = std::fread(&disk_record, sizeof(disk_record), 1, file);
		if (read_count != 1) {
			if (std::feof(file)) {
				break;
			}
			if (outError != nullptr) {
				*outError = "truncated dump record in: " + path;
			}
			std::fclose(file);
			outRecords->clear();
			return false;
		}

		NV2AConstDumpRecord record = {};
		record.frame_number = disk_record.frame_number;
		record.draw_number = disk_record.draw_number;
		record.flags = disk_record.flags;
		std::memcpy(record.dirty_mask.data(), disk_record.dirty_mask, sizeof(disk_record.dirty_mask));
		std::memcpy(record.parsed_constants.data(), disk_record.parsed_constants, sizeof(disk_record.parsed_constants));
		std::memcpy(record.gold_constants.data(), disk_record.gold_constants, sizeof(disk_record.gold_constants));
		outRecords->push_back(record);
	}

	std::fclose(file);
	return true;
}

bool NV2AConstDumpCurrentHeuristicWouldSkip(const NV2AConstDumpRecord &record)
{
	const uint32_t required = NV2A_CONST_DUMP_FLAG_PARSED_PENDING_CONSTANTS;
	const uint32_t blockers = NV2A_CONST_DUMP_FLAG_HAS_MORE_PENDING
		| NV2A_CONST_DUMP_FLAG_REACHED_DRAW_BOUNDARY
		| NV2A_CONST_DUMP_FLAG_SAW_NON_CONSTANT_METHODS;
	return (record.flags & required) == required && (record.flags & blockers) == 0;
}

int NV2AConstDumpFirstMismatchSlot(const NV2AConstDumpRecord &record)
{
	for (int slot = 0; slot < NV2A_VERTEXSHADER_CONSTANTS; slot++) {
		if (record.dirty_mask[slot] == 0) {
			continue;
		}

		const int base = slot * 4;
		if (std::memcmp(&record.parsed_constants[base], &record.gold_constants[base], 4 * sizeof(uint32_t)) != 0) {
			return slot;
		}
	}

	return -1;
}

int NV2AConstDumpDirtySlotCount(const NV2AConstDumpRecord &record)
{
	int count = 0;
	for (uint8_t dirty : record.dirty_mask) {
		count += dirty != 0 ? 1 : 0;
	}
	return count;
}