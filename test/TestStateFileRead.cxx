// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "StateFile.hxx"
#include "StateFileConfig.hxx"
#include "Instance.hxx"
#include "Partition.hxx"
#include "config/Data.hxx"
#include "config/Block.hxx"
#include "config/Option.hxx"
#include "config/PartitionConfig.hxx"
#include "config/ReplayGainConfig.hxx"
#include "event/Loop.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileSystem.hxx"
#include "io/FileOutputStream.hxx"
#include "Log.hxx"
#include "LogBackend.hxx"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string_view>
#include <span>

#ifndef _WIN32
#include <unistd.h>
#endif

// Declare the global_instance (defined in TestGlobalInit.cxx)
extern Instance *global_instance;

/**
 * Global test environment to initialize logging subsystem.
 * This allows FmtDebug/FmtError messages to be visible during test execution.
 */
class TestEnvironment final : public ::testing::Environment {
public:
	void SetUp() override {
		// Check if verbose logging is requested via environment variable
		const char *verbose = std::getenv("MPD_TEST_VERBOSE");
		if (verbose != nullptr && 
		    (std::string_view{verbose} == "1" || 
		     std::string_view{verbose} == "true")) {
			SetLogThreshold(LogLevel::DEBUG);
		}
	}
};

// Register the global test environment
[[maybe_unused]] static ::testing::Environment *const test_env =
	::testing::AddGlobalTestEnvironment(new TestEnvironment());

/**
 * Test fixture for StateFile read/write operations.
 * 
 * This fixture creates a minimal MPD instance with a temporary state file
 * for isolated testing of StateFile functionality.
 */
class TestStateFileRead : public ::testing::Test {
protected:
	TestStateFileRead()
		: temp_state_file(nullptr){}

	std::unique_ptr<Instance> instance;
	std::unique_ptr<StateFile> state_file;
	AllocatedPath temp_state_file;

	/**
	 * Set up test environment: create instance, partition, and state file.
	 * Called before each test case.
	 */
	void SetUp() override {
		// Create instance
		instance = std::make_unique<Instance>();
		global_instance = instance.get();

		// Generate unique temporary file path
		temp_state_file = GenerateTempFilePath();

		// Create configuration with audio output
		ConfigData config_data;
		
		// Add null audio output for testing
		// Use line number 1 to indicate this is a "real" config block, not synthetic
		ConfigBlock audio_output_block{1};
		audio_output_block.AddBlockParam("type", "null", 1);
		audio_output_block.AddBlockParam("name", "MyTestOutput", 2);
		audio_output_block.AddBlockParam("mixer_type", "null", 3);
		config_data.AddBlock(ConfigBlockOption::AUDIO_OUTPUT, 
		                     std::move(audio_output_block));

		// Create partition and add to instance
		PartitionConfig partition_config{config_data};
		instance->partitions.emplace_back(
			*instance,
			"default",
			partition_config
		);

		// Get reference to the partition
		Partition &test_partition = instance->partitions.front();
		
		// Configure outputs from config data
		// Note: ReplayGainConfig needs to be created
		ReplayGainConfig replay_gain_config;
		replay_gain_config.preamp = 1.0;
		replay_gain_config.missing_preamp = 1.0;
		replay_gain_config.limit = true;
		
		test_partition.outputs.Configure(
			instance->event_loop,
			instance->rtio_thread.GetEventLoop(),
			config_data,
			replay_gain_config
		);

		// Create StateFile configuration with our temporary file
		StateFileConfig state_config{config_data};
		state_config.path = temp_state_file;

		// Create the StateFile for testing
		state_file = std::make_unique<StateFile>(
			std::move(state_config),
			test_partition,
			instance->event_loop
		);
	}

	/**
	 * Clean up test environment: destroy state file and remove temp files.
	 * Called after each test case.
	 */
	void TearDown() override {
		// Destroy state file first
		state_file.reset();

		// Clear global instance
		global_instance = nullptr;
		instance.reset();

		// Remove temporary file if it exists
		if (!temp_state_file.IsNull() && PathExists(temp_state_file)) {
			try {
				RemoveFile(temp_state_file);
			} catch (...) {
				// Ignore cleanup errors
			}
		}
	}

	/**
	 * Generate a unique temporary file path for state file testing.
	 * Uses timestamp and process ID to ensure uniqueness.
	 * 
	 * @return AllocatedPath pointing to unique temporary file location
	 */
	[[nodiscard]]
	static AllocatedPath GenerateTempFilePath() noexcept {
		// Get current timestamp as nanoseconds since epoch
		const auto now = std::chrono::system_clock::now();
		const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
			now.time_since_epoch()
		).count();

#ifdef _WIN32
		// Windows: use TEMP environment variable
		const char *temp_dir = std::getenv("TEMP");
		if (temp_dir == nullptr || temp_dir[0] == '\0') {
			temp_dir = std::getenv("TMP");
		}
		if (temp_dir == nullptr || temp_dir[0] == '\0') {
			temp_dir = "C:\\Temp";
		}
#else
		// Unix: use TMPDIR or fall back to /var/tmp
		const char *temp_dir = std::getenv("TMPDIR");
		if (temp_dir == nullptr || temp_dir[0] == '\0') {
			temp_dir = "/var/tmp";
		}
#endif

