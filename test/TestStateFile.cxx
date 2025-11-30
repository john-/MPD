#include <gtest/gtest.h>
#include <StateFile.hxx>
#include <config/Data.hxx>
#include "Instance.hxx"
#include "Partition.hxx"
#include "event/Loop.hxx"
#include "config/PartitionConfig.hxx"

// Declare the global_instance (defined in TestGlobalInit.cxx)
extern Instance *global_instance;

class TestStateFile : public ::testing::Test {
protected:
    // These will be created fresh for each test
    std::unique_ptr<Instance> instance;
    std::unique_ptr<StateFile> state_file;
    
    void SetUp() override {
        // Create a new Instance for this test
        instance = std::make_unique<Instance>();
        
        // Set the global_instance pointer (required by some MPD code)
        global_instance = instance.get();
        
        // Create configuration objects
        ConfigData config_data;
        PartitionConfig partition_config(config_data);
        
        // Add a partition to the instance
        // Note: We're using emplace_back to construct in-place
        instance->partitions.emplace_back(
            *instance,
            "test_partition",
            partition_config
        );
        
        // Get reference to the partition we just created
        Partition &test_partition = instance->partitions.front();
        
        // Create StateFile configuration
        StateFileConfig state_config(config_data);
        
        // Create the StateFile to test
        // Note: Use instance->event_loop (not a separate dummy_event_loop)
        state_file = std::make_unique<StateFile>(
            std::move(state_config),
            test_partition,
            instance->event_loop
        );
    }

    void TearDown() override {
        // Clean up in reverse order
        state_file.reset();
        
        // Clear the global instance
        global_instance = nullptr;
        instance.reset();
    }
};

TEST_F(TestStateFile, DummyTest) {
    // This is a placeholder test. Actual tests should be implemented here.
    ASSERT_TRUE(state_file != nullptr);
    ASSERT_TRUE(instance != nullptr);
    ASSERT_FALSE(instance->partitions.empty());
}

TEST_F(TestStateFile, CanCreate) {
    ASSERT_NE(state_file, nullptr);
}

TEST_F(TestStateFile, PartitionExists) {
    ASSERT_FALSE(instance->partitions.empty());
    ASSERT_EQ(instance->partitions.front().name, "test_partition");
}
