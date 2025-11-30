// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include <StateFile.hxx>
#include "config/PartitionConfig.hxx"
#include <config/Data.hxx>
#include "Instance.hxx"
#include "Partition.hxx"
#include "event/Loop.hxx"
#include "fs/AllocatedPath.hxx"
#include "Log.hxx"
#include "LogBackend.hxx"

#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>

// Declare the global_instance (defined in TestGlobalInit.cxx)
extern Instance *global_instance;

// Global test environment to initialize logging once
class TestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Check if verbose logging is requested via environment variable
        const char* verbose = std::getenv("MPD_TEST_VERBOSE");
        if (verbose && (std::string(verbose) == "1" || std::string(verbose) == "true")) {
            // Initialize logging to stderr with verbose level
            SetLogThreshold(LogLevel::DEBUG);
            std::cerr << "MPD test logging enabled (DEBUG level)" << std::endl;
        }
    }
    
    void TearDown() override {
        // Cleanup if needed
    }
};

// Register the global test environment
[[maybe_unused]] static ::testing::Environment* const test_env =
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());

class TestStateFileRead : public ::testing::Test {
protected:
    std::unique_ptr<Instance> instance;
    std::unique_ptr<StateFile> state_file;
    std::filesystem::path temp_state_file;
    
    void SetUp() override {
        // Create a new Instance for this test
        instance = std::make_unique<Instance>();
        global_instance = instance.get();
        
        // Create a temporary state file path
        temp_state_file = std::filesystem::temp_directory_path() / 
                         ("mpd_test_state_" + std::to_string(getpid()) + ".txt");
        
        // Create empty configuration
        ConfigData config_data;
        
        // Create partition configuration and add partition
        PartitionConfig partition_config(config_data);
        instance->partitions.emplace_back(
            *instance,
            "default",
            partition_config
        );
        
        // Create StateFile configuration manually
        // We'll construct it with an empty ConfigData, then manually set the path
        StateFileConfig state_config(config_data);
        state_config.path = AllocatedPath::FromFS(temp_state_file.c_str());
        
        Partition &test_partition = instance->partitions.front();
        state_file = std::make_unique<StateFile>(
            std::move(state_config),
            test_partition,
            instance->event_loop
        );
    }

    void TearDown() override {
        // Clean up
        state_file.reset();
        global_instance = nullptr;
        instance.reset();
        
        // Remove temp file if it exists
        if (std::filesystem::exists(temp_state_file)) {
            std::filesystem::remove(temp_state_file);
        }
    }
    
    // Helper method to write test data to the state file
    void WriteStateFile(const std::string& content) {
        std::ofstream file(temp_state_file);
        ASSERT_TRUE(file.is_open()) << "Failed to create temp state file";
        file << content;
        file.close();
    }
    
    // Helper to read the partition state
    Partition& GetPartition(const std::string& name = "default") {
        auto* part = instance->FindPartition(name.c_str());
        EXPECT_NE(part, nullptr) << "Partition '" << name << "' not found";
        return *part;
    }
};

TEST_F(TestStateFileRead, ReadValidStateFile) {
    // Create a state file with valid content
    WriteStateFile(
        "sw_volume: 80\n"
        "state: pause\n"
        "random: 1\n"
        "repeat: 0\n"
    );
    
    // Read the state file
    state_file->Read();
    
    // Basic verification that Read() completed without crashing
    // (StateFile::Read() logs errors internally but doesn't throw)
    SUCCEED();
}

TEST_F(TestStateFileRead, ReadEmptyStateFile) {
    // Create an empty state file
    WriteStateFile("");
    
    // Should handle empty file gracefully
    state_file->Read();
    
    SUCCEED();
}

TEST_F(TestStateFileRead, ReadMalformedStateFile) {
    // Create a malformed state file
    WriteStateFile(
        "invalid line without colon\n"
        ":::too:many:colons:::\n"
        "incomplete:"
    );
    
    // Should handle malformed file gracefully (logs errors but doesn't crash)
    state_file->Read();
    
    SUCCEED();
}

TEST_F(TestStateFileRead, ReadNonExistentFile) {
    // Don't create a file - test reading when file doesn't exist
    // Should handle missing file gracefully (logs error but doesn't crash)
    state_file->Read();
    
    SUCCEED();
}

TEST_F(TestStateFileRead, ReadWithMultiplePartitions) {
    // Create a state file with multiple partitions
    WriteStateFile(
        "partition: secondary\n"
        "sw_volume: 50\n"
        "state: pause\n"
    );
    
    // Read the state file
    state_file->Read();
    
    // Verify that multiple partitions were created/loaded
    ASSERT_GE(instance->partitions.size(), 1);
    
    // The "secondary" partition should have been created
    auto* secondary = instance->FindPartition("secondary");
    EXPECT_NE(secondary, nullptr) << "Secondary partition should have been created";
    
    SUCCEED();
}

TEST_F(TestStateFileRead, ReadAndWrite) {
    // Create initial state file
    WriteStateFile(
        "sw_volume: 75\n"
        "state: play\n"
        "random: 1\n"
    );
    
    // Read it
    state_file->Read();
    
    // Write it back out
    state_file->Write();
    
    // Verify the file still exists
    ASSERT_TRUE(std::filesystem::exists(temp_state_file));
    
    // Basic check - file should not be empty after write
    auto file_size = std::filesystem::file_size(temp_state_file);
    EXPECT_GT(file_size, 0) << "State file should not be empty after Write()";
}