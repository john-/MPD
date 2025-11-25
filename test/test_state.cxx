// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "StateFile.hxx"
#include "StateFileConfig.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "io/BufferedOutputStream.hxx"
#include "fs/AllocatedPath.hxx"
#include "event/Loop.hxx"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <sstream>
#include <string>
#include <vector>
#include <cstring>

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::Invoke;

// =============================================================================
// Mock Classes and Stubs
// =============================================================================

/**
 * Mock output stream that captures written data to a string
 */
class MockBufferedOutputStream {
public:
	std::ostringstream buffer;

	void Write(const char *data, std::size_t size) {
		buffer.write(data, size);
	}

	void Flush() {
		// No-op for testing
	}

	/**
	 * Template method to support Fmt() calls
	 */
	template<typename... Args>
	void Fmt(fmt::format_string<Args...> format_str, Args&&... args) {
		buffer << fmt::format(format_str, std::forward<Args>(args)...);
	}

	std::string GetOutput() const {
		return buffer.str();
	}

	void Clear() {
		buffer.str("");
		buffer.clear();
	}
};

/**
 * Mock file line reader that provides predetermined lines
 */
class MockFileLineReader {
	std::vector<std::string> lines;
	std::size_t current_line = 0;

public:
	explicit MockFileLineReader(std::vector<std::string> input_lines)
		: lines(std::move(input_lines)) {}

	const char* ReadLine() {
		if (current_line >= lines.size()) {
			return nullptr;
		}
		return lines[current_line++].c_str();
	}

	void Reset() {
		current_line = 0;
	}
};

/**
 * Mock mixer memento for volume state
 */
class MockMixerMemento {
	unsigned volume_hash = 100;
	int software_volume = 50;

public:
	unsigned GetSoftwareVolumeStateHash() const noexcept {
		return volume_hash;
	}

	void SetVolumeHash(unsigned hash) noexcept {
		volume_hash = hash;
	}

	void SaveSoftwareVolumeState(MockBufferedOutputStream &os) const {
		os.Fmt("sw_volume: {}\n", software_volume);
	}

	bool LoadSoftwareVolumeState(const char *line, [[maybe_unused]] auto &outputs) {
		if (std::strncmp(line, "sw_volume: ", 11) == 0) {
			software_volume = std::atoi(line + 11);
			volume_hash++;
			return true;
		}
		return false;
	}

	void SetSoftwareVolume(int vol) noexcept {
		software_volume = vol;
		volume_hash++;
	}

	int GetSoftwareVolume() const noexcept {
		return software_volume;
	}
};

/**
 * Mock outputs for audio output state
 */
class MockMultipleOutputs {
	std::vector<std::string> output_states;

public:
	void AddOutputState(std::string state) {
		output_states.push_back(std::move(state));
	}

	const std::vector<std::string>& GetOutputStates() const {
		return output_states;
	}

	void Clear() {
		output_states.clear();
	}
};

/**
 * Mock playlist
 */
class MockPlaylist {
	unsigned hash_value = 200;
	std::vector<std::string> songs;

public:
	unsigned GetHash() const noexcept {
		return hash_value;
	}

	void SetHash(unsigned hash) noexcept {
		hash_value = hash;
	}

	void AddSong(std::string song) {
		songs.push_back(std::move(song));
		hash_value++;
	}

	const std::vector<std::string>& GetSongs() const {
		return songs;
	}

	void Clear() {
		songs.clear();
	}
};

/**
 * Mock player control
 */
class MockPlayerControl {
	unsigned version = 300;

public:
	unsigned GetVersion() const noexcept {
		return version;
	}

	void SetVersion(unsigned ver) noexcept {
		version = ver;
	}
};

// =============================================================================
// Global Mock Functions (required by StateFile)
// =============================================================================

static unsigned g_audio_output_version = 1000;
static std::vector<std::string> g_saved_output_states;
static std::vector<std::string> g_saved_playlist_states;

unsigned audio_output_state_get_version() noexcept {
	return g_audio_output_version;
}

void audio_output_state_save(MockBufferedOutputStream &os,
                              const MockMultipleOutputs &outputs) {
	for (const auto &state : outputs.GetOutputStates()) {
		os.buffer << state << "\n";
	}
	g_saved_output_states = outputs.GetOutputStates();
}

