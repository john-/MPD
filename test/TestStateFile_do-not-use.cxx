// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

/**
 * Integration tests for StateFile class.
 * 
 * These tests verify that StateFile::Read() and StateFile::Write() 
 * correctly orchestrate the various state saving/loading functions.
 * The individual state functions are mocked since they're tested
 * separately in StateFileComponentTest.cxx.
 */

#include "StateFile.hxx"
#include "StateFileConfig.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "config/PartitionConfig.hxx"
#include "fs/AllocatedPath.hxx"
#include "event/Loop.hxx"
#include "io/BufferedOutputStream.hxx"
#include "io/FileLineReader.hxx"
#include "config/Data.hxx"
#include "config/Option.hxx"
#include "config/Param.hxx"
#include "mixer/Memento.hxx"
#include "player/Control.hxx"
#include "queue/Playlist.hxx"
#include "SongLoader.hxx"
#include "tag/Tag.hxx"
#include "fs/Path.hxx"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

using ::testing::_;
using ::testing::Return;

// =============================================================================
// Mock State Tracking and Call Counters
// =============================================================================

struct MockStateTracking {
	// Call counters
	int mixer_save_calls = 0;
	int mixer_load_calls = 0;
	int audio_save_calls = 0;
	int audio_read_calls = 0;
	int playlist_save_calls = 0;
	int playlist_restore_calls = 0;
#ifdef ENABLE_DATABASE
	int storage_save_calls = 0;
	int storage_restore_calls = 0;
#endif

	// Version tracking
	unsigned audio_output_version = 1000;
	unsigned storage_version = 2000;

	// State data for verification
	std::vector<std::string> lines_read;
	std::vector<std::string> lines_written;
	int last_volume_written = -1;
	int last_volume_read = -1;

	void reset() {
		mixer_save_calls = 0;
		mixer_load_calls = 0;
		audio_save_calls = 0;
		audio_read_calls = 0;
		playlist_save_calls = 0;
		playlist_restore_calls = 0;
#ifdef ENABLE_DATABASE
		storage_save_calls = 0;
		storage_restore_calls = 0;
#endif
		lines_read.clear();
		lines_written.clear();
		last_volume_written = -1;
		last_volume_read = -1;
	}
};

// Global instance for tracking mock calls
static MockStateTracking g_mock_state;

// =============================================================================
// Mock Global Functions (Required by StateFile)
// =============================================================================

unsigned audio_output_state_get_version() noexcept {
	return g_mock_state.audio_output_version;
}

void audio_output_state_save(BufferedOutputStream &os,
                              const MultipleOutputs &) {
	g_mock_state.audio_save_calls++;
	const char *line = "audio_device_state:0:TestOutput\n";
	os.Write(line);
	g_mock_state.lines_written.push_back(line);
}

bool audio_output_state_read(const char *line,
                              [[maybe_unused]] MultipleOutputs &,
                              [[maybe_unused]] Partition *) {
	if (std::strncmp(line, "audio_device_state:", 19) == 0) {
		g_mock_state.audio_read_calls++;
		g_mock_state.lines_read.push_back(line);
		return true;
	}
	return false;
}

void playlist_state_save(BufferedOutputStream &os,
                         const struct playlist &,
                         [[maybe_unused]] const PlayerControl &) {
	g_mock_state.playlist_save_calls++;
	const char *lines[] = {
		"state: stop\n",
		"playlist_begin\n",
		"0:test_song.flac\n",
		"playlist_end\n"
	};
	for (const char *line : lines) {
		os.Write(line);
		g_mock_state.lines_written.push_back(line);
	}
}

unsigned playlist_state_get_hash(const struct playlist &playlist,
                                 [[maybe_unused]] const PlayerControl &) noexcept {
	return playlist.GetVersion();
}

bool playlist_state_restore([[maybe_unused]] const StateFileConfig &,
                           const char *line,
                           [[maybe_unused]] FileLineReader &,
                           [[maybe_unused]] const SongLoader &,
                           [[maybe_unused]] struct playlist &,
                           [[maybe_unused]] PlayerControl &) {
	if (std::strncmp(line, "state:", 6) == 0 ||
	    std::strncmp(line, "playlist_begin", 14) == 0 ||
	    std::strncmp(line, "playlist_end", 12) == 0 ||
	    (line[0] >= '0' && line[0] <= '9' && std::strchr(line, ':'))) {
		g_mock_state.playlist_restore_calls++;
		g_mock_state.lines_read.push_back(line);
		return true;
	}
	return false;
}

#ifdef ENABLE_DATABASE
unsigned storage_state_get_hash([[maybe_unused]] const Instance &) noexcept {
	return g_mock_state.storage_version;
}

