/*
 * SPDX-License-Identifier: MIT
 */
#include "motion_safety_store.h"

#include <cstdint>

#ifndef COMPANION_HOST_TEST
#include <esp_log.h>
#include <nvs.h>
#endif

namespace stackchan::companion {
namespace {

#ifndef COMPANION_HOST_TEST
static_assert(NVS_NS_NAME_MAX_SIZE - 1 == kMotionSafetyNvsNameMaximumLength);
#endif

constexpr char kProfileVersionKey[]    = "profile_version";
constexpr char kCalibratedKey[]        = "calibrated";
constexpr char kYawCenterKey[]         = "yaw_center";
constexpr char kPitchCenterKey[]       = "pitch_center";
constexpr char kYawDirectionKey[]      = "yaw_direction";
constexpr char kPitchDirectionKey[]    = "pitch_direction";
constexpr char kYawAmplitudeKey[]      = "yaw_amplitude";
constexpr char kPitchAmplitudeKey[]    = "pitch_amplitude";
constexpr char kYawLimitMinimumKey[]   = "yaw_limit_min";
constexpr char kYawLimitMaximumKey[]   = "yaw_limit_max";
constexpr char kPitchLimitMinimumKey[] = "pitch_limit_min";
constexpr char kPitchLimitMaximumKey[] = "pitch_limit_max";

static_assert(validMotionSafetyNvsName(kProfileVersionKey));
static_assert(validMotionSafetyNvsName(kCalibratedKey));
static_assert(validMotionSafetyNvsName(kYawCenterKey));
static_assert(validMotionSafetyNvsName(kPitchCenterKey));
static_assert(validMotionSafetyNvsName(kYawDirectionKey));
static_assert(validMotionSafetyNvsName(kPitchDirectionKey));
static_assert(validMotionSafetyNvsName(kYawAmplitudeKey));
static_assert(validMotionSafetyNvsName(kPitchAmplitudeKey));
static_assert(validMotionSafetyNvsName(kYawLimitMinimumKey));
static_assert(validMotionSafetyNvsName(kYawLimitMaximumKey));
static_assert(validMotionSafetyNvsName(kPitchLimitMinimumKey));
static_assert(validMotionSafetyNvsName(kPitchLimitMaximumKey));

ProjectMotionSafety lockedProfile()
{
    // Aggregate initialization keeps all safety fields at their locked
    // defaults, including zero amplitudes and calibrated=false.
    return {};
}

bool samePersistedProfile(const ProjectMotionSafety& left, const ProjectMotionSafety& right)
{
    return left.calibrated == right.calibrated && left.yawCenter == right.yawCenter &&
           left.pitchCenter == right.pitchCenter && left.yawDirection == right.yawDirection &&
           left.pitchDirection == right.pitchDirection && left.yawAmplitude == right.yawAmplitude &&
           left.pitchAmplitude == right.pitchAmplitude && left.yawLimitMinimum == right.yawLimitMinimum &&
           left.yawLimitMaximum == right.yawLimitMaximum && left.pitchLimitMinimum == right.pitchLimitMinimum &&
           left.pitchLimitMaximum == right.pitchLimitMaximum;
}

bool forceLocked(detail::MotionSafetyStorage& storage)
{
    return storage.writeBool(kCalibratedKey, false) && storage.commit();
}

#ifndef COMPANION_HOST_TEST

constexpr char kLogTag[] = "MotionSafetyStore";

class NvsMotionSafetyStorage final : public detail::MotionSafetyStorage {
public:
    explicit NvsMotionSafetyStorage(nvs_open_mode_t mode)
        : result_(nvs_open(kMotionSafetyStorageNamespace, mode, &handle_))
    {
    }

    ~NvsMotionSafetyStorage() override
    {
        if (isOpen()) {
            nvs_close(handle_);
        }
    }

    bool isOpen() const
    {
        return result_ == ESP_OK;
    }
    esp_err_t result() const
    {
        return result_;
    }

    bool readInt(const char* key, int& value) override
    {
        std::int32_t stored = 0;
        if (!isOpen() || nvs_get_i32(handle_, key, &stored) != ESP_OK) {
            return false;
        }
        value = stored;
        return true;
    }

    bool readBool(const char* key, bool& value) override
    {
        std::uint8_t stored = 0;
        if (!isOpen() || nvs_get_u8(handle_, key, &stored) != ESP_OK) {
            return false;
        }
        value = stored != 0;
        return true;
    }

    bool writeInt(const char* key, int value) override
    {
        return isOpen() && nvs_set_i32(handle_, key, value) == ESP_OK;
    }

    bool writeBool(const char* key, bool value) override
    {
        return isOpen() && nvs_set_u8(handle_, key, value ? 1 : 0) == ESP_OK;
    }

    bool commit() override
    {
        return isOpen() && nvs_commit(handle_) == ESP_OK;
    }

    bool erase(const char* key)
    {
        if (!isOpen()) {
            return false;
        }
        const esp_err_t result = nvs_erase_key(handle_, key);
        return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
    }

private:
    nvs_handle_t handle_ = 0;
    esp_err_t result_    = ESP_FAIL;
};

void logOpenFailure(const char* operation, esp_err_t result)
{
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(kLogTag, "%s failed to open motion profile storage: %s", operation, esp_err_to_name(result));
    }
}

#endif

}  // namespace