bool audio_output_state_read(const char *line,
                              [[maybe_unused]] MockMultipleOutputs &outputs,
                              [[maybe_unused]] void *partition) {
	if (std::strncmp(line, "audio_device_state:", 19) == 0) {
		// Store for verification
		g_saved_output_states.push_back(line);
		return true;
	}
	return false;
}

void playlist_state_save(MockBufferedOutputStream &os,
                         const MockPlaylist &playlist,
                         [[maybe_unused]] const MockPlayerControl &pc) {
	os.buffer << "state: stop\n";
	os.buffer << "playlist_begin\n";
	for (const auto &song : playlist.GetSongs()) {
		os.buffer << song << "\n";
	}
	os.buffer << "playlist_end\n";
}

unsigned playlist_state_get_hash(const MockPlaylist &playlist,
                                 [[maybe_unused]] const MockPlayerControl &pc) noexcept {
	return playlist.GetHash();
}

bool playlist_state_restore([[maybe_unused]] const StateFileConfig &config,
                           const char *line,
                           [[maybe_unused]] MockFileLineReader &file,
                           [[maybe_unused]] const void *song_loader,
                           [[maybe_unused]] MockPlaylist &playlist,
                           [[maybe_unused]] MockPlayerControl &pc) {
	if (std::strncmp(line, "state:", 6) == 0 ||
	    std::strncmp(line, "playlist_begin", 14) == 0 ||
	    std::strncmp(line, "playlist_end", 12) == 0 ||
	    std::strncmp(line, "mixrampdelay:", 13) == 0 ||
	    std::strncmp(line, "lastloadedplaylist:", 19) == 0 ||
	    (line[0] >= '0' && line[0] <= '9' && std::strchr(line, ':'))) {
		g_saved_playlist_states.push_back(line);
		return true;
	}
	return false;
}

#ifdef ENABLE_DATABASE
static unsigned g_storage_version = 2000;

unsigned storage_state_get_hash([[maybe_unused]] const void *instance) noexcept {
	return g_storage_version;
}

void storage_state_save(MockBufferedOutputStream &os,
                        [[maybe_unused]] const void *instance) {
	os.buffer << "storage: mock_storage\n";
}

bool storage_state_restore(const char *line,
                          [[maybe_unused]] MockFileLineReader &file,
                          [[maybe_unused]] void *instance) {
	if (std::strncmp(line, "storage:", 8) == 0) {
		return true;
	}
	return false;
}
#endif

// =============================================================================
// Mock Partition and Instance (Minimal Implementation)
// =============================================================================

/**
 * Minimal mock partition for testing
 */
struct MockPartition {
	void *instance_ptr;
	std::string name;
	MockMixerMemento mixer_memento;
	MockMultipleOutputs outputs;
	MockPlaylist playlist;
	MockPlayerControl pc;

	MockPartition(void *inst, const char *partition_name)
		: instance_ptr(inst), name(partition_name) {}

	void UpdateEffectiveReplayGainMode() noexcept {
		// No-op for testing
	}
};

/**
 * Minimal mock instance for testing
 */
struct MockInstance {
	std::list<MockPartition> partitions;

	MockPartition* FindPartition(const char *name) noexcept {
		for (auto &partition : partitions) {
			if (partition.name == name) {
				return &partition;
			}
		}
		return nullptr;
	}

#ifdef ENABLE_DATABASE
	void* GetDatabase() noexcept {
		return nullptr;
	}

	void* storage = nullptr;
#endif
};

// =============================================================================
// Test Fixture
// =============================================================================

class StateFileTest : public ::testing::Test {
protected:
	MockInstance mock_instance;
	EventLoop event_loop;
	MockPartition *default_partition;

	void SetUp() override {
		// Reset global state
		g_audio_output_version = 1000;
		g_saved_output_states.clear();
		g_saved_playlist_states.clear();
#ifdef ENABLE_DATABASE
		g_storage_version = 2000;
#endif

		// Create default partition
		mock_instance.partitions.emplace_back(&mock_instance, "default");
		default_partition = &mock_instance.partitions.front();
	}

	void TearDown() override {
		mock_instance.partitions.clear();
	}

