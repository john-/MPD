/*
 * Provides global_instance for unit tests
 * This is normally defined in Main.cxx, but tests need their own instance
 */

#include "Instance.hxx"

// Provide a global_instance pointer for code that references it
// Tests will need to create and initialize an actual Instance object
Instance *global_instance = nullptr;