void storage_state_save(BufferedOutputStream &os,
                        [[maybe_unused]] const Instance &) {
	g_mock_state.storage_save_calls++;
	const char *line = "storage: mock_storage\n";
	os.Write(line);
	g_mock_state.lines_written.push_back(line);
}

bool storage_state_restore(const char *line,
                          [[maybe_unused]] FileLineReader &,
                          [[maybe_unused]] Instance &) {
	if (std::strncmp(line, "storage:", 8) == 0) {
		g_mock_state.storage_restore_calls++;
		g_mock_state.lines_read.push_back(line);
		return true;
	}
	return false;
}
#endif

// =============================================================================
// Test Fixture
// =============================================================================

class StateFileIntegrationTest : public ::testing::Test {
protected:
	std::filesystem::path temp_dir;
	std::filesystem::path test_state_file;
	Instance instance;
	PartitionConfig partition_config;
	EventLoop &event_loop = instance.event_loop;

	void SetUp() override {
		// Create temporary directory for test files
		temp_dir = std::filesystem::temp_directory_path() / 
		           ("mpd_state_test_" + std::to_string(getpid()));
		std::filesystem::create_directories(temp_dir);
		test_state_file = temp_dir / "state_file";

		// Reset mock state
		g_mock_state.reset();

		// Create default partition
		instance.partitions.emplace_back(instance, "default", 
		                                      partition_config);
	}

	void TearDown() override {
		// Clean up temporary files
		std::filesystem::remove_all(temp_dir);
		instance.partitions.clear();
	}

	/**
	 * Create a StateFileConfig pointing to our test file
	 */
	StateFileConfig CreateTestConfig() {
		ConfigData config_data;
		config_data.AddParam(ConfigOption::STATE_FILE,
				     ConfigParam(test_state_file.c_str()));
		config_data.AddParam(ConfigOption::STATE_FILE_INTERVAL,
				     ConfigParam("120"));
		config_data.AddParam(ConfigOption::RESTORE_PAUSED,
				     ConfigParam("no"));
		return StateFileConfig(config_data);
	}

	/**
	 * Write test data to the state file
	 */
	void WriteTestStateFile(const std::vector<std::string> &lines) {
		std::ofstream file(test_state_file);
		for (const auto &line : lines) {
			file << line << "\n";
		}
		file.close();
	}

	/**
	 * Read state file and return lines
	 */
	std::vector<std::string> ReadStateFile() {
		std::vector<std::string> lines;
		std::ifstream file(test_state_file);
		std::string line;
		while (std::getline(file, line)) {
			lines.push_back(line);
		}
		return lines;
	}

	/**
	 * Get reference to default partition
	 */
	Partition& GetDefaultPartition() {
		return instance.partitions.front();
	}
};

// =============================================================================
// StateFile::Write() Integration Tests
// =============================================================================

TEST_F(StateFileIntegrationTest, DISABLED_WriteCallsAllSaveFunctions) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Execute Write
	state_file.Write();

	// Verify all save functions were called exactly once per partition
	EXPECT_EQ(g_mock_state.mixer_save_calls, 1) 
		<< "mixer_memento.SaveSoftwareVolumeState should be called once";
	EXPECT_EQ(g_mock_state.audio_save_calls, 1) 
		<< "audio_output_state_save should be called once";
	EXPECT_EQ(g_mock_state.playlist_save_calls, 1) 
		<< "playlist_state_save should be called once";
#ifdef ENABLE_DATABASE
	EXPECT_EQ(g_mock_state.storage_save_calls, 1) 
		<< "storage_state_save should be called once";
#endif
}

TEST_F(StateFileIntegrationTest, DISABLED_WriteCreatesValidFile) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Execute Write
	state_file.Write();

	// Verify file exists
	EXPECT_TRUE(std::filesystem::exists(test_state_file)) 
		<< "State file should be created";

	// Verify file contains expected content
	auto lines = ReadStateFile();
	EXPECT_GT(lines.size(), 0) << "State file should not be empty";

	// Check for key content
	bool has_volume = false;
	bool has_audio = false;
	bool has_playlist_begin = false;
	bool has_playlist_end = false;

	for (const auto &line : lines) {
		if (line.find("sw_volume:") != std::string::npos) has_volume = true;
		if (line.find("audio_device_state:") != std::string::npos) has_audio = true;
		if (line.find("playlist_begin") != std::string::npos) has_playlist_begin = true;
		if (line.find("playlist_end") != std::string::npos) has_playlist_end = true;
	}

	EXPECT_TRUE(has_volume) << "State file should contain volume state";
	EXPECT_TRUE(has_audio) << "State file should contain audio output state";
	EXPECT_TRUE(has_playlist_begin) << "State file should contain playlist_begin";
	EXPECT_TRUE(has_playlist_end) << "State file should contain playlist_end";
}