	/**
	 * Helper to create a StateFileConfig for testing
	 */
	StateFileConfig CreateTestConfig(const char *path_str = "/tmp/test_state") const {
		StateFileConfig config;
		config.path = AllocatedPath::FromFS(path_str);
		config.interval = std::chrono::seconds(120);
		config.restore_paused = false;
		return config;
	}

	/**
	 * Helper to setup partition with known state
	 */
	void SetupPartitionState(MockPartition &partition,
	                         int volume = 50,
	                         std::vector<std::string> outputs = {},
	                         std::vector<std::string> songs = {}) {
		partition.mixer_memento.SetSoftwareVolume(volume);
		
		for (auto &output : outputs) {
			partition.outputs.AddOutputState(std::move(output));
		}
		
		for (auto &song : songs) {
			partition.playlist.AddSong(std::move(song));
		}
	}
};

// =============================================================================
// Write Tests
// =============================================================================

TEST_F(StateFileTest, WriteEmptyState) {
	auto config = CreateTestConfig();
	MockBufferedOutputStream mock_os;

	// Write empty partition state
	default_partition->mixer_memento.SaveSoftwareVolumeState(mock_os);
	audio_output_state_save(mock_os, default_partition->outputs);
	playlist_state_save(mock_os, default_partition->playlist, default_partition->pc);

	std::string output = mock_os.GetOutput();
	
	EXPECT_TRUE(output.find("sw_volume:") != std::string::npos);
	EXPECT_TRUE(output.find("state: stop") != std::string::npos);
	EXPECT_TRUE(output.find("playlist_begin") != std::string::npos);
	EXPECT_TRUE(output.find("playlist_end") != std::string::npos);
}

TEST_F(StateFileTest, WriteSinglePartitionState) {
	SetupPartitionState(*default_partition,
	                    17,  // volume
	                    {"audio_device_state:0:MyTestOutput", "audio_device_state:1:Kitchen"},
	                    {"0:path/to/song.flac"});

	MockBufferedOutputStream mock_os;

	// Simulate Write() method behavior for single partition
	default_partition->mixer_memento.SaveSoftwareVolumeState(mock_os);
	audio_output_state_save(mock_os, default_partition->outputs);
	playlist_state_save(mock_os, default_partition->playlist, default_partition->pc);

	std::string output = mock_os.GetOutput();

	EXPECT_TRUE(output.find("sw_volume: 17") != std::string::npos);
	EXPECT_TRUE(output.find("audio_device_state:0:MyTestOutput") != std::string::npos);
	EXPECT_TRUE(output.find("audio_device_state:1:Kitchen") != std::string::npos);
	EXPECT_TRUE(output.find("0:path/to/song.flac") != std::string::npos);
}

TEST_F(StateFileTest, WriteMultiplePartitions) {
	// Setup first partition
	SetupPartitionState(*default_partition,
	                    17,
	                    {"audio_device_state:0:MyTestOutput"},
	                    {"0:path/to/song.flac"});

	// Add second partition
	mock_instance.partitions.emplace_back(&mock_instance, "TestPartition");
	auto &second_partition = mock_instance.partitions.back();
	SetupPartitionState(second_partition,
	                    10,
	                    {"audio_device_state:1:MyTestOutput"},
	                    {"0:path/to/another_song.flac"});

	MockBufferedOutputStream mock_os;

	// Simulate Write() method behavior for multiple partitions
	bool first = true;
	for (auto &partition : mock_instance.partitions) {
		if (!first) {
			mock_os.Fmt("partition: {}\n", partition.name);
		}
		first = false;

		partition.mixer_memento.SaveSoftwareVolumeState(mock_os);
		audio_output_state_save(mock_os, partition.outputs);
		playlist_state_save(mock_os, partition.playlist, partition.pc);
	}

	std::string output = mock_os.GetOutput();

	// First partition (no prefix)
	EXPECT_TRUE(output.find("sw_volume: 17") != std::string::npos);
	EXPECT_TRUE(output.find("0:path/to/song.flac") != std::string::npos);

	// Second partition (with prefix)
	EXPECT_TRUE(output.find("partition: TestPartition") != std::string::npos);
	
	// Verify partition order - second partition info comes after partition marker
	std::size_t partition_pos = output.find("partition: TestPartition");
	ASSERT_NE(partition_pos, std::string::npos);
	
	std::size_t volume10_pos = output.find("sw_volume: 10");
	ASSERT_NE(volume10_pos, std::string::npos);
	EXPECT_GT(volume10_pos, partition_pos);

	std::size_t song2_pos = output.find("0:path/to/another_song.flac");
	ASSERT_NE(song2_pos, std::string::npos);
	EXPECT_GT(song2_pos, partition_pos);
}

