#include "Database.h"
#include <bcrypt.h>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace
{

    constexpr ULONG kHashLength = 32;
    constexpr ULONG kSaltLength = 16;
    constexpr ULONG kHashIterations = 100000;

    class StmtGuard
    {
    public:
        explicit StmtGuard(SQLHSTMT stmt) : stmt_(stmt) {}
        ~StmtGuard()
        {
            if (stmt_ != SQL_NULL_HSTMT)
                SQLFreeHandle(SQL_HANDLE_STMT, stmt_);
        }
        StmtGuard(const StmtGuard&) = delete;
        StmtGuard& operator=(const StmtGuard&) = delete;

    private:
        SQLHSTMT stmt_;
    };

    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return std::wstring();
        const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
        return wide;
    }

    bool GenerateSalt(uint8_t* outSalt, ULONG len)
    {
        return BCryptGenRandom(nullptr, outSalt, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
    }

    // PBKDF2-HMAC-SHA256(password, salt, kHashIterations) -> outHash[kHashLength].
    bool DeriveHash(const std::string& password, const uint8_t* salt, ULONG saltLen, uint8_t* outHash, ULONG hashLen)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
            return false;

        const NTSTATUS status = BCryptDeriveKeyPBKDF2(
            alg,
            reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
            const_cast<PUCHAR>(salt), saltLen,
            kHashIterations,
            outHash, hashLen,
            0);

        BCryptCloseAlgorithmProvider(alg, 0);
        return status == 0;
    }

    // Not early-exit on first mismatch, so comparison time doesn't leak
    // which byte differed.
    bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < len; ++i)
            diff |= a[i] ^ b[i];
        return diff == 0;
    }
}

namespace Wop
{
    Database& Database::Get()
    {
        static Database instance;
        return instance;
    }

    Database::~Database()
    {
        if (hdbc_ != SQL_NULL_HDBC)
        {
            SQLDisconnect(hdbc_);
            SQLFreeHandle(SQL_HANDLE_DBC, hdbc_);
        }
        if (henv_ != SQL_NULL_HENV)
        {
            SQLFreeHandle(SQL_HANDLE_ENV, henv_);
        }
    }

    void Database::LogDiag(const char* context, SQLSMALLINT handleType, SQLHANDLE handle) const
    {
        SQLCHAR state[6]{};
        SQLINTEGER nativeError = 0;
        SQLCHAR message[SQL_MAX_MESSAGE_LENGTH]{};
        SQLSMALLINT messageLen = 0;
        SQLSMALLINT record = 1;

        while (SQLGetDiagRecA(handleType, handle, record, state, &nativeError, message, sizeof(message), &messageLen) == SQL_SUCCESS)
        {
            std::printf("[DB] %s: [%s] %s\n", context, state, message);
            ++record;
        }
    }

