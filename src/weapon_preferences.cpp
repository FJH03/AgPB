// AgPB GPL-3.0-only. Config keys and index mapping from SyPB 4c364fb
// source/interface.cpp general.cfg. See weapon_selection.h for attribution.
#include <climits>
#include <filesystem.h>
#include <tier1/strtools.h>
#include <tier1/utlvector.h>
#include "weapon_selection.h"

namespace AgPBWeaponSelect {
namespace {
bool ParseNumbers(const char *cursor, int *values, int count, int maximum)
{
    for (int i = 0; i < count; ++i) {
        while (V_isspace(*cursor)) ++cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        int value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            const int digit = *cursor++ - '0';
            if (value > (maximum - digit) / 10) return false;
            value = value * 10 + digit;
        }
        values[i] = value;
        while (V_isspace(*cursor)) ++cursor;
        if (i + 1 == count) return *cursor == '\0';
        if (*cursor++ != ',') return false;
    }
    return false;
}

int ReadRows(char *text, Detail::Preferences &preferences)
{
    const char *const personalityKeys[] = { "PersonalityNormal", "PersonalityRusher", "PersonalityCareful" };
    int invalid = 0;
    char *next = nullptr;
    for (char *line = text; line != nullptr; line = next) {
        next = V_strstr(line, "\n");
        if (next != nullptr) *next++ = '\0';
        char *comment = V_strstr(line, "//");
        if (comment != nullptr) *comment = '\0';
        char *separator = V_strstr(line, "=");
        if (separator == nullptr) continue;
        *separator++ = '\0';
        V_StrTrim(line);

        for (int i = 0; i < 3; ++i) {
            if (V_strcmp(line, personalityKeys[i]) != 0) continue;
            int values[WeaponCount];
            if (ParseNumbers(separator, values, WeaponCount, WeaponCount - 1)) {
                for (int j = 0; j < WeaponCount; ++j)
                    preferences.orders[i][j] = static_cast<Weapon>(values[j]);
            } else {
                ++invalid;
            }
            break;
        }

        const bool chance = V_strcmp(line, "GrenadePercent") == 0;
        if (chance || V_strcmp(line, "GrenadeMoney") == 0) {
            int values[GrenadeCount];
            if (ParseNumbers(separator, values, GrenadeCount, chance ? 100 : INT_MAX)) {
                int *destination = chance ? preferences.grenadeChance : preferences.grenadeMoney;
                for (int i = 0; i < GrenadeCount; ++i) destination[i] = values[i];
            } else {
                ++invalid;
            }
        }
    }
    return invalid;
}
}

bool LoadPreferences(IFileSystem *fileSystem, int *invalidEntries)
{
    Detail::Preferences preferences;
    Detail::PreferenceStorage() = preferences;
    if (invalidEntries != nullptr) *invalidEntries = 0;
    if (fileSystem == nullptr) return false;

    const char *const paths[] = {
        "addons/AgPB/configs/weapons.cfg",
        "addons/AgPB/configs/general.cfg",
        "addons/AgPB/configs/defaults/weapons.cfg"
    };
    for (const char *path : paths) {
        FileHandle_t file = fileSystem->Open(path, "rb", "GAME");
        if (file == FILESYSTEM_INVALID_HANDLE) continue;
        const unsigned int size = fileSystem->Size(file);
        if (size > 64 * 1024) {
            fileSystem->Close(file);
            continue;
        }
        CUtlVector<char> buffer;
        const int length = static_cast<int>(size);
        buffer.SetCount(length + 1);
        const int bytesRead = fileSystem->Read(buffer.Base(), length, file);
        fileSystem->Close(file);
        if (bytesRead != length) continue;
        buffer[length] = '\0';
        if (V_strlen(buffer.Base()) != length) continue;

        char *text = buffer.Base();
        if (length >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
            static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
            text += 3;
        const int invalid = ReadRows(text, preferences);
        if (invalidEntries != nullptr) *invalidEntries = invalid;
        Detail::PreferenceStorage() = preferences;
        return true;
    }
    return false;
}
}