// =============================================================================
// Read Tests
// =============================================================================

TEST_F(StateFileTest, ReadSinglePartitionState) {
	std::vector<std::string> input_lines = {
		"sw_volume: 17",
		"audio_device_state:0:MyTestOutput",
		"audio_device_state:1:Kitchen",
		"state: stop",
		"playlist_begin",
		"0:path/to/song.flac",
		"playlist_end"
	};

	MockFileLineReader file_reader(input_lines);

	// Simulate Read() method behavior
	const char *line;
	while ((line = file_reader.ReadLine()) != nullptr) {
		bool success = default_partition->mixer_memento.LoadSoftwareVolumeState(
			line, default_partition->outputs) ||
			audio_output_state_read(line, default_partition->outputs, default_partition) ||
			playlist_state_restore(CreateTestConfig(), line, file_reader, nullptr,
			                      default_partition->playlist, default_partition->pc);
		
		EXPECT_TRUE(success) << "Failed to parse line: " << line;
	}

	// Verify state was loaded
	EXPECT_EQ(default_partition->mixer_memento.GetSoftwareVolume(), 17);
	EXPECT_EQ(g_saved_output_states.size(), 2);
	EXPECT_EQ(g_saved_playlist_states.size(), 4);  // state, playlist_begin, song, playlist_end
}

TEST_F(StateFileTest, ReadMultiplePartitions) {
	std::vector<std::string> input_lines = {
		"sw_volume: 17",
		"audio_device_state:0:MyTestOutput",
		"state: stop",
		"playlist_begin",
		"0:path/to/song.flac",
		"playlist_end",
		"partition: TestPartition",
		"sw_volume: 10",
		"audio_device_state:1:Kitchen",
		"state: play",
		"playlist_begin",
		"0:path/to/another_song.flac",
		"playlist_end"
	};

	MockFileLineReader file_reader(input_lines);
	MockPartition *current_partition = default_partition;

	// Simulate Read() method behavior with partition switching
	const char *line;
	while ((line = file_reader.ReadLine()) != nullptr) {
		// Check for partition switch
		const char *partition_name = nullptr;
		if (std::strncmp(line, "partition: ", 11) == 0) {
			partition_name = line + 11;
			
			// Try to find existing partition
			MockPartition *new_partition = mock_instance.FindPartition(partition_name);
			if (new_partition == nullptr) {
				// Create new partition
				mock_instance.partitions.emplace_back(&mock_instance, partition_name);
				new_partition = &mock_instance.partitions.back();
				new_partition->UpdateEffectiveReplayGainMode();
			}
			current_partition = new_partition;
			continue;
		}

		// Process state line for current partition
		bool success = current_partition->mixer_memento.LoadSoftwareVolumeState(
			line, current_partition->outputs) ||
			audio_output_state_read(line, current_partition->outputs, current_partition) ||
			playlist_state_restore(CreateTestConfig(), line, file_reader, nullptr,
			                      current_partition->playlist, current_partition->pc);
		
		EXPECT_TRUE(success) << "Failed to parse line: " << line;
	}

	// Verify both partitions exist
	EXPECT_EQ(mock_instance.partitions.size(), 2);
	
	auto *first_partition = &mock_instance.partitions.front();
	auto *second_partition = mock_instance.FindPartition("TestPartition");
	
	ASSERT_NE(second_partition, nullptr);
	EXPECT_EQ(second_partition->name, "TestPartition");
	
	// Verify first partition state
	EXPECT_EQ(first_partition->mixer_memento.GetSoftwareVolume(), 17);
	
	// Verify second partition state
	EXPECT_EQ(second_partition->mixer_memento.GetSoftwareVolume(), 10);
}