    bool Database::Connect()
    {
        std::lock_guard<std::mutex> guard(lock_);

        if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv_) != SQL_SUCCESS)
            return false;
        SQLSetEnvAttr(henv_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);

        if (SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_) != SQL_SUCCESS)
        {
            LogDiag("SQLAllocHandle(DBC)", SQL_HANDLE_ENV, henv_);
            return false;
        }

        // Windows Integrated auth against the local SQL Server Express
        // LocalDB instance created for this project (WithStandGameDB).
        SQLCHAR connStr[] =
            "Driver={ODBC Driver 17 for SQL Server};"
            "Server=(localdb)\\MSSQLLocalDB;"
            "Database=WithStandGameDB;"
            "Trusted_Connection=yes;";

        const SQLRETURN ret = SQLDriverConnectA(hdbc_, nullptr, connStr, SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("SQLDriverConnect", SQL_HANDLE_DBC, hdbc_);
            return false;
        }

        connected_ = true;
        std::printf("[DB] Connected to WithStandGameDB\n");
        return true;
    }

    AuthResult Database::Authenticate(const std::string& username, const std::string& password, int& outAccountId)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return AuthResult::DatabaseError;

        const std::wstring wideUsername = Utf8ToWide(username);

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt);
        StmtGuard stmtGuard(stmt);

        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, wideUsername.size(), 0,
            const_cast<wchar_t*>(wideUsername.c_str()), 0, nullptr);

        const SQLRETURN selectRet = SQLExecDirectA(stmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "SELECT AccountId, PasswordHash, PasswordSalt FROM dbo.Accounts WHERE Username = ?")),
            SQL_NTS);
        if (selectRet != SQL_SUCCESS && selectRet != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("Authenticate: SELECT", SQL_HANDLE_STMT, stmt);
            return AuthResult::DatabaseError;
        }

        const SQLRETURN fetchRet = SQLFetch(stmt);
        if (fetchRet == SQL_NO_DATA)
        {
            std::printf("[DB] Login rejected for '%s': no such account\n", username.c_str());
            return AuthResult::AccountNotFound;
        }
        if (fetchRet != SQL_SUCCESS && fetchRet != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("Authenticate: fetch", SQL_HANDLE_STMT, stmt);
            return AuthResult::DatabaseError;
        }

        // Existing account: verify the password.
        SQLINTEGER accountId = 0;
        uint8_t storedHash[kHashLength]{};
        uint8_t storedSalt[kSaltLength]{};
        SQLLEN indicator = 0;

        SQLGetData(stmt, 1, SQL_C_LONG, &accountId, 0, &indicator);
        SQLGetData(stmt, 2, SQL_C_BINARY, storedHash, sizeof(storedHash), &indicator);
        SQLGetData(stmt, 3, SQL_C_BINARY, storedSalt, sizeof(storedSalt), &indicator);

        uint8_t computedHash[kHashLength]{};
        if (!DeriveHash(password, storedSalt, kSaltLength, computedHash, kHashLength))
            return AuthResult::DatabaseError;

        if (!ConstantTimeEquals(computedHash, storedHash, kHashLength))
        {
            std::printf("[DB] Login rejected for '%s': wrong password\n", username.c_str());
            return AuthResult::WrongPassword;
        }

        outAccountId = accountId;

        // Best-effort; a failure here shouldn't block the login.
        SQLHSTMT updateStmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &updateStmt);
        StmtGuard updateGuard(updateStmt);
        SQLBindParameter(updateStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accountId, 0, nullptr);
        SQLExecDirectA(updateStmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "UPDATE dbo.Accounts SET LastLoginAt = SYSUTCDATETIME() WHERE AccountId = ?")),
            SQL_NTS);

        return AuthResult::Success;
    }

    AuthResult Database::Register(const std::string& username, const std::string& password, int& outAccountId)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return AuthResult::DatabaseError;

        const std::wstring wideUsername = Utf8ToWide(username);

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt);
        StmtGuard stmtGuard(stmt);

        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, wideUsername.size(), 0,
            const_cast<wchar_t*>(wideUsername.c_str()), 0, nullptr);

        const SQLRETURN selectRet = SQLExecDirectA(stmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "SELECT AccountId FROM dbo.Accounts WHERE Username = ?")),
            SQL_NTS);
        if (selectRet != SQL_SUCCESS && selectRet != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("Register: SELECT", SQL_HANDLE_STMT, stmt);
            return AuthResult::DatabaseError;
        }

        const SQLRETURN fetchRet = SQLFetch(stmt);
        if (fetchRet == SQL_SUCCESS || fetchRet == SQL_SUCCESS_WITH_INFO)
        {
            std::printf("[DB] Register rejected for '%s': username taken\n", username.c_str());
            return AuthResult::UsernameTaken;
        }
        if (fetchRet != SQL_NO_DATA)
        {
            LogDiag("Register: fetch", SQL_HANDLE_STMT, stmt);
            return AuthResult::DatabaseError;
        }

        // Username is free: create the account.
        uint8_t salt[kSaltLength]{};
        uint8_t hash[kHashLength]{};
        if (!GenerateSalt(salt, kSaltLength) || !DeriveHash(password, salt, kSaltLength, hash, kHashLength))
            return AuthResult::DatabaseError;

        SQLHSTMT insertStmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &insertStmt);
        StmtGuard insertGuard(insertStmt);

        SQLBindParameter(insertStmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, wideUsername.size(), 0,
            const_cast<wchar_t*>(wideUsername.c_str()), 0, nullptr);
        SQLLEN hashLenInd = static_cast<SQLLEN>(kHashLength);
        SQLBindParameter(insertStmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, kHashLength, 0, hash, kHashLength, &hashLenInd);
        SQLLEN saltLenInd = static_cast<SQLLEN>(kSaltLength);
        SQLBindParameter(insertStmt, 3, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, kSaltLength, 0, salt, kSaltLength, &saltLenInd);

        const SQLRETURN insertRet = SQLExecDirectA(insertStmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "INSERT INTO dbo.Accounts (Username, PasswordHash, PasswordSalt) "
                "OUTPUT INSERTED.AccountId VALUES (?, ?, ?)")),
            SQL_NTS);
        if (insertRet != SQL_SUCCESS && insertRet != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("Register: INSERT", SQL_HANDLE_STMT, insertStmt);
            return AuthResult::DatabaseError;
        }

        if (SQLFetch(insertStmt) != SQL_SUCCESS)
        {
            LogDiag("Register: INSERT fetch id", SQL_HANDLE_STMT, insertStmt);
            return AuthResult::DatabaseError;
        }

        SQLINTEGER newAccountId = 0;
        SQLLEN indicator = 0;
        SQLGetData(insertStmt, 1, SQL_C_LONG, &newAccountId, 0, &indicator);
        outAccountId = newAccountId;

        std::printf("[DB] Registered new account '%s' (id=%d)\n", username.c_str(), newAccountId);
        return AuthResult::Success;
    }

    bool Database::LoadProgress(int accountId, ProtoType::Net::Vec3& outPosition, ProtoType::Net::Rotator& outLook, uint8_t& outWeaponType)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return false;

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt);
        StmtGuard stmtGuard(stmt);

        SQLINTEGER accId = accountId;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);

        const SQLRETURN ret = SQLExecDirectA(stmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "SELECT PosX, PosY, PosZ, LookPitch, LookYaw, LookRoll, WeaponType "
                "FROM dbo.PlayerProgress WHERE AccountId = ?")),
            SQL_NTS);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("LoadProgress: SELECT", SQL_HANDLE_STMT, stmt);
            return false;
        }

        const SQLRETURN fetchRet = SQLFetch(stmt);
        if (fetchRet == SQL_NO_DATA)
            return false;
        if (fetchRet != SQL_SUCCESS && fetchRet != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("LoadProgress: fetch", SQL_HANDLE_STMT, stmt);
            return false;
        }

        SQLDOUBLE px = 0, py = 0, pz = 0, pitch = 0, yaw = 0, roll = 0;
        SQLSMALLINT weaponType = 0;
        SQLLEN indicator = 0;
        SQLGetData(stmt, 1, SQL_C_DOUBLE, &px, 0, &indicator);
        SQLGetData(stmt, 2, SQL_C_DOUBLE, &py, 0, &indicator);
        SQLGetData(stmt, 3, SQL_C_DOUBLE, &pz, 0, &indicator);
        SQLGetData(stmt, 4, SQL_C_DOUBLE, &pitch, 0, &indicator);
        SQLGetData(stmt, 5, SQL_C_DOUBLE, &yaw, 0, &indicator);
        SQLGetData(stmt, 6, SQL_C_DOUBLE, &roll, 0, &indicator);
        SQLGetData(stmt, 7, SQL_C_SHORT, &weaponType, 0, &indicator);

        outPosition = ProtoType::Net::Vec3(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
        outLook = ProtoType::Net::Rotator(static_cast<float>(pitch), static_cast<float>(yaw), static_cast<float>(roll));
        outWeaponType = static_cast<uint8_t>(weaponType);
        return true;
    }

    bool Database::SaveProgress(int accountId, const ProtoType::Net::Vec3& position, const ProtoType::Net::Rotator& look, uint8_t weaponType)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return false;

        SQLINTEGER accId = accountId;
        SQLDOUBLE px = position.x(), py = position.y(), pz = position.z();
        SQLDOUBLE pitch = look.pitch(), yaw = look.yaw(), roll = look.roll();
        SQLSMALLINT wt = weaponType;

        SQLHSTMT updateStmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &updateStmt);
        {
            StmtGuard updateGuard(updateStmt);
            SQLBindParameter(updateStmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &px, 0, nullptr);
            SQLBindParameter(updateStmt, 2, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &py, 0, nullptr);
            SQLBindParameter(updateStmt, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &pz, 0, nullptr);
            SQLBindParameter(updateStmt, 4, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &pitch, 0, nullptr);
            SQLBindParameter(updateStmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &yaw, 0, nullptr);
            SQLBindParameter(updateStmt, 6, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &roll, 0, nullptr);
            SQLBindParameter(updateStmt, 7, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_TINYINT, 0, 0, &wt, 0, nullptr);
            SQLBindParameter(updateStmt, 8, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);

            const SQLRETURN ret = SQLExecDirectA(updateStmt,
                const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                    "UPDATE dbo.PlayerProgress SET PosX=?, PosY=?, PosZ=?, LookPitch=?, LookYaw=?, LookRoll=?, "
                    "WeaponType=?, UpdatedAt=SYSUTCDATETIME() WHERE AccountId=?")),
                SQL_NTS);
            // SQL_NO_DATA here just means the UPDATE matched zero rows (no
            // PlayerProgress row for this account yet) -- not an error, it's
            // exactly the "fall through to INSERT below" case.
            if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO && ret != SQL_NO_DATA)
            {
                LogDiag("SaveProgress: UPDATE", SQL_HANDLE_STMT, updateStmt);
                return false;
            }

            SQLLEN rowCount = 0;
            if (ret != SQL_NO_DATA)
                SQLRowCount(updateStmt, &rowCount);
            if (rowCount > 0)
                return true; // updated an existing row, done
        }

        // No existing row: insert one.
        SQLHSTMT insertStmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &insertStmt);
        StmtGuard insertGuard(insertStmt);

        SQLBindParameter(insertStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);
        SQLBindParameter(insertStmt, 2, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &px, 0, nullptr);
        SQLBindParameter(insertStmt, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &py, 0, nullptr);
        SQLBindParameter(insertStmt, 4, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &pz, 0, nullptr);
        SQLBindParameter(insertStmt, 5, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &pitch, 0, nullptr);
        SQLBindParameter(insertStmt, 6, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &yaw, 0, nullptr);
        SQLBindParameter(insertStmt, 7, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_FLOAT, 0, 0, &roll, 0, nullptr);
        SQLBindParameter(insertStmt, 8, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_TINYINT, 0, 0, &wt, 0, nullptr);

        const SQLRETURN ret = SQLExecDirectA(insertStmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "INSERT INTO dbo.PlayerProgress (AccountId, PosX, PosY, PosZ, LookPitch, LookYaw, LookRoll, WeaponType) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?)")),
            SQL_NTS);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("SaveProgress: INSERT", SQL_HANDLE_STMT, insertStmt);
            return false;
        }
        return true;
    }

    bool Database::LoadInventory(int accountId, std::vector<InventoryItemRecord>& outItems)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return false;

        outItems.clear();

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt);
        StmtGuard stmtGuard(stmt);

        SQLINTEGER accId = accountId;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);

        const SQLRETURN ret = SQLExecDirectA(stmt,
            const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                "SELECT ItemId, GridX, GridY, IsRotated, StackCount "
                "FROM dbo.PlayerInventoryItems WHERE AccountId = ?")),
            SQL_NTS);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
        {
            LogDiag("LoadInventory: SELECT", SQL_HANDLE_STMT, stmt);
            return false;
        }

        for (;;)
        {
            const SQLRETURN fetchRet = SQLFetch(stmt);
            if (fetchRet == SQL_NO_DATA)
                break;
            if (fetchRet != SQL_SUCCESS && fetchRet != SQL_SUCCESS_WITH_INFO)
            {
                LogDiag("LoadInventory: fetch", SQL_HANDLE_STMT, stmt);
                return false;
            }

            char itemIdBuf[65]{};
            SQLLEN itemIdLen = 0;
            SQLGetData(stmt, 1, SQL_C_CHAR, itemIdBuf, sizeof(itemIdBuf), &itemIdLen);

            InventoryItemRecord record;
            record.itemId.assign(itemIdBuf, itemIdLen > 0 ? static_cast<size_t>(itemIdLen) : 0);

            SQLSMALLINT gridX = 0, gridY = 0, stackCount = 0;
            uint8_t rotated = 0;
            SQLLEN indicator = 0;
            SQLGetData(stmt, 2, SQL_C_SHORT, &gridX, 0, &indicator);
            SQLGetData(stmt, 3, SQL_C_SHORT, &gridY, 0, &indicator);
            SQLGetData(stmt, 4, SQL_C_BIT, &rotated, 0, &indicator);
            SQLGetData(stmt, 5, SQL_C_SHORT, &stackCount, 0, &indicator);

            record.gridX = gridX;
            record.gridY = gridY;
            record.rotated = rotated != 0;
            record.stackCount = stackCount;
            outItems.push_back(std::move(record));
        }

        return true;
    }

    bool Database::SaveInventory(int accountId, const std::vector<InventoryItemRecord>& items)
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!connected_)
            return false;

        // Full replace, in one transaction: the client always sends its
        // complete current grid (see C2S_SaveInventory's comment), so
        // delete-then-reinsert can't lose anything an in-flight partial
        // write couldn't also have lost -- but the transaction at least
        // keeps a mid-failure from leaving the account's inventory half-empty.
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);

        bool ok = true;
        SQLINTEGER accId = accountId;

        {
            SQLHSTMT deleteStmt = SQL_NULL_HSTMT;
            SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &deleteStmt);
            StmtGuard deleteGuard(deleteStmt);
            SQLBindParameter(deleteStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);
            const SQLRETURN ret = SQLExecDirectA(deleteStmt,
                const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                    "DELETE FROM dbo.PlayerInventoryItems WHERE AccountId = ?")),
                SQL_NTS);
            if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO && ret != SQL_NO_DATA)
            {
                LogDiag("SaveInventory: DELETE", SQL_HANDLE_STMT, deleteStmt);
                ok = false;
            }
        }

        for (const InventoryItemRecord& item : items)
        {
            if (!ok)
                break;

            SQLHSTMT insertStmt = SQL_NULL_HSTMT;
            SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &insertStmt);
            StmtGuard insertGuard(insertStmt);

            SQLLEN itemIdLenInd = SQL_NTS;
            SQLBindParameter(insertStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                item.itemId.size(), 0, const_cast<char*>(item.itemId.c_str()), 0, &itemIdLenInd);
            SQLSMALLINT gridX = item.gridX;
            SQLSMALLINT gridY = item.gridY;
            uint8_t rotated = item.rotated ? 1 : 0;
            SQLSMALLINT stackCount = item.stackCount;
            SQLBindParameter(insertStmt, 2, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, &accId, 0, nullptr);
            SQLBindParameter(insertStmt, 3, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &gridX, 0, nullptr);
            SQLBindParameter(insertStmt, 4, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &gridY, 0, nullptr);
            SQLBindParameter(insertStmt, 5, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 0, 0, &rotated, 0, nullptr);
            SQLBindParameter(insertStmt, 6, SQL_PARAM_INPUT, SQL_C_SHORT, SQL_SMALLINT, 0, 0, &stackCount, 0, nullptr);

            const SQLRETURN ret = SQLExecDirectA(insertStmt,
                const_cast<SQLCHAR*>(reinterpret_cast<const SQLCHAR*>(
                    "INSERT INTO dbo.PlayerInventoryItems (ItemId, AccountId, GridX, GridY, IsRotated, StackCount) "
                    "VALUES (?, ?, ?, ?, ?, ?)")),
                SQL_NTS);
            if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO)
            {
                LogDiag("SaveInventory: INSERT", SQL_HANDLE_STMT, insertStmt);
                ok = false;
            }
        }

        SQLEndTran(SQL_HANDLE_DBC, hdbc_, ok ? SQL_COMMIT : SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);

        return ok;
    }
}
