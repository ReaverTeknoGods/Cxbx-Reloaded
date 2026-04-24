#include "devices\video\NV2AConstDump.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FailureSummary {
	uint32_t frame_number;
	uint32_t draw_number;
	int mismatch_slot;
	int dirty_slot_count;
	uint32_t flags;
};

void print_usage(const char *argv0)
{
	std::cout
		<< "Usage: " << argv0 << " [dump-file]\n"
		<< "Default dump file: " << NV2AConstDumpDefaultPath() << "\n";
}

} // namespace

int main(int argc, char **argv)
{
	if (argc > 1 && std::string(argv[1]) == "--help") {
		print_usage(argv[0]);
		return 0;
	}

	std::string dump_path = (argc > 1) ? argv[1] : NV2AConstDumpDefaultPath();
	std::vector<NV2AConstDumpRecord> records;
	std::string error;
	if (!NV2AConstDumpLoadFile(dump_path, &records, &error)) {
		std::cerr << error << "\n";
		return 1;
	}

	std::vector<FailureSummary> failures;
	size_t mismatch_records = 0;
	size_t current_heuristic_candidates = 0;
	size_t current_heuristic_false_positives = 0;

	for (const NV2AConstDumpRecord &record : records) {
		const int mismatch_slot = NV2AConstDumpFirstMismatchSlot(record);
		if (mismatch_slot >= 0) {
			mismatch_records++;
		}

		if (NV2AConstDumpCurrentHeuristicWouldSkip(record)) {
			current_heuristic_candidates++;
			if (mismatch_slot >= 0) {
				current_heuristic_false_positives++;
				failures.push_back({
					record.frame_number,
					record.draw_number,
					mismatch_slot,
					NV2AConstDumpDirtySlotCount(record),
					record.flags,
				});
			}
		}
	}

	std::cout << "Loaded " << records.size() << " record(s) from " << dump_path << "\n";
	std::cout << "Records with parsed-vs-gold mismatches: " << mismatch_records << "\n";
	std::cout << "Current pure-constant heuristic candidates: " << current_heuristic_candidates << "\n";
	std::cout << "Current pure-constant heuristic false positives: " << current_heuristic_false_positives << "\n";

	if (!failures.empty()) {
		const size_t limit = std::min<size_t>(failures.size(), 10);
		std::cout << "First " << limit << " failing records:\n";
		for (size_t i = 0; i < limit; i++) {
			const FailureSummary &failure = failures[i];
			std::cout
				<< "  F" << failure.frame_number
				<< " D" << failure.draw_number
				<< " mismatch_slot=" << failure.mismatch_slot
				<< " dirty_slots=" << failure.dirty_slot_count
				<< " flags=0x" << std::hex << failure.flags << std::dec
				<< "\n";
		}
	}

	return current_heuristic_false_positives == 0 ? 0 : 2;
}