TEST_F(StateFileTest, ReadUnrecognizedLines) {
	std::vector<std::string> input_lines = {
		"sw_volume: 50",
		"invalid_line_format",
		"state: stop",
		"another_invalid: line"
	};

	MockFileLineReader file_reader(input_lines);
	std::vector<std::string> unrecognized_lines;

	const char *line;
	while ((line = file_reader.ReadLine()) != nullptr) {
		bool success = default_partition->mixer_memento.LoadSoftwareVolumeState(
			line, default_partition->outputs) ||
			audio_output_state_read(line, default_partition->outputs, default_partition) ||
			playlist_state_restore(CreateTestConfig(), line, file_reader, nullptr,
			                      default_partition->playlist, default_partition->pc);
		
		if (!success) {
			unrecognized_lines.push_back(line);
		}
	}

	// Should have 2 unrecognized lines
	EXPECT_EQ(unrecognized_lines.size(), 2);
	EXPECT_EQ(unrecognized_lines[0], "invalid_line_format");
	EXPECT_EQ(unrecognized_lines[1], "another_invalid: line");
}

// =============================================================================
// Round-Trip Tests
// =============================================================================

TEST_F(StateFileTest, RoundTripSinglePartition) {
	// Setup initial state
	SetupPartitionState(*default_partition,
	                    42,
	                    {"audio_device_state:0:TestOutput"},
	                    {"0:song1.flac", "1:song2.flac"});

	// Write state
	MockBufferedOutputStream write_stream;
	default_partition->mixer_memento.SaveSoftwareVolumeState(write_stream);
	audio_output_state_save(write_stream, default_partition->outputs);
	playlist_state_save(write_stream, default_partition->playlist, default_partition->pc);

	std::string written_data = write_stream.GetOutput();

	// Clear partition state
	mock_instance.partitions.clear();
	mock_instance.partitions.emplace_back(&mock_instance, "default");
	default_partition = &mock_instance.partitions.front();

	// Parse written data back
	std::istringstream iss(written_data);
	std::string line_str;
	std::vector<std::string> lines;
	while (std::getline(iss, line_str)) {
		lines.push_back(line_str);
	}

	MockFileLineReader file_reader(lines);
	const char *line;
	while ((line = file_reader.ReadLine()) != nullptr) {
		default_partition->mixer_memento.LoadSoftwareVolumeState(
			line, default_partition->outputs);
		audio_output_state_read(line, default_partition->outputs, default_partition);
		playlist_state_restore(CreateTestConfig(), line, file_reader, nullptr,
		                      default_partition->playlist, default_partition->pc);
	}

	// Verify restored state
	EXPECT_EQ(default_partition->mixer_memento.GetSoftwareVolume(), 42);
}

// =============================================================================
// Version Tracking Tests
// =============================================================================

TEST_F(StateFileTest, IsModifiedDetectsVolumeChange) {
	// Initial state
	unsigned initial_volume_hash = default_partition->mixer_memento.GetSoftwareVolumeStateHash();
	unsigned initial_output_version = audio_output_state_get_version();
	unsigned initial_playlist_hash = playlist_state_get_hash(
		default_partition->playlist, default_partition->pc);

	// Modify volume
	default_partition->mixer_memento.SetSoftwareVolume(75);

	unsigned new_volume_hash = default_partition->mixer_memento.GetSoftwareVolumeStateHash();

	// Should be modified
	EXPECT_NE(initial_volume_hash, new_volume_hash);
	EXPECT_EQ(initial_output_version, audio_output_state_get_version());
	EXPECT_EQ(initial_playlist_hash, playlist_state_get_hash(
		default_partition->playlist, default_partition->pc));
}

TEST_F(StateFileTest, IsModifiedDetectsPlaylistChange) {
	unsigned initial_playlist_hash = playlist_state_get_hash(
		default_partition->playlist, default_partition->pc);

	// Modify playlist
	default_partition->playlist.AddSong("new_song.flac");

	unsigned new_playlist_hash = playlist_state_get_hash(
		default_partition->playlist, default_partition->pc);

	// Should be modified
	EXPECT_NE(initial_playlist_hash, new_playlist_hash);
}

TEST_F(StateFileTest, IsModifiedDetectsOutputChange) {
	unsigned initial_output_version = audio_output_state_get_version();

	// Modify output version
	g_audio_output_version++;

	unsigned new_output_version = audio_output_state_get_version();

	// Should be modified
	EXPECT_NE(initial_output_version, new_output_version);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}