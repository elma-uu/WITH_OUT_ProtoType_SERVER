#pragma once
#include "NetCommon.h"
#include "common.h"
#include <sql.h>
#include <sqlext.h>
#include <mutex>
#include <string>
#include <vector>

namespace Wop
{

    enum class AuthResult
    {
        Success,
        AccountNotFound,  // Authenticate(): no account with this username
        WrongPassword,    // Authenticate(): account exists, password didn't match
        UsernameTaken,    // Register(): an account with this username already exists
        DatabaseError,    // not connected, or an ODBC call failed
    };

    // One placed item in the grid inventory. itemId is the item Data Asset's
    // own object name (e.g. "DA_Item_AK47"), not a numeric id -- see the
    // client's inventory.fbs comment for why.
    struct InventoryItemRecord
    {
        std::string itemId;
        int16_t gridX = 0;
        int16_t gridY = 0;
        bool rotated = false;
        int16_t stackCount = 1;
    };

    class Database
    {
    public:
        static Database& Get();

        bool Connect();
        bool IsConnected() const { return connected_; }

        /*-------------------
         계정
        -------------------*/
        // Login: username must already exist and password must match.
        AuthResult Authenticate(const std::string& username, const std::string& password, int& outAccountId);

        // Sign up: username must not already exist. Creates the account.
        AuthResult Register(const std::string& username, const std::string& password, int& outAccountId);

        /*-------------------
         진행 상황 (위치/장착 무기)
        -------------------*/
        bool LoadProgress(int accountId, ProtoType::Net::Vec3& outPosition, ProtoType::Net::Rotator& outLook, uint8_t& outWeaponType);

        // Upsert.
        bool SaveProgress(int accountId, const ProtoType::Net::Vec3& position, const ProtoType::Net::Rotator& look, uint8_t weaponType);

        /*-------------------
         인벤토리 (그리드)
        -------------------*/
        bool LoadInventory(int accountId, std::vector<InventoryItemRecord>& outItems);

        // Full replace: deletes this account's existing rows and inserts
        // outItems fresh, inside one transaction -- the client always sends
        // its complete current grid (see C2S_SaveInventory), never a diff.
        bool SaveInventory(int accountId, const std::vector<InventoryItemRecord>& items);

    private:
        Database() = default;
        ~Database();
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        void LogDiag(const char* context, SQLSMALLINT handleType, SQLHANDLE handle) const;

        SQLHENV henv_ = SQL_NULL_HENV;
        SQLHDBC hdbc_ = SQL_NULL_HDBC;
        mutable std::mutex lock_;
        bool connected_ = false;
    };
}