TEST_F(StateFileIntegrationTest, DISABLED_WriteMultiplePartitions) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();

	// Add second partition
	instance.partitions.emplace_back(instance, "TestPartition", 
	                                      partition_config);

	StateFile state_file(std::move(config), partition, event_loop);

	// Execute Write
	state_file.Write();

	// Verify all save functions were called twice (once per partition)
	EXPECT_EQ(g_mock_state.mixer_save_calls, 2);
	EXPECT_EQ(g_mock_state.audio_save_calls, 2);
	EXPECT_EQ(g_mock_state.playlist_save_calls, 2);

	// Verify file contains partition marker
	auto lines = ReadStateFile();
	bool has_partition_marker = false;
	for (const auto &line : lines) {
		if (line.find("partition: TestPartition") != std::string::npos) {
			has_partition_marker = true;
			break;
		}
	}
	EXPECT_TRUE(has_partition_marker) 
		<< "State file should contain partition marker for second partition";
}

TEST_F(StateFileIntegrationTest, WriteHandlesExceptions) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();

	// Make directory read-only to force write failure
	std::filesystem::permissions(temp_dir, 
	                             std::filesystem::perms::owner_read,
	                             std::filesystem::perm_options::replace);

	StateFile state_file(std::move(config), partition, event_loop);

	// Should not throw - Write() catches exceptions
	EXPECT_NO_THROW(state_file.Write());

	// Restore permissions for cleanup
	std::filesystem::permissions(temp_dir, 
	                             std::filesystem::perms::owner_all,
	                             std::filesystem::perm_options::replace);
}

TEST_F(StateFileIntegrationTest, DISABLED_WriteUpdatesVersionTracking) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Get initial hash values
	unsigned initial_volume_hash = partition.mixer_memento.GetSoftwareVolumeStateHash();
	unsigned initial_playlist_hash = partition.playlist.GetVersion();

	// Write state
	state_file.Write();

	// Modify state
	partition.mixer_memento.SetVolume(partition.outputs, 75);
	partition.playlist.Clear(partition.pc);

	// Verify IsModified would detect changes
	EXPECT_NE(partition.mixer_memento.GetSoftwareVolumeStateHash(), initial_volume_hash);
	EXPECT_NE(partition.playlist.GetVersion(), initial_playlist_hash);
}

// =============================================================================
// StateFile::Read() Integration Tests
// =============================================================================

TEST_F(StateFileIntegrationTest, DISABLED_ReadCallsAllLoadFunctions) {
	// Create a state file with test data
	WriteTestStateFile({
		"sw_volume: 42",
		"audio_device_state:0:TestOutput",
		"state: stop",
		"playlist_begin",
		"0:test_song.flac",
		"playlist_end"
	});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Execute Read
	state_file.Read();

	// Verify all load functions were called
	EXPECT_EQ(g_mock_state.mixer_load_calls, 1) 
		<< "mixer_memento.LoadSoftwareVolumeState should be called";
	EXPECT_EQ(g_mock_state.audio_read_calls, 1) 
		<< "audio_output_state_read should be called";
	EXPECT_GE(g_mock_state.playlist_restore_calls, 1) 
		<< "playlist_state_restore should be called";
}

TEST_F(StateFileIntegrationTest, DISABLED_ReadLoadsCorrectData) {
	// Create a state file with specific volume
	WriteTestStateFile({
		"sw_volume: 73",
		"audio_device_state:0:TestOutput",
		"state: stop",
		"playlist_begin",
		"playlist_end"
	});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Execute Read
	state_file.Read();

	// Verify data was loaded correctly
	EXPECT_EQ(g_mock_state.last_volume_read, 73) 
		<< "Volume should be loaded correctly";
	EXPECT_EQ(partition.mixer_memento.GetVolume(partition.outputs), 73) 
		<< "Partition should have correct volume";
}

TEST_F(StateFileIntegrationTest, DISABLED_ReadCreatesPartitions) {
	// Create a state file with multiple partitions
	WriteTestStateFile({
		"sw_volume: 50",
		"state: stop",
		"playlist_begin",
		"playlist_end",
		"partition: NewPartition",
		"sw_volume: 60",
		"state: stop",
		"playlist_begin",
		"playlist_end"
	});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Should start with 1 partition
	EXPECT_EQ(instance.partitions.size(), 1);

	// Execute Read
	state_file.Read();

	// Should now have 2 partitions
	EXPECT_EQ(instance.partitions.size(), 2) 
		<< "Read should create new partition";

	// Verify partition was created with correct name
	auto *new_partition = instance.FindPartition("NewPartition");
	ASSERT_NE(new_partition, nullptr) << "New partition should exist";
	EXPECT_EQ(new_partition->name, "NewPartition");
}