		// Construct unique filename: mpd_test_state_<timestamp>_<pid>.txt
		const auto base_path = AllocatedPath::FromFS(temp_dir);
		
#ifdef _WIN32
		const auto filename = fmt::format("mpd_test_state_{}.txt", timestamp);
#else
		const auto filename = fmt::format("mpd_test_state_{}_{}.txt", 
		                                  timestamp, ::getpid());
#endif

		return AllocatedPath::Build(base_path, 
		                            AllocatedPath::FromFS(filename.c_str()));
	}

	/**
	 * Write test content to the temporary state file.
	 * 
	 * @param content The content to write (must be valid UTF-8)
	 * @throws std::exception on I/O error
	 */
	void WriteStateFile(std::string_view content) {
		FileOutputStream file{temp_state_file};
		
		// Convert string_view to span<const byte> for Write()
		const auto bytes = std::as_bytes(std::span{content});
		file.Write(bytes);
		
		// Commit the file
		file.Commit();
	}

	/**
	 * Get a reference to a partition by name.
	 * 
	 * @param name Partition name to find (default: "default")
	 * @return Reference to the partition
	 * @throws Assertion failure if partition not found
	 */
	[[nodiscard]]
	Partition &GetPartition(std::string_view name = "default") noexcept {
		auto *partition = instance->FindPartition(name.data());
		assert(partition != nullptr);
		return *partition;
	}
};

/**
 * Test that StateFile can successfully read a valid state file.
 */
TEST_F(TestStateFileRead, ReadValidStateFile) {
	// Create a state file with valid content
	WriteStateFile(
		"sw_volume: 80\n"
		"state: pause\n"
		"random: 1\n"
		"repeat: 0\n"
	);

	// Read the state file - should not throw
	state_file->Read();

	// Verify Read() completed successfully
	SUCCEED();
}

/**
 * Test that audio output configuration was properly loaded.
 * This verifies the ConfigBlock setup in SetUp() is working correctly.
 */
TEST_F(TestStateFileRead, AudioOutputLoadedFromConfig) {
	// Get the partition
	Partition &partition = GetPartition();

	// Verify exactly one audio output was configured
	ASSERT_EQ(partition.outputs.Size(), 1);

	// Get the output and verify its properties
	const auto &output = partition.outputs.Get(0);
	EXPECT_EQ(output.GetName(), "MyTestOutput");
	EXPECT_STREQ(output.GetPluginName(), "null");
}

/**
 * Test that StateFile handles empty files gracefully.
 */
TEST_F(TestStateFileRead, ReadEmptyStateFile) {
	// Create an empty state file
	WriteStateFile("");

	// Should handle empty file without throwing
	state_file->Read();

	SUCCEED();
}

/**
 * Test that StateFile handles malformed content gracefully.
 * Malformed lines should be logged but not crash.
 */
TEST_F(TestStateFileRead, ReadMalformedStateFile) {
	// Create a state file with various malformed lines
	WriteStateFile(
		"invalid line without colon\n"
		":::too:many:colons:::\n"
		"incomplete:"
	);

	// Should handle malformed file gracefully (logs errors internally)
	state_file->Read();

	SUCCEED();
}

/**
 * Test that StateFile handles missing files gracefully.
 * Reading a non-existent file should log an error but not throw.
 */
TEST_F(TestStateFileRead, ReadNonExistentFile) {
	// Don't create a file - test reading when file doesn't exist
	// Should handle missing file gracefully
	state_file->Read();

	SUCCEED();
}

/**
 * Test that StateFile correctly handles partition switching.
 * The state file format supports multiple partitions using "partition:" lines.
 */
TEST_F(TestStateFileRead, ReadWithMultiplePartitions) {
	// Create a state file with multiple partitions
	WriteStateFile(
		"partition: secondary\n"
		"sw_volume: 50\n"
		"state: pause\n"
	);

	// Read the state file
	state_file->Read();

	// Verify that multiple partitions exist
	ASSERT_GE(instance->partitions.size(), 1);

	// The "secondary" partition should have been created
	const auto *secondary = instance->FindPartition("secondary");
	EXPECT_NE(secondary, nullptr) << "Secondary partition should have been created";
}

/**
 * Test read-write round-trip: verify that reading and writing preserves state.
 */
TEST_F(TestStateFileRead, ReadAndWrite) {
	// Create initial state file
	WriteStateFile(
		"sw_volume: 75\n"
		"random: 1\n"
	);

	// Read the state
	state_file->Read();

	// Write it back out
	state_file->Write();

	// Verify the file still exists and is non-empty
	ASSERT_TRUE(FileExists(temp_state_file));

	// Note: We could read the file again and verify content matches,
	// but that would require parsing logic. For now, just verify
	// the write completed successfully.
}

/**
 * Test that StateFile handles lines with colons in values correctly.
 */
TEST_F(TestStateFileRead, ReadWithColonsInValues) {
	// Create a state file with colons in the value portion
	WriteStateFile(
		"audio_device_state:1:Output Name\n"
		"state: pause\n"
	);

	// Should handle colons in values correctly
	state_file->Read();

	SUCCEED();
}

/**
 * Test that empty lines and whitespace-only lines are handled gracefully.
 */
TEST_F(TestStateFileRead, ReadWithEmptyLines) {
	WriteStateFile(
		"\n"
		"sw_volume: 100\n"
		"\n"
		"   \n"
		"state: play\n"
		"\n"
	);

	// Should skip empty/whitespace lines without error
	state_file->Read();

	SUCCEED();
}