namespace detail {

bool loadProjectMotionSafetyFromStorage(MotionSafetyStorage& storage, ProjectMotionSafety& safety)
{
    safety = lockedProfile();

    int version     = 0;
    bool calibrated = false;
    if (!storage.readInt(kProfileVersionKey, version) || version != kMotionSafetyProfileVersion ||
        !storage.readBool(kCalibratedKey, calibrated) || !calibrated) {
        return false;
    }

    ProjectMotionSafety candidate;
    candidate.calibrated = true;
    if (!storage.readInt(kYawCenterKey, candidate.yawCenter) ||
        !storage.readInt(kPitchCenterKey, candidate.pitchCenter) ||
        !storage.readInt(kYawDirectionKey, candidate.yawDirection) ||
        !storage.readInt(kPitchDirectionKey, candidate.pitchDirection) ||
        !storage.readInt(kYawAmplitudeKey, candidate.yawAmplitude) ||
        !storage.readInt(kPitchAmplitudeKey, candidate.pitchAmplitude) ||
        !storage.readInt(kYawLimitMinimumKey, candidate.yawLimitMinimum) ||
        !storage.readInt(kYawLimitMaximumKey, candidate.yawLimitMaximum) ||
        !storage.readInt(kPitchLimitMinimumKey, candidate.pitchLimitMinimum) ||
        !storage.readInt(kPitchLimitMaximumKey, candidate.pitchLimitMaximum) || !validProjectMotionSafety(candidate)) {
        return false;
    }

    safety = candidate;
    return true;
}

bool saveProjectMotionSafetyToStorage(MotionSafetyStorage& storage, const ProjectMotionSafety& safety)
{
    if (!validProjectMotionSafety(safety)) {
        return false;
    }

    // Each phase is committed separately. Any reset or write failure before
    // the final phase therefore leaves the last durable state locked.
    if (!forceLocked(storage)) {
        return false;
    }

    const bool fieldsWritten = storage.writeInt(kProfileVersionKey, kMotionSafetyProfileVersion) &&
                               storage.writeInt(kYawCenterKey, safety.yawCenter) &&
                               storage.writeInt(kPitchCenterKey, safety.pitchCenter) &&
                               storage.writeInt(kYawDirectionKey, safety.yawDirection) &&
                               storage.writeInt(kPitchDirectionKey, safety.pitchDirection) &&
                               storage.writeInt(kYawAmplitudeKey, safety.yawAmplitude) &&
                               storage.writeInt(kPitchAmplitudeKey, safety.pitchAmplitude) &&
                               storage.writeInt(kYawLimitMinimumKey, safety.yawLimitMinimum) &&
                               storage.writeInt(kYawLimitMaximumKey, safety.yawLimitMaximum) &&
                               storage.writeInt(kPitchLimitMinimumKey, safety.pitchLimitMinimum) &&
                               storage.writeInt(kPitchLimitMaximumKey, safety.pitchLimitMaximum) && storage.commit();
    if (!fieldsWritten || !storage.writeBool(kCalibratedKey, true) || !storage.commit()) {
        forceLocked(storage);
        return false;
    }

    ProjectMotionSafety persisted;
    if (!loadProjectMotionSafetyFromStorage(storage, persisted) || !samePersistedProfile(persisted, safety)) {
        forceLocked(storage);
        return false;
    }
    return true;
}

}  // namespace detail

ProjectMotionSafety loadProjectMotionSafety()
{
#ifdef COMPANION_HOST_TEST
    return lockedProfile();
#else
    NvsMotionSafetyStorage storage(NVS_READONLY);
    if (!storage.isOpen()) {
        logOpenFailure("load", storage.result());
        return lockedProfile();
    }
    ProjectMotionSafety safety;
    if (!detail::loadProjectMotionSafetyFromStorage(storage, safety)) {
        return lockedProfile();
    }
    return safety;
#endif
}

bool saveProjectMotionSafety(const ProjectMotionSafety& safety)
{
#ifdef COMPANION_HOST_TEST
    (void)safety;
    return false;
#else
    if (!validProjectMotionSafety(safety)) {
        return false;
    }

    NvsMotionSafetyStorage storage(NVS_READWRITE);
    if (!storage.isOpen()) {
        logOpenFailure("save", storage.result());
        return false;
    }
    return detail::saveProjectMotionSafetyToStorage(storage, safety);
#endif
}

bool clearProjectMotionSafety()
{
#ifdef COMPANION_HOST_TEST
    return true;
#else
    NvsMotionSafetyStorage storage(NVS_READWRITE);
    if (!storage.isOpen()) {
        logOpenFailure("clear", storage.result());
        return false;
    }
    if (!forceLocked(storage)) {
        ESP_LOGE(kLogTag, "failed to lock motion profile before clearing fields");
        return false;
    }
    const bool erased =
        storage.erase(kProfileVersionKey) && storage.erase(kYawCenterKey) && storage.erase(kPitchCenterKey) &&
        storage.erase(kYawDirectionKey) && storage.erase(kPitchDirectionKey) && storage.erase(kYawAmplitudeKey) &&
        storage.erase(kPitchAmplitudeKey) && storage.erase(kYawLimitMinimumKey) && storage.erase(kYawLimitMaximumKey) &&
        storage.erase(kPitchLimitMinimumKey) && storage.erase(kPitchLimitMaximumKey) && storage.commit();
    if (!erased) {
        ESP_LOGE(kLogTag, "motion profile remained locked but field cleanup failed");
    }
    return erased;
#endif
}

}  // namespace stackchan::companion
