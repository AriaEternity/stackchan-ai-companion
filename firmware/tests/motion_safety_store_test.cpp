#include <companion/motion_safety_plan.h>
#include <companion/motion_safety_store.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace stackchan::companion;

namespace {

class FakeStorage final : public detail::MotionSafetyStorage {
public:
    bool readInt(const char* key, int& value) override
    {
        const std::string name(key);
        if (failReads) {
            return false;
        }
        const auto pending = pendingInts.find(name);
        if (pending != pendingInts.end()) {
            value = pending->second;
        } else {
            const auto stored = ints.find(name);
            if (stored == ints.end()) {
                return false;
            }
            value = stored->second;
        }
        if (corruptYawCenterRead && name == "yaw_center") {
            ++value;
        }
        return true;
    }

    bool readBool(const char* key, bool& value) override
    {
        const std::string name(key);
        if (failReads) {
            return false;
        }
        const auto pending = pendingBools.find(name);
        if (pending != pendingBools.end()) {
            value = pending->second;
            return true;
        }
        const auto stored = bools.find(name);
        if (stored == bools.end()) {
            return false;
        }
        value = stored->second;
        return true;
    }

    bool writeInt(const char* key, int value) override
    {
        if (failWrites) {
            return false;
        }
        pendingInts[key] = value;
        return true;
    }

    bool writeBool(const char* key, bool value) override
    {
        if (failWrites) {
            return false;
        }
        pendingBools[key] = value;
        return true;
    }

    bool commit() override
    {
        ++commitCount;
        if (failCommitNumber == commitCount) {
            return false;
        }
        for (const auto& item : pendingInts) {
            ints[item.first] = item.second;
        }
        for (const auto& item : pendingBools) {
            bools[item.first] = item.second;
        }
        pendingInts.clear();
        pendingBools.clear();
        return true;
    }

    bool isDurablyCalibrated() const
    {
        const auto value = bools.find("calibrated");
        return value != bools.end() && value->second;
    }

    std::unordered_map<std::string, int> ints;
    std::unordered_map<std::string, bool> bools;
    std::unordered_map<std::string, int> pendingInts;
    std::unordered_map<std::string, bool> pendingBools;
    int commitCount           = 0;
    int failCommitNumber      = 0;
    bool failWrites           = false;
    bool failReads            = false;
    bool corruptYawCenterRead = false;
};

void expectSamePersistedProfile(const ProjectMotionSafety& actual, const ProjectMotionSafety& expected)
{
    assert(actual.calibrated == expected.calibrated);
    assert(actual.yawCenter == expected.yawCenter);
    assert(actual.pitchCenter == expected.pitchCenter);
    assert(actual.yawDirection == expected.yawDirection);
    assert(actual.pitchDirection == expected.pitchDirection);
    assert(actual.yawAmplitude == expected.yawAmplitude);
    assert(actual.pitchAmplitude == expected.pitchAmplitude);
    assert(actual.yawLimitMinimum == expected.yawLimitMinimum);
    assert(actual.yawLimitMaximum == expected.yawLimitMaximum);
    assert(actual.pitchLimitMinimum == expected.pitchLimitMinimum);
    assert(actual.pitchLimitMaximum == expected.pitchLimitMaximum);
}

}  // namespace

int main()
{
    static_assert(validMotionSafetyNvsName(kMotionSafetyStorageNamespace));
    static_assert(sizeof(kMotionSafetyStorageNamespace) - 1 <= 15);

    // Host-facing production APIs have no NVS and stay locked by construction.
    const ProjectMotionSafety hostLoaded = loadProjectMotionSafety();
    assert(!hostLoaded.calibrated);
    assert(hostLoaded.yawAmplitude == 0);
    assert(hostLoaded.pitchAmplitude == 0);
    assert(!saveProjectMotionSafety(hostLoaded));

    const ProjectMotionSafety candidate = makeConservativeMotionSafetyProfile(1, 1);
    assert(validProjectMotionSafety(candidate));

    FakeStorage successful;
    assert(detail::saveProjectMotionSafetyToStorage(successful, candidate));
    assert(successful.commitCount == 3);
    assert(successful.isDurablyCalibrated());
    ProjectMotionSafety roundTrip;
    assert(detail::loadProjectMotionSafetyFromStorage(successful, roundTrip));
    expectSamePersistedProfile(roundTrip, candidate);

    FakeStorage missingField = successful;
    missingField.ints.erase("pitch_center");
    ProjectMotionSafety missingLoaded;
    assert(!detail::loadProjectMotionSafetyFromStorage(missingField, missingLoaded));
    assert(!missingLoaded.calibrated);

    FakeStorage invalidField          = successful;
    invalidField.ints["pitch_center"] = kProjectPitchLimitMaximum + 1;
    ProjectMotionSafety invalidLoaded;
    assert(!detail::loadProjectMotionSafetyFromStorage(invalidField, invalidLoaded));
    assert(!invalidLoaded.calibrated);

    FakeStorage failedFieldsCommit;
    failedFieldsCommit.failCommitNumber = 2;
    assert(!detail::saveProjectMotionSafetyToStorage(failedFieldsCommit, candidate));
    assert(!failedFieldsCommit.isDurablyCalibrated());

    FakeStorage mismatchedReadBack;
    mismatchedReadBack.corruptYawCenterRead = true;
    assert(!detail::saveProjectMotionSafetyToStorage(mismatchedReadBack, candidate));
    assert(!mismatchedReadBack.isDurablyCalibrated());

    FakeStorage failedInitialWrite;
    failedInitialWrite.failWrites = true;
    assert(!detail::saveProjectMotionSafetyToStorage(failedInitialWrite, candidate));
    assert(!failedInitialWrite.isDurablyCalibrated());

    assert(clearProjectMotionSafety());
    return 0;
}
