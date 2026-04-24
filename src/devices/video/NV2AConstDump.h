#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nv2a_regs.h"

enum NV2AConstDumpFlags : uint32_t {
	NV2A_CONST_DUMP_FLAG_PARSED_PENDING_CONSTANTS = 1u << 0,
	NV2A_CONST_DUMP_FLAG_HAS_MORE_PENDING = 1u << 1,
	NV2A_CONST_DUMP_FLAG_REACHED_DRAW_BOUNDARY = 1u << 2,
	NV2A_CONST_DUMP_FLAG_SAW_NON_CONSTANT_METHODS = 1u << 3,
	NV2A_CONST_DUMP_FLAG_PUSHBUFFER_NOT_PRIMED = 1u << 4,
	NV2A_CONST_DUMP_FLAG_KICKOFF_SUBMITTED_WORK = 1u << 5,
};

struct NV2AConstDumpRecord {
	uint32_t frame_number = 0;
	uint32_t draw_number = 0;
	uint32_t flags = 0;
	std::array<uint8_t, NV2A_VERTEXSHADER_CONSTANTS> dirty_mask = {};
	std::array<uint32_t, NV2A_VERTEXSHADER_CONSTANTS * 4> parsed_constants = {};
	std::array<uint32_t, NV2A_VERTEXSHADER_CONSTANTS * 4> gold_constants = {};
};

bool NV2AConstDumpCaptureEnabled();
bool NV2AConstDumpAppend(const NV2AConstDumpRecord &record);
std::string NV2AConstDumpDefaultPath();
bool NV2AConstDumpLoadFile(const std::string &path,
	std::vector<NV2AConstDumpRecord> *outRecords,
	std::string *outError);
bool NV2AConstDumpCurrentHeuristicWouldSkip(const NV2AConstDumpRecord &record);
int NV2AConstDumpFirstMismatchSlot(const NV2AConstDumpRecord &record);
int NV2AConstDumpDirtySlotCount(const NV2AConstDumpRecord &record);