#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sqlite3.h>
#include <windows.h>
#include <direct.h>
#include <sys/types.h>
#include <errno.h>

#define DB_NAME "file_inventory.db"

void create_tables(sqlite3 *db) {
    const char *sql[] = {
        // Main files table with extension column
        "CREATE TABLE IF NOT EXISTS files ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "path TEXT NOT NULL,"
        "extension TEXT,"
        "size INTEGER,"
        "is_directory INTEGER,"
        "created_time INTEGER,"
        "modified_time INTEGER,"
        "accessed_time INTEGER,"
        "attributes TEXT,"
        "is_deleted INTEGER DEFAULT 0,"
        "last_seen INTEGER"
        ");",
        
        // Create indexes for better performance
        "CREATE INDEX IF NOT EXISTS idx_path ON files(path);",
        "CREATE INDEX IF NOT EXISTS idx_extension ON files(extension);"
    };

    char *err_msg = NULL;
    for (int i = 0; i < sizeof(sql) / sizeof(sql[0]); i++) {
        if (sqlite3_exec(db, sql[i], 0, 0, &err_msg) != SQLITE_OK) {
            fprintf(stderr, "SQL error: %s\n", err_msg);
            sqlite3_free(err_msg);
            exit(1);
        }
    }
}

void get_file_extension(const char *filename, char *extension, size_t ext_size) {
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        const char *src = dot + 1;
        char *dst = extension;
        size_t i = 0;
        while (*src && i < ext_size - 1) {
            *dst = tolower(*src);
            dst++;
            src++;
            i++;
        }
        *dst = '\0';
    } else {
        extension[0] = '\0';
    }
}

void get_file_attributes_string(DWORD attrs, char *attr_str, size_t attr_size) {
    memset(attr_str, 0, attr_size);
    char *ptr = attr_str;
    
    if (attrs & FILE_ATTRIBUTE_READONLY)  *ptr++ = 'R';
    if (attrs & FILE_ATTRIBUTE_HIDDEN)    *ptr++ = 'H';
    if (attrs & FILE_ATTRIBUTE_SYSTEM)    *ptr++ = 'S';
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) *ptr++ = 'D';
    if (attrs & FILE_ATTRIBUTE_ARCHIVE)   *ptr++ = 'A';
    if (attrs & FILE_ATTRIBUTE_NORMAL)    *ptr++ = 'N';
    *ptr = '\0';
}

__int64 filetime_to_unix_time(FILETIME ft) {
    __int64 ll = ((__int64)ft.dwHighDateTime << 32) + ft.dwLowDateTime;
    return (ll - 116444736000000000LL) / 10000000LL;
}

void scan_directory(sqlite3 *db, const char *path, sqlite3_stmt *insert_stmt, sqlite3_stmt *update_stmt, time_t scan_time) {
    WIN32_FIND_DATA find_data;
    HANDLE find_handle;
    char search_path[MAX_PATH];
    char full_path[MAX_PATH];
    char extension[256];
    char attr_str[32];
    
    snprintf(search_path, sizeof(search_path), "%s\\*", path);
    
    find_handle = FindFirstFile(search_path, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Cannot open directory '%s': %d\n", path, GetLastError());
        return;
    }

    do {
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0)
            continue;
            
        if (strcmp(find_data.cFileName, DB_NAME) == 0)
            continue;

        snprintf(full_path, sizeof(full_path), "%s\\%s", path, find_data.cFileName);
        get_file_extension(find_data.cFileName, extension, sizeof(extension));
        get_file_attributes_string(find_data.dwFileAttributes, attr_str, sizeof(attr_str));

        __int64 file_size = ((__int64)find_data.nFileSizeHigh << 32) + find_data.nFileSizeLow;
        __int64 created_time = filetime_to_unix_time(find_data.ftCreationTime);
        __int64 modified_time = filetime_to_unix_time(find_data.ftLastWriteTime);
        __int64 accessed_time = filetime_to_unix_time(find_data.ftLastAccessTime);

        // Try to update existing record first
        sqlite3_bind_int64(update_stmt, 1, file_size);
        sqlite3_bind_int64(update_stmt, 2, modified_time);
        sqlite3_bind_int64(update_stmt, 3, accessed_time);
        sqlite3_bind_text(update_stmt, 4, attr_str, -1, SQLITE_STATIC);
        sqlite3_bind_int64(update_stmt, 5, scan_time);
        sqlite3_bind_text(update_stmt, 6, full_path, -1, SQLITE_STATIC);

        if (sqlite3_step(update_stmt) != SQLITE_DONE) {
            fprintf(stderr, "Update SQL error: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_reset(update_stmt);

        // If no rows were updated, insert new record
        if (sqlite3_changes(db) == 0) {
            sqlite3_bind_text(insert_stmt, 1, find_data.cFileName, -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 2, full_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(insert_stmt, 3, extension, -1, SQLITE_STATIC);
            sqlite3_bind_int64(insert_stmt, 4, file_size);
            sqlite3_bind_int(insert_stmt, 5, (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
            sqlite3_bind_int64(insert_stmt, 6, created_time);
            sqlite3_bind_int64(insert_stmt, 7, modified_time);
            sqlite3_bind_int64(insert_stmt, 8, accessed_time);
            sqlite3_bind_text(insert_stmt, 9, attr_str, -1, SQLITE_STATIC);
            sqlite3_bind_int64(insert_stmt, 10, scan_time);

            if (sqlite3_step(insert_stmt) != SQLITE_DONE) {
                fprintf(stderr, "Insert SQL error: %s\n", sqlite3_errmsg(db));
            }
            sqlite3_reset(insert_stmt);
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory(db, full_path, insert_stmt, update_stmt, scan_time);
        }
    } while (FindNextFile(find_handle, &find_data));

    FindClose(find_handle);
}

int main(int argc, char *argv[]) {
    sqlite3 *db;
    sqlite3_stmt *insert_stmt, *update_stmt, *mark_deleted_stmt;
    char *err_msg = NULL;
    int refresh_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-refresh") == 0) {
            refresh_mode = 1;
            break;
        }
    }

    if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, &err_msg);
    
    create_tables(db);

    time_t scan_time = time(NULL);

    sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, &err_msg);

    const char *insert_sql = 
        "INSERT INTO files (name, path, extension, size, is_directory, created_time, "
        "modified_time, accessed_time, attributes, last_seen) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    const char *update_sql =
        "UPDATE files SET "
        "size = ?, modified_time = ?, accessed_time = ?, "
        "attributes = ?, is_deleted = 0, last_seen = ? "
        "WHERE path = ?;";

    const char *mark_deleted_sql =
        "UPDATE files SET is_deleted = 1 "
        "WHERE last_seen < ? AND is_deleted = 0;";
    
    if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, mark_deleted_sql, -1, &mark_deleted_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statements: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    scan_directory(db, ".", insert_stmt, update_stmt, scan_time);

    if (refresh_mode) {
        sqlite3_bind_int64(mark_deleted_stmt, 1, scan_time);
        if (sqlite3_step(mark_deleted_stmt) != SQLITE_DONE) {
            fprintf(stderr, "Mark deleted SQL error: %s\n", sqlite3_errmsg(db));
        }
    }

    sqlite3_exec(db, "COMMIT;", 0, 0, &err_msg);

    sqlite3_finalize(insert_stmt);
    sqlite3_finalize(update_stmt);
    sqlite3_finalize(mark_deleted_stmt);
    sqlite3_close(db);

    printf("File inventory has been %s in %s\n", 
           refresh_mode ? "refreshed" : "created", DB_NAME);
    return 0;
}