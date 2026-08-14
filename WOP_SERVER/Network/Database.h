#pragma once
#include "NetCommon.h"
#include "common.h"
#include <sql.h>
#include <sqlext.h>
#include <mutex>
#include <string>

namespace Wop
{
    // Thin wrapper around a single ODBC connection to the game's MSSQL
    // database (WithStandGameDB on (localdb)\MSSQLLocalDB, Windows
    // Integrated auth). Not meant to be fast -- called at login time and
    // on an infrequent progress-save cadence, not per-frame -- so a single
    // shared connection guarded by a mutex is enough for a 2-player server.
    class Database
    {
    public:
        static Database& Get();

        // Opens the ODBC connection. Call once at server startup. Returns
        // false (and logs why) if the driver/instance isn't reachable --
        // callers should keep running without persistence rather than
        // refuse to start the game server over a DB hiccup.
        bool Connect();
        bool IsConnected() const { return connected_; }

        /*-------------------
         계정
        -------------------*/
        // Looks up `username`; if it doesn't exist yet, creates it with the
        // given password (auto-register on first login). If it exists,
        // verifies the password against the stored PBKDF2 hash. Returns
        // false (wrong password, or a DB error) without creating or
        // modifying anything.
        bool AuthenticateOrRegister(const std::string& username, const std::string& password, int& outAccountId);

        /*-------------------
         진행 상황 (위치/장착 무기)
        -------------------*/
        // False if this account has no saved PlayerProgress row yet.
        bool LoadProgress(int accountId, ProtoType::Net::Vec3& outPosition, ProtoType::Net::Rotator& outLook, uint8_t& outWeaponType);

        // Upsert.
        bool SaveProgress(int accountId, const ProtoType::Net::Vec3& position, const ProtoType::Net::Rotator& look, uint8_t weaponType);

    private:
        Database() = default;
        ~Database();
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // Logs the ODBC diagnostic record(s) for `handle` (of `handleType`)
        // after a non-SQL_SUCCESS return, prefixed with `context`.
        void LogDiag(const char* context, SQLSMALLINT handleType, SQLHANDLE handle) const;

        SQLHENV henv_ = SQL_NULL_HENV;
        SQLHDBC hdbc_ = SQL_NULL_HDBC;
        mutable std::mutex lock_;
        bool connected_ = false;
    };
}