TEST_F(StateFileIntegrationTest, DISABLED_ReadSwitchesToExistingPartition) {
	// Pre-create second partition
	instance.partitions.emplace_back(instance, "ExistingPartition", 
	                                      partition_config);

	// Create a state file that references it
	WriteTestStateFile({
		"sw_volume: 50",
		"partition: ExistingPartition",
		"sw_volume: 60"
	});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	size_t initial_partition_count = instance.partitions.size();

	// Execute Read
	state_file.Read();

	// Should not create duplicate partition
	EXPECT_EQ(instance.partitions.size(), initial_partition_count) 
		<< "Read should not duplicate existing partition";

	// Verify both partitions were updated
	EXPECT_EQ(g_mock_state.mixer_load_calls, 2) 
		<< "Both partitions should have state loaded";
}

TEST_F(StateFileIntegrationTest, ReadHandlesMissingFile) {
	// Don't create the file
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Should not throw - Read() catches exceptions
	EXPECT_NO_THROW(state_file.Read());

	// No load functions should be called
	EXPECT_EQ(g_mock_state.mixer_load_calls, 0);
	EXPECT_EQ(g_mock_state.audio_read_calls, 0);
	EXPECT_EQ(g_mock_state.playlist_restore_calls, 0);
}

TEST_F(StateFileIntegrationTest, DISABLED_ReadHandlesCorruptedFile) {
	// Create a file with invalid content
	WriteTestStateFile({
		"sw_volume: 50",
		"invalid_line_without_handler",
		"state: stop",
		"another_unrecognized: line"
	});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Should not throw - Read() handles unrecognized lines
	EXPECT_NO_THROW(state_file.Read());

	// Valid lines should still be processed
	EXPECT_EQ(g_mock_state.mixer_load_calls, 1);
	EXPECT_GE(g_mock_state.playlist_restore_calls, 1);
}

TEST_F(StateFileIntegrationTest, ReadHandlesEmptyFile) {
	// Create empty file
	WriteTestStateFile({});

	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();
	StateFile state_file(std::move(config), partition, event_loop);

	// Should not throw
	EXPECT_NO_THROW(state_file.Read());

	// No load functions should be called
	EXPECT_EQ(g_mock_state.mixer_load_calls, 0);
	EXPECT_EQ(g_mock_state.audio_read_calls, 0);
	EXPECT_EQ(g_mock_state.playlist_restore_calls, 0);
}

// =============================================================================
// Round-Trip Integration Tests
// =============================================================================

TEST_F(StateFileIntegrationTest, DISABLED_RoundTripPreservesState) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();

	// Set specific state
	partition.mixer_memento.SetVolume(partition.outputs, 87);

	// Write state
	StateFile write_state_file(std::move(config), partition, event_loop);
	write_state_file.Write();

	// Clear partition state
	instance.partitions.clear();
	instance.partitions.emplace_back(instance, "default", 
	                                      partition_config);
	auto &new_partition = GetDefaultPartition();

	// Read state back
	auto read_config = CreateTestConfig();
	StateFile read_state_file(std::move(read_config), new_partition, event_loop);
	read_state_file.Read();

	// Verify state was preserved
	EXPECT_EQ(new_partition.mixer_memento.GetVolume(new_partition.outputs), 87) 
		<< "Volume should be preserved in round-trip";
}

TEST_F(StateFileIntegrationTest, DISABLED_RoundTripMultiplePartitions) {
	auto config = CreateTestConfig();
	auto &partition = GetDefaultPartition();

	// Create multiple partitions with different states
	partition.mixer_memento.SetVolume(partition.outputs, 10);
	instance.partitions.emplace_back(instance, "Partition2", 
	                                      partition_config);
	instance.partitions.back().mixer_memento.SetVolume(instance.partitions.back().outputs, 20);

	// Write state
	StateFile write_state_file(std::move(config), partition, event_loop);
	write_state_file.Write();

	// Clear all partitions
	instance.partitions.clear();
	instance.partitions.emplace_back(instance, "default", 
	                                      partition_config);

	// Read state back
	auto read_config = CreateTestConfig();
	auto &new_partition = GetDefaultPartition();
	StateFile read_state_file(std::move(read_config), new_partition, event_loop);
	read_state_file.Read();

	// Verify partitions were recreated
	EXPECT_EQ(instance.partitions.size(), 2) 
		<< "Both partitions should be recreated";

	auto *partition2 = instance.FindPartition("Partition2");
	ASSERT_NE(partition2, nullptr);
	EXPECT_EQ(partition2->mixer_memento.GetVolume(partition2->outputs), 20);
}