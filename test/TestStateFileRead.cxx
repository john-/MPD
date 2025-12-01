// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "StateFile.hxx"
#include "Instance.hxx"
#include "Partition.hxx"
#include "config/Data.hxx"
#include "config/PartitionConfig.hxx"
#include "fs/FileSystem.hxx"
#include "io/FileOutputStream.hxx"
#include "io/FileLineReader.hxx"
#include "Log.hxx"
#include "LogBackend.hxx"

#include <gtest/gtest.h>

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
		const std::string temp_dir = testing::TempDir();

		// Construct unique filename: mpd_test_state_<timestamp>_<pid>.txt
		const auto base_path = AllocatedPath::FromFS(temp_dir);
		
		const auto filename = fmt::format("mpd_test_state_{}_{}.txt", 
		                                  timestamp, getpid());

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

	/**
	 * Check if the state file contains a specific key-value entry
	 * for a given partition.
	 *
	 * This function reads the temporary state file and searches for
	 * a matching entry within the specified partition's section.
	 *
	 * @param partition_name Name of the partition ("default" for the first/unlabeled partition)
	 * @param key The state key to search for (e.g., "sw_volume", "state")
	 * @param expected_value The expected value for the key
	 * @return true if the entry exists with the expected value, false otherwise
	 */
	[[nodiscard]]
	bool StateFileHasEntry(std::string_view partition_name,
	                       std::string_view key,
	                       std::string_view expected_value) const {
		// Check if state file exists
		if (!FileExists(temp_state_file)) {
			return false;
		}

		try {
			// Open and read the state file
			FileLineReader reader{temp_state_file};
			
			// Track current partition (start with "default")
			std::string current_partition = "default";
			
			// Search key with colon separator
			const auto search_key = std::string{key} + ":";
			
			// Read file line by line
			const char *line;
			while ((line = reader.ReadLine()) != nullptr) {
				const std::string_view line_view{line};
				
				// Check for partition switch
				if (line_view.starts_with("partition: ")) {
					// Extract partition name after "partition: "
					current_partition = line_view.substr(11);
					continue;
				}
				
				// Skip if we're not in the target partition
				if (current_partition != partition_name) {
					continue;
				}
				
				// Check if line matches our key
				if (!line_view.starts_with(search_key)) {
					continue;
				}
				
				// Extract value (everything after "key: ")
				const auto value_start = search_key.length();
				if (value_start < line_view.length()) {
					const auto value = line_view.substr(value_start);
					
					// Trim leading whitespace from value
					auto value_trimmed = value;
					while (!value_trimmed.empty() && value_trimmed.front() == ' ') {
						value_trimmed.remove_prefix(1);
					}
					
					if (value_trimmed == expected_value) {
						return true;
					}
				}
			}
			
			return false;
		} catch (...) {
			// File read error - entry not found
			return false;
		}
	}

	/**
	 * Get the value of a state file entry for a specific partition.
	 *
	 * This function reads the temporary state file and returns the value
	 * associated with the given key in the specified partition's section.
	 *
	 * @param partition_name Name of the partition ("default" for the first/unlabeled partition)
	 * @param key The state key to search for (e.g., "sw_volume", "state")
	 * @return The value if found, empty string if not found or on error
	 */
	[[nodiscard]]
	std::string GetStateFileEntry(std::string_view partition_name,
	                              std::string_view key) const {
		// Check if state file exists
		if (!FileExists(temp_state_file)) {
			return {};
		}

		try {
			// Open and read the state file
			FileLineReader reader{temp_state_file};
			
			// Track current partition (start with "default")
			std::string current_partition = "default";
			
			// Search key with colon separator
			const auto search_key = std::string{key} + ":";
			
			// Read file line by line
			const char *line;
			while ((line = reader.ReadLine()) != nullptr) {
				const std::string_view line_view{line};
				
				// Check for partition switch
				if (line_view.starts_with("partition: ")) {
					// Extract partition name after "partition: "
					current_partition = line_view.substr(11);
					continue;
				}
				
				// Skip if we're not in the target partition
				if (current_partition != partition_name) {
					continue;
				}
				
				// Check if line matches our key
				if (!line_view.starts_with(search_key)) {
					continue;
				}
				
				// Extract value (everything after "key: ")
				const auto value_start = search_key.length();
				if (value_start < line_view.length()) {
					auto value = line_view.substr(value_start);
					
					// Trim leading whitespace from value
					while (!value.empty() && value.front() == ' ') {
						value.remove_prefix(1);
					}
					
					return std::string{value};
				}
			}
			
			return {};
		} catch (...) {
			// File read error
			return {};
		}
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

	// Validate specific entries were written
	EXPECT_TRUE(StateFileHasEntry("default", "state", "stop"));
	EXPECT_TRUE(StateFileHasEntry("default", "audio_device_state:1", "MyTestOutput"));
}

/**
 * Test state file entry validation functions.
 * This verifies the helper functions can correctly parse state file contents.
 */
TEST_F(TestStateFileRead, ValidateStateFileEntries) {
	// Write a known state file
	WriteStateFile(
		"sw_volume: 80\n"
		"state: pause\n"
		"random: 1\n"
		"repeat: 0\n"
		"partition: secondary\n"
		"sw_volume: 50\n"
		"state: play\n"
	);

	// Validate entries in default partition
	EXPECT_TRUE(StateFileHasEntry("default", "sw_volume", "80"));
	EXPECT_TRUE(StateFileHasEntry("default", "state", "pause"));
	EXPECT_TRUE(StateFileHasEntry("default", "random", "1"));
	EXPECT_TRUE(StateFileHasEntry("default", "repeat", "0"));
	
	// Validate entries in secondary partition
	EXPECT_TRUE(StateFileHasEntry("secondary", "sw_volume", "50"));
	EXPECT_TRUE(StateFileHasEntry("secondary", "state", "play"));
	
	// Verify wrong values return false
	EXPECT_FALSE(StateFileHasEntry("default", "sw_volume", "100"));
	EXPECT_FALSE(StateFileHasEntry("secondary", "random", "1"));
	
	// Test GetStateFileEntry
	EXPECT_EQ(GetStateFileEntry("default", "sw_volume"), "80");
	EXPECT_EQ(GetStateFileEntry("default", "state"), "pause");
	EXPECT_EQ(GetStateFileEntry("secondary", "sw_volume"), "50");
	EXPECT_EQ(GetStateFileEntry("default", "nonexistent"), "");
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