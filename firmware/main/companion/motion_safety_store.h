/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "command_scheduler.h"

#include <cstddef>

namespace stackchan::companion {

// The profile is deliberately separate from credentials and from the
// upstream servo zero-position keys. A missing, partial, or invalid profile
// always resolves to the locked ProjectMotionSafety default.
constexpr int kMotionSafetyProfileVersion               = 1;
constexpr char kMotionSafetyStorageNamespace[]          = "comp_motion";
constexpr std::size_t kMotionSafetyNvsNameMaximumLength = 15;

template <std::size_t N>
constexpr bool validMotionSafetyNvsName(const char (&name)[N])
{
    (void)name;
    return N > 1 && (N - 1) <= kMotionSafetyNvsNameMaximumLength;
}

static_assert(validMotionSafetyNvsName(kMotionSafetyStorageNamespace));

namespace detail {

// Small storage boundary used by the production NVS adapter and host fault
// injection tests. Every operation reports failure instead of aborting.
class MotionSafetyStorage {
public:
    virtual ~MotionSafetyStorage()                      = default;
    virtual bool readInt(const char* key, int& value)   = 0;
    virtual bool readBool(const char* key, bool& value) = 0;
    virtual bool writeInt(const char* key, int value)   = 0;
    virtual bool writeBool(const char* key, bool value) = 0;
    virtual bool commit()                               = 0;
};

bool loadProjectMotionSafetyFromStorage(MotionSafetyStorage& storage, ProjectMotionSafety& safety);
bool saveProjectMotionSafetyToStorage(MotionSafetyStorage& storage, const ProjectMotionSafety& safety);

}  // namespace detail

ProjectMotionSafety loadProjectMotionSafety();

// These functions are local firmware APIs only. No protocol command maps to
// them; the Setup workflow is the only production caller.
bool saveProjectMotionSafety(const ProjectMotionSafety& safety);
bool clearProjectMotionSafety();

}  // namespace stackchan::